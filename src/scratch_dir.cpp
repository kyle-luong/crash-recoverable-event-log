#include "eventlog/scratch_dir.h"

#include <unistd.h>

namespace eventlog {

Status ScratchDirectory::Create(const std::filesystem::path& root, std::string_view prefix,
                                std::string_view suffix) {
  if (root.empty()) {
    return Error(ErrorKind::kInvalidArgument, "a scratch root is required");
  }

  std::error_code ec;
  // This object creates and deletes files under `root`, so it must not be aimed through a
  // symlink (docs/PRD.md section 6).
  if (std::filesystem::is_symlink(root, ec)) {
    return Error(ErrorKind::kInvalidArgument,
                 root.string() + " is a symlink; refusing to use it as a scratch root");
  }
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return Error(ErrorKind::kIo, "could not create " + root.string(), ec.value());
  }

  const std::filesystem::path candidate =
      root / (std::string(prefix) + "-" + std::to_string(::getpid()) + "-" + std::string(suffix));
  // create_directory returns false when the path already exists. Adopting a directory
  // this process did not make would mean deleting files it does not own.
  if (!std::filesystem::create_directory(candidate, ec) || ec) {
    return Error(ErrorKind::kInvalidArgument,
                 "could not create a fresh scratch directory at " + candidate.string());
  }
  path_ = candidate;
  return Status::Ok();
}

}  // namespace eventlog
