#include "eventlog/crc32c.h"

#include <array>

namespace eventlog {
namespace {

// Reflected CRC-32C (Castagnoli) polynomial.
constexpr std::uint32_t kPolynomial = 0x82F63B78u;

constexpr std::array<std::uint32_t, 256> MakeTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t byte = 0; byte < 256u; ++byte) {
    std::uint32_t crc = byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = ((crc & 1u) != 0u) ? ((crc >> 1) ^ kPolynomial) : (crc >> 1);
    }
    table[byte] = crc;
  }
  return table;
}

// Built at compile time, so there is no initialization order or thread-safety question.
constexpr std::array<std::uint32_t, 256> kTable = MakeTable();

}  // namespace

std::uint32_t Crc32cUpdate(std::uint32_t crc, std::span<const std::byte> data) {
  // The zlib convention: callers pass and receive a finalized value, and the inversion
  // happens inside, which is what makes chaining across chunks exact.
  std::uint32_t state = crc ^ 0xFFFFFFFFu;
  for (const std::byte b : data) {
    state = kTable[(state ^ static_cast<std::uint32_t>(b)) & 0xFFu] ^ (state >> 8);
  }
  return state ^ 0xFFFFFFFFu;
}

}  // namespace eventlog
