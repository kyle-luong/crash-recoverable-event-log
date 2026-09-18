// CPP-AC01 / CPP-AC02, tests CPP-T01 to CPP-T03: the v1 record format must be exact,
// endian-stable, independent of host struct layout, and must reject malformed input.

#include "eventlog/record.h"

#include <array>
#include <string_view>
#include <vector>

#include "eventlog/crc32c.h"
#include "eventlog/endian.h"
#include "test_support.h"

namespace eventlog {
namespace {

std::vector<std::byte> Bytes(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (const char c : text) out.push_back(static_cast<std::byte>(c));
  return out;
}

std::vector<std::byte> Pattern(std::size_t size) {
  std::vector<std::byte> out(size);
  for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);
  return out;
}

// Golden vectors produced by an independent implementation, not by this codec.
TEST(GoldenVectorSequenceOneHello) {
  const std::array<std::byte, 29> expected = {
      std::byte{0x45}, std::byte{0x4C}, std::byte{0x4F}, std::byte{0x47}, std::byte{0x01},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x05}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x8E}, std::byte{0xF1}, std::byte{0x5A}, std::byte{0x97}, std::byte{0x68},
      std::byte{0x65}, std::byte{0x6C}, std::byte{0x6C}, std::byte{0x6F}};

  const std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(1, Bytes("hello")));
  EL_CHECK_EQ(encoded.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (encoded[i] != expected[i]) {
      EL_FAIL("byte " + std::to_string(i) + " is " + test::Describe(encoded[i]) + ", expected " +
              test::Describe(expected[i]));
    }
  }
}

TEST(GoldenVectorEmptyPayload) {
  const std::array<std::byte, 24> expected = {
      std::byte{0x45}, std::byte{0x4C}, std::byte{0x4F}, std::byte{0x47}, std::byte{0x01},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x48}, std::byte{0x74}, std::byte{0xD6}, std::byte{0x2B}};

  const std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(1, {}));
  EL_CHECK_EQ(encoded.size(), std::size_t{kHeaderSize});
  for (std::size_t i = 0; i < expected.size(); ++i) EL_CHECK_EQ(encoded[i], expected[i]);
}

TEST(GoldenVectorSequenceTwo) {
  const std::array<std::byte, 4> payload = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                            std::byte{0xEF}};
  const std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(2, payload));
  // Sequence is at offset 8 as a little-endian uint64.
  EL_CHECK_EQ(LoadLe64(encoded.data() + kSequenceOffset), std::uint64_t{2});
  EL_CHECK_EQ(LoadLe32(encoded.data() + kCrcOffset), 0xBD6CB2E7u);
}

TEST(HeaderLayoutIsExplicitLittleEndian) {
  // Field positions are part of the format. This fails if anyone "optimizes" the codec
  // into a struct memcpy, which would make the file host-dependent.
  const std::vector<std::byte> encoded =
      EL_EXPECT_OK(EncodeRecord(0x0102030405060708ull, Pattern(0x1234)));
  EL_CHECK_EQ(encoded[0], std::byte{'E'});
  EL_CHECK_EQ(encoded[1], std::byte{'L'});
  EL_CHECK_EQ(encoded[2], std::byte{'O'});
  EL_CHECK_EQ(encoded[3], std::byte{'G'});
  EL_CHECK_EQ(LoadLe16(encoded.data() + kVersionOffset), std::uint16_t{1});
  EL_CHECK_EQ(LoadLe16(encoded.data() + kFlagsOffset), std::uint16_t{0});
  // Least significant byte first.
  EL_CHECK_EQ(encoded[kSequenceOffset], std::byte{0x08});
  EL_CHECK_EQ(encoded[kSequenceOffset + 7], std::byte{0x01});
  EL_CHECK_EQ(encoded[kPayloadLengthOffset], std::byte{0x34});
  EL_CHECK_EQ(encoded[kPayloadLengthOffset + 1], std::byte{0x12});
  EL_CHECK_EQ(LoadLe32(encoded.data() + kPayloadLengthOffset), std::uint32_t{0x1234});
}

TEST(HeaderSizeIsTwentyFourBytes) {
  EL_CHECK_EQ(kHeaderSize, std::size_t{24});
  EL_CHECK_EQ(kCrcOffset + 4, kHeaderSize);
  EL_CHECK_EQ(kMaxPayloadBytes, std::uint32_t{1048576});
}

// CPP-T01: round-trip at the boundary sizes named in the PRD.
TEST(RoundTripAtBoundarySizes) {
  for (const std::size_t size :
       {std::size_t{0}, std::size_t{1}, std::size_t{128}, std::size_t{kMaxPayloadBytes}}) {
    const std::vector<std::byte> payload = Pattern(size);
    const std::uint64_t sequence = size + 1;
    const std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(sequence, payload));
    EL_CHECK_EQ(encoded.size(), kHeaderSize + size);

    const RecordHeader header = EL_EXPECT_OK(DecodeHeader(encoded));
    EL_CHECK_EQ(header.version, kFormatVersion);
    EL_CHECK_EQ(header.flags, std::uint16_t{0});
    EL_CHECK_EQ(header.sequence, sequence);
    EL_CHECK_EQ(header.payload_length, static_cast<std::uint32_t>(size));
    EL_CHECK_EQ(header.total_size(), kHeaderSize + size);

    const std::span<const std::byte> decoded_payload(encoded.data() + kHeaderSize, size);
    EL_EXPECT_STATUS_OK(VerifyRecordCrc(header, decoded_payload));
    // Payload bytes must survive byte for byte.
    for (std::size_t i = 0; i < size; ++i) EL_CHECK_EQ(decoded_payload[i], payload[i]);
  }
}

// CPP-T02: rejections.
TEST(RejectsOversizedPayload) {
  const std::vector<std::byte> payload(std::size_t{kMaxPayloadBytes} + 1, std::byte{0xAB});
  EL_EXPECT_ERROR(EncodeRecord(1, payload), ErrorKind::kLimitExceeded);
}

TEST(RejectsSequenceZeroOnEncode) {
  EL_EXPECT_ERROR(EncodeRecord(0, Bytes("x")), ErrorKind::kInvalidArgument);
}

TEST(RejectsTruncatedHeader) {
  const std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(1, Bytes("payload")));
  for (std::size_t length = 0; length < kHeaderSize; ++length) {
    const std::span<const std::byte> partial(encoded.data(), length);
    EL_EXPECT_ERROR(DecodeHeader(partial), ErrorKind::kIncompleteTail);
  }
}

TEST(RejectsBadMagic) {
  std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(1, Bytes("payload")));
  for (std::size_t i = 0; i < kMagicSize; ++i) {
    std::vector<std::byte> damaged = encoded;
    damaged[i] = damaged[i] ^ std::byte{0x01};
    EL_EXPECT_ERROR(DecodeHeader(damaged), ErrorKind::kCorruptRecord);
  }
}

TEST(RejectsUnsupportedVersion) {
  for (const std::uint16_t version : {std::uint16_t{0}, std::uint16_t{2}, std::uint16_t{0xFFFF}}) {
    std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(1, Bytes("payload")));
    StoreLe16(encoded.data() + kVersionOffset, version);
    EL_EXPECT_ERROR(DecodeHeader(encoded), ErrorKind::kUnsupportedVersion);
  }
}

TEST(RejectsAnyUnknownFlagBit) {
  // v1 defines no flags, so every one of the 16 bits must be rejected.
  for (unsigned bit = 0; bit < 16; ++bit) {
    std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(1, Bytes("payload")));
    StoreLe16(encoded.data() + kFlagsOffset, static_cast<std::uint16_t>(1u << bit));
    EL_EXPECT_ERROR(DecodeHeader(encoded), ErrorKind::kCorruptRecord);
  }
}

TEST(RejectsImpossiblePayloadLength) {
  // A damaged or hostile length must be refused before anything allocates from it.
  for (const std::uint32_t length : {kMaxPayloadBytes + 1, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
    std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(1, Bytes("payload")));
    StoreLe32(encoded.data() + kPayloadLengthOffset, length);
    EL_EXPECT_ERROR(DecodeHeader(encoded), ErrorKind::kCorruptRecord);
  }
}

TEST(RejectsSequenceZeroOnDecode) {
  std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(1, Bytes("payload")));
  StoreLe64(encoded.data() + kSequenceOffset, 0);
  EL_EXPECT_ERROR(DecodeHeader(encoded), ErrorKind::kCorruptRecord);
}

TEST(DetectsCrcMismatchInHeaderFields) {
  // Flipping a header field the CRC covers must be caught even though the header still
  // parses structurally.
  const std::vector<std::byte> payload = Pattern(64);
  std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(7, payload));
  StoreLe64(encoded.data() + kSequenceOffset, 8);

  const RecordHeader header = EL_EXPECT_OK(DecodeHeader(encoded));
  EL_CHECK_EQ(header.sequence, std::uint64_t{8});
  EL_EXPECT_ERROR(VerifyRecordCrc(header, std::span<const std::byte>(encoded.data() + kHeaderSize,
                                                                     payload.size())),
                  ErrorKind::kCorruptRecord);
}

TEST(DetectsCrcMismatchInPayload) {
  const std::vector<std::byte> payload = Pattern(200);
  std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(3, payload));
  const RecordHeader header = EL_EXPECT_OK(DecodeHeader(encoded));

  for (const std::size_t index : {std::size_t{0}, std::size_t{99}, payload.size() - 1}) {
    std::vector<std::byte> damaged = encoded;
    damaged[kHeaderSize + index] = damaged[kHeaderSize + index] ^ std::byte{0x80};
    EL_EXPECT_ERROR(VerifyRecordCrc(header, std::span<const std::byte>(damaged.data() + kHeaderSize,
                                                                       payload.size())),
                    ErrorKind::kCorruptRecord);
  }
}

TEST(CrcCoversHeaderPrefixAndPayloadOnly) {
  // The stored CRC field itself is excluded from the checksummed range.
  const std::vector<std::byte> payload = Pattern(32);
  const std::vector<std::byte> encoded = EL_EXPECT_OK(EncodeRecord(5, payload));

  std::vector<std::byte> covered(encoded.begin(), encoded.begin() + kCrcOffset);
  covered.insert(covered.end(), payload.begin(), payload.end());
  EL_CHECK_EQ(ComputeRecordCrc(5, 0, payload), Crc32c(covered));
  EL_CHECK_EQ(LoadLe32(encoded.data() + kCrcOffset), ComputeRecordCrc(5, 0, payload));
}

TEST(EncodeIntoReusesCallerBuffer) {
  std::vector<std::byte> buffer;
  EL_EXPECT_STATUS_OK(EncodeRecordInto(buffer, 1, Pattern(1000)));
  const std::size_t capacity_after_first = buffer.capacity();
  EL_CHECK_EQ(buffer.size(), kHeaderSize + 1000);

  // A second, smaller record must replace the contents rather than append to them.
  EL_EXPECT_STATUS_OK(EncodeRecordInto(buffer, 2, Pattern(10)));
  EL_CHECK_EQ(buffer.size(), kHeaderSize + 10);
  EL_CHECK(buffer.capacity() >= capacity_after_first);
  EL_CHECK_EQ(LoadLe64(buffer.data() + kSequenceOffset), std::uint64_t{2});
}

}  // namespace
}  // namespace eventlog
