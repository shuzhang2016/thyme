# Orchestration: launch order, readiness and restarts

This is the deployment contract for one acquisition stack on one image. It
covers what starts in which order, what "ready" means for each participant,
and who restarts what. It is the thyme counterpart of eCAL Sys' launch
configuration: systemd is the launch and dependency engine; thyme owns the
data-plane lifecycle inside each process.

The units live in [`deploy/systemd/`](../../deploy/systemd); supervisor lifecycle
(PID 1 -> `thyme-app` -> role graph) is documented in
[`deploy/README.md`](../../deploy/README.md), rollback in
[`upgrade.md`](upgrade.md).

## Participants and dependency order

```
filesystems, /dev/shm
        |
        v
producers                         thyme-app.service (role graph) or a site unit
  perception / rest roles          creates named SHM pages and channels
        |
        v
recorder                          thyme-trigger-center.service
  trigger_center                   attaches to every configured source,
                                   binds trigger + record-control sockets,
                                   arms continuous capture if configured
        |
        +--> control clients      triggerctl (trigger / arm / disarm / status)
        +--> read-only consumers  thyme_probe, thyme-replay, thyme-bin readers
```

Rules:

1. **Producers before the recorder.** `trigger_center` attaches each
   `[channel.*]` source during `Init()` and fails closed when a page is
   missing or has an unexpected geometry/hash. There is no retry loop around
   initial attach, so starting first is not a recoverable state.
2. **Recorder before control clients.** `triggerctl` and the record-control
   socket are useless until `trigger_center` has bound them; clients retry or
   fail fast, they never create the socket.
3. **Consumers are independent.** `thyme_probe` and `thyme-replay` only map
   pages read-only. They may start at any point; a consumer that starts before
   its page exists exits with "not found" (or waits, for `watch`) rather than
   blocking the stack.
4. **One recorder per page set.** Two `trigger_center` instances attaching to
   the same mirror page both consume records; the control sockets collide
   first, which is the intended fail-fast. Use separate output dirs, sockets
   and page names for a second stack.

The units encode rules 1 and 2:

| Unit | Role | Ordering |
|---|---|---|
| `thyme-app.service` | producer supervisor | `After=local-fs.target` |
| `thyme-trigger-center.service` | recorder | `After=thyme-app.service`, `Wants=thyme-app.service`, `PartOf=thyme-app.service` |
| `thyme-acquisition.target` | group switch | `Wants=thyme-app.service thyme-trigger-center.service`, `After=` both |

`PartOf=` means stopping or restarting `thyme-app.service` also stops the
recorder. A supervisor restart creates a new session; leaving the old
recorder attached to the previous session's pages would silently record
nothing. If your producers are not `thyme-app.service`, override `Wants=`,
`After=` and `PartOf=` together in a drop-in (see the comments in the target).

## Installation

```sh
# The unit runs as the unprivileged thyme account (created by the installer on
# supervisor installs; create it once on a recorder-only image).
sudo install -Dm644 deploy/systemd/thyme-trigger-center.service \
  /etc/systemd/system/thyme-trigger-center.service
sudo install -Dm644 deploy/systemd/thyme-acquisition.target \
  /etc/systemd/system/thyme-acquisition.target
sudo install -Dm644 deploy/thyme-trigger-center.env.example \
  /etc/thyme/thyme-trigger-center.env
sudo systemctl daemon-reload
sudo systemctl enable --now thyme-acquisition.target
```

`deploy/install-service.sh` still installs only the supervisor unit; the
recorder units are staged by packaging. The binary is expected at
`/opt/usr/thyme/bin/trigger_center` and the config at
`/etc/thyme/trigger-center.ini`; change the paths in a drop-in rather than
editing the unit in place so package upgrades keep working.

**Socket paths.** The unit sets `PrivateTmp=yes`, so `/tmp` is private to the
service. `[trigger_center].socket` and `[recorder].socket` must point into a
shared directory, normally `/run/thyme` (`RuntimeDirectory=thyme`). A config
that keeps the built-in `/tmp/...` defaults starts successfully but cannot be
reached by `triggerctl` or monitoring.

## Readiness

`Type=notify` + `NotifyAccess=main`. `trigger_center` sends `READY=1` only
after:

1. the config parsed (`--check-config` already passed in `ExecStartPre`);
2. the host-capacity gate passed (`--check-resources` in `ExecStartPre`,
   repeated after the output directory exists);
3. every configured source attached;
4. both sockets are bound and continuous mode (if `auto_arm=true`) is armed.

Before that the unit is `activating`, and systemd holds dependent units. Sending
READY earlier would let clients send triggers into a center that still rejects
them. `TimeoutStartSec=60s` bounds a producer that never publishes its pages.

Readiness is not health. Once active, use these checks:

```sh
systemctl is-active thyme-trigger-center.service
systemctl show thyme-trigger-center.service -p ActiveState -p SubState -p WatchdogUSec
triggerctl status --control-socket /run/thyme/recorder.sock
build/tools/inspect/thyme_probe/thyme_probe list --json
```

- `triggerctl status` is the recorder's control-path probe: it sends `status`
  to the record-control **datagram** socket; exit 5 means the path is not
  reachable (process gone, wrong path or a foreign socket). A successful send
  proves the socket is bound, not that the loop is consuming it - the
  watchdog covers a wedged loop, and `thyme_probe` covers the data plane.
- `thyme_probe list` shows the producer pages the recorder attached to and
  their ABI/geometry. `thyme_probe hz <channel>` and
  `thyme_probe watch <channel>` prove the producers are actually publishing.
- There is no `/status` JSON endpoint on `trigger_center`: that endpoint
  belongs to the supervisor, not to this process. The control socket plus
  `thyme_probe` are the health surface today; see "Follow-ups".

## Restarts and watchdog

| Event | Handled by | Effect |
|---|---|---|
| one producer role exits or stalls | the supervisor inside `thyme-app` | respawn that role, bounded backoff; session and recorder stay up |
| producer supervisor exits non-zero | systemd (`Restart=on-failure`) | new supervisor session; `PartOf=` restarts the recorder too |
| producer supervisor exits 0 | nobody | planned stop, recorder stops via `PartOf=` |
| `trigger_center` exits non-zero (Init failed, config re-check failed, crash) | systemd `Restart=on-failure`, `RestartSec=5s` | fresh attach to the current producer pages |
| `trigger_center` exits 0 | nobody | planned stop; an in-flight capture is sealed first |
| `trigger_center` main loop stops feeding `WATCHDOG=1` | systemd, `WatchdogSec=30s` | SIGABRT (core for post-mortem), then `Restart=on-failure` |
| start loop | `StartLimitBurst=5` / `StartLimitIntervalSec=300` | unit stays failed; no boot loop |

The watchdog proves the **main loop is scheduling**: `center.Tick()`,
`rec.Tick()` and the control poll all live in it, and the feed is rate-limited
to one datagram per second. It does not prove the producers are publishing or
that a capture can seal - those are data-plane checks (`triggerctl`,
`thyme_probe`). A clean SIGTERM stop sends `STOPPING=1` before sealing, so it
is not restarted.

Do not add `Restart=always`: `systemctl stop` for maintenance must stay
stopped. Do not restart the recorder on producer role failures either - the
supervisor already handles the role scale, and a recorder restart would discard
the in-flight pending window for a recoverable fault.

## Resource limits

The unit mirrors the deployment resource plan: `LimitNOFILE=65536`,
`TasksMax=512`, `IOWeight=100`. `ExecStartPre=--check-resources` fails the
start when `RLIMIT_NOFILE`, `/dev/shm` or the output disk cannot meet the
configured plan (`THYME_RESOURCE_CHECK=enforce`, the default). `warn` is a
deliberate, audited downgrade for capacity-constrained benches; `off` removes
the gate. The full threshold table and remediation steps are in
[`runbook.md` §7](runbook.md#7-startup-resource-self-check).

## Failure drills

```sh
# 1. Producer outage: recorder stays up, its pages go stale.
systemctl stop thyme-app.service          # PartOf= also stops the recorder
systemctl start thyme-app.service          # target/PartOf brings both back

# 2. Recorder crash: producers keep running, systemd restarts the center.
sudo kill -ABRT "$(systemctl show -p MainPID --value thyme-trigger-center.service)"
systemctl status thyme-trigger-center.service

# 3. Wedged main loop: watchdog fires within WatchdogSec.
sudo kill -STOP "$(systemctl show -p MainPID --value thyme-trigger-center.service)"
journalctl -u thyme-trigger-center -n 50   # expect SIGABRT + restart
```

After a drill, `triggerctl status` must answer and `thyme_probe list` must
still show the producer pages. Do not clear `/dev/shm` by hand: stale pages
are reclaimed by their owner (reclamation proves staleness first); a blind `rm`
can unlink a segment a live peer still maps.

## Rollback

Rolling back either unit follows [`upgrade.md`](upgrade.md): stop the stack
(`systemctl stop thyme-acquisition.target`), install the previous unit and
binary, re-run `systemctl daemon-reload`, start the target, then repeat the
readiness checks above. `thyme_upgrade.sh` covers the binary/package side and
its `--purge-shm` path is limited to provably unmapped pages; there is no
unit-level SHM sweep to undo.

## Validation (and what is not validated)

- `tools/ci/check_deploy_units.py` (ctest `test_deploy_units`, mr_gate) checks
  the required directives, the `--check-config`/`--check-resources` pre-start
  gates, the ordering graph, and that `docs/guides/orchestration.md` links
  `upgrade.md`. When `systemd-analyze` is installed, it also runs
  `systemd-analyze verify` on rewritten copies of the units (paths replaced by
  a dummy executable) to validate syntax and directive names.
- `tools/ci/test_trigger_center_ready.py` (ctest
  `test_trigger_center_ready`) starts a real `demo_producer` + `trigger_center`
  pair with a private `NOTIFY_SOCKET`, and asserts `--check-config`,
  `--check-resources`, `READY=1`, `WATCHDOG=1` and a clean `STOPPING=1` +
  exit 0 on SIGTERM.
- Not validated in CI: an actual boot on the target image, kernel/cgroup
  hardening enforcement, and cross-unit ordering under real PID 1. Those are
  HIL/boot-test items; the static checks only prove the units describe what
  this document says.

## Follow-ups

- `trigger_center` has no read-only JSON status endpoint. Adding one (or
  exposing capture counters on the record-control socket in a machine-readable
  form) would let the health check avoid `thyme_probe` and make WatchdogSec
  observable the way the supervisor's `/status` endpoint is.
- No `.timer` is shipped. Periodic health sampling belongs to the site's
  monitoring agent; a timer that merely runs `triggerctl status` would add a
  second failure domain without a remediation action.
- Consumer units (probe/replay) are intentionally out of scope; they are
  read-only and often started interactively.
