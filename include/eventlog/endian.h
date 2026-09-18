#ifndef EVENTLOG_ENDIAN_H_
#define EVENTLOG_ENDIAN_H_

#include <cstddef>
#include <cstdint>

namespace eventlog {

// Explicit little-endian serialization. The on-disk format must not depend on host
// endianness, struct padding, or alignment (docs/PRD.md section 3), so these helpers are
// the only sanctioned way to move integers in and out of the file format. Never
// memcpy or reinterpret_cast a struct across that boundary.

inline std::uint64_t ByteValue(std::byte b) {
  return static_cast<std::uint64_t>(b);
}

inline void StoreLe16(std::byte* dst, std::uint16_t value) {
  for (unsigned i = 0; i < 2; ++i) dst[i] = static_cast<std::byte>((value >> (8u * i)) & 0xFFu);
}

inline void StoreLe32(std::byte* dst, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) dst[i] = static_cast<std::byte>((value >> (8u * i)) & 0xFFu);
}

inline void StoreLe64(std::byte* dst, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) dst[i] = static_cast<std::byte>((value >> (8u * i)) & 0xFFu);
}

inline std::uint16_t LoadLe16(const std::byte* src) {
  return static_cast<std::uint16_t>(ByteValue(src[0]) | (ByteValue(src[1]) << 8));
}

inline std::uint32_t LoadLe32(const std::byte* src) {
  return static_cast<std::uint32_t>(ByteValue(src[0]) | (ByteValue(src[1]) << 8) |
                                    (ByteValue(src[2]) << 16) | (ByteValue(src[3]) << 24));
}

inline std::uint64_t LoadLe64(const std::byte* src) {
  std::uint64_t value = 0;
  for (unsigned i = 8; i > 0; --i) value = (value << 8) | ByteValue(src[i - 1]);
  return value;
}

}  // namespace eventlog

#endif  // EVENTLOG_ENDIAN_H_
