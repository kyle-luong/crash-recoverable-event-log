#ifndef EVENTLOG_FILE_OPS_H_
#define EVENTLOG_FILE_OPS_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include "eventlog/error.h"

namespace eventlog {

// Owning file descriptor. Every descriptor in this project lives in one of these; there
// are no bare close() calls in logic code.
class FileDescriptor {
 public:
  FileDescriptor() = default;
  explicit FileDescriptor(int fd) : fd_(fd) {}
  ~FileDescriptor() { reset(); }

  FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  [[nodiscard]] int release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  // Closes and discards any error. Used by the destructor, where nothing can be reported.
  void reset();
  // Closes and reports failure. close() is never retried on EINTR: on Linux the
  // descriptor is already released, so a retry could close an unrelated file.
  [[nodiscard]] Status Close();

 private:
  int fd_ = -1;
};

// Narrow POSIX boundary. Justified by docs/PRD.md section 3: the fault tests must be able
// to produce short writes, EINTR, EIO, and a failed sync without depending on OS timing.
// Normal runtime uses PosixFileOps, which does nothing but call the syscalls.
class FileOps {
 public:
  virtual ~FileOps() = default;

  // One raw write. May legitimately transfer fewer bytes than requested; callers use
  // WriteFully rather than assuming completion.
  virtual Result<std::size_t> Write(int fd, std::span<const std::byte> data) = 0;

  // Positional read of up to out.size() bytes, returning how many were transferred --
  // short only at end of file. Positional so a reader cannot disturb the offset of
  // anything else holding the same file.
  //
  // On this boundary so a read fault can be injected as well as a write one. An I/O
  // error while reading must be reported as an I/O error and never as corruption, and
  // that distinction is only worth having if a test can produce the error.
  virtual Result<std::size_t> ReadAt(int fd, std::uint64_t offset, std::span<std::byte> out) = 0;

  // The durability barrier (ADR-001): fdatasync on Linux, fcntl(F_FULLFSYNC) on macOS.
  // Nothing may be reported durable until this returns success.
  virtual Status SyncData(int fd) = 0;

  // Makes a newly created file's directory entry durable.
  virtual Status SyncDirectory(const std::filesystem::path& directory) = 0;

  virtual Status Truncate(int fd, std::uint64_t length) = 0;
  virtual Result<std::uint64_t> FileSize(int fd) = 0;
};

class PosixFileOps final : public FileOps {
 public:
  Result<std::size_t> Write(int fd, std::span<const std::byte> data) override;
  Result<std::size_t> ReadAt(int fd, std::uint64_t offset, std::span<std::byte> out) override;
  Status SyncData(int fd) override;
  Status SyncDirectory(const std::filesystem::path& directory) override;
  Status Truncate(int fd, std::uint64_t length) override;
  Result<std::uint64_t> FileSize(int fd) override;
};

std::shared_ptr<FileOps> DefaultFileOps();

// Which primitive SyncData actually calls on this build, for honest result reporting.
std::string_view SyncPrimitiveName();

// Reads every byte of `out` unless end of file comes first, returning how many bytes
// were transferred. Retries only EINTR.
Result<std::size_t> ReadFullyAt(FileOps& ops, int fd, std::uint64_t offset,
                                std::span<std::byte> out);

// Writes every byte of `data`, looping over short writes and retrying only EINTR.
// `file_offset` is the file position `data` starts at, used for error context only.
Status WriteFully(FileOps& ops, int fd, std::span<const std::byte> data, std::uint64_t file_offset);

struct OpenedFile {
  FileDescriptor fd;
  bool created = false;
};

// Every open below requires the path to resolve to a regular file. The format is read
// with pread(2), sized with fstat(2) and shortened with ftruncate(2), none of which mean
// anything on a pipe, socket, or device, so this is a correctness requirement rather than
// a policy choice. It is checked on the descriptor, never on the path, so nothing can be
// swapped in between the check and the open.
//
// O_WRONLY|O_APPEND|O_CREAT, mode 0644. O_APPEND means a buggy offset can never
// overwrite committed data in an append-only log.
Result<OpenedFile> OpenLogForAppend(const std::filesystem::path& path);
Result<FileDescriptor> OpenLogForRead(const std::filesystem::path& path);
// For recovery. Opens O_WRONLY|O_NOFOLLOW so a symlink cannot redirect a truncation.
Result<FileDescriptor> OpenLogForUpdate(const std::filesystem::path& path);

// flock(LOCK_EX|LOCK_NB). Excludes cooperating writers only; a process that ignores
// flock is not prevented from writing, and this project does not claim otherwise.
Status AcquireExclusiveLock(int fd, const std::filesystem::path& path);

}  // namespace eventlog

#endif  // EVENTLOG_FILE_OPS_H_
