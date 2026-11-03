# CRC and copy budget

This page records a reproducible measurement method for the 240 KiB
(245,760 B) CRC, a real in-process `NotifPipe` copy vs in-place comparison,
cross-process SHM copies of small/medium structs, and the
`RecordMirrorRingPage` tap copy. `bench_copy_budget` asserts only on **path
and data correctness**, never on ns or throughput; every run emits both a
human-readable table and machine-readable JSON.

## Running

```bash
cmake -S . -B build-perf07 -DCMAKE_BUILD_TYPE=Release -DTHYME_WERROR=ON
cmake --build build-perf07 -j4 --target bench_copy_budget
mkdir -p bench-report
./build-perf07/bench_copy_budget \
  --json bench-report/bench_copy_budget.json --tag x86_64-local

# CTest likewise gates only the measurement pipeline and correctness; RUN_SERIAL avoids contending with other tests.
ctest --test-dir build-perf07 --output-on-failure -j1 -R '^bench_copy_budget$'
```

The default runner tag is `<arch>-local` (`<arch>-ci` under `CI`) and can be
overridden with `THYME_BENCH_RUNNER_TAG` or `--tag`. The JSON `kind` is
`thyme-bench-copy-budget` and carries the git SHA, hostname/arch, CPU,
toolchain, build type, runner tag, iterations per batch, the median
`ns_per_op` over three rounds, decimal `mb_per_sec`, the timing range, and the
correctness result. It is standalone budget evidence: it is not written into
`tools/ci/bench_baseline.json` and is not part of the nightly throughput gate
or fuzz runs.

Each round runs 4 untimed warmups, then a fixed iteration count; the table and
JSON use the median `ns/op` of 3 rounds. For 512 B and 16 KiB, the
NotifPipe/SHM/tap measurements run 2,000 and 800 iterations per round; 240 KiB
runs 1,000 per round; the 240 KiB CRC runs 30 per round.

## Measurement boundary

- **CRC**: calls the current `Crc32Update` hardware dispatch path and the
  `detail::Crc32UpdateBitwise` reference path separately, and cross-checks
  their results on identical input. Covers only the 240 KiB payload CRC, not
  frame publish, payload copy, or disk write.
- **In-process NotifPipe**: on a real `NotifPipe<T, 2>`, the same thread
  completes one publish + `TryPop` before starting the next. The copy row's
  timing includes `Push`'s copy-in, `CommitWrite`, and `TryPop`'s copy-out;
  the in-place row includes `BeginWrite`, the caller filling the slot's
  generation/body, `CommitWrite`, and the same copy-out. Both paths
  pre-materialise the source body, so source-object construction is not timed;
  full generation and body validation happen outside the timing region. No
  Notify target is set, so there is no cross-thread wait, blocking wakeup, or
  backpressure; the comparison is publish/pop cost, not end-to-end event
  latency.
- **Cross-process SHM**: the child writes through
  `ShmSpmcRing<T, 2>::Push` and the parent calls `SpmcReader::ReadLatest`
  through an independently opened mapping. This is not a single-process
  `memcpy` model; the 240 KiB travels directly over the SPMC POD transport,
  which neither needs nor pretends to use the
  `PinnedSlotWriter`/`PinnedSlotReader` channel. The write side includes atomic payload copy-in,
  seq/head publication, and `Notify`, but no tap/CRC; the read side includes
  head/seq validation, atomic payload copy-out, and a second seq read.
- **tap**: measures exactly `RecordMirrorRingPage::TryWriteRing`, the call the
  production tap makes, while the parent calls `ReadRingSample` through a
  read-only SHM mapping. The write side includes the CAS seqlock, metadata,
  the atomic copy, and `ring_head` publication; the read side includes
  index/capacity checks, the atomic copy, and a seq re-check. It excludes
  transport `Push`/`Notify`, `HarvestWindow`, serialisation, and disk writes.

Parent and child keep a single operation in flight with a request/ack pipe per
iteration, avoiding latest/ring overwrites; pipe waits and byte-by-byte
correctness checks are outside the timing region. These values are therefore
**the per-side cost of each operation**, not cross-process round-trip latency.
All reads and writes validate the full payload; the tap also validates the
mono/source sequence for `publish_index`.

## x86 local sample (2026-10-26)

Environment: desktop host / x86_64, 12th Gen Intel Core i5-12490F, GCC 11.4.0,
Release, `runner_tag=x86_64-local`, `crc32_hw_accelerated=true`. The CRC, SHM,
and tap tables reuse the copy-budget samples from an earlier work tree; the
NotifPipe table comes from this round's re-measurement on a later base
revision. These are single short-window samples; rerunning the command
produces a new JSON file and git revision.

CRC (240 KiB/op):

| Path | ns/op | MB/s | Relative to reference |
| --- | ---: | ---: | ---: |
| Current hardware dispatch `Crc32Update` | 23,211.3 | 10,587.93 | 88.2× |
| bitwise reference | 2,047,887.4 | 120.01 | 1× |

Real in-process NotifPipe (one full publish + `TryPop` per cell; decimal MB/s):

| payload | copy ns/op | copy MB/s | in-place ns/op | in-place MB/s | Saving this round |
| --- | ---: | ---: | ---: | ---: | ---: |
| 512 B | 55.553 | 9,216.51 | 48.129 | 10,638.19 | 13.36% |
| 16 KiB | 487.076 | 33,637.44 | 481.614 | 34,018.96 | 1.12% |
| 240 KiB | 9,937.165 | 24,731.40 | 9,910.485 | 24,797.98 | 0.27% |

MB/s here is one payload divided by the full round-trip ns/op, not total DRAM
bandwidth. In-place removes only copy-in; `TryPop`'s copy-out remains. Gains
for large objects therefore are not promised to scale with size, and this
round's ratio is a short-window observation, not a hard selection threshold.

Real cross-process SPMC (one operation per cell; decimal MB/s):

| payload | `Push` ns/op | `Push` MB/s | `ReadLatest` ns/op | `ReadLatest` MB/s |
| --- | ---: | ---: | ---: | ---: |
| 512 B | 101.0 | 5,070.84 | 136.0 | 3,765.40 |
| 16 KiB | 721.8 | 22,699.52 | 1,528.1 | 10,721.56 |
| 240 KiB | 8,549.3 | 28,746.13 | 10,297.3 | 23,866.42 |

Real record tap (one operation per cell; the read side is another process's
read-only mapping):

| payload | `TryWriteRing` ns/op | Write MB/s | `ReadRingSample` ns/op | Read MB/s |
| --- | ---: | ---: | ---: | ---: |
| 512 B | 133.9 | 3,823.99 | 149.4 | 3,428.17 |
| 16 KiB | 836.2 | 19,592.67 | 1,515.0 | 10,814.74 |
| 240 KiB | 7,588.5 | 32,385.90 | 10,589.8 | 23,207.19 |

`test_crc32_hw` keeps the more complete CRC small-size/segmented/ACLE-model
correctness checks plus an independent 240 KiB microbenchmark;
`bench_atomic_payload` keeps the aligned/unaligned `memcpy` vs atomic-helper
comparison. The irreplaceable evidence on this page is the real in-process
NotifPipe, parent/child SHM/SPMC, and read-only-attacher record tap paths.

## NotifPipe selection and steady-state budget

- **New code uses in-place by default**: follow the
  [coding rules](coding-rules.md) and fill the slot returned by `BeginWrite`
  directly, then `CommitWrite`; handle failure per the backpressure policy,
  and `AbortWrite` if filling is aborted. Keep `Push(const T&)` only for paths
  such as replay/injection that already materialise an object or bytes and
  genuinely need a copy. Do not manufacture another temporary object just for
  API compatibility.
- **Choose the channel by semantics first, then check the budget**:
  `NotifPipe` is same-process SPSC, ordered, full delivery, and `TryPop`
  always copies out. If a same-process full-delivery edge also needs to
  eliminate that copy-out, use the index and acquire/release ownership of
  `ObjectPool + SpscRing`; do not add a private lease to `NotifPipe`. For
  cross-process bulk payloads such as images, point clouds, or H.264, or when
  the consumer needs to hold a lease to avoid materialisation, switch to the
  `PinnedSlotWriter`/`PinnedSlotReader` channel for large frames; the graph's
  `Frame<T>` already has a compile-time 64 KB wire limit, and contracts above
  it must go through PinnedSlot. That 64 KB is an existing wire contract,
  **not** a `NotifPipe` performance knee derived from this table.
- **Do not invent a fixed size threshold**: for larger `T`, account for the
  resident memory `Cap × sizeof(T)`, cache effects, and the actual `rate_hz`.
  Switch channel when the CPU, payload bandwidth, latency, or memory computed
  for the target machine exceeds that edge's budget, or when zero-copy read
  lifetime is genuinely needed; small objects must not skip the semantics and
  backpressure checks merely because of their size.

Start from the configured steady-state rate, not the benchmark's peak
capability:

```text
steady_MB_s = sum(payload_bytes × actual_rate_hz) / 1,000,000
single_core_percent = sum(round_trip_ns × actual_rate_hz) / 10,000,000
max_rate_hz = min(cpu_budget_percent × 10,000,000 / round_trip_ns,
                  bandwidth_budget_MB_s × 1,000,000 / payload_bytes)
```

`max_rate_hz` is compared with the actual rate; the CPU/bandwidth budget is
allocated by the owning link, and this page does not set fixed product values.
If producer and consumer are on different cores, measure and account
separately; do not charge the merged round-trip cost twice to both cores.
Plugging this round's ns numbers into three **hypothetical configurations**
gives the following magnitudes:

| payload and assumed steady-state rate | payload MB/s | copy single-core % | in-place single-core % |
| --- | ---: | ---: | ---: |
| 512 B, 1,000 Hz | 0.512 | 0.00556 | 0.00481 |
| 16 KiB, 100 Hz | 1.6384 | 0.00487 | 0.00482 |
| 240 KiB, 100 Hz | 24.576 | 0.09937 | 0.09910 |

These are only examples of the "actual rate × microbenchmark per-op cost"
budget method; they do not claim the host is running those workloads. Only a
steady-state measurement on the target machine, re-checked under contention,
can serve as a release gate.

## Relationship to the ADR 0008 5% / 50 MB/s threshold

The threshold in
[ADR 0008](../design/adr/0008-swap-ring-copy-on-trigger-deferral.md) applies
to the **actual steady-state workload**: re-evaluate the swap ring only when
the copy volume of all armed taps reaches 50 MB/s, or its CPU usage reaches 5%
of one core. The 32,385.90 MB/s above is the path capability of a single tap
write operation in a short window on this host; it **must not** be compared
directly with 50 MB/s, and it does not mean the system is producing that much
steady-state traffic.

The budget should be computed from the actual channel configuration:

```text
steady_MB_s = sum(payload_bytes × armed_rate_hz) / 1,000,000
single_core_percent = sum(tap_write_ns × armed_rate_hz) / 10,000,000
```

For example, just as an order-of-magnitude conversion: one 240 KiB tap at
100 Hz is 24.576 MB/s, which at this host's 7,588.5 ns write cost is about
0.0759% of one core. 200 Hz is 49.152 MB/s, about 0.1518% of one core. Whether
the ADR threshold actually triggers must be summed over all channels and use
target-machine measurements instead of these ns values; the read side usually
belongs to drain and must not be miscounted as armed steady-state write
traffic. The ADR's currently recorded steady-state load of about 1.2 MB/s of
small structs is still below 50 MB/s; this microbenchmark does not change that
decision.

## Limitations and target-board follow-up

- This page is x86 short-window data only, **not a conclusion for AArch64 or
  HIL**. The target board must be re-measured with the same binary/JSON
  method, and the workload budget computed with the real channel count,
  frequency, and CPU/memory contention.
- Measurements keep one operation in flight and do not create concurrent
  overwrites, seqlock retries, or backpressure; extra re-copies under
  contention are not in these steady-state numbers. Contention correctness is
  covered by `test_record_mirror`, `test_record_mirror_ring_mp`, and
  `test_tap_cross_process`.
- NotifPipe uses a same-thread sequential round trip to isolate data-plane
  cost; it does not cover real producer/consumer concurrency, cache
  contention, Notify targets, wakeups, or queueing delay; those must be
  measured separately by link-level tests.
- Pipe synchronisation is not timed, but synchronisation and short windows
  still affect cache, frequency, and scheduling; when comparing across hosts,
  keep the host/toolchain/build/runner metadata rather than comparing a single
  throughput number.
- The CRC numbers are not the full seal pipeline; tap write and read are
  measured separately and must not be added into a single trigger-drain
  latency without stating that boundary.
