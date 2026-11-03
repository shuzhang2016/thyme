# Threat model — trigger/acquisition control plane

Status: **SEC-01 implemented** (threat model + attack-surface inventory),
**SEC-02 implemented** (peer-credential authz, priority clamp, audit log),
**SEC-03 implemented** (evidence-package hash-chain manifest + optional HMAC
signature; encryption explicitly out of scope), **SEC-04 implemented**
(control-message CRC32 + monotonic sequence + response deadline), **SEC-10
implemented** (recorder-control V2 authentication, integrity, anti-replay and
audit).
Scope: the `trigger_center` daemon, the recorder control socket it hosts, and
the data/tooling surfaces the team deploys with it. Platform: Linux, UNIX
domain sockets, POSIX shared memory.

## 1. Assets and trust boundaries

| Asset | Why it matters |
| --- | --- |
| Trigger admission (capture start/preempt) | A forged or replayed trigger can evict an in-flight safety capture, pierce mute windows, or burn quota. |
| Recorded packages (`out_dir`, `high/` zone) | Evidence for incidents; integrity is the point of recording. |
| SHM ring pages (small structs, PinnedSlot) | Live vehicle data and the recorder's input; writable by uid peers. |
| Config files (`trigger_config.ini`) | Define who may trigger, socket paths, quotas, output layout. |
| Audit log | Accountability for accept/reject decisions; itself evidence. |
| Generated code and role binaries | Executed by the supervisor; a tampered binary defeats every control above. |

Trust boundaries:

- **uid is the boundary this design enforces.** Same-uid processes are *not*
  isolated from each other (they can read/write each other's SHM, packages,
  config and audit file). Root is trusted (it can do all of the above anyway).
- `[security].allowed_uids` can *reduce* the set of accepted peers below
  "own uid"; it cannot make same-uid peers mutually distrustful.
- Kernel-supplied peer credentials (`SCM_CREDENTIALS` with `SO_PASSCRED`) are
  trusted: the kernel stamps them and overwrites sender-supplied values.
  `SO_PEERCRED` is the stream/connected equivalent; for an unconnected
  datagram server `SCM_CREDENTIALS` is the correct mechanism
  (`src/record/trigger_center.cpp:131-135`, `src/record/trigger_center.cpp:666`).

## 2. STRIDE matrix

Legend: **S**poofing, **T**ampering, **R**epudiation, **I**nformation
disclosure, **D**enial of service, **E**levation of privilege. "Planned"
marks items that remain follow-up work.

| # | Surface / asset | STRIDE threat | Current mitigation (code) | Residual risk | Planned |
| --- | --- | --- | --- | --- | --- |
| 1 | Trigger socket `/tmp/thyme_trigger.sock` (AF_UNIX/SOCK_DGRAM) — admission | **S**: peer claims another uid/pid | Kernel `SCM_CREDENTIALS` via `SO_PASSCRED` (`trigger_center.cpp:131-135`); parsed at `:666`; `PeerCred` rejects missing creds (`:443-471`). Socket is chmod 0600 and a stale path is only unlinked when it is a socket owned by the center's euid (`:124-127`, `:152`). | A peer with the center's uid is indistinguishable; every same-uid process is trusted unless the allowlist is narrowed. | SEC-07 |
| 2 | Trigger socket — request fields | **T**: priority self-elevation, unknown source, bogus timestamp | `priority_hint` is clamped to `[0, max_priority_hint]` and may only lower the rule priority (`trigger_center.cpp:259-273`); the effective value is echoed in the V3 response and audit. Unknown source → `kUnknownSource`; timestamp bounds → `kInvalidTimestamp` (`:245-250`, `:278-286`). Strict size/magic/version check (`:562-615`); SEC-04 validates the V3 CRC32 before sequence and field handling. | A same-uid peer can still *lower* priority below the rule and be silently ignored/muted by the FSM. | — |
| 3 | Trigger socket — replay | **S/T**: capture a valid datagram and inject it later | V3 request carries `sequence`; the server keeps a per-`(uid,pid)` 64-entry sliding window, rejects 0, duplicates and old values with `kReplayRejected` (`trigger_center.cpp:473-517`, `:572-581`); a bad CRC is rejected before the window advances. V1 has no sequence and is rejected unless `allow_legacy_unsequenced=true`; V2 has a sequence but no CRC and is rejected unless `allow_legacy_v2=true` (`:597-602`). | Window is keyed by pid; PID reuse can reject a fresh client until the entry is evicted (LRU, 64 peers). A replay older than 64 requests, or one whose peer entry was evicted (>64 distinct peers), is outside the window. Legacy flags deliberately disable part of the profile. | — |
| 4 | Trigger socket — audit | **R**: deny having triggered | Every request (accept, FSM reject, auth reject, malformed) appends one audit record before the reply (`trigger_center.cpp:520-639`); audit file opens 0600, O_APPEND, one `write()` per line (`trigger_audit.cpp:98`, `:195`). | The audit file is not tamper-evident: same-uid/root can edit or truncate it. A power loss loses un-fsynced tail records (fsync at shutdown or `audit_fsync_interval_s`). SEC-03 chains **packages**, not this log. | documented (no item) |
| 5 | Trigger socket — resource exhaustion | **D**: trigger storm, datagram flood, audit-disk flood | Per-`Tick` budget of 16 datagrams (`trigger_center.cpp:645`); fixed-size replay table (`trigger_center.h:151-160`); FSM queue depth 1 → `kBusy`; per-class quota windows; crash-level-only mute/quota override; bounded audit rotation. | No per-peer rate limit or backpressure; a same-uid peer can keep the center busy and grow audit I/O (bounded disk, not bounded CPU). | SEC-07, OPS-05, OPS-09 |
| 6 | Trigger socket — auth failure path | **I**: probe protocol state / enumerate sources | Unauthorized peers get **no reply** (handler returns false, `trigger_center.cpp:550`); decision still audited. | Timing of a silent timeout is the same for "bad uid" and "message dropped"; no direct leak found. | SEC-07 |
| 7 | Recorder control socket `/tmp/thyme_recorder.sock` (Arm/Disarm/Status/Trigger) | **S/T/E/R**: unauthorized or replayed control, corrupted requests, denied accountability | SEC-10 V2 has an independent opcode namespace, per-`(uid,pid)` 64-entry replay window and CRC32 checked before the window; kernel uid/pid plus `[security].allowed_uids`; every decision is audited to `<recorder-socket>.audit`. V1 text is rejected with `unsupported_version` unless `allow_legacy_control=true` (warning + audit). | Same-uid processes remain mutually trusted; bounded replay table/window and audit have the same PID-reuse/eviction and tamper/durability residuals as the trigger surface. Legacy opt-in deliberately removes sequence/CRC protection. | SEC-07 |
| 8 | SHM pages (small structs, PinnedSlot, mirror ring) | **T/I**: same-uid writes fabricated samples/frames into a ring; reads live data | `shm_open(..., 0600)` on create/open (`shm_region.cpp:78-110`); probe maps read-only (`page_reader.cpp:358`); geometry/version validation on attach (`triggered_recorder.cpp:172-205`); CRC on frames. | **Any same-uid process can open, write, truncate or unlink the segment** (0600 is per-uid, not per-process). No producer authentication, no per-writer MAC. | SEC-03, SEC-04 |
| 9 | Config file (`trigger_config.ini`) | **T**: change allowlist/quota/output dir; **I**: read policy | Parser rejects unknown sections/keys and bad values with line numbers (`trigger_config.cpp:94-671`); no accepted-but-ignored keys; audit path cannot equal the socket (`:646-658`). Reload via SIGHUP re-validates. | File permissions are whatever the deployment/umask created; no signature. Anyone who can write it also controls the daemon. | SEC-08, OPS-06 |
| 10 | Recorded packages on disk (`out_dir`, `high/`) | **T**: alter/delete evidence; **I**: read data; **R**: disown a package | Package tree is created 0700 (`triggered_recorder.cpp:169`, `:547-549`); retention only touches `out_dir` (`retention.cpp:138`); `wal.log` marks incomplete captures (`triggered_recorder.cpp:603`). SEC-03: a clean seal writes `integrity.manifest` (per-file sha256 + sorted path chain, trigger window ids, seal time) and, when `[security].signing_key` is set, a detached HMAC-SHA256 `integrity.hmac`; `thyme-replay integrity` verifies offline. | Plain files: **no encryption** (at-rest confidentiality absent — SEC-03 leaves threat **I** open); same uid/root can rewrite a package, and a complete package with a valid manifest can be replaced wholesale by anyone holding the signing key (or on unsigned deployments). The manifest makes *partial* tampering detectable, not deletion/rollback. | SEC-08 (key storage/rotation), ADR for encryption/Ed25519 |
| 11 | Generated code / role binaries (gen_graph, supervisor restart) | **T/E**: graph input or replaced binary produces code executed with the daemon's uid | Generated code only from the operator-supplied graph at build time (`tools/gen/gen_graph/emit.cpp:19-39`); supervisor restarts only configured role paths (`mp_supervisor.cpp:494-529`); `PR_SET_PDEATHSIG` plus `getppid()` re-check (`parent_death.cpp:13-23`). | No signature on generated output or role binaries; a same-uid write to the deploy dir becomes code execution on the next restart. Graph parser is trusted input. | SEC-08, SEC-07 |
| 12 | MCAP/export/replay tools | **T/D**: malicious package crashes or corrupts the analysis host | Parsers are read-only with bounds checks and magic/version validation; tools run as the invoking user, no daemon privileges. | Not fuzzed end-to-end; a malformed package may still crash a tool (fuzz targets exist for selected decoders). | SEC-07 |
| 13 | Probe (`thyme_probe`) | **I**: reads live SHM; **T**: none intended | Read-only API and `O_RDONLY` mapping (`page_reader.cpp:358`); no write/commit/lease API is exposed. | Same-uid only; no authentication (it is a diagnostic tool). It observes inconsistent snapshots if a producer is mid-write, by design. | — |
| 14 | Supervisor restart path | **D**: restart loop / give-up hides an outage; **E**: see #11 | Bounded backoff and window (`mp_supervisor.cpp:255-274`, `:494-529`); restart stats exposed; SIGTERM→grace→SIGKILL (`:561-573`). | A role that keeps crashing consumes the restart budget and stays down; restart events are logged to stderr, not the audit log. | OPS-02, OPS-08 |
| 15 | Tooling TCP capture helper (`ThymeBinTcpCaptureToFile`) | **I/T/D**: unauthenticated network peer streams bytes into a file | Binds `INADDR_ANY` with no auth (`thyme_bin_stream.cpp:94-141`); it is a library helper used by tooling/tests, not wired into the daemons. | If a deployment uses it on an untrusted network there is no authentication/encryption. | SEC-07 |
| 16 | Audit file on disk | **T/I/R**: rewrite history | 0600, O_APPEND, single-write records; rotation keeps N files (`trigger_audit.cpp:98-205`). | Same uid can edit; no hash chain; crash loses the un-fsynced tail. | documented (no item) |

## 3. Attack-surface inventory

Listening sockets:

| Path | Type | Created by | Permissions | Notes |
| --- | --- | --- | --- | --- |
| `/tmp/thyme_trigger.sock` (configurable `[trigger_center].socket`) | AF_UNIX `SOCK_DGRAM` | `trigger_center` (`trigger_center.cpp:149-155`) | `0600` (chmod after bind) | Trigger admission; stale path unlinked only if it is a socket owned by euid (`:122-127`). |
| `/tmp/thyme_recorder.sock` (configurable `[recorder].socket`) | AF_UNIX `SOCK_DGRAM` | `trigger_center` (`record_control.cpp`) | `0600` | SEC-10 V2 Arm/Disarm/Status/Trigger control; uid/pid allowlist, sequence+CRC and audit. |
| *(none)* | TCP | — | — | No daemon opens a TCP port. `ThymeBinTcpCaptureToFile` binds `INADDR_ANY` (`thyme_bin_stream.cpp:94-141`) but is a tooling helper, not started by `trigger_center`. |

Files and directories created by the daemons/tools:

| Path | Created by | Mode | Notes |
| --- | --- | --- | --- |
| `<trigger-socket>.audit`, `<recorder-socket>.audit` and `.1 … .N` | `trigger_center` / `record_control` (`trigger_audit.cpp`) | `0600` | Independent append-only trigger/control audits; rotated at `audit_max_bytes`. |
| `<out_dir>` and each path component | `TriggeredRecorder::Mkdirs` (`triggered_recorder.cpp:169`) | per component `0700` (existing dirs unchanged) | Package root. |
| `<out_dir>/<trigger_id>_<t0>/`, `<out_dir>/high/<...>/` | recorder (`triggered_recorder.cpp:547-549`) | `0700` | `high/` is excluded from low-priority retention. |
| `.../meta.tbin`, `.../wal.log`, `.../frames/*.raw`, continuous slices | recorder (`triggered_recorder.cpp:603`, `:762`, `mcap_writer.cpp:168`) | `0666 & ~umask` inside a `0700` dir | Typically `0644`; relies on the package directory for isolation. |
| `/dev/shm/<name>` (channel/ring pages) | producers via `shm_open` (`shm_region.cpp:78-110`) | `0600` | Same-uid read/write; probe opens `O_RDONLY`. |
| perf dump file (path from role config) | role host via `PerfLogDumper::Open` (`perf_dump.h:29`) | `0666 & ~umask` | Append-only, not covered by the audit chain. |
| MCAP/export output (user-supplied path) | `thyme_bin_to_mcap` (`mcap_writer.cpp:168`) | `0666 & ~umask` | Tooling, runs as the invoking user. |

Not listening/created by the daemon: config files are only read
(`trigger_config.cpp:676`); the probe never writes SHM; `thyme-replay` opens
packages read-only and drives injectors only when explicitly run.

## 4. What is explicitly NOT protected

1. **Same-uid peers.** Any process running as the center's uid can open the
   trigger socket, read/write mappable SHM (`0600` is per-uid), read/alter
   packages, edit the config and rewrite the audit file. `allowed_uids` can
   exclude *other* uids; it cannot subdivide one uid.
2. **Replay of V1 requests.** V1 has no sequence. `allow_legacy_unsequenced`
   defaults to `false`; enabling it deliberately gives up replay protection
   (audited as `proto=1 seq=0`).
3. **Recorder-control legacy mode.** V2 protects Arm/Disarm/Status/Trigger with
   uid/pid authorization, sequence, CRC32 and audit. The explicit
   `allow_legacy_control=true` compatibility mode accepts V1 text without
   sequence/CRC; those downgraded requests remain replayable (and are audited).
4. **Package confidentiality.** SEC-03 now gives sealed packages a hash-chain
   manifest and optional HMAC signature (partial tampering is detectable when
   verified with the key), but **packages are not encrypted**: at-rest
   confidentiality is not provided, and a same-uid/root reader can read every
   payload. Deleting or replacing a whole package (including its manifest) is
   also outside what the manifest can prove; policy must treat a missing
   manifest on a "complete" package as untrusted.
5. **SHM authenticity.** There is no producer authentication or per-writer
   MAC; CRC detects corruption, not a malicious same-uid writer. SEC-04 and
   SEC-10 CRC32/profile cover trigger and recorder-control datagrams only, not
   SHM or bulk payloads.
6. **Audit durability.** One small `write()` per record is crash-*consistent*
   (records never interleave) but not crash-*durable*: without
   `audit_fsync_interval_s` (or at shutdown) a power loss loses the recent
   tail. The log is not tamper-evident (`SEC-03`).
7. **Slow-disk blocking.** The audit path performs one bounded write and drops
   on error (`trigger_audit.cpp:195-208`); a disk that blocks indefinitely can
   still stall the single-threaded control loop because there is no separate
   audit thread. The drop counter (`CenterStats::audit_drops`) makes failures
   observable but does not prevent the stall.
8. **Storm resistance.** There is no per-peer rate limit; each Tick drains to
   `EAGAIN`, the configured time budget (default 200 us), or a 4096-datagram
   safety cap. Backlog is reported only when a limited poll leaves queued data;
   admission is still bounded by the FSM and quotas, not by peer identity.
9. **Network exposure of tooling.** `ThymeBinTcpCaptureToFile` has no
   authentication and binds all interfaces; do not deploy it on untrusted
   networks.
10. **Audit coverage of restarts.** Trigger and recorder-control requests are
    audited; supervisor restarts still go only to stderr.
11. **Trigger-client response spoofing.** The client binds a predictable
    per-process path under `/tmp` (`trigger_client.h:86-99`) to receive the
    reply. A same-uid process can send a forged reply there (e.g. `kMuted`);
    the client treats the reply as advisory — the authoritative outcome is the
    package directory, as documented in `trigger_client.h`.

## 5. Residual-risk register (summary)

| Risk | Severity | Owner item |
| --- | --- | --- |
| Same-uid can read/write SHM and packages | High | SEC-03 (partial tamper detection), SEC-04, SEC-06 (privacy) |
| Package tampering detectable only with a verified manifest + secret key; whole-package replacement/rollback remains possible | Medium | SEC-03 (documented), SEC-08 (key storage) |
| No encryption: at-rest package confidentiality absent | High | documented, no roadmap item (ADR) |
| Audit log tampering undetectable (not covered by SEC-03) | High | documented, no item |
| Recorder-control V1 legacy mode disables replay/integrity protection | Medium (opt-in only) | documented |
| PID-reuse false replay rejects; replay older than 64 requests possible on either control socket | Low | documented, no item |
| Trigger storm / audit I/O flood CPU/disk | Medium | OPS-05, OPS-09, SEC-07 |
| Slow-disk audit write can block the control thread | Medium | OPS-05 (async audit) |
| V1 legacy mode disables replay protection | Medium (opt-in only) | documented |
| No role-binary signature; restart executes replaced files | High | SEC-08 |
| Tooling TCP helper unauthenticated | Low (not daemon-wired) | SEC-07 |
| Client reply path predictable; same-uid reply spoofing possible | Low (client result is advisory; packages are authoritative) | documented |
| HMAC is symmetric: a verifier (or anyone reading the key file) can re-sign a tampered package | Medium | documented (Ed25519 ADR in `security/evidence-integrity.md`) |

## 6. Verification

- `tests/test_trigger_security.cpp` (SEC-02): allowlisted uid accepted,
  non-allowlisted uid rejected without a reply, clamp echoed in the response
  and audit, duplicate/old sequence rejected, malformed/oversized datagrams
  rejected without a crash, V1 fail-closed + opt-in, audit field/rotation
  checks, `[security]` parse/dump/round-trip.
- `tests/test_trigger_protocol.cpp`: V3 wire roundtrip, auth, malformed, client
  deadline (`kTransportTimeout`).
- `tests/test_trigger_priority_clamp.cpp`: hint may only lower a rule.
- `tests/test_trigger_config.cpp` / `--check-config` / `--dump-config`:
  every `[security]` key is either effective or a parse error.
- `tests/test_package_integrity.cpp` (SEC-03): real sealed capture verifies
  OK; one-byte tamper and deleted file fail per-file + chain; unlisted file
  fails; edited manifest is invalid; absent manifest reports `absent` without
  a crash; HMAC correct/wrong/missing key; the `thyme-replay integrity`
  command mirrors all of it (exit codes included).
- `tests/test_trigger_security.cpp` (SEC-04 additions): good V3 CRC accepted,
  bad CRC rejected as `kMalformedRequest` without touching the replay window,
  V2 rejected by default and accepted only with `allow_legacy_v2`, client
  deadline against an unserviced socket.
- `tests/test_record_control.cpp` / `test_triggerctl_cli.py` (SEC-10): V2 status
  and command compatibility, bad CRC not advancing the replay window,
  duplicate/old rejection, allowlist enforcement, independent audit records,
  V1 default rejection + explicit opt-in, and clear CLI replay diagnostics.
