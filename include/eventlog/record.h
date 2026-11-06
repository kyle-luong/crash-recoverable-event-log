#ifndef EVENTLOG_RECORD_H_
#define EVENTLOG_RECORD_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "eventlog/error.h"

namespace eventlog {

// Record layout v1 (docs/PRD.md section 3). FROZEN: an incompatible change needs a new
// version number and human approval.
//
//   offset  width  field
//        0      4  magic "ELOG"
//        4      2  version (uint16 LE)
//        6      2  flags   (uint16 LE)
//        8      8  sequence (uint64 LE)
//       16      4  payload_length (uint32 LE)
//       20      4  crc32c (uint32 LE)
//       24    var  payload
//
// CRC32C is the last header field on purpose: the checksummed range is then the
// contiguous header prefix [0, 20) followed immediately by the payload.

inline constexpr std::size_t kMagicSize = 4;
inline constexpr std::array<std::byte, kMagicSize> kMagic = {std::byte{'E'}, std::byte{'L'},
                                                             std::byte{'O'}, std::byte{'G'}};
inline constexpr std::uint16_t kFormatVersion = 1;
// v1 defines no flags, so any set bit is from a format this build does not understand.
inline constexpr std::uint16_t kKnownFlagsMask = 0;
inline constexpr std::size_t kHeaderSize = 24;
inline constexpr std::uint32_t kMaxPayloadBytes = 1u << 20;  // 1 MiB
inline constexpr std::size_t kMaxRecordSize = kHeaderSize + kMaxPayloadBytes;
inline constexpr std::uint64_t kFirstSequence = 1;

inline constexpr std::size_t kMagicOffset = 0;
inline constexpr std::size_t kVersionOffset = 4;
inline constexpr std::size_t kFlagsOffset = 6;
inline constexpr std::size_t kSequenceOffset = 8;
inline constexpr std::size_t kPayloadLengthOffset = 16;
inline constexpr std::size_t kCrcOffset = 20;

static_assert(kCrcOffset + 4 == kHeaderSize, "CRC32C must be the final header field");

using HeaderBytes = std::array<std::byte, kHeaderSize>;

struct RecordHeader {
  std::uint16_t version = kFormatVersion;
  std::uint16_t flags = 0;
  std::uint64_t sequence = 0;
  std::uint32_t payload_length = 0;
  std::uint32_t crc32c = 0;

  std::uint64_t total_size() const {
    return static_cast<std::uint64_t>(kHeaderSize) + payload_length;
  }
};

struct Record {
  std::uint64_t sequence = 0;
  std::vector<std::byte> payload;
};

// CRC32C over header bytes [0, kCrcOffset) followed by the payload.
std::uint32_t ComputeRecordCrc(std::uint64_t sequence, std::uint16_t flags,
                               std::span<const std::byte> payload);

// Serializes the 24-byte header for this record. Payload size must already be in bounds.
void EncodeHeader(std::uint64_t sequence, std::uint16_t flags, std::span<const std::byte> payload,
                  HeaderBytes& out);

// Replaces `out` with one complete record. Fails with kLimitExceeded on an oversized
// payload and kInvalidArgument on sequence 0.
Status EncodeRecordInto(std::vector<std::byte>& out, std::uint64_t sequence,
                        std::span<const std::byte> payload);

// Appends one complete record to whatever `out` already holds, leaving it untouched on
// failure. This is how a batch is assembled into a single contiguous write (ADR-006).
Status AppendRecordTo(std::vector<std::byte>& out, std::uint64_t sequence,
                      std::span<const std::byte> payload);

Result<std::vector<std::byte>> EncodeRecord(std::uint64_t sequence,
                                            std::span<const std::byte> payload);

// Structural header validation: magic, version, unknown flag bits, payload bound.
// Does NOT check the CRC, which needs the payload. `bytes` must be at least kHeaderSize.
Result<RecordHeader> DecodeHeader(std::span<const std::byte> bytes);

// Confirms the stored CRC matches the header fields plus this payload.
Status VerifyRecordCrc(const RecordHeader& header, std::span<const std::byte> payload);

}  // namespace eventlog

#endif  // EVENTLOG_RECORD_H_
