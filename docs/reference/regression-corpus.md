# Bug-regression corpus (TST-05)

Every bug fixed in recent work should have a test that would fail if it came
back. This document maps each `fix(...)` commit to that test, and
[`tools/ci/check_regression_corpus.py`](../../tools/ci/check_regression_corpus.py)
fails the MR gate when a scoped fix commit has no row here. The companion
nightly job (`nightly:repeat`, [TST-08](../guides/test-matrix.md#repeat-until-fail-tst-08))
runs the same fast tests up to 20 times to catch intermittent regressions.

**Scope of the gate.** The checker scans the most recent 250 commits
(`--limit`, the window this table was built from) and requires a row for every
`fix(...)`/`fix:` commit that touches `src/` or `include/`. Fixes confined to
`tools/`, `tests/` or `docs/` are listed when a regression test exists but are
not gated. Rows older than the scan window stay in the table and are accepted
without re-validation.

**Test pointer.** The fourth column names the ctest case (or fuzz target) that
encodes the regression. A fix with no practical in-process test must carry an
explicit marker with the reason:

```markdown
| `abc1234` | record | ... | <!-- no-test: why a test is not practical --> | no test |
```

The marker is deliberate: an empty cell is a checker error, and a reason-free
marker is too. Marking a fix "no regression test" honestly is better than
pointing at a test that cannot fail for that bug.

## Fix commits

<!-- regression-corpus:begin -->

| Fix commit | Area | Bug | Regression test | Coverage |
|---|---|---|---|---|
| `1974e37` | replay | Playlist stopped skipping after the first duplicate and enabled overlap filtering even for non-absolute players | `test_replay_playlist` | covered |
| `41db07e` | record | Control response memcpy could leave the payload unterminated; now uses bounded copy with local termination space and unchanged wire length | `test_record_control` | covered |
| `5274ff6` | record | A complete seal followed by a partial WAL row was accepted because `feof` did not reveal the truncated tail | `test_retention` | covered |
| `8ce24f3` | config | Generated empty/null record manifests disabled subset validation instead of rejecting every recorded channel/select-all plan | `test_record_manifest_subset` | covered |
| `ab40fa9` | test/runtime | Supervisor test stored dangling `c_str()` after vector reallocation; ASan intercept made zero-RLIMIT mlock test invalid | `test_mp_supervisor`, `test_app` (default + ASan/UBSan) | covered |
| `a71699a` | test/trigger | Overflow test assumed two Ticks always drained 17 datagrams despite the adaptive time budget | `test_trigger_overflow` (100 repeats) | covered |
| `ae4a8e1` | runtime | Lease copy owner could survive pre-active process death; steal test also shared one reader's non-atomic counters across threads | `test_pinned_slot_lease_copy`, `test_pinned_slot_lease_steal` | covered |
| `6afde18` | config | Shared trigger/recorder socket passed initial validation but collided audit defaults and broke dump round-trip | `test_trigger_security`, `test_fuzz_trigger_config_full` | covered |
| `a46b92a` | packaging | Package export test forced Release installation regardless of the actual build configuration | `test_package_export` | covered |
| `d3049f7` | test/runtime | PinnedSlot View outlived the reader mapping and crashed its debug watch in default builds | `test_abi_cross_version` | covered |
| `a8d6101` | record | Re-attach admitted same-hash geometry drift with stale per-struct cursor sizes | `test_triggered_recorder` | covered |
| `c7ab79c` | contracts/runtime | CRC seal left `FrameHeader.size` stale; PinnedSlot geometry helpers lacked compile-time checks | `test_frame_header`, `test_abi_layout` | covered |
| `3ce0c8b` | trigger | Deferred start/preempt rollback cleared an active mute deadline | `test_trigger_preempt_fail` | covered |
| `da71fb0` | ABI | PinnedSlotPageHeader asserts still described the v2 layout after REP-10 | `test_abi_layout` | covered |
| `ae4e1e7` | runtime | LatestSlot sequence widened to 64 bit; wraparound delivered stale/future payloads (RT-03) | `test_latest_slot_seq_wrap` | covered |
| `303cd3d` | record | Retention followed symlinks, no depth cap, deleted active packages and mishandled `.incomplete` (REC-10) | `test_retention_symlink`, `test_retention` | covered |
| `52e4ead` | record | Continuous slices reordered frames from one source across `mono_ns` (REP-07 follow-up) | `test_continuous_mono_order` | covered |
| `738ee2a` | config | TRG-10 reload matrix missed `[recorder]`, channel params and `[chassis]` (SIGHUP drift) | `test_trigger_protocol` | covered |
| `e9b2df8` | runtime | SPSC full-ring pop lost the race with eviction and replayed an older slot | `test_spsc_ring` | covered |
| `418462b` | config | Continuous/channel reloads that require a restart were accepted at SIGHUP | `test_trigger_protocol` | covered |
| `4b04fb7` | config | `wait_idle` accepted without a chassis or `demo_idle` source (TST-14) | `test_trigger_protocol` | covered |
| `518a533` | config | A continuous selector matching no source started an empty recorder (TST-14) | `test_trigger_protocol` | covered |
| `7b10ac6` | config | Duplicate source/quota/channel sections silently used the last one (TST-14) | `test_trigger_protocol` | covered |
| `c0d5e15` | config | Zero and oversized continuous slice sizes were accepted (TST-14) | `test_trigger_protocol` | covered |
| `edf1af5` | config | Continuous-only configs were rejected by the empty-source check | `test_trigger_protocol` | covered |
| `1511d23` | config | Empty topology needed an explicit opt-in instead of a warning (TST-14) | `test_trigger_protocol` | covered |
| `4dd8732` | runtime | Shared wake words were gated on a local waiter count, dropping cross-process wakes | `test_event_wake_word`, `test_shm_spsc_ring` | covered |
| `17992e9` | lease | A failed acquire refreshed a dead pin epoch, keeping the slot pinned (BF-17) | `test_pinned_slot_lease_epoch` | covered |
| `96fb8a0` | record | Post-write raw rollback left a half-committed big frame | `test_continuous_lease_tear`, `test_triggered_recorder` | covered |
| `4d064b7` | lease | Lease epoch loads were not seq_cst; ABI bumped to v4 | `test_pinned_slot_lease_abi`, `test_pinned_slot_lease_epoch` | covered |
| `b4b3f4d` | lease | Acquire validated the slot before pinning; drain did not re-check the post-write state | `test_pinned_slot_lease_epoch`, `test_continuous_lease_tear` | covered |
| `6f11d2f` | lease | Epoch-before-pin ordering left a permanent-pin window | `test_pinned_slot_lease_epoch` | covered |
| `93e3b58` | record | Pre/post harvest partitioned on a head snapshot taken too late | `test_trigger_prescan_boundary` | covered |
| `256a78f` | record | Big-frame captures straddled the pre/post head-snapshot gap | `test_trigger_prescan_boundary`, `test_continuous_pinned_slot` | covered |
| `afb319d` | lease | Epoch was not cleared when the pin count reached zero | `test_pinned_slot_lease_epoch`, `test_lease_live_pin_not_stolen` | covered |
| `5c86979` | lease | `steal_timeout_ns == 0` was accepted at channel Create | `test_pinned_slot_lease_abi` | covered |
| `72b131e` | record | An injected write failure was ignored on the lease drain path | `test_pinned_slot_drain_tear` | covered |
| `cecb3b7` | lease | The debug seq watch stayed armed after a failed AcquireLease | `test_pinned_slot_lease_race_debug` | covered |
| `47bf5ae` | lease | Release-after-steal underflowed the pin count; cells needed generations | `test_pinned_slot_lease_pin`, `test_pinned_slot_lease_steal` | covered |
| `591963a` | perf | Epoch blocks wrapped and mis-unwrapped perf timestamps past 19.5 h | `test_perf_dump_on_crash` | covered |
| `0091ab7` | record | Token-bucket accrual overflowed near the 64-bit nanosecond boundary | `test_rate_limit` | covered |
| `be41acb` | trigger | Caller-supplied trigger timestamps were not validated | `test_trigger_timestamp` | covered |
| `63eaa50` | trigger | The FSM was not rolled back when a deferred preempt/start failed | `test_trigger_preempt_fail` | covered |
| `b88f6f1` | trigger | Unknown `quota_class` values were accepted at startup | `test_quota_class_typo` | covered |
| `764487a` | record | Seal synced the package dir but not the parent dir on rename | <!-- no-test: directory fsync ordering has no in-process observable --> | no test |
| `be0752c` | record | `CloseSlice` ignored seal failure and a new slice started afterwards | `test_roll_after_close_failure` | covered |
| `34f7578` | record | `WalRow` ABI size was implicit; `InvalidatePackage` used element-count fseek | <!-- no-test: the 24-byte layout is a static_assert; fseek path needs a crafted on-disk package --> | no test |
| `3e4b8a2` | record | Default `post_grace_ns` (2 ms) was shorter than the scheduling jitter | `test_post_grace_preempt` | covered |
| `478092c` | record | Big-frame per-frame payload CRC was missing from the index | `test_pinned_slot_raw_crc` | covered |
| `0c27c94` | trigger | `priority_hint` above the source's configured priority was honoured | `test_trigger_priority_clamp` | covered |
| `96de819` | record | Big frames were validated once, then written after the slot changed | `test_pinned_slot_drain_tear` | covered |
| `5e6584d` | record | A seal-path fsync failure left the package looking sealed | `test_package_integrity`, `test_retention` | covered |
| `bef7043` | record | Not every package file was fsynced before the seal row | `test_package_integrity` | covered (seal-fault path) |
| `7a5c6e5` | record | Retention deleted an in-flight package under quota pressure | `test_retention` | covered |
| `99fce77` | record | The rate limit still delayed the Preempt drain | `test_triggered_recorder` | covered |
| `09fb07d` | record-mirror | The seqlock CAS write path allowed the store to become visible before the payload | `test_record_mirror`, `test_seqlock_payload_model` | covered |
| `8644505` | contracts | `FrameHeaderOk` accepted a size/span mismatch | `test_frame`, `test_frame_header` | covered |
| `b40dbdc` | runtime | `THYME_NO_MLOCK` parsing rejected truthy spellings | `test_app` | covered |
| `09b13c3` | record-mirror | `oversize_drop_cnt` was not surfaced by the supervisor/viz bridge | <!-- no-test: generated-scaffold logging only; no runtime assert --> | no test |
| `47f5409` | record-mirror | Init tear window, zero-payload accept, odd-seq recheck and drop logging were inconsistent | `test_record_mirror`, `test_record_mirror_ring` | covered |
| `7146b4d` | record-mirror | Release builds segfaulted on stack-allocated mirror pages | `test_record_large_payload`, `test_record_mirror` | covered |
| `53927c2` | record-mirror | RecordMirrorPage had an Init tear window and an unbounded read clamp | `test_record_mirror` | covered |
| `a9762c8` | gen_graph | `viz_poll` read `seq` directly and lacked a per-slot capacity guard | <!-- no-test: scaffold-generated code has no runtime fixture --> | no test |
| `3f54162` | gen_graph | Scaffold supervisor read slots through the old seq-only field | <!-- no-test: scaffold-generated code has no runtime fixture --> | no test |
| `307feae` | contracts | ACLE CRC32 accumulator convention differed from the software model | `test_crc32_hw` | covered |

## Historical regressions

These fixes predate the current `fix(...)` window or landed under a feature
commit. They are kept here because each one is a documented regression class.

| Fix commit | Area | Bug | Regression test | Coverage |
|---|---|---|---|---|
| `c688f00` | ABI | Cross-version readers accepted unknown trigger v3 flags, `.tbin` version 0 and undefined `FrameHeader` reserved bits (ABI-04) | `test_abi_cross_version` | covered |
| `123318d` | ABI | `FrameHeader.valid` ordering was not enforced by `FrameInit` (ABI-05) | `test_frame_header` | covered |
| `b064ebe` | contracts | `datatypes.json` schema errors surfaced as `TypeError` instead of collected paths (ABI-08) | `test_gen_contracts` | covered |
| `7e7a92f` | ops | Log/perf/package rotation and disk caps did not enforce generations (OPS-05) | `test_log_rotation` | covered |
| `a3f1a05` | obs | Structured logging lacked rate limiting and rotation (OBS-08) | `test_log`, `test_rate_limit` | covered |
| `6ca59d6` | fuzz | Parser bugs found by structured fuzz had no deterministic reproducers (TST-03) | `test_fuzz_thyme_bin`, `test_fuzz_trigger_protocol`, `test_fuzz_trigger_config` | covered |
| `ae28f0b` | runtime | SPMC notify registry raced a late-attaching reader (RT-08) | `test_spmc_notify_lifecycle` | covered |
| `6cfd41c` | record | Replay accepted a record manifest that disagreed with the package (ABI-10) | `test_record_manifest` | covered |
| `b79fcfe` | lease | A live pin could be stolen during the epoch-stamp window (BF-15) | `test_lease_live_pin_not_stolen` | covered |
| `3bc13a4` | replay | Record-replay-record was not deterministic (REP-07) | `test_replay_determinism` | covered |
| `09d47ae` | record | Index seek/read failures were indistinguishable from an absent footer (ARCH-05) | `test_thyme_bin_index` (`TestIndexIoErrors`) | covered |
| `db87983` | trigger | Restarting the center reset one-hour quota windows (TRG-04/T5) | `test_trigger_center_fsm` (`TestPersistentQuota`, `TestQuotaStateClocks`) | covered |
| `4bc9e02` | generator | Emit placement failure was discarded and emit errors lacked graph locations (GEN-01/FUP-26) | `test_gen_graph_atomic`, `test_gen_graph_negative_goldens`, `test_gen_graph_yaml_reference` | covered |
| `f8d61d6` | coverage | Default/refresh coverage export used a nonexistent tool path (CI-04) | `test_check_coverage` | covered |
| `544db25` | runtime | Health drop credit truncated large deltas and wrapped the 32-bit counter (RT-20) | `test_shm_spmc_ring` (saturation and two-reader ledger) | covered |
| `3f708bd` | runtime | Single-writer guard ignored Commit/Abort after a completed section or without Begin (RT-21) | `test_single_writer_guard` (`TestMisuseRejections`) | covered |
| `e09f485` | runtime | Guard accepted mismatched primitive identity (RT-24) | `test_single_writer_guard` (wrong primitive/slot/publish_seq) | covered |
| `76ba00c` | runtime | SPMC per-reader drops had no stable cross-wrapper identity (RT-22) | `test_shm_spmc_ring`, `test_thyme_probe`, `test_abi_cross_version`, `test_spmc_notify_lifecycle`, `test_thyme_metrics_exporter` | covered |
| `b0c51d8` | record | Incremental hashing accepted seek+overwrite as append and ignored external size drift (REC-24) | `test_package_integrity` (`TestIncrementalWriteContract`) | covered |
| `cd49a12` | record | Crash recovery re-read all payloads; stale/truncated checkpoints and same-size WAL overwrites could invalidate the fast path (REC-25) | `test_package_integrity` (`TestHashCheckpoint`, `TestCheckpointAfterSigkill`) | covered |

<!-- regression-corpus:end -->

## Running the checker

The live check is `test_regression_corpus` (labels `unit;mr_gate`) and runs on
every MR. Its fixture tests are `test_check_regression_corpus`
(`unit;mr_gate`). Locally:

```bash
python3 tools/ci/check_regression_corpus.py
python3 tools/ci/test_check_regression_corpus.py
```

A new `fix(...)` commit touching `src/` or `include/` therefore needs one of:

- a row pointing at a test that reproduces the bug,
- an added test (preferred when cheap), or
- an explicit `<!-- no-test: reason -->` marker, reviewed as technical debt.

The checker is intentionally shallow-tolerant: on a shallow CI checkout it
validates the commits that are present and reports the scanned count, so it
never invents a failure for history the runner does not have.
