# Upgrade and rollback

This page is the operations manual for upgrading and rolling back thyme
binaries and ABIs. It turns rules 1–9 of
[`abi-compatibility.md`](../reference/abi-compatibility.md) into an executable
sequence and provides two tools:

- `thyme_probe abi`: ABI compatibility preflight before/after an upgrade
  (read-only, see §1);
- `tools/ops/upgrade/thyme_upgrade.sh`: a driver script that follows the
  sections of this document; dry-run by default, it only acts with `--apply`
  (see §6).

For fault handling (disk full, SHM leak, trigger storm, crash recovery) see
[`runbook.md`](runbook.md); for systemd/busybox unit semantics see
[`../deploy/README.md`](../../deploy/README.md).

## 0. Why the switch must be all-or-nothing

thyme's cross-process objects write the version number into the page header
(`PinnedSlotPageHeader`, `ShmSpscRing`, `SharedHealthPage`, v3 `LatestSlot`
…). When versions do not match, a new peer **refuses explicitly** instead of
reading a downgraded layout. Any page-layout change can therefore only be
handled by "stop producer → upgrade everything → recreate SHM" (rule 6); the
v3 `LatestSlot` super-header/geometry also lets old v2 body pages be rejected
explicitly.

An upgrade is thus an action on the **whole session**, not on a single role or
process. The current external rollout baseline is:

| Interface | Current ABI | Upgrade requirement for external components |
| --- | --- | --- |
| `PinnedSlotWriter` / `PinnedSlotReader` (including the lease) | channel **v4**, lease **v6** | cloud/image/h264 producers, consumers and replay/injection tools must be rebuilt from the same tree as thyme; v3 and earlier channels do not run mixed |
| `RecordMirrorRingPage` | **v6**, with an independent `ProducerGate` embedded in every ring descriptor | small-struct producers and replay/injection tools must be rebuilt from the same tree; v5 and earlier mirror pages do not run mixed, and a per-struct gate forbids mixing in old pages |
| `FrameHeader` | 40 B, strict reserved/data-valid validation | external producers that build frame headers by hand migrate per §1.4; recordings with the old 36 B header must be re-recorded |

Replay gating is not an ABI compatibility layer: real producers, replay/
injection tools and the target channel/mirror page must all use the same
version. `thyme-replay --force`, `--no-gate` and
`PinnedSlotWriter::Create(force_replace=true)` cannot make an old peer write a
new page, nor migrate old data.

- Stop the producer before the consumer; never swap binaries while they run;
- every reader/writer (including drivers from external repositories) must come
  from the same tree;
- SHM segment names embed the session pid (`shm_prefix_fmt`, default
  `/c9_<supervisor-pid>`), so a restart naturally creates new segments; old
  segments are stale and are reclaimed per runbook scenario 2;
- rollback is all-or-nothing as well: restore the prefix from the backup tree,
  then recreate the SHM session.

## 1. ABI preflight before upgrading

### 1.1 Constant matrix of this build

```bash
build/tools/inspect/thyme_probe/thyme_probe abi --matrix
# Equivalent alias: thyme_probe --abi-matrix
```

The output is deterministic JSON (deployment scripts consume it as-is; the
golden file is `tools/ops/upgrade/abi_matrix.golden.json`):

```json
{"schema":1,"entries":[
{"component":"pinned_slot","object":"PinnedSlotPageHeader","constant":"kPinnedSlotStoreVersion","version":4,"surface":"shm_page","probe_kind":"pinned_slot","primary":true,"in_page_version":true,"note":"..."},
...
]}
```

- `version` comes straight from the header constant (not a hand-copied
  literal), so the matrix follows version bumps automatically;
- `surface`: `shm_page` / `wire` / `file` / `layout`;
- `probe_kind`: the page type the probe can recognise; `null` means the probe
  does not classify that object (trigger socket, `.thyme.bin`, manifest).
  `MpControl` has had a magic since v3 and is classified as `mp_control`;
- `in_page_version=false`: the object has no in-page version word (for example
  manifest / file rows), or the version lives in the message or file header
  rather than in the SHM page. `LatestSlot<T>` has been a `shm_page` row since
  v3, with the version supplied by `ShmSuperHeader`.

The matrix covers every versioned object in the "current versions" table of
`abi-compatibility.md`. `FrameHeader.frame.version` is the per-contract
`kFrameVersion` generated from `datatypes.json` and is not part of the SHM
preflight; the contract/frame version compatibility matrix is locked by
`test_abi_cross_version`.

### 1.2 Host page preflight

```bash
# Only the current session's segments (the MP supervisor default prefix is c9_; business channels usually use thyme_)
build/tools/inspect/thyme_probe/thyme_probe abi \
    --prefix thyme_ --prefix c9_ \
    --manifest /etc/thyme/probe.manifest        # optional: adds frame_shaped/struct information

# Machine-readable: full report / list of incompatible pages (name expected=N actual=N)
thyme_probe abi --json
thyme_probe abi --mismatches
# Equivalent top-level alias: thyme_probe --check-abi [--json|--mismatches]
```

Exit codes follow the `thyme_probe` convention:

| Code | Meaning | Upgrade implication |
| --- | --- | --- |
| 0 | All pages match this build | Upgrade directly (or you are already on the target version) |
| 2 | No thyme pages found | The host is not running; a normal upgrade |
| 3 | At least one page version/geometry mismatch | Old/new peers coexist; **stop everything and replace everything**, never just half |

Report fields: `name/kind/object/expected/actual/checked/compatible/note`.
PinnedSlot additionally checks `<name>.lease` (lease v6, super-header + body);
`LatestSlot` v3 validates version/page geometry through `ShmSuperHeader` plus
the geometry block and no longer depends on a manifest to be recognised.

Note that the probe scans **all of `/dev/shm`**, so it also sees stale segments
left over from a previous session. A mismatch on an old segment is expected
(after the upgrade it is replaced by the new segment created at restart, or
removed explicitly per §2 step 5); whether a live peer still exists is decided by
whether a process maps the segment (the script's `--purge-shm`/verification
logic uses `/proc/*/maps` and `/proc/*/fd`, following the runbook scenario 2
liveness rule).

### 1.3 Pre-upgrade checklist

1. `thyme_probe abi --matrix` matches the deployment package (the packaged
   `thyme_probe` output must equal the golden file);
2. `thyme_probe abi` either returns 0 (same version) or 3 with only old pages
   where `actual < expected` (a normal cross-version upgrade); `actual >
   expected` means the host is newer than the deployment package — that is a
   **downgrade** and requires an explicit `--allow-downgrade`;
3. external drivers (cloud/image/h264, see [`replay.md`](replay.md) §2.2) have
   been rebuilt from the same tree; drivers built against old headers cannot
   write the current v4 pinned-slot channel pages or v6
   `RecordMirrorRingPage` pages, and replay/injection tools have been switched
   to the same build as well;
4. configuration changes (`[logging]` and other startup-only keys) ship
   together with the upgrade — these keys are rejected by `ReloadConfig`
   (`ReloadRequiresRestart`) and only take effect through a restart;
5. there is space for evidence/package directories and the
   `integrity.manifest` verification tools are available (§5).

### 1.4 `FrameHeader` migration for external producers

External code that fills frame headers by hand must publish in the following
order; do not treat `valid=1` as "the header is complete" and set it before the
payload:

1. call `FrameInit` to initialise the full 40 B header and metadata; at this
   point `valid == 0`;
2. write the payload completely;
3. on a normal channel call `FrameSeal` or `FrameSealNoCrc`, which sets
   `valid == 1` and completes the corresponding CRC policy. Only when a channel
   explicitly does not seal may you call `FrameMarkValid` after step 2; it is
   not a substitute for sealing on a normal channel.

The current reader always validates strictly: only bit0 of `reserved[0]`
(`kFrameFlagCrcDisabled`) is defined, the remaining bits of `reserved[0]`,
`reserved[1..2]` and `reserved2` must be 0, and `valid > 1` is rejected
outright. External code must not build headers from uninitialised memory, nor
patch `valid` only at the end. There is **no `--allow-legacy-reserved`** or
equivalent compatibility switch in the implementation; recordings with the old
36 B header must be re-recorded and cannot be consumed by passing a startup
flag.

The data-valid read/write boundary and the CRC flag/reserved rules are defined
by the "`FrameHeader.valid`: data-valid" subsection and rules 3/4/9 of
[`abi-compatibility.md`](../reference/abi-compatibility.md); for API examples
see [`api-runtime.md`](../usage/api-runtime.md).

## 2. Upgrade order (rule 6)

Single-unit (one supervisor manages all roles) and multi-unit (producer and
consumer separate) deployments differ only in whether steps 1 and 2 are merged;
the order itself is unchanged:

1. **Preflight**: §1; record the mismatch list;
2. **Stop the producer** (producer first, then consumer):
   `systemctl stop <producer-unit>`; a single-unit deployment stops all roles
   at this step;
3. **Stop the consumer**: `systemctl stop <consumer-unit>`;
4. **Upgrade the binaries**: overlay the staged package tree onto the prefix
   (`--package`), or replace `bin/`, `lib/` through the deployment system's
   package manager; all peers must come from the same build;
5. **Recreate/clean SHM**:
   - by default do nothing: after the restart the producer creates new segments
     with a new session pid, and old segments naturally become stale;
   - cross-version leftovers (the probe reports a mismatch and no process maps
     them) can be removed explicitly with `--purge-shm`; do **not** blindly
     delete `/dev/shm/thyme_*` (runbook scenario 2 and the deploy README forbid
     it);
   - do not use `PinnedSlotWriter::Create(force_replace=true)` /
     `thyme-replay --force` to "make old pages compatible": `force_replace`
     only bypasses the version check and recreates the page; it neither
     migrates data nor bypasses the live gate;
6. **Start the producer**, then **start the consumer** (producer creates pages
   first, consumer attaches afterwards);
7. **Verify**: §5.

The corresponding script:

```bash
# Dry run (default): prints only; does not stop, copy, unlink or delete packages
tools/ops/upgrade/thyme_upgrade.sh \
    --package /opt/packages/thyme-1.2.3 \
    --prefix /opt/thyme \
    --backup-dir /var/backups/thyme/1.2.2 \
    --producer-unit thyme-producers --consumer-unit thyme-consumers \
    --probe build/tools/inspect/thyme_probe/thyme_probe \
    --manifest /etc/thyme/probe.manifest \
    --purge-shm

# Actually execute
tools/ops/upgrade/thyme_upgrade.sh ...same flags... --apply
```

Single-unit deployments omit `--producer-unit/--consumer-unit`; the script then
stops and starts all roles at once with `--unit thyme-app` (the default) and
says so in its output.

## 3. What happens when versions are mixed (per object)

| Object | Result of mixed old/new peers | Self-identifying at runtime |
| --- | --- | --- |
| `PinnedSlotWriter` / `PinnedSlotReader` | a v3 reader/writer cannot open a v4 page (described-open version/hash rejection); v4 `Create()` refuses to overwrite an old header by default (`kIncompatible`) | yes (super-header magic/version/layout hash) |
| PinnedSlot lease | v5/v4/v3 lease × v6 reader → `Open()==false`; the v6 super-header moves body/cells, and the v5 copy-active protocol does not run mixed either | yes (`<name>.lease` super-header version) |
| `ShmSpscRing` | v3 × v4 → `Open()==false` (v4 adds a super-header) | yes (super-header magic/version/layout hash) |
| `ShmSpmcRing` | v3 × v4 → `Open()==false` (v4 adds a super-header) | yes (super-header magic/version/layout hash) |
| `LatestSlot<T>` | v2 body (no super-header) × v3 described page → `kBadMagic`/`kBadAbiVersion`; v3 with a wrong payload hash/geometry → `kBadLayoutHash`/`kBadGeometry` | yes (`THYLSLOT` magic + super-header + geometry) |
| `RecordMirrorRingPage` | v5 × v6 → rejected by magic/version/layout hash; the v6 per-struct gate grows the descriptor from 64 B to 128 B and the fixed table from `192+192*N` to `192+256*N` | yes (super-header) |
| `MpSessionIdentityPage` | missing v1 page → legacy fallback with a warning; page present but hash/health mismatch → attach fails | yes (super-header) |
| `SharedHealthPage` | v1/v2 × v3 rejected (v2 has no super-header, `sizeof` 4160→4224; v3 widens `drop_cnt` to 64 bits) | yes (magic@0/version@8 + super-header) |
| `MpControl` | v1/v2 × v3 rejected (v2 has only a bare `abi_version` and no magic; v3 rearranges every field after the super-header) | yes (`THYME_MP` magic + super-header) |
| trigger control socket | the datagram size selects the layout and the version must match; v1/v2 default to `kUnsupportedVersion` and are only allowed with explicit `[security].allow_legacy_*` | yes (inside the message) |
| `.thyme.bin` / `.tbin` | the current reader accepts v1 (no footer index), v2 (with index), v3 (manifest block + 40 B registry) and v4 (128 B keyed registry); v0 and future versions are rejected; v1..v3 packages use the legacy `struct_id` mapping | yes (file header) |
| `FrameHeader` | 40 B wire layout (the trailing `reserved2` must be 0); recordings with the old 36 B header must be re-recorded; `reserved[0]` bit0 = CRC-disabled, unknown reserved bits are rejected; `valid` is data-valid | yes (frame header) |

Conclusion: every SHM page (including `LatestSlot` v3) fails explicitly on
attach/open/read when versions are mixed; it never reads the wrong layout
silently. An upgrade still requires "stop the producer first, upgrade
everything, recreate SHM". **"Just restart one role and see" is not a supported
procedure.**

## 4. Rollback

### 4.1 When to roll back

- after startup `thyme_probe abi` still reports a mismatch on pages **mapped by
  a live process** (the script exits 4);
- the smoke test fails: the trigger chain does not come up, `thyme-replay
  integrity` reports `FAILED`, roles in `/run/thyme/status.sock` are stuck in
  `waiting_backoff/gave_up`;
- the new version shows a functional regression inside the acceptance window.

Do not "roll back one role" while versions are mixed: that only turns a
mixed-version problem into data corruption.

### 4.2 Rollback steps

1. Stop the producer, then the consumer (same as the upgrade);
2. restore the backup tree to the prefix (`--rollback <backup-dir>`; the backup
   is produced by `--backup-dir` during the upgrade and overlaid with `cp -a`);
3. when rolling back across ABI versions, pages created by the new build are
   incompatible with the old build. Run `abi --mismatches` again with the
   **restored** `thyme_probe` (`--probe <prefix>/bin/thyme_probe`), then use
   `--purge-shm` as needed to remove leftovers that no process maps; in
   rollback mode the script does not treat "pages newer than the restored
   build" as a downgrade and refuses — that is the normal state of a rollback,
   and it only emits a note — so `--allow-downgrade` is not needed;
4. start the producer, then the consumer;
5. verify with the restored probe (§5). If the rollback crossed ABI versions,
   packages recorded during the rolling window are still readable: the `.tbin`
   v2 reader accepts v1/v2, `integrity.manifest` is a read-only v1 format, and
   packages do not become invalid because of an upgrade/rollback.

```bash
tools/ops/upgrade/thyme_upgrade.sh --apply \
    --rollback /var/backups/thyme/1.2.2 \
    --prefix /opt/thyme \
    --probe /opt/thyme/bin/thyme_probe \
    --purge-shm \
    --unit thyme-app
```

### 4.3 Deletion boundaries of the script

- package directories are deleted only with an explicit `--delete-package DIR`
  and `--apply`;
- SHM is deleted only with an explicit `--purge-shm` and `--apply`, and only
  pages listed by `abi --mismatches` that `/proc` proves are not mapped by any
  process;
- backup directories only ever grow; the script never deletes recording or
  evidence packages.

## 5. Verification

```bash
# 1) ABI: the target build's probe should report 0 (or no pages) or only stale segments
build/tools/inspect/thyme_probe/thyme_probe abi \
    --prefix thyme_ --prefix c9_ --manifest /etc/thyme/probe.manifest

# 2) Spot-check channel geometry/version
build/tools/inspect/thyme_probe/thyme_probe list --prefix c9_
build/tools/inspect/thyme_probe/thyme_probe info /h264_front

# 3) Evidence package integrity (new packages must still pass after upgrade/rollback)
build/tools/replay/thyme-replay/thyme-replay integrity \
    /opt/usr/triggered_data/<trigger_id>_<t0>
#   → integrity: OK; an incomplete package → PARTIAL

# 4) Trigger-chain smoke test (any one of these)
build/tools/apps/trigger_center/triggerctl status
curl --unix-socket /run/thyme/status.sock http://localhost/status
journalctl -u thyme-app --since '10 min ago' | grep 'level=error'

# 5) Test smoke (when the deployment machine has a build tree)
ctest --test-dir build -L mr_gate --output-on-failure -j2
# Unit-test focus: test_abi_matrix / test_abi_check / test_abi_matrix_json /
#                   test_upgrade_script / test_abi_cross_version
```

`thyme_probe abi` returning 0 only means the page versions match this build;
`LatestSlot` v3 is validated through the in-page super-header as well and no
longer falls into `unverifiable` (see §3).

## 6. Script reference: `tools/ops/upgrade/thyme_upgrade.sh`

Default is **dry-run**: it prints every step and does not stop, copy, unlink or
delete anything. Only `--apply` executes; the script prints the actual command
for every action.

| Option | Effect |
| --- | --- |
| `--apply` | Actually execute; without it only the plan is printed |
| `--check-only` | Run the preflight only (equivalent to `thyme_probe abi --mismatches` + direction check); change nothing |
| `--matrix` | Print this build's ABI constant matrix JSON |
| `--package DIR` | Staged package tree (upgrade source); mutually exclusive with `--rollback DIR` |
| `--prefix DIR` | Installation prefix, default `/opt/thyme` |
| `--backup-dir DIR` | `cp -a` the current prefix here before installing |
| `--rollback DIR` | Overlay the prefix with the backup tree (rollback) |
| `--delete-package DIR` | Delete the old package directory after success (only with `--apply`, and the path must be explicit) |
| `--probe PATH` | `thyme_probe` to use (point at the restored binary when rolling back) |
| `--manifest FILE` | probe manifest (optional: adds frame_shaped/struct information) |
| `--shm-prefix P` | Scan prefix, repeatable; defaults `thyme_`, `c9_` |
| `--purge-shm` | Delete `/dev/shm` pages that the preflight judged incompatible and that no process maps (only with `--apply`) |
| `--allow-downgrade` | Continue even when pages are newer than the build (refused by default; automatically allowed in `--rollback` mode) |
| `--skip-abi-check` | Continue even when the preflight fails (prints a warning; not recommended) |
| `--unit NAME` | Unit name for a single-unit deployment, default `thyme-app` |
| `--producer-unit` / `--consumer-unit` | Stop/start order for multi-unit deployments, repeatable |
| `--unit-ctl CMD` | Unit control command, default `systemctl` (tests can inject a fake) |
| `--smoke-cmd CMD` | Smoke command to run after startup (the script exits 3 if it fails) |
| `--verify-wait-ms N` | Wait before re-checking after startup, default 2000 |

Script exit codes: `0` success/dry-run completed; `1` usage error; `2`
preflight refusal (the version direction is a downgrade, or the probe failed);
`3` a command failed; `4` after startup an incompatible page is still mapped by
a **live process**.

## 7. Notes

- **`[logging]` is startup-only**: changes to `fsm_max_bytes`, `*_rotate_keep`,
  `perf_max_bytes` and `total_budget_mb` must ride on this restart; `SIGHUP` is
  rejected with `ReloadRequiresRestart` and never hot-reloads them.
- **External drivers**: the current `PinnedSlotWriter`/`PinnedSlotReader` v4
  `ProducerGate` is on the core write path and `RecordMirrorRingPage` v6 uses a
  per-struct gate; the upgrade must rebuild cloud/image/h264 drivers and
  replay/injection tools from the same tree, otherwise old peers are rejected
  by the version/layout check.
- **Primitive renames**: renaming or moving headers is a source-level change,
  so external drivers must be recompiled; do not mix binaries built against old
  headers even when the version words are identical.
- **Do not edit layout assertions by hand**: an assertion failure means the
  layout really changed; bump the version per rules 1/2 instead of editing the
  assertion (see "layout assertions" in `abi-compatibility.md`).
- **Never blindly delete SHM**: see runbook scenario 2 and the deploy README;
  the script's `--purge-shm` already follows the liveness rules and should
  still only be used inside a maintenance window.
- **Do not treat `force_replace` / `--force` as an upgrade mechanism**: they
  only recreate pages; they migrate no data and do not bypass the live gate.

## References

- [`abi-compatibility.md`](../reference/abi-compatibility.md) — rules 1–10, cross-version matrix, layout assertions
- [`runbook.md`](runbook.md) — disk full / SHM leak / trigger storm / crash recovery, safe SHM reclamation
- [`replay.md`](replay.md) — producer gate, current v4 channel / v6 mirror and producer constraints during replay
- [`onboarding.md`](onboarding.md) — build/run/test entry points
- [`trigger-config.md`](trigger-config.md) — SIGHUP reload matrix and startup-only keys
- [`../deploy/README.md`](../../deploy/README.md) — units, watchdog, restart matrix
- `include/thyme/probe/abi_matrix.h`, `src/probe/abi_check.cpp` — matrix and checker implementation
- `tools/ops/upgrade/thyme_upgrade.sh`, `tools/ops/upgrade/test_upgrade_dry_run.cmake` — driver and test
