# PinnedSlot lease-copy A/B

`bench_pinned_slot_copy` compares 240 KiB, 1 MiB, and 19 MiB payloads using
the real `PinnedSlotWriter` / `PinnedSlotReader` and the public lease API. It
reports the copy itself, the full pin duration, and producer pressure
counters; it asserts only on the measurement pipeline, return status, and
payload correctness, never on cross-host speed.

## Build and run

```bash
cmake -S . -B build-perf15 -DCMAKE_BUILD_TYPE=Release -DTHYME_WERROR=ON
cmake --build build-perf15 -j4 --target bench_pinned_slot_copy
mkdir -p bench-report
./build-perf15/bench_pinned_slot_copy \
  --json bench-report/bench_pinned_slot_copy.json --tag x86_64-local

# Quick correctness test; RUN_SERIAL, and not part of the merge gate.
ctest --test-dir build-perf15 --output-on-failure -j1 -R '^bench_pinned_slot_copy$'
```

The default runner tag is `<arch>-local` (`<arch>-ci` under `CI`) and can be
overridden with `THYME_BENCH_RUNNER_TAG` or `--tag`. The JSON `kind` is
`thyme-bench-pinned_slot-copy` and `schema` is 1; metadata includes git SHA,
host/arch, CPU, toolchain, build type, and runner tag. The three data groups
are:

- `copy_benchmarks`: size, path, iteration count, `ns_per_op`, decimal
  `mb_per_sec`, `ratio_to_memcpy`, and correctness result;
- `pin_benchmarks`: `pin_ns_per_op` for acquire+copy+release;
- `producer_pressure`: write attempts/successful frames, `pinned_skip`,
  `lease_steal`, successful copies, `copy_failures`, `dropped_frames`, payload
  mismatches, and the mean pin time on the pressure path.

This JSON is standalone evidence: it is not written into
`tools/ci/bench_baseline.json` and is not part of the nightly throughput gate.

## Measurement boundary

### The copy itself

Each size runs the following three paths; the source is the payload in the
same real PinnedSlot slot, and the destination is a 64 B-aligned caller
buffer:

1. same-host `std::memcpy` reference;
2. explicit `thyme::detail::AtomicPayloadCopyOut` reference;
3. public `PinnedSlotReader::Lease::CopyPayloadTo`.

Timing covers exactly one copy call. Lease acquire/release, payload
generation, frame publish, and full byte validation all happen outside the
copy timing region. Each round runs 2 warmups then a fixed iteration count;
240 KiB, 1 MiB, and 19 MiB run 24, 12, and 3 iterations per round; the table
and JSON use the median `ns/op` of 3 rounds. Ratios are computed from
throughput:

```text
ratio_to_memcpy = path_MB_s / memcpy_MB_s = memcpy_ns / path_ns
```

**The 90% target constrains only the lease-copy body measured here**; it
excludes acquire/release, producer frame filling, scheduling, wakeups, frame
drops, and pin duration. The full pin path is measured separately and must not
be mixed into, or substituted for, the 90% copy metric.

### Pin and producer pressure

Without producer contention, the atomic and lease paths each measure the full
`Acquire + CopyPayloadTo/AtomicPayloadCopyOut + Release`. The pressure
comparison uses a 4-slot channel with a fixed 24 write attempts; the producer
fills frames as fast as it can while the consumer follows synchronously and
copies; no sleep is injected and no frame skips are manufactured. The mean
pressure pin time uses only the number of actually executed acquires as its
denominator; frames evicted entirely without an acquire do not enter that
mean.

`dropped_frames` includes overwrite losses observed by the consumer,
pinned-skip holes, and read failures; `pinned_skip` is the writer's
independent shared counter, and the two must not be added. `copy_failures`
counts only copy/release failures for frames already acquired. Every
successfully copied frame is compared against the expected full payload; a
copy failure or byte mismatch fails the benchmark, while naturally occurring
skips/drops/steals are reported as-is and are not forced to zero.

## x86 local unoptimised baseline (2026-10-26)

Environment: desktop host / x86_64, 12th Gen Intel Core i5-12490F, GCC 11.4.0,
Release, `runner_tag=x86_64-local`. This is a single short-window sample;
reruns are affected by CPU frequency, cache, and scheduling. At this baseline
`CopyPayloadTo` **has not yet been replaced with lease-memcpy and still goes
through atomic copy plus additional lease checks**; the following are not
optimisation results, and none of the three sizes can claim to meet the 90%
target.

The copy itself:

| payload | path | ns/op | MB/s | vs memcpy |
| --- | --- | ---: | ---: | ---: |
| 240 KiB | memcpy | 10,139.625 | 24,237.58 | 100.00% |
| 240 KiB | atomic | 17,591.667 | 13,970.25 | 57.64% |
| 240 KiB | lease (unoptimised) | 14,483.167 | 16,968.66 | 70.01% |
| 1 MiB | memcpy | 37,120.167 | 28,248.15 | 100.00% |
| 1 MiB | atomic | 41,913.667 | 25,017.52 | 88.56% |
| 1 MiB | lease (unoptimised) | 41,925.250 | 25,010.61 | 88.54% |
| 19 MiB | memcpy | 663,215.667 | 30,039.92 | 100.00% |
| 19 MiB | atomic | 1,452,597.000 | 13,715.40 | 45.66% |
| 19 MiB | lease (unoptimised) | 1,390,299.333 | 14,329.97 | 47.70% |

Full pin without contention:

| payload | atomic pin ns/op | lease pin ns/op |
| --- | ---: | ---: |
| 240 KiB | 9,217.167 | 9,842.292 |
| 1 MiB | 41,140.917 | 41,182.000 |
| 19 MiB | 2,933,691.333 | 2,928,198.667 |

Producer pressure (each row is 24 write attempts):

| payload | path | written frames | pinned_skip | lease_steal | copy ok | copy fail | drop | mismatch | pin ns/op |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 240 KiB | atomic | 21 | 3 | 0 | 11 | 0 | 13 | 0 | 11,806.3 |
| 240 KiB | lease (unoptimised) | 21 | 3 | 0 | 11 | 0 | 13 | 0 | 11,764.8 |
| 1 MiB | atomic | 21 | 3 | 0 | 10 | 0 | 14 | 0 | 32,122.9 |
| 1 MiB | lease (unoptimised) | 21 | 3 | 0 | 10 | 0 | 14 | 0 | 29,891.9 |
| 19 MiB | atomic | 21 | 3 | 0 | 11 | 0 | 13 | 0 | 1,727,939.3 |
| 19 MiB | lease (unoptimised) | 20 | 4 | 0 | 10 | 0 | 14 | 0 | 1,695,724.9 |

These x86 short-window numbers provide same-host A/B evidence only; they are
**not conclusions for AArch64, the whole machine, or HIL**. Once lease-memcpy
lands on main, re-measure on the same host with the build and run commands
above, then re-test the three-size copy ratios and the joint pin/skip/drop
results on the target board/HIL with the same protocol; do not cite this
page's unoptimised numbers as meeting the target.

## x86 local post-optimisation re-measurement (2026-10-26)

The implementation is lease ABI v6 (v5 semantics plus the shared
`ShmSuperHeader`): copy-active and steal arbitrate on the same `pin_gen` atomic
word, and the writer does not write the slot while a copy is active; the owner
is identified by pid+starttime, a live copier is not preempted by timeout, and
a dead owner can be recovered. Concurrent copiers and environments without a
reliable start token keep the atomic fallback; non-lease paths are unchanged.
Protocol and recovery tests are `test_pinned_slot_lease_copy`; the multi-reader
contention regression is `test_pinned_slot_lease_steal`.

Environment as above, on a later mainline revision. The copy itself meets the
90% target at all three sizes:

| payload | memcpy ns/op | atomic ns/op | lease ns/op | lease / memcpy |
| --- | ---: | ---: | ---: | ---: |
| 240 KiB | 3,761.708 | 5,993.250 | 3,845.708 | **97.82%** |
| 1 MiB | 37,970.750 | 39,328.417 | 37,204.833 | **102.06%** |
| 19 MiB | 678,167.333 | 1,375,714.667 | 717,895.333 | **94.47%** |

Full pin without contention (acquire+copy+release):

| payload | atomic pin ns/op | lease pin ns/op |
| --- | ---: | ---: |
| 240 KiB | 8,124.208 | 6,893.875 |
| 1 MiB | 58,328.333 | 54,555.667 |
| 19 MiB | 2,669,349.333 | 2,577,914.000 |

Producer pressure (each row is 24 write attempts):

| payload | path | written frames | pinned_skip | lease_steal | copy ok | copy fail | drop | mismatch | pin ns/op |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 240 KiB | atomic | 20 | 4 | 0 | 9 | 0 | 15 | 0 | 21,793.2 |
| 240 KiB | lease | 21 | 3 | 0 | 12 | 0 | 12 | 0 | 11,057.9 |
| 1 MiB | atomic | 21 | 3 | 0 | 11 | 0 | 13 | 0 | 46,919.4 |
| 1 MiB | lease | 21 | 3 | 0 | 11 | 0 | 13 | 0 | 45,242.8 |
| 19 MiB | atomic | 20 | 4 | 0 | 10 | 0 | 14 | 0 | 2,001,791.4 |
| 19 MiB | lease | 22 | 2 | 0 | 12 | 0 | 12 | 0 | 1,290,095.0 |

The lease path added no copy failures or mismatches this time; skip/drop for
240 KiB and 19 MiB is lower than the atomic control and equal at 1 MiB, so the
copy ratio was not bought with heavy producer frame skipping. Because the
pressure test is a short-window scheduling sample, these counters are for
joint checks and trend comparison, not a promise that every run is identical.
ARM still needs re-testing with the same protocol.
