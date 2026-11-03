# Mono↔wall-clock conversion report

This report closes the conversion-error and rejection-rule exit criteria for
clock conversion. The NTP / 72 h on-device evidence remains open and is not
claimed as passing here.

## Inputs and outputs

| Source | Anchor | Reader |
|---|---|---|
| Trigger package / continuous slice `wal.log` | `kWalOpClockAnchor`: `index = CLOCK_MONOTONIC`, `mono_ns = CLOCK_REALTIME` | `ReadPackageClockAnchors` → `MonoToRealtimeNs` |
| perf log block type 10 | 16 B `mono_ns` / `realtime_ns`, refreshed per generation and on periodic dumps | `thyme_log_merge` converts independently per input file and emits an optional `realtime_ns` |

The replay data plane uses only the original `CLOCK_MONOTONIC` axis. The
wall-clock envelope does not enter `thyme-replay play`, and deterministic
re-recording explicitly exempts wall-clock fields.

Old packages have no anchor rows: `ReadPackageClockAnchors` returns an empty
list (success, no fabrication). Truncation, invalid fields, and mono rollback
all fail and must not be treated as "no anchor".

## Algorithm (no interpolation)

Given an ordered list `anchors[i] = (mono_i, real_i)`:

1. Select the **last entry not later than the target `t`**; if `t` precedes
   every anchor, use the first one.
2. `realtime(t) = real_selected ± |t − mono_selected|` (in the matching
   direction).
3. Check for `uint64` overflow before adding or subtracting; on overflow
   return `false` rather than wrapping.

There is no linear interpolation between adjacent anchors. Any NTP step or
slew between adjacent anchors takes effect only after the later anchor;
interpolation would invent an intermediate wall clock near a step.

Implementation: `src/record/clock_anchor.cpp`. `thyme_log_merge` keeps an
independent "latest anchor" per input file, with the same rules.

Regression (`test_package_integrity::TestClockAnchors`):

| Anchors | Target mono | Result | Meaning |
|---|---|---|---|
| `(100, 1000), (200, 5000)` | 50 | 950 | Before the first anchor: use the first, subtract backwards |
| same | 150 | 1050 | Between two anchors: **no** interpolation to 3000, still follows the first |
| same | 250 | 5050 | After the last anchor: use the second |
| `[]` | any | reject | An empty list does not fabricate a wall clock |
| `(0, UINT64_MAX)` | 1 | reject | Forward overflow |
| `(1, 0)` | 0 | reject | Backward underflow |

## Error bound

The anchor refresh period is **1 s** (`triggered_recorder_package.cpp`: the
non-force path skips when `now - last < 1e9 ns`; creation and seal force a
write).

Between two anchors:

```
|realtime_est(t) − CLOCK_REALTIME(t)|
  ≤ |CLOCK_REALTIME slew/step during (mono_selected, t]|
  ≤ max NTP step observed in that 1 s window
    + |freq error| × (t − mono_selected)
```

Without an NTP log, software can only promise:

- **Quantisation error**: the conversion itself is exact integer addition and
  subtraction, with no rounding.
- **Sampling error**: the wall clock is read at the instant the anchor is
  written; the scheduling delay between the two `clock_gettime` calls is
  normally far below 1 ms and is not modelled separately.
- **Drift error**: determined by the host NTP/chrony; thyme does not
  compensate. The 1 s refresh bounds unobserved drift to one refresh period.
- **Step error**: if NTP steps by 1 s between two anchors, every conversion in
  that window is off by at most 1 s; it recovers after the next anchor.

The verifiable bound this report gives is therefore **the host wall-clock
change within one refresh period**, not an internal algorithm error. The
algorithm error is 0.

The remaining evidence is a continuous 72 h `|est − CLOCK_REALTIME|` curve
under a real NTP environment. Without that curve, this item must not be marked
as fully evidenced.

## Rejection and compatibility

| Case | Behaviour |
|---|---|
| No anchors (legacy package / perf log without block type 10) | Emit no wall clock |
| Anchor mono rollback, truncation, invalid WAL field | Parse failure; clear the list already read |
| Add/subtract overflow | `MonoToRealtimeNs` returns false |
| Replay / re-record | Continue to use mono only; the wall clock takes no part in determinism comparison |

All persistent readers convert uniformly: one place on the package side
(`clock_anchor.{h,cpp}`) and one place on the perf side (`thyme_log_merge`).
probe/replay do not invent a second formula.
