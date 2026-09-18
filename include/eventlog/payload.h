#ifndef EVENTLOG_PAYLOAD_H_
#define EVENTLOG_PAYLOAD_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace eventlog {

// Deterministic synthetic payloads for `append`, `crash-test`, and `bench`.
//
// docs/PRD.md section 3 requires deterministic records but does not define the bytes
// (resolution R3 in docs/SPEC_VALIDATION.md). The content is a SplitMix64 stream keyed by
// the record's sequence number, stored little-endian, so a given (sequence, size) pair
// yields identical bytes on every platform and any reader can regenerate the expected
// payload to check a recovered log.
void FillDeterministicPayload(std::uint64_t sequence, std::span<std::byte> out);

std::vector<std::byte> MakeDeterministicPayload(std::uint64_t sequence, std::size_t size);

}  // namespace eventlog

#endif  // EVENTLOG_PAYLOAD_H_
