# Recorder CLI (`triggerctl`)

`triggerctl` is the single command-line surface for recorder operations in this
repository: continuous-capture arm/disarm, recorder status, sealed-package
verification, and manual trigger injection. It replaces the split between the
generated `thyme-recorderctl` scaffold and ad-hoc control scripts.

The `recorder_cli` front-end mentioned in `ARCHITECTURE.md` belongs to the
optional `thyme-viz` sibling and is not built here; recorder *operations* are
served by `triggerctl`. Package replay/CRC verification remains available as
`thyme-replay verify` (see [replay.md](replay.md)); `triggerctl verify` checks
the seal journal and the SEC-03 evidence manifest without decoding payloads.

## Commands

```text
triggerctl arm|disarm [--socket PATH] [--timeout MS] [--json]
triggerctl status    [--socket PATH] [--timeout MS] [--json]
triggerctl verify <package-dir> [--key PATH] [--json]
triggerctl <source> [--socket PATH] [--priority N] [--force]
           [--timestamp NS] [--timeout MS] [--json]
```

| Command | Socket | Effect |
| --- | --- | --- |
| `arm` | control | Arm continuous capture; V2 request waits for an authenticated result. |
| `disarm` | control | Disarm continuous capture; V2 request waits for an authenticated result. |
| `status` | control | Request/response snapshot of the recorder. |
| `verify <pkg>` | none | Verify `integrity.manifest` + the WAL seal row. |
| `<source>` | trigger | Send one manual trigger through the trigger center. |

The control socket defaults to `/tmp/thyme_recorder.sock`
(`[recorder] socket`); the trigger socket defaults to
`/tmp/thyme_trigger.sock`. Both are Unix datagram sockets, bound with mode
`0600`, and the recorder rejects datagrams from a different uid.

## Flags

| Flag | Applies to | Meaning |
| --- | --- | --- |
| `--socket PATH` | arm/disarm/status/trigger | Socket for the selected surface (control for recorder commands, trigger otherwise). |
| `--control-socket PATH` | arm/disarm/status | Pre-ECO-02 alias of `--socket`; wins when both are given. |
| `--timeout MS` | arm/disarm/status/trigger | Total reply deadline in milliseconds, `1..60000`. Defaults: 1000 (control), 100 (trigger). Trigger V4 splits this budget across at most three send/reply attempts with `SO_SNDTIMEO`; `--timeout-ms` is an alias. |
| `--json` | all commands | Emit one JSON object on stdout. Errors still go to stderr. |
| `--priority N` | trigger | Priority hint. |
| `--force` | trigger | Force a preemption. |
| `--timestamp NS` | trigger | Explicit trigger timestamp. |
| `--key PATH` | verify | Raw HMAC-SHA256 key for a signed manifest. |
| `-h`, `--help` | all | Usage on stdout, exit 0. |

## Exit codes

Every surface uses the same codes:

| Code | Meaning |
| --- | --- |
| `0` | Command accepted, status returned, or package verified. |
| `1` | Recorder rejected the trigger/command, or package verification failed. |
| `2` | Usage error: unknown flag/command, missing value, wrong positional. |
| `5` | Transport failure: socket unreachable or reply timed out. |

## `status --json` schema

`schema` is the compatibility marker; consumers must ignore unknown keys.

```json
{
  "schema": 1,
  "kind": "thyme-recorder-status",
  "ok": true,
  "socket": "/tmp/thyme_recorder.sock",
  "armed": true,
  "phase": "armed",
  "next_trigger_id": 12,
  "auth_rejects": 0,
  "captures_done": 3,
  "slices_done": 0,
  "pending_bytes": 0,
  "disk_full": false,
  "integrity_mode": "full",
  "stats": {
    "captures_done": 3,
    "triggers_rejected": 0,
    "descriptors_queued": 120,
    "pending_evicted": 0,
    "overrun_before_queue": 0,
    "samples_written": 100,
    "frames_written": 20,
    "holes_written": 0,
    "torn_reads": 0,
    "harvest_retries": 0,
    "session_reattaches": 0,
    "source_errors": 0,
    "disk_errors": 0,
    "sync_errors": 0
  },
  "trigger": {
    "audit_drops": 0,
    "fsm_event_drops": 0
  },
  "control": {
    "protocol_version": 2,
    "auth_rejects": 0,
    "replay_rejects": 0,
    "crc_rejects": 0,
    "legacy_requests": 0,
    "audit_drops": 0
  }
}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | integer | Document schema, currently `1`. |
| `kind` | string | Always `thyme-recorder-status`. |
| `ok` | bool | `true` when the recorder answered. A transport failure emits a `kind`-matched object with `ok:false` and `error`, and exits 5. |
| `socket` | string | Control socket that answered. |
| `armed` | bool | Continuous-capture arm flag. |
| `phase` | string | `armed`, `post`, or `settle` (capture FSM phase). |
| `next_trigger_id` | integer | Next id the recorder will allocate. |
| `auth_rejects` | integer | Datagrams rejected by the peer-uid check since start. |
| `captures_done` | integer | Completed trigger captures (`trigger_center` host only). |
| `slices_done` | integer | Completed continuous slices (`trigger_center` host only). |
| `pending_bytes` | integer | Byte-accounted pending queue (trigger + continuous). |
| `disk_full` | bool | Recorder set the ENOSPC latch. |
| `integrity_mode` | string | Evidence manifest scope: `full`, `metadata`, `off`, or `mixed`. Also present in plain status as `integrity_mode=...`. |
| `stats` | object | Live `TriggeredRecorder::Stats` counters; field names match the C++ struct. |
| `trigger` | object | `CenterStats::audit_drops` and `fsm_event_drops` from the same trigger-center loop. |
| `control` | object | SEC-10 protocol version and auth/replay/CRC/legacy/audit-drop counters. Existing `stats`, `trigger` and `integrity_mode` fields are unchanged. |

Example:

```bash
triggerctl status --json --socket /tmp/thyme_recorder.sock
```

The plain `status` form prints the original summary fields as one `key=value`
line (`socket=... armed=true phase=armed ...`); nested stats are JSON-only.
`arm`/`disarm` print `arm sent` or, with `--json`, a
`thyme-recorder-control` acknowledgement after the V2 server accepts the
request. A duplicate/old sequence exits 1 and explicitly reports
`replay_rejected`; transport failures exit 5.

## `verify --json` schema

`verify` requires a sealed package directory. It checks the WAL seal row
(`TriggeredRecorder::PackageComplete`) and the SEC-03 evidence manifest
(per-file SHA-256, hash chain, optional HMAC). Full record/frame CRC
verification is `thyme-replay verify`.

```json
{
  "schema": 1,
  "kind": "thyme-recorder-verify",
  "ok": true,
  "package": "/data/captures/trigger_000012",
  "sealed": true,
  "manifest": "ok",
  "intact": true,
  "partial": false,
  "signed": true,
  "integrity_mode": "full",
  "signature_checked": true,
  "signature_ok": true,
  "chain_ok": true,
  "files_ok": 9,
  "files_missing": 0,
  "files_mismatch": 0,
  "files_unlisted": 0,
  "files_unverified": 0,
  "error": ""
}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `sealed` | bool | Last WAL row is a seal and no abort row is present. |
| `manifest` | string | `ok`, `absent`, or `invalid`. |
| `intact` | bool | Listed files are present and unchanged; the chain matches. |
| `partial` | bool | Manifest covers an invalidated `.incomplete` package. |
| `signed` / `signature_checked` / `signature_ok` | bool | HMAC state (a signed manifest without `--key` fails closed). |
| `integrity_mode` | string | Manifest-declared verification scope. |
| `files_*` | integer | Counts from the manifest walk; `files_unverified` is the number of size-only payload entries. |
| `error` | string | Empty on success. |

`ok` is true only for a sealed package whose declared scope is intact. A
`.incomplete` or unmanifested package exits 1. For `metadata`/`off`/`mixed`,
plain output says `OK(mode)` and reports `unverified_payload`; this is not a
full-content authenticity result.

## Examples

```bash
triggerctl status
triggerctl arm --json --timeout 500
triggerctl disarm --control-socket /tmp/thyme_recorder.sock
triggerctl verify /data/captures/trigger_000012 --key /etc/thyme/audit.key --json
triggerctl manual --priority 3 --json
```

## Trigger retry and result JSON

Manual triggers use protocol V4. Every invocation creates one nonzero
`request_id`; all bounded retries reuse the identical request, sequence and id.
The center therefore returns the first decision instead of recording twice.
`thyme-trigger-result` includes `request_id`, `attempts`, `retries`,
`deduplicated` and `result_unknown`. A timeout exits 5 with
`result_unknown:true`: the trigger **may have executed**, so this is not proof
that no package exists. A deduplicated success has `deduplicated:true` and the
original `trigger_id`. There is no automatic V1–V3 downgrade; older servers
that do not answer V4 produce the same bounded result-unknown outcome.

## Compatibility

- The pre-ECO-02 invocation `triggerctl <source> --socket PATH` and
  `triggerctl arm|disarm|status --control-socket PATH` keep working.
- `--timeout-ms` is still accepted as an alias of `--timeout`.
- The C++ `SendClientCommand` and `QueryRecorderStatus` entry points and the
  command names `arm`, `disarm`, `status`, `status-json` remain available, but
  emit record_control V2. V2 carries per-process monotonic sequence + CRC32;
  status replies carry the unchanged text/JSON body as a protected payload.
- V1 text peers are rejected by default. A bound V1 status peer receives an
  explicit `unsupported_version` error; arm/disarm peers that never listened
  for replies cannot be notified asynchronously. Deployments must set
  `[security] allow_legacy_control=true` to accept V1, which logs/audits the
  downgrade. There is no automatic fallback after a V2 timeout.
