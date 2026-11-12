#include "eventlog/payload.h"

#include <algorithm>
#include <array>

#include "eventlog/endian.h"

namespace eventlog {
namespace {

constexpr std::uint64_t kGoldenGamma = 0x9E3779B97F4A7C15ull;

}  // namespace

std::uint64_t SplitMix64(std::uint64_t& state) {
  state += kGoldenGamma;
  std::uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

void FillDeterministicPayload(std::uint64_t sequence, std::span<std::byte> out) {
  std::uint64_t state = sequence * kGoldenGamma;
  std::size_t written = 0;
  while (written < out.size()) {
    std::array<std::byte, 8> chunk{};
    StoreLe64(chunk.data(), SplitMix64(state));
    const std::size_t take = std::min<std::size_t>(chunk.size(), out.size() - written);
    for (std::size_t i = 0; i < take; ++i) out[written + i] = chunk[i];
    written += take;
  }
}

std::vector<std::byte> MakeDeterministicPayload(std::uint64_t sequence, std::size_t size) {
  std::vector<std::byte> payload(size);
  FillDeterministicPayload(sequence, payload);
  return payload;
}

}  // namespace eventlog
