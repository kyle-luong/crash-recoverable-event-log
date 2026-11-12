#ifndef EVENTLOG_SCRATCH_DIR_H_
#define EVENTLOG_SCRATCH_DIR_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "eventlog/error.h"

namespace eventlog {

// A directory this process created and owns, removed when it goes out of scope.
//
// The crash harness and the benchmark both need somewhere to put fixtures and both must
// be sure it is gone afterwards, including on every early return. The rules that make
// that safe live here once rather than twice: the caller's root must not be a symlink,
// the subdirectory must be newly created rather than adopted, and only that subdirectory
// is ever removed. The root itself belongs to the caller.
class ScratchDirectory {
 public:
  ScratchDirectory() = default;
  ~ScratchDirectory() {
    if (keep_ || path_.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;

  // Creates `root` if needed, then a fresh `<prefix>-<pid>-<suffix>` inside it. Fails if
  // that subdirectory already exists, so two concurrent runs cannot share fixtures.
  [[nodiscard]] Status Create(const std::filesystem::path& root, std::string_view prefix,
                              std::string_view suffix);

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path file(std::string_view name) const { return path_ / std::string(name); }

  // Leaves the directory on disk: used to preserve a failing run's evidence.
  void Keep() { keep_ = true; }

 private:
  std::filesystem::path path_;
  bool keep_ = false;
};

}  // namespace eventlog

#endif  // EVENTLOG_SCRATCH_DIR_H_
