#ifndef EVENTLOG_TESTS_SCRIPTED_FILE_OPS_H_
#define EVENTLOG_TESTS_SCRIPTED_FILE_OPS_H_

// CPP-06: the narrow FileOps simulation shared by the writer, batch, and recovery
// suites. Short writes, EINTR, EIO and a failed sync cannot be produced reliably any
// other way, which is what justifies the FileOps interface existing at all
// (docs/PRD.md section 3). These are simulations of syscall outcomes, never claims
// about real power loss.

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <span>

#include "eventlog/error.h"
#include "eventlog/file_ops.h"

namespace eventlog {
namespace test {

// Narrow test double over the real syscalls. This is the alternate FileOps
// implementation that justifies the interface existing (docs/PRD.md section 3): short
// writes, EINTR, EIO, and a failed sync cannot be produced reliably any other way.
class ScriptedFileOps final : public FileOps {
 public:
  // Caps every write to this many bytes, forcing the short-write loop to run.
  std::size_t max_write_bytes = 0;  // 0 = no cap
  // Returns EINTR this many times in total, across this double's whole lifetime, before
  // writes are allowed through. Not per write: the counter below is never reset.
  int eintr_rounds = 0;
  // After this many successful writes, fail with write_errno. -1 disables.
  int fail_write_after = -1;
  int write_errno = EIO;
  // After this many successful syncs, fail with sync_errno. -1 disables.
  int fail_sync_after = -1;
  int sync_errno = EIO;
  // After this many successful reads, fail with read_errno. -1 disables. A read fault
  // must surface as an I/O error, never as a verdict about the log's contents.
  int fail_read_after = -1;
  int read_errno = EIO;
  // Caps every read to this many bytes, forcing the read loop to run. 0 = no cap.
  std::size_t max_read_bytes = 0;
  // After this many successful directory syncs, fail with sync_directory_errno. -1
  // disables. A new log's directory entry must be durable before any record in it can be
  // acknowledged (ADR-001), so that failure has to be reachable in a test.
  int fail_sync_directory_after = -1;
  int sync_directory_errno = EIO;

  int write_calls = 0;
  int eintr_returned = 0;
  int successful_writes = 0;
  int sync_calls = 0;
  int successful_syncs = 0;
  int read_calls = 0;
  int successful_reads = 0;
  int sync_directory_calls = 0;
  int successful_sync_directories = 0;

  Result<std::size_t> Write(int fd, std::span<const std::byte> data) override {
    ++write_calls;
    if (eintr_returned < eintr_rounds) {
      ++eintr_returned;
      return IoError("write", EINTR, "injected");
    }
    if (fail_write_after >= 0 && successful_writes >= fail_write_after) {
      return IoError("write", write_errno, "injected");
    }
    std::span<const std::byte> slice = data;
    if (max_write_bytes > 0 && slice.size() > max_write_bytes) {
      slice = slice.first(max_write_bytes);
    }
    Result<std::size_t> result = real_.Write(fd, slice);
    if (result.ok()) ++successful_writes;
    return result;
  }

  Result<std::size_t> ReadAt(int fd, std::uint64_t offset, std::span<std::byte> out) override {
    ++read_calls;
    if (fail_read_after >= 0 && successful_reads >= fail_read_after) {
      return IoError("pread", read_errno, "injected");
    }
    std::span<std::byte> slice = out;
    if (max_read_bytes > 0 && slice.size() > max_read_bytes) slice = slice.first(max_read_bytes);
    Result<std::size_t> result = real_.ReadAt(fd, offset, slice);
    if (result.ok()) ++successful_reads;
    return result;
  }

  Status SyncData(int fd) override {
    ++sync_calls;
    if (fail_sync_after >= 0 && successful_syncs >= fail_sync_after) {
      return IoError("fdatasync", sync_errno, "injected");
    }
    Status status = real_.SyncData(fd);
    if (status.ok()) ++successful_syncs;
    return status;
  }

  Status SyncDirectory(const std::filesystem::path& directory) override {
    ++sync_directory_calls;
    if (fail_sync_directory_after >= 0 &&
        successful_sync_directories >= fail_sync_directory_after) {
      return IoError("fsync", sync_directory_errno, "injected");
    }
    Status status = real_.SyncDirectory(directory);
    if (status.ok()) ++successful_sync_directories;
    return status;
  }
  Status Truncate(int fd, std::uint64_t length) override { return real_.Truncate(fd, length); }
  Result<std::uint64_t> FileSize(int fd) override { return real_.FileSize(fd); }

 private:
  PosixFileOps real_;
};

}  // namespace test
}  // namespace eventlog

#endif  // EVENTLOG_TESTS_SCRIPTED_FILE_OPS_H_
