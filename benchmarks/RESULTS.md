# Measured benchmark results

Produced by `eventlog bench` following the procedure in [README.md](README.md). The raw reports are
committed beside this file as `results-64.json`, `results-512.json` and `results-4096.json`, and
every figure below was generated from them rather than transcribed. Each report carries its own
environment block and its own `publishable` flag; all three are marked publishable, meaning a
Release build with sanitizers off on the supported platform.

## Read the spread before the medians

**The durable policies varied by up to 4.3× between the slowest and fastest trial of the
same workload.** `write_only`, which never calls the barrier, varied by at most 1.06×. That
contrast localises the noise precisely: it is `fdatasync` latency in this environment, not the
record path.

So these numbers support a conclusion about *order of magnitude* and nothing finer. Batching the
barrier was worth between 9× and 85× the `sync_each` throughput across the three payload sizes, and
some gain of that scale appeared in every trial of every workload. The exact multipliers below are not stable figures, and the places
where they are non-monotonic — `sync_batch(64)` reaching 85× at 512-byte payloads but
39× at 4096-byte ones — sit inside that spread rather than describing a real
difference between the two workloads.

Five trials is the documented minimum and is visibly not enough to pin a number on this machine.
Anyone wanting stable figures should run on quiet bare metal with more trials; the method supports
up to 50 with `--trials`.

## Environment

| | |
|---|---|
| OS | Linux 6.10.14-linuxkit aarch64 |
| CPU | aarch64 (CPU implementer 0x61, part 0x000) |
| Cores | 10 |
| RAM | 7.7 GiB |
| Filesystem | ext2/ext3/ext4 (container volume) |
| Storage class | unknown |
| Compiler | GCC 12.2.0 |
| Build | Release, sanitizers off |
| Durability barrier | `fdatasync` |
| Load average (1 min) | 1.16 |

**This is a Linux VM, not bare metal.** `linuxkit` in the kernel string is Docker Desktop's virtual
machine on an Apple-silicon host, so the block device under `ext4` is a virtual disk backed by a
file on APFS, and the host was not idle. A virtualised `fdatasync` is a different cost from a real
one, in both directions: it skips physical media but adds a hypervisor round trip. These numbers
describe this machine, which is what the environment block is for.

## Results

### 64-byte payloads, 10,000 records, 5 trials

| Policy | records/sec (median) | records/sec (min–max) | trial spread | ack p50 | ack p95 | ack p99 |
|---|---:|---:|---:|---:|---:|---:|
| `sync_each` | 935 | 790 – 1,076 | 1.36× | 1,254 µs | 2,964 µs | 3,496 µs |
| `sync_batch(16)` | 13,466 | 8,092 – 27,003 | 3.34× | 1,361 µs | 3,106 µs | 5,164 µs |
| `sync_batch(64)` | 32,809 | 29,040 – 86,164 | 2.97× | 1,510 µs | 3,422 µs | 4,773 µs |
| `write_only` \* | 2,598,153 | 2,530,663 – 2,633,722 | 1.04× | 0.38 µs | 0.42 µs | 0.71 µs |

### 512-byte payloads, 10,000 records, 5 trials

| Policy | records/sec (median) | records/sec (min–max) | trial spread | ack p50 | ack p95 | ack p99 |
|---|---:|---:|---:|---:|---:|---:|
| `sync_each` | 867 | 634 – 1,286 | 2.03× | 414 µs | 3,296 µs | 7,298 µs |
| `sync_batch(16)` | 7,959 | 4,273 – 11,021 | 2.58× | 1,630 µs | 3,346 µs | 5,280 µs |
| `sync_batch(64)` | 73,378 | 50,679 – 78,445 | 1.55× | 515 µs | 1,914 µs | 3,208 µs |
| `write_only` \* | 765,065 | 752,157 – 779,804 | 1.04× | 1.25 µs | 1.58 µs | 1.83 µs |

### 4096-byte payloads, 10,000 records, 5 trials

| Policy | records/sec (median) | records/sec (min–max) | trial spread | ack p50 | ack p95 | ack p99 |
|---|---:|---:|---:|---:|---:|---:|
| `sync_each` | 691 | 491 – 941 | 1.92× | 931 µs | 3,376 µs | 7,984 µs |
| `sync_batch(16)` | 16,685 | 9,044 – 17,012 | 1.88× | 624 µs | 2,132 µs | 3,297 µs |
| `sync_batch(64)` | 26,774 | 7,232 – 31,033 | 4.29× | 1,671 µs | 6,135 µs | 9,233 µs |
| `write_only` \* | 108,747 | 104,151 – 110,493 | 1.06× | 8.71 µs | 11 µs | 15 µs |
\* `write_only` is **accepted, not durable**. It never calls the durability barrier, so its latency
column is time to accept a record and durable-ack latency does not apply to it. It is here as the
floor — what the record path costs with no durability at all — and must never be read as a
durability result.

## Throughput against the `sync_each` baseline

Computed from measured medians after the fact, never from a target.

| Payload | `sync_batch(16)` | `sync_batch(64)` | `write_only` |
|---|---:|---:|---:|
| 64 B | 14.4× | 35.1× | 2779× |
| 512 B | 9.2× | 84.6× | 882× |
| 4096 B | 24.2× | 38.8× | 157× |

## What these numbers do and do not show

**Supported by the data:**

- The durability barrier dominates the append path. Median per-acknowledgment latency for a durable
  policy ran from 72× to 4,026× that of `write_only`, which never calls the barrier — a gap far
  outside the trial noise even at its narrowest.
- Batching recovers a large part of that gap: 9.2× to 84.6× the baseline throughput, at every
  payload size tested.
- The gain is sublinear in batch size. `sync_batch(64)` does not reach 64× the `sync_each` baseline
  at 64-byte payloads; amortising a barrier is not the same as removing it.
- Per-acknowledgment latency does not improve with batching, and often worsens, because one
  acknowledgment now covers N records. Throughput and ack latency pull in opposite directions, which
  is the actual tradeoff a batch size is choosing between.
- Every policy wrote byte-identical files for the same workload. The tool checks this and fails the
  run otherwise, so the comparison is between barrier schedules and nothing else.

**Not supported by the data:**

- Any precise speedup multiplier. See the spread.
- Anything about bare-metal or cloud storage performance. One virtual machine was measured.
- Anything about power-loss durability. `fdatasync` returning success is the contract
  ([ADR-001](../docs/decisions/ADR-001-durability-primitive.md)); hardware that lies about flushing
  is outside what any of this proves.
- Any claim that a larger batch is simply better. It is more unacknowledged work to lose when a
  process dies, which `eventlog crash-test` demonstrates rather than argues.

## Reproducing

```console
$ cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DEVENTLOG_SANITIZERS=OFF
$ cmake --build build/release --parallel
$ for p in 64 512 4096; do
    ./build/release/eventlog bench --records 10000 --payload-bytes "$p" --trials 5 \
      --workdir /var/tmp/eventlog-bench --out "out/results-$p.json" --overwrite
  done
```

Payloads come from the deterministic SplitMix64 stream keyed by sequence number, so the workload is
identical on any machine. A build that is not Release-without-sanitizers on Linux will mark its own
report `"publishable": false` and print a warning above the table
([ADR-008](../docs/decisions/ADR-008-write-only-and-benchmark-honesty.md)).
