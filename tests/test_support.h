#ifndef EVENTLOG_TEST_SUPPORT_H_
#define EVENTLOG_TEST_SUPPORT_H_

// Minimal test harness. The project takes no third-party dependencies, so this provides
// just enough to register cases, assert, and report. Each suite is its own executable and
// includes this header exactly once, which is why main() can live here.

#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "eventlog/error.h"

namespace eventlog::test {

class AssertionFailure : public std::exception {
 public:
  explicit AssertionFailure(std::string message) : message_(std::move(message)) {}
  const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

struct TestCase {
  std::string name;
  std::function<void()> body;
};

inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

struct Registrar {
  Registrar(std::string name, void (*body)()) { Registry().push_back({std::move(name), body}); }
};

template <typename T>
std::string Describe(const T& value) {
  std::ostringstream out;
  if constexpr (std::is_same_v<std::decay_t<T>, std::byte>) {
    out << "byte(0x" << std::hex << static_cast<unsigned>(value) << ")";
  } else if constexpr (std::is_enum_v<std::decay_t<T>>) {
    out << static_cast<std::int64_t>(static_cast<std::underlying_type_t<std::decay_t<T>>>(value));
  } else if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
    out << (value ? "true" : "false");
  } else {
    out << value;
  }
  return out.str();
}

inline int RunAll(const char* suite) {
  int failures = 0;
  std::cout << "[suite] " << suite << " (" << Registry().size() << " cases)\n";
  for (const TestCase& test : Registry()) {
    try {
      test.body();
      std::cout << "  PASS  " << test.name << "\n";
    } catch (const AssertionFailure& failure) {
      std::cout << "  FAIL  " << test.name << "\n        " << failure.what() << "\n";
      ++failures;
    } catch (const std::exception& error) {
      std::cout << "  FAIL  " << test.name << "\n        unexpected exception: " << error.what()
                << "\n";
      ++failures;
    }
  }
  if (failures == 0) {
    std::cout << "[suite] " << suite << ": all " << Registry().size() << " cases passed\n";
  } else {
    std::cout << "[suite] " << suite << ": " << failures << " of " << Registry().size()
              << " cases FAILED\n";
  }
  return failures == 0 ? 0 : 1;
}

// --- scratch directory -------------------------------------------------------

// Every test writes only inside one of these. Removed on destruction even if a case
// throws, which is the scratch-root discipline docs/PRD.md section 6 requires.
class TempDir {
 public:
  explicit TempDir(std::string_view label) {
    static std::atomic<std::uint64_t> counter{0};
    std::error_code ec;
    const auto unique = std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)) +
                        "-" + std::string(label);
    path_ = std::filesystem::temp_directory_path(ec) / ("eventlog-test-" + unique);
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
    if (ec) throw AssertionFailure("could not create scratch dir: " + ec.message());
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path file(std::string_view name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

// --- raw file helpers (tests manufacture damage that the library must detect) ---

inline std::vector<std::byte> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw AssertionFailure("could not read " + path.string());
  std::vector<std::byte> bytes;
  char buffer[4096];
  while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
    const auto got = static_cast<std::size_t>(in.gcount());
    for (std::size_t i = 0; i < got; ++i) bytes.push_back(static_cast<std::byte>(buffer[i]));
    if (!in) break;
  }
  return bytes;
}

inline void WriteFileBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw AssertionFailure("could not write " + path.string());
  for (const std::byte b : bytes) out.put(static_cast<char>(b));
  out.flush();
  if (!out) throw AssertionFailure("failed while writing " + path.string());
}

inline void TruncateFileTo(const std::filesystem::path& path, std::uint64_t length) {
  std::error_code ec;
  std::filesystem::resize_file(path, length, ec);
  if (ec) throw AssertionFailure("could not truncate " + path.string() + ": " + ec.message());
}

inline std::uint64_t FileSizeOf(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) throw AssertionFailure("could not stat " + path.string() + ": " + ec.message());
  return static_cast<std::uint64_t>(size);
}

// Flips one bit so the byte is guaranteed to change, which is what a CRC must catch.
inline void FlipByteAt(const std::filesystem::path& path, std::uint64_t offset) {
  std::vector<std::byte> bytes = ReadFileBytes(path);
  if (offset >= bytes.size()) throw AssertionFailure("flip offset past end of file");
  bytes[offset] = bytes[offset] ^ std::byte{0x01};
  WriteFileBytes(path, bytes);
}

}  // namespace eventlog::test

// --- macros ------------------------------------------------------------------

#define EL_LOCATION (std::string(__FILE__) + ":" + std::to_string(__LINE__))

#define TEST(test_name)                                                                     \
  static void test_name();                                                                  \
  static const ::eventlog::test::Registrar el_registrar_##test_name(#test_name, test_name); \
  static void test_name()

#define EL_FAIL(message) throw ::eventlog::test::AssertionFailure(EL_LOCATION + ": " + (message))

#define EL_CHECK(condition)                                                \
  do {                                                                     \
    if (!(condition)) EL_FAIL(std::string("CHECK failed: ") + #condition); \
  } while (false)

#define EL_CHECK_EQ(actual, expected)                                              \
  do {                                                                             \
    const auto el_actual = (actual);                                               \
    const auto el_expected = (expected);                                           \
    if (!(el_actual == el_expected)) {                                             \
      EL_FAIL(std::string("CHECK_EQ failed: ") + #actual + " == " + #expected +    \
              "\n          actual:   " + ::eventlog::test::Describe(el_actual) +   \
              "\n          expected: " + ::eventlog::test::Describe(el_expected)); \
    }                                                                              \
  } while (false)

#define EL_CHECK_NE(actual, unexpected)                                                       \
  do {                                                                                        \
    const auto el_actual = (actual);                                                          \
    const auto el_unexpected = (unexpected);                                                  \
    if (el_actual == el_unexpected) {                                                         \
      EL_FAIL(std::string("CHECK_NE failed: ") + #actual + " != " + #unexpected + " (both " + \
              ::eventlog::test::Describe(el_actual) + ")");                                   \
    }                                                                                         \
  } while (false)

// Unwraps a Result<T>, failing the test with the error message if it holds an Error.
//
// The return type must be deduced by value. Result<T>::value() && returns T&&, so a
// decltype(auto) return type would deduce T&& and hand back a reference to el_result,
// which dies with the lambda. ASan caught exactly that as a double free.
#define EL_EXPECT_OK(result_expr)                                    \
  ([&]() {                                                           \
    auto&& el_result = (result_expr);                                \
    if (!el_result.ok()) {                                           \
      EL_FAIL(std::string("expected success from ") + #result_expr + \
              " but got: " + el_result.error().message());           \
    }                                                                \
    return std::move(el_result).value();                             \
  })()

// Asserts a Status succeeded.
#define EL_EXPECT_STATUS_OK(status_expr)                             \
  do {                                                               \
    const ::eventlog::Status el_status = (status_expr);              \
    if (!el_status.ok()) {                                           \
      EL_FAIL(std::string("expected success from ") + #status_expr + \
              " but got: " + el_status.error().message());           \
    }                                                                \
  } while (false)

// Asserts a Result<T> or Status failed with a specific ErrorKind.
#define EL_EXPECT_ERROR(expr, expected_kind)                                                       \
  do {                                                                                             \
    auto&& el_outcome = (expr);                                                                    \
    if (el_outcome.ok()) {                                                                         \
      EL_FAIL(std::string("expected ") + #expected_kind + " from " + #expr + " but it succeeded"); \
    }                                                                                              \
    if (el_outcome.error().kind() != (expected_kind)) {                                            \
      EL_FAIL(std::string("expected ") + #expected_kind + " from " + #expr + " but got " +         \
              std::string(::eventlog::ErrorKindName(el_outcome.error().kind())) + ": " +           \
              el_outcome.error().message());                                                       \
    }                                                                                              \
  } while (false)

int main() {
  return ::eventlog::test::RunAll(EVENTLOG_SUITE_NAME);
}

#endif  // EVENTLOG_TEST_SUPPORT_H_
