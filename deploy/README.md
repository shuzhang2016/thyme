# Deploying thyme (OPS-03)

This directory ships the process-supervision layer for a thyme deployment:
one systemd unit for systemd images, one SysV/BusyBox init script plus an
inittab snippet for images without systemd (target boards without systemd), and an
installer that picks the right one.

The units start **one supervisor**, not one process per role. The supervisor
(`MpSupervisorSession`) forks the graph roles, owns the SHM set and exposes the
OPS-10 status endpoint. Role lifecycle is OPS-01; SHM reclamation is OPS-02.
Init only supervises the supervisor.

```
PID 1 (systemd) or BusyBox init
  └─ thyme-app  (exec wrapper -> supervisor, main PID)
       ├─ role perception ─┐
       ├─ role rest         ├─ OPS-01: exit/stall detect, SIGTERM -> SIGKILL,
       └─ role ...         ┘          exponential backoff, restart budget,
                                       kGaveUp (never respawned again)
       ├─ SHM  /dev/shm/c9_<supervisor-pid>, /dev/shm/thyme_rec_<pid>, channels
       └─ status  /run/thyme/status.sock  (OPS-10, read-only JSON)
```

## Files

| Source | Installed as (systemd) | Installed as (BusyBox) |
|---|---|---|
| `systemd/thyme-app.service` | `/etc/systemd/system/thyme-app.service` | — |
| `systemd/thyme-app-run` | `/usr/libexec/thyme/thyme-app-run` | same |
| `systemd/thyme-trigger-center.service` | `/etc/systemd/system/thyme-trigger-center.service` (OPS-11) | — |
| `systemd/thyme-acquisition.target` | `/etc/systemd/system/thyme-acquisition.target` (OPS-11) | — |
| `thyme-app.env.example` | `/etc/thyme/thyme-app.env` (only if absent) | same |
| `thyme-trigger-center.env.example` | `/etc/thyme/thyme-trigger-center.env` (OPS-11) | — |
| `sysvinit/thyme-app` | — | `/etc/init.d/thyme-app` |
| `busybox/inittab` | — | appended to `/etc/inittab` with `--with-inittab` |
| `README.md` | `/usr/share/doc/thyme/deploy/README.md` | same |

`/opt/usr/thyme/bin/thyme-app` is the default supervisor binary. It is **not**
installed by this repository: the downstream app builds the supervisor
entrypoint. Point `THYME_APP_BIN` in the env file at yours.

## Trigger center and ordered launch (OPS-11)

`thyme-trigger-center.service` runs the recorder/trigger host after the
producer graph. It is `Type=notify` (READY after every source attaches and
both sockets bind), restarts on failure, feeds a 30 s watchdog from its main
loop, and gates start-up on `trigger_center --check-config` and
`--check-resources` (OPS-12/OPS-07). `thyme-acquisition.target` groups
producers and recorder into one ordered switch.

The full dependency order, readiness/health commands, restart matrix and
rollback pointer are in [`docs/guides/orchestration.md`](../docs/guides/orchestration.md).
`deploy/install-service.sh` still installs only the supervisor unit; the
recorder units are staged by packaging (the install commands are in the
orchestration document).

## systemd semantics

### Start-up (`Type=notify`)

`ForkRoles()` sends `READY=1` after every role has been forked (OPS-03 shim,
`include/thyme/app/systemd_notify.h`). `TimeoutStartSec=60s` bounds a stuck
start. The gate is `THYME_SD_NOTIFY=1` **and** a non-empty `NOTIFY_SOCKET`;
without both, the shim is a no-op and `Type=notify` would wait for the start
timeout. The unit sets `THYME_SD_NOTIFY=1`; the env file can override it.

The exec wrapper resolves `${THYME_APP_BIN:-...}` and `exec`s it. Because it
`exec`s, the main PID is the supervisor and `NotifyAccess=main` accepts its
notifications.

If you disable the shim, or point `--app-bin` at a tool that is not a
supervisor, also switch the unit to `Type=simple`; otherwise the unit waits for
`READY=1` until `TimeoutStartSec` and fails:

```ini
# /etc/systemd/system/thyme-app.service.d/simple.conf
[Service]
Type=simple
WatchdogSec=0
```

### Restart matrix (double-restart protection)

| Event | Handled by | Effect |
|---|---|---|
| one role exits | OPS-01 (`PollRestarts`) | respawn that role, bounded backoff/budget, session and SHM stay up |
| one role stops beating / looping | OPS-01 stall detector | SIGTERM, SIGKILL after grace, respawn |
| restart budget exhausted | OPS-01 | `kGaveUp`; supervisor stays up and reports it |
| supervisor process exits non-zero | systemd `Restart=on-failure` | fresh session after `RestartSec=5s` |
| supervisor exits 0 | nobody | treated as a planned stop (no restart) |
| watchdog expires | systemd | SIGABRT, then `Restart=on-failure` |
| start loop | systemd `StartLimitBurst=5` / `StartLimitIntervalSec=300` | unit stays failed; no boot loop |

systemd must not restart on role failures: that would kill healthy roles,
discard OPS-01's restart budget and invalidate the session's SHM identity,
turning a recoverable role fault into a full acquisition outage. The two
policies also have different time scales (role backoff starts at 100 ms and
caps at 5 s; systemd waits 5 s and allows 5 starts per 5 minutes), so they
cannot resonate.

`Restart=on-failure` (not `always`) is deliberate: a clean exit is a planned
stop, and a supervisor that exits 0 after giving up would otherwise look
healthy. **Downstream exit policy:** map `kGaveUp` to a non-zero exit so a
session that cannot recover is restarted by systemd; if the playbook exits 0,
the unit stays stopped and the failure is only visible through `/status`.

### Watchdog

`WatchdogSec=30s`. While the supervisor poll loop runs, `PollRestarts()` sends
`WATCHDOG=1` at most once per second. If the supervisor stops polling (deadlock,
blocked syscall, frozen process), pings stop and systemd kills it with
`SIGABRT`; `Restart=on-failure` starts a fresh session and OPS-02 reclaims the
stale SHM.

The watchdog proves the **supervisor poll loop**, not the graph. A single
stalled role is OPS-01's job and must not cost a session restart. An embedder
that does not call `PollRestarts()` must set `WatchdogSec=0` in a drop-in:

```ini
# /etc/systemd/system/thyme-app.service.d/no-watchdog.conf
[Service]
WatchdogSec=0
```

`WatchdogSignal=SIGABRT` is the default and is intentional: a hung supervisor
gets a core dump for post-mortem, and the restart is still automatic.

### Hardening block

| Directive | Why / caveat |
|---|---|
| `NoNewPrivileges=yes` | no setuid/setcap escalation from the service |
| `ProtectSystem=strict` | rootfs read-only; output dirs are opened explicitly |
| `ProtectHome=yes` | no access to `/home`, `/root`, `/run/user` |
| `PrivateTmp=yes` | private `/tmp` and `/var/tmp`; **`/dev/shm` stays shared** so `thyme_probe`/readers and OPS-02 leases see the real segments |
| `ReadWritePaths=/dev/shm /opt/usr/thyme/triggered_data /var/lib/thyme` | the only writable paths the app needs |
| `RestrictAddressFamilies=AF_UNIX` | the status socket is the only family thyme opens. Widen it if an embedder opens DDS/TCP sockets |
| `LockPersonality=yes` | no execution-domain changes |
| `MemoryDenyWriteExecute=yes` | verified safe: thyme does not JIT, and the only mappings are file-backed `PROT_READ` (SHM/replay) or anonymous read-write |
| `RuntimeDirectory=thyme` | creates `/run/thyme` (0750) for `status.sock` |
| `User=thyme` / `Group=thyme` | dedicated unprivileged account; the installer creates it. Add `SupplementaryGroups=` for sensor/reader access. It cannot raise `SCHED_FIFO` without `LimitRTPRIO`/`CAP_SYS_NICE`; grant that only when PERF-03 is deployed |

`RestrictRealtime` is deliberately **not** set: thyme's RT scheduling work
(PERF-03) needs `sched_setscheduler`. Add it only if you do not use RT classes.

### Resource limits

`LimitNOFILE=65536`, `TasksMax=512`, `IOWeight=100`. `IOWeight` is only a
best-effort relative weight; per-device `io.max` (OPS-04) is applied by the
deployment, not by this unit.

### Stale SHM, sockets and `ExecStartPre`

`ExecStartPre` only creates and chowns the output directory. It does **not**
delete `/dev/shm` objects or the status socket:

- SHM names are per-session (`/c9_<pid>`, `/thyme_rec_<pid>`, plus channel
  names). A blind `rm -rf /dev/shm/thyme_*` is both wrong (it misses most of
  them) and dangerous (it can unlink a segment a live peer still maps, and
  POSIX shm has no unlink-if-inode). OPS-02 is the safe path:
  `ShmRegion::Create()` calls `SweepStaleShmSegment()`, which scans
  `/proc/<pid>/fd` and `/proc/<pid>/maps`, takes an exclusive `F_SETLEASE`,
  and unlinks only when staleness is proved. Anything inconclusive is left
  alone.
- The OPS-10 socket is replaced by `StatusServer::Start()` only when the path
  is an euid-owned socket; a foreign or non-socket path is refused, never
  unlinked. This mirrors the record/trigger stale-path policy.

Do not add a shell sweep to `ExecStartPre` unless it goes through the same
validated OPS-02 code path.

## BusyBox / SysV path

`/etc/init.d/thyme-app` is a POSIX-sh LSB-style script:

| Action | Behaviour |
|---|---|
| `start` | daemonise with `start-stop-daemon` (`-S -b -m -p /run/thyme-app.pid -a <runner> -c thyme:thyme`), 5 s liveness check; idempotent |
| `stop` | SIGTERM, 15 s grace, SIGKILL, remove PID file |
| `restart` | `stop` then `start` |
| `status` | PID-file liveness; exit 0 running, 1 dead pidfile, 3 stopped |
| `run` | foreground `exec` for inittab `::respawn:`; writes the PID file before exec so `status`/`stop` and the shutdown hook can find it |

The script sources `/etc/thyme/thyme-app.env` (plain `KEY=VALUE`), which is
also what systemd's `EnvironmentFile=` accepts. Logs go to
`/var/log/thyme/app.log` because there is no journald; the script creates and
chowns that directory when running as root.

**Automatic restart** comes from the inittab respawn line, not from `start`:

```
::respawn:/etc/init.d/thyme-app run
::shutdown:/etc/init.d/thyme-app stop
```

BusyBox init re-runs the command whenever it exits, which is the equivalent of
`Restart=always` for this service. Consequences:

- Use **either** the respawn line **or** `/etc/init.d/thyme-app start` from
  `rcS`, never both; one supervisor per image.
- `stop` cannot defeat PID 1's respawn. For maintenance, comment the respawn
  line out, `kill -HUP 1`, then run `/etc/init.d/thyme-app stop`.
- BusyBox init only notices process death; it has no watchdog. Role stalls are
  still OPS-01, and a hung supervisor is visible through the OPS-10 `/health`
  endpoint to external monitoring.
- The `start` path (rcS) does **not** auto-restart the supervisor; use it only
  where the app is expected to run for one shot or where another manager owns
  restart.

Privilege separation depends on the action: `start` drops to `thyme:thyme` via
`start-stop-daemon -c` when that account exists (otherwise it warns and runs as
the invoking user). `run` cannot change uid -- BusyBox inittab has no uid
field -- so the respawn path executes with PID 1's uid (root) and prints a
warning. If non-root execution is mandatory, use the rcS `start` path and
accept that init will not auto-restart the supervisor (or add an external
supervisor). Create the account and/or set `THYME_USER=` explicitly to opt out.
`THYME_LOG_FILE` defaults to `/var/log/thyme/app.log`; log rotation is out of
scope for OPS-03 (see the OPS-09 runbook item).

## Installer

```sh
sudo deploy/install-service.sh                 # auto-detect, enable, do not start
sudo deploy/install-service.sh --start         # install + enable + restart
sudo deploy/install-service.sh --dry-run       # print actions, no root needed
sudo deploy/install-service.sh --prefix /tmp/stage --init systemd   # staging
```

- Detects systemd from `/run/systemd/system` + `systemctl`, otherwise BusyBox
  (`busybox` binary or `/etc/inittab`). Override with `--init`.
- Idempotent: unchanged files are not rewritten, an existing
  `/etc/thyme/thyme-app.env` is never overwritten (local edits win), and the
  inittab respawn line is appended only once.
- Refuses to run as non-root unless `--dry-run` is given.
- Creates the `thyme` system user/group on systemd installs unless `--user`/
  `--group` were given explicitly; creates the data directory with mode 0750.
- On a live systemd host: `daemon-reload` and `enable` (not `start` unless
  `--start`). With `--prefix` it never calls systemctl or useradd.
- `--with-inittab` is the only mode that edits `/etc/inittab`.

## Verification

Systemd host:

```sh
systemd-analyze verify /etc/systemd/system/thyme-app.service   # syntax + ExecStart
systemctl status thyme-app
journalctl -u thyme-app -n 100
curl --unix-socket /run/thyme/status.sock http://localhost/health
curl --unix-socket /run/thyme/status.sock http://localhost/status
```

`/health` returns `{"ok":true}` only while every role is `kRunning`; `/status`
adds per-role state, PID, restart counters, last exit code/signal, stall age and
loop counts. A healthy unit with `ok:false` means a role is not running even
though the supervisor is alive (OPS-01 is retrying or gave up). The socket is
created 0600 by `StatusServer`; run the probe as the service account or root
(group-readable exposure would be an OPS-10 change, not a unit change).

BusyBox target:

```sh
/etc/init.d/thyme-app status
ps | grep '[t]hyme'
kill -0 "$(cat /run/thyme-app.pid)" && echo alive
tail -n 100 /var/log/thyme/app.log
```

Watchdog sanity check (systemd): `systemctl show thyme-app -p WatchdogUSec`
must be non-zero (the unit sets 30s), and the unit must not be `activating`
(READY never sent means `THYME_SD_NOTIFY` was disabled or `ForkRoles()` was
never reached).

## Limitations / follow-ups

- `Type=notify` and the watchdog require the gated `THYME_SD_NOTIFY` shim and
  a supervisor that drives `ForkRoles()`/`PollRestarts()`; there is no
  libsystemd dependency and no notify support for non-supervisor tools.
- The systemd unit covers one supervisor. Multiple independent graphs need a
  template unit (`thyme-app@.service`) and separate `RuntimeDirectory` names;
  that is not provided here.
- No log rotation for the BusyBox file log (OPS-09 runbook item).
- No cgroup `io.max` here (OPS-04).
- The installer does not build or install the downstream app binary or its
  graph/config; `THYME_APP_BIN` must point at an executable built against this
  library.
- Changing `User=` after a root run leaves root-owned SHM objects. OPS-02
  refuses to unlink segments it does not own, so sweep once as the old user
  (or before the account change); do not work around it with `rm`.
- SHM cleanup is OPS-02 only. Do not add blind `rm` steps to the units.
