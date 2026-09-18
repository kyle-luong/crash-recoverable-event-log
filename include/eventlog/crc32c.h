#ifndef EVENTLOG_CRC32C_H_
#define EVENTLOG_CRC32C_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace eventlog {

// CRC-32C (Castagnoli): reflected polynomial 0x82F63B78, init 0xFFFFFFFF, reflected in
// and out, final xor 0xFFFFFFFF. Canonical check value: Crc32c("123456789") == 0xE3069283.
//
// This detects accidental corruption. It is NOT cryptographic authentication and gives no
// protection against a deliberately crafted file (docs/PRD.md section 3).

// Incremental form. Start with crc == 0 and feed consecutive chunks; chaining is exact,
// so Crc32cUpdate(Crc32cUpdate(0, a), b) == Crc32c(a ++ b). The writer relies on this to
// checksum the header prefix and the payload without concatenating them.
std::uint32_t Crc32cUpdate(std::uint32_t crc, std::span<const std::byte> data);

inline std::uint32_t Crc32c(std::span<const std::byte> data) {
  return Crc32cUpdate(0, data);
}

}  // namespace eventlog

#endif  // EVENTLOG_CRC32C_H_
