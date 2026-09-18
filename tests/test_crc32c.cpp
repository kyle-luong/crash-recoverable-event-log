// CPP-AC01: the checksum must match a published CRC-32C implementation, not merely be
// self-consistent. Every vector below was produced by an independent bit-level
// implementation written from the polynomial definition, and the canonical check value
// 0xE3069283 for "123456789" is the one published in the CRC catalogue and RFC 3720.

#include "eventlog/crc32c.h"

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#include "test_support.h"

namespace eventlog {
namespace {

std::vector<std::byte> Bytes(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (const char c : text) out.push_back(static_cast<std::byte>(c));
  return out;
}

TEST(CanonicalCheckValue) {
  // The defining check value of CRC-32C. If this fails, nothing else here is meaningful.
  EL_CHECK_EQ(Crc32c(Bytes("123456789")), 0xE3069283u);
}

TEST(PublishedVectors) {
  EL_CHECK_EQ(Crc32c(Bytes("")), 0x00000000u);
  EL_CHECK_EQ(Crc32c(Bytes("a")), 0xC1D04330u);
  EL_CHECK_EQ(Crc32c(Bytes("The quick brown fox jumps over the lazy dog")), 0x22620404u);

  // The four 32-byte patterns from RFC 3720 appendix B.
  std::vector<std::byte> zeros(32, std::byte{0x00});
  EL_CHECK_EQ(Crc32c(zeros), 0x8A9136AAu);

  std::vector<std::byte> ones(32, std::byte{0xFF});
  EL_CHECK_EQ(Crc32c(ones), 0x62A8AB43u);

  std::vector<std::byte> ascending(32);
  for (std::size_t i = 0; i < ascending.size(); ++i) ascending[i] = static_cast<std::byte>(i);
  EL_CHECK_EQ(Crc32c(ascending), 0x46DD794Eu);

  std::vector<std::byte> descending(32);
  for (std::size_t i = 0; i < descending.size(); ++i) {
    descending[i] = static_cast<std::byte>(31 - i);
  }
  EL_CHECK_EQ(Crc32c(descending), 0x113FDB5Cu);
}

TEST(ChainingMatchesSingleShot) {
  // The record codec checksums the header prefix and then the payload as separate
  // chunks, so exact chaining is a correctness requirement, not a convenience.
  const std::vector<std::byte> head = Bytes("header-bytes-go-here");
  const std::vector<std::byte> tail = Bytes("and then the payload");

  std::vector<std::byte> whole = head;
  whole.insert(whole.end(), tail.begin(), tail.end());

  EL_CHECK_EQ(Crc32cUpdate(Crc32c(head), tail), Crc32c(whole));
}

TEST(ChainingAcrossManySmallChunks) {
  std::vector<std::byte> whole(1000);
  for (std::size_t i = 0; i < whole.size(); ++i) {
    whole[i] = static_cast<std::byte>((i * 7 + 13) & 0xFF);
  }

  std::uint32_t chained = 0;
  for (std::size_t offset = 0; offset < whole.size(); offset += 7) {
    const std::size_t take = std::min<std::size_t>(7, whole.size() - offset);
    chained = Crc32cUpdate(chained, std::span<const std::byte>(whole.data() + offset, take));
  }
  EL_CHECK_EQ(chained, Crc32c(whole));
}

TEST(SingleBitFlipChangesChecksum) {
  std::vector<std::byte> data(256);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::byte>(i);
  const std::uint32_t original = Crc32c(data);

  // A CRC must detect every single-bit error; check all 2048 of them.
  for (std::size_t byte_index = 0; byte_index < data.size(); ++byte_index) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      const std::byte saved = data[byte_index];
      data[byte_index] = saved ^ static_cast<std::byte>(1u << bit);
      EL_CHECK_NE(Crc32c(data), original);
      data[byte_index] = saved;
    }
  }
}

TEST(EmptyInputLeavesRunningValueUnchanged) {
  const std::vector<std::byte> data = Bytes("abcdef");
  const std::uint32_t crc = Crc32c(data);
  EL_CHECK_EQ(Crc32cUpdate(crc, std::span<const std::byte>()), crc);
}

}  // namespace
}  // namespace eventlog
