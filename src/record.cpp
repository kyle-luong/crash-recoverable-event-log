#include "eventlog/record.h"

#include <string>

#include "eventlog/crc32c.h"
#include "eventlog/endian.h"

namespace eventlog {
namespace {

// v1 defines no flags, so every bit is an unknown bit.
constexpr std::uint16_t kUnknownFlagsMask = static_cast<std::uint16_t>(~kKnownFlagsMask);

// Writes everything up to but excluding the CRC field, which is the exact byte range the
// checksum covers.
void FillHeaderPrefix(std::uint64_t sequence, std::uint16_t flags, std::uint32_t payload_length,
                      HeaderBytes& out) {
  for (std::size_t i = 0; i < kMagicSize; ++i) out[kMagicOffset + i] = kMagic[i];
  StoreLe16(out.data() + kVersionOffset, kFormatVersion);
  StoreLe16(out.data() + kFlagsOffset, flags);
  StoreLe64(out.data() + kSequenceOffset, sequence);
  StoreLe32(out.data() + kPayloadLengthOffset, payload_length);
}

}  // namespace

std::uint32_t ComputeRecordCrc(std::uint64_t sequence, std::uint16_t flags,
                               std::span<const std::byte> payload) {
  HeaderBytes header{};
  FillHeaderPrefix(sequence, flags, static_cast<std::uint32_t>(payload.size()), header);
  const std::uint32_t header_crc =
      Crc32cUpdate(0, std::span<const std::byte>(header.data(), kCrcOffset));
  return Crc32cUpdate(header_crc, payload);
}

void EncodeHeader(std::uint64_t sequence, std::uint16_t flags, std::span<const std::byte> payload,
                  HeaderBytes& out) {
  FillHeaderPrefix(sequence, flags, static_cast<std::uint32_t>(payload.size()), out);
  StoreLe32(out.data() + kCrcOffset, ComputeRecordCrc(sequence, flags, payload));
}

Status EncodeRecordInto(std::vector<std::byte>& out, std::uint64_t sequence,
                        std::span<const std::byte> payload) {
  if (sequence < kFirstSequence) {
    return Error(ErrorKind::kInvalidArgument, "sequence numbers start at 1");
  }
  if (payload.size() > kMaxPayloadBytes) {
    return Error(ErrorKind::kLimitExceeded, "payload is " + std::to_string(payload.size()) +
                                                " bytes; the v1 limit is " +
                                                std::to_string(kMaxPayloadBytes));
  }
  HeaderBytes header{};
  EncodeHeader(sequence, 0, payload, header);
  out.clear();
  out.reserve(kHeaderSize + payload.size());
  out.insert(out.end(), header.begin(), header.end());
  out.insert(out.end(), payload.begin(), payload.end());
  return Status::Ok();
}

Result<std::vector<std::byte>> EncodeRecord(std::uint64_t sequence,
                                            std::span<const std::byte> payload) {
  std::vector<std::byte> out;
  Status status = EncodeRecordInto(out, sequence, payload);
  if (!status.ok()) return status.error();
  return out;
}

Result<RecordHeader> DecodeHeader(std::span<const std::byte> bytes) {
  if (bytes.size() < kHeaderSize) {
    return Error(ErrorKind::kIncompleteTail, "header is " + std::to_string(bytes.size()) + " of " +
                                                 std::to_string(kHeaderSize) + " bytes");
  }

  for (std::size_t i = 0; i < kMagicSize; ++i) {
    if (bytes[kMagicOffset + i] != kMagic[i]) {
      return Error(ErrorKind::kCorruptRecord, "record magic is not \"ELOG\"");
    }
  }

  RecordHeader header;
  header.version = LoadLe16(bytes.data() + kVersionOffset);
  if (header.version != kFormatVersion) {
    return Error(ErrorKind::kUnsupportedVersion,
                 "record format version " + std::to_string(header.version) +
                     " is not supported; this build implements version " +
                     std::to_string(kFormatVersion));
  }

  header.flags = LoadLe16(bytes.data() + kFlagsOffset);
  if ((header.flags & kUnknownFlagsMask) != 0u) {
    return Error(ErrorKind::kCorruptRecord,
                 "record sets unknown flag bits 0x" + std::to_string(header.flags));
  }

  // Validated before the payload length is trusted, and the length is validated before
  // any caller allocates a buffer for it.
  header.sequence = LoadLe64(bytes.data() + kSequenceOffset);
  if (header.sequence < kFirstSequence) {
    return Error(ErrorKind::kCorruptRecord, "sequence 0 is not a valid record sequence");
  }

  header.payload_length = LoadLe32(bytes.data() + kPayloadLengthOffset);
  if (header.payload_length > kMaxPayloadBytes) {
    return Error(ErrorKind::kCorruptRecord,
                 "payload length " + std::to_string(header.payload_length) +
                     " exceeds the v1 limit of " + std::to_string(kMaxPayloadBytes));
  }

  header.crc32c = LoadLe32(bytes.data() + kCrcOffset);
  return header;
}

Status VerifyRecordCrc(const RecordHeader& header, std::span<const std::byte> payload) {
  if (payload.size() != header.payload_length) {
    return Error(ErrorKind::kInternal, "payload size does not match the decoded header length");
  }
  const std::uint32_t expected = ComputeRecordCrc(header.sequence, header.flags, payload);
  if (expected != header.crc32c) {
    return Error(ErrorKind::kCorruptRecord,
                 "CRC32C mismatch for sequence " + std::to_string(header.sequence) + ": stored " +
                     std::to_string(header.crc32c) + ", computed " + std::to_string(expected));
  }
  return Status::Ok();
}

}  // namespace eventlog
