# Channel observability audit

Whether a consumer can detect that a channel was overwritten or rejected should
not depend on reading the code and guessing. `tools/ci/channel_audit.py` reads
the generation-time `generated/channel_capabilities.json`, reports a conclusion
per edge, and enforces that risky channels are registered in an allowlist.

## Running

```bash
python3 tools/ci/channel_audit.py \
    --manifest path/to/generated/channel_capabilities.json \
    --allow path/to/channel_audit.allow.json \
    --matrix /tmp/channel_matrix.md
```

Exit codes: 0 = every channel has a conclusion and every risky channel is
registered; 1 = an unregistered risky channel exists; 2 = manifest or argument
error.

Generated output changes are caught directly by the `test_channel_audit` CTest
registration in consuming applications, rather than being discovered later.

## Decision rules

| Channel property | Conclusion | Metric |
|---|---|---|
| `retention=latest` | Consumers **cannot** count dropped frames | None; `TickIn.skipped_<name>` is always 0 (unknown) for pure latest |
| `overflow=reject` (formerly `block`) | The writer receives a failure status | `Publish` return value / `push_fail_count` |
| `overflow=drop` (multi-reader broadcast) | Readers can count dropped frames | `reader_drop_slots` / `drop_count` |
| `transport=pinned_slot` | The writer sees `pinned_skip`; readers see only holes | `pinned_skip` / `lease_steal` plus `FrameGap` in the packet |
| `pinned_slot` with `slot_count < 2` | No pre-record window; every pin is skipped | Error; must be changed |

The first two categories (`latest`, `reject`) and single-slot PinnedSlot must
appear in the allowlist with a non-empty `reason`. Each allowlist entry must
match a real channel, so stale entries cannot silently persist. Allowlist
format:

```json
{"schema": 1,
 "entries": [{"channel": "can_state", "from": "can_ingress",
              "to": "can_worker", "reason": "level-triggered state"}]}
```

## Guarantees already enforced at generation time

- `lossless` cannot be applied to PinnedSlot or bulk channels (`validate.cpp`).
- `latest` across processes must declare a shared-memory kind; `depth`/`read`
  are forbidden on `latest`.
- The `record` flag must be consistent across all edges of a channel.
- `Publish` is `[[nodiscard]]`, so a hand-written node that ignores a failure
  gets a compile-time warning.
- A cyclic node's `TickIn` carries `skipped_<name>`: the number of old frames
  released on each drain of a queue/IndexPipe/follow input. Pure latest is
  always 0 (unknown). See ADR 0016.
