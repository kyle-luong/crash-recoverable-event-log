#include "eventlog/file_ops.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <limits>
#include <string>
#include <utility>

namespace eventlog {
namespace {

#if defined(__linux__)
constexpr std::string_view kSyncPrimitive = "fdatasync";
#elif defined(__APPLE__)
constexpr std::string_view kSyncPrimitive = "fcntl(F_FULLFSYNC)";
#else
constexpr std::string_view kSyncPrimitive = "fsync";
#endif

constexpr int kLogFileMode = 0644;

}  // namespace

std::string_view SyncPrimitiveName() {
  return kSyncPrimitive;
}

void FileDescriptor::reset() {
  if (fd_ >= 0) {
    // Errors are unreportable here. Callers that need to see them use Close().
    ::close(fd_);
    fd_ = -1;
  }
}

Status FileDescriptor::Close() {
  if (fd_ < 0) return Status::Ok();
  const int fd = fd_;
  // Cleared first: after close() the descriptor is gone even if it reported an error, so
  // the destructor must not close it again. EINTR is never retried for the same reason.
  fd_ = -1;
  if (::close(fd) != 0) return IoError("close", errno, "");
  return Status::Ok();
}

Result<std::size_t> PosixFileOps::Write(int fd, std::span<const std::byte> data) {
  const ssize_t written = ::write(fd, data.data(), data.size());
  if (written < 0) return IoError("write", errno, "");
  return static_cast<std::size_t>(written);
}

Result<std::size_t> PosixFileOps::ReadAt(int fd, std::uint64_t offset, std::span<std::byte> out) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return Error(ErrorKind::kInvalidArgument,
                 "read offset " + std::to_string(offset) + " exceeds off_t range");
  }
  const ssize_t got = ::pread(fd, out.data(), out.size(), static_cast<off_t>(offset));
  if (got < 0) return IoError("pread", errno, "");
  return static_cast<std::size_t>(got);
}

Status PosixFileOps::SyncData(int fd) {
  // ADR-001: one boundary, mapped per platform. Nothing is acknowledged durable until
  // this returns success.
#if defined(__linux__)
  if (::fdatasync(fd) != 0) return IoError("fdatasync", errno, "");
  return Status::Ok();
#elif defined(__APPLE__)
  // F_FULLFSYNC asks the drive to flush its own write cache; plain fsync on macOS does
  // not, and would overstate the guarantee.
  if (::fcntl(fd, F_FULLFSYNC) != -1) return Status::Ok();
  const int fullfsync_errno = errno;
  // Some filesystems (network mounts, some virtualized volumes) reject F_FULLFSYNC
  // outright. Fall back to fsync and accept the weaker barrier rather than refusing to
  // write at all; the weaker guarantee is recorded in ADR-001.
  if (fullfsync_errno == ENOTSUP || fullfsync_errno == EOPNOTSUPP || fullfsync_errno == EINVAL) {
    if (::fsync(fd) != 0) return IoError("fsync", errno, "after F_FULLFSYNC was rejected");
    return Status::Ok();
  }
  return IoError("fcntl(F_FULLFSYNC)", fullfsync_errno, "");
#else
  if (::fsync(fd) != 0) return IoError("fsync", errno, "");
  return Status::Ok();
#endif
}

Status PosixFileOps::SyncDirectory(const std::filesystem::path& directory) {
  const int raw = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (raw < 0) return IoError("open", errno, directory.string());
  FileDescriptor dir(raw);
  // A directory is synced with fsync even where data uses a different primitive: what
  // must persist here is the metadata entry itself.
  if (::fsync(dir.get()) != 0) return IoError("fsync", errno, directory.string());
  return dir.Close();
}

Status PosixFileOps::Truncate(int fd, std::uint64_t length) {
  if (length > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return Error(ErrorKind::kInvalidArgument,
                 "truncation length " + std::to_string(length) + " exceeds off_t range");
  }
  if (::ftruncate(fd, static_cast<off_t>(length)) != 0) return IoError("ftruncate", errno, "");
  return Status::Ok();
}

Result<std::uint64_t> PosixFileOps::FileSize(int fd) {
  // `stat` names both this struct and the syscall, so the type needs an elaborated
  // specifier. Confining that to an alias keeps the declaration itself free of one:
  // clang-format 14 and 17 disagree about `struct ::stat info{}` (14 inserts a space
  // before the braces, 17 removes it), and the supported platform's formatter is 14.
  using StatBuf = struct ::stat;
  StatBuf info{};
  if (::fstat(fd, &info) != 0) return IoError("fstat", errno, "");
  if (info.st_size < 0) return Error(ErrorKind::kInternal, "fstat reported a negative size");
  return static_cast<std::uint64_t>(info.st_size);
}

std::shared_ptr<FileOps> DefaultFileOps() {
  // PosixFileOps holds no state, so one shared instance is safe to hand out.
  static const std::shared_ptr<FileOps> ops = std::make_shared<PosixFileOps>();
  return ops;
}

Status WriteFully(FileOps& ops, int fd, std::span<const std::byte> data,
                  std::uint64_t file_offset) {
  std::size_t written = 0;
  int stalled_attempts = 0;
  while (written < data.size()) {
    Result<std::size_t> result = ops.Write(fd, data.subspan(written));
    if (!result.ok()) {
      // EINTR is the only retryable condition: the write was interrupted before
      // transferring anything. Every other error is surfaced with its offset.
      if (result.error().kind() == ErrorKind::kIo && result.error().errno_value() == EINTR) {
        continue;
      }
      Error error = result.error();
      return error.at_offset(file_offset + written);
    }

    const std::size_t chunk = result.value();
    if (chunk > data.size() - written) {
      return Error(ErrorKind::kInternal, "write reported more bytes than were requested")
          .at_offset(file_offset + written);
    }
    if (chunk == 0) {
      // A regular file should never do this. Bounded so a broken FileOps cannot spin.
      if (++stalled_attempts >= 3) {
        return Error(ErrorKind::kIo, "write made no progress after 3 attempts")
            .at_offset(file_offset + written);
      }
      continue;
    }
    stalled_attempts = 0;
    written += chunk;
  }
  return Status::Ok();
}

Result<std::size_t> ReadFullyAt(FileOps& ops, int fd, std::uint64_t offset,
                                std::span<std::byte> out) {
  std::size_t total = 0;
  while (total < out.size()) {
    Result<std::size_t> got = ops.ReadAt(fd, offset + total, out.subspan(total));
    if (!got.ok()) {
      // EINTR is the only retryable condition. Every other error is surfaced as an I/O
      // error, which is a different verdict from corruption and must stay one.
      if (got.error().kind() == ErrorKind::kIo && got.error().errno_value() == EINTR) continue;
      Error error = got.error();
      return error.at_offset(offset + total);
    }
    if (got.value() == 0) break;  // end of file
    total += got.value();
  }
  return total;
}

namespace {

// A log must be a regular file; see the note in file_ops.h. Checked on the descriptor.
//
// O_NONBLOCK on every open above is what makes this reachable for a named pipe: opening
// one for writing would otherwise block until a reader appeared, so the tool would hang
// on a path it should refuse. The flag has no effect on a regular file, which is the only
// thing that gets past here.
Status RequireRegularFile(int fd, const std::filesystem::path& path) {
  using StatBuf = struct ::stat;
  StatBuf info{};
  if (::fstat(fd, &info) != 0) return IoError("fstat", errno, path.string());
  if (!S_ISREG(info.st_mode)) {
    return Error(ErrorKind::kInvalidArgument,
                 path.string() + " is not a regular file; an event log must be one");
  }
  return Status::Ok();
}

}  // namespace

Result<OpenedFile> OpenLogForAppend(const std::filesystem::path& path) {
  // O_EXCL first, so "did this call create the file?" is answered by the kernel instead
  // of by a separate stat that could race. The answer decides whether the directory
  // entry still needs an fsync before anything can be acknowledged.
  int raw = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC | O_NONBLOCK,
                   kLogFileMode);
  if (raw >= 0) return OpenedFile{FileDescriptor(raw), true};
  if (errno != EEXIST) return IoError("open", errno, path.string());

  raw = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NONBLOCK);
  if (raw < 0) return IoError("open", errno, path.string());
  FileDescriptor fd(raw);
  Status regular = RequireRegularFile(fd.get(), path);
  if (!regular.ok()) return regular.error();
  return OpenedFile{std::move(fd), false};
}

Result<FileDescriptor> OpenLogForRead(const std::filesystem::path& path) {
  const int raw = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (raw < 0) return IoError("open", errno, path.string());
  FileDescriptor fd(raw);
  Status regular = RequireRegularFile(fd.get(), path);
  if (!regular.ok()) return regular.error();
  return fd;
}

Result<FileDescriptor> OpenLogForUpdate(const std::filesystem::path& path) {
  // O_NOFOLLOW: a destructive operation must never be redirected through a symlink.
  const int raw = ::open(path.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (raw < 0) return IoError("open", errno, path.string());
  FileDescriptor fd(raw);
  // Truncating a device or a pipe is not a recovery, whatever it is.
  Status regular = RequireRegularFile(fd.get(), path);
  if (!regular.ok()) return regular.error();
  return fd;
}

Status AcquireExclusiveLock(int fd, const std::filesystem::path& path) {
  if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return Status::Ok();
  const int lock_errno = errno;
  if (lock_errno == EWOULDBLOCK || lock_errno == EAGAIN) {
    return Error(ErrorKind::kLockUnavailable,
                 "another cooperating writer already holds the lock on " + path.string(),
                 lock_errno);
  }
  return IoError("flock", lock_errno, path.string());
}

}  // namespace eventlog
