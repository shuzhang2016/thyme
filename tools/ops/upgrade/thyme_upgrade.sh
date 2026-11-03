#!/usr/bin/env bash
# thyme_upgrade.sh — OPS-06 upgrade / rollback driver.
#
# Implements the sequence documented in docs/guides/upgrade.md:
#
#   0. pre-flight ABI check against the pages currently in /dev/shm
#   1. stop producers
#   2. stop consumers
#   3. install the staged package over the prefix (backup first if asked)
#   4. optionally unlink incompatible SHM pages (explicit --purge-shm only)
#   5. start producers, then consumers
#   6. verify: ABI check again (+ optional smoke command)
#
# Default is a DRY RUN: every step is printed, nothing is executed. Pass
# --apply to actually stop units, copy files, unlink SHM or delete packages.
# Packages are never deleted unless --delete-package is given (and applied);
# SHM is never unlinked unless --purge-shm is given (and applied), and even
# then only pages this build rejects and no live process maps.
#
# Exit codes: 0 ok, 1 usage, 2 pre-flight ABI failure / downgrade refused,
#             3 command failure, 4 post-start verification failed.

set -euo pipefail

PROG="$(basename "$0")"

APPLY=0
CHECK_ONLY=0
SKIP_ABI=0
PURGE_SHM=0
ALLOW_DOWNGRADE=0
MATRIX_ONLY=0

PACKAGE=""
PREFIX="/opt/thyme"
BACKUP_DIR=""
ROLLBACK_DIR=""
DELETE_PACKAGE=""
SMOKE_CMD=""
MANIFEST=""
PROBE=""
UNIT_CTL="systemctl"
UNIT="thyme-app"
PRODUCER_UNITS=()
CONSUMER_UNITS=()
SHM_PREFIXES=("thyme_" "c9_")
VERIFY_WAIT_MS=2000

usage() {
  cat <<EOF
$PROG — thyme upgrade/rollback driver (OPS-06)

usage: $PROG [options]

  --apply                 execute the plan (default: dry run, nothing changes)
  --check-only            run only the pre-flight ABI check
  --matrix                print this build's ABI constants matrix as JSON

  --package DIR           staged package tree to install (required to upgrade)
  --prefix DIR            install prefix (default: $PREFIX)
  --backup-dir DIR        save the current prefix tree here before installing
  --rollback DIR          restore binaries from DIR instead of installing
  --delete-package DIR    remove an old package directory after success
                          (only with --apply; never implied)

  --probe PATH            thyme_probe binary (default: thyme_probe in PATH)
  --manifest FILE         probe manifest (needed for LatestSlot pages)
  --shm-prefix P          /dev/shm prefix to inspect (repeatable; default
                          thyme_, c9_)
  --purge-shm             unlink incompatible, unmapped SHM pages after stop
                          (only with --apply)
  --allow-downgrade       proceed although a page is NEWER than this build
  --skip-abi-check        do not abort when the pre-flight check fails

  --unit NAME             single unit holding all roles (default: $UNIT)
  --producer-unit NAME    producer-only unit (repeatable; stopped first)
  --consumer-unit NAME    consumer-only unit (repeatable; stopped after
                          producers)
  --unit-ctl CMD          unit control command (default: $UNIT_CTL)

  --smoke-cmd CMD         command to run after start (e.g.
                          "thyme-replay integrity /opt/usr/triggered_data/PKG")
  --verify-wait-ms N      wait before the post-start ABI check (default: 2000)
  --help

Sequence: pre-flight ABI -> stop producers -> stop consumers -> install ->
(optional SHM purge) -> start producers -> start consumers -> verify.
With a single --unit, producers and consumers stop/start together; the
ordering only differs in multi-unit deployments.
EOF
}

die() {
  printf '%s: %s\n' "$PROG" "$*" >&2
  exit 3
}

usage_error() {
  printf '%s: %s\n' "$PROG" "$*" >&2
  usage >&2
  exit 1
}

log() { printf '%s\n' "$*"; }

step() { printf '\n== %s\n' "$*"; }

# run: dry-run prints, --apply executes. Never use shell evaluation on values
# that originate from arguments.
run() {
  if [ "$APPLY" -eq 1 ]; then
    log "  + $*"
    "$@"
  else
    log "  (dry-run) would run: $*"
  fi
}

need_value() {
  if [ "$#" -lt 2 ] || [ -z "$2" ]; then
    usage_error "missing value for $1"
  fi
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --apply) APPLY=1 ;;
    --check-only) CHECK_ONLY=1 ;;
    --matrix) MATRIX_ONLY=1 ;;
    --purge-shm) PURGE_SHM=1 ;;
    --allow-downgrade) ALLOW_DOWNGRADE=1 ;;
    --skip-abi-check) SKIP_ABI=1 ;;
    --package) need_value "$@"; PACKAGE="$2"; shift ;;
    --prefix) need_value "$@"; PREFIX="$2"; shift ;;
    --backup-dir) need_value "$@"; BACKUP_DIR="$2"; shift ;;
    --rollback) need_value "$@"; ROLLBACK_DIR="$2"; shift ;;
    --delete-package) need_value "$@"; DELETE_PACKAGE="$2"; shift ;;
    --smoke-cmd) need_value "$@"; SMOKE_CMD="$2"; shift ;;
    --manifest) need_value "$@"; MANIFEST="$2"; shift ;;
    --probe) need_value "$@"; PROBE="$2"; shift ;;
    --shm-prefix) need_value "$@"; SHM_PREFIXES+=("$2"); shift ;;
    --unit) need_value "$@"; UNIT="$2"; shift ;;
    --producer-unit) need_value "$@"; PRODUCER_UNITS+=("$2"); shift ;;
    --consumer-unit) need_value "$@"; CONSUMER_UNITS+=("$2"); shift ;;
    --unit-ctl) need_value "$@"; UNIT_CTL="$2"; shift ;;
    --verify-wait-ms) need_value "$@"; VERIFY_WAIT_MS="$2"; shift ;;
    --help|-h) usage; exit 0 ;;
    *) usage_error "unknown option $1" ;;
  esac
  shift
done

case "$VERIFY_WAIT_MS" in
  ''|*[!0-9]*) usage_error "--verify-wait-ms must be an integer" ;;
esac

if [ -z "$PROBE" ]; then
  if command -v thyme_probe >/dev/null 2>&1; then
    PROBE="$(command -v thyme_probe)"
  else
    PROBE="thyme_probe"
  fi
fi

probe_args() {
  local out=()
  local prefix
  for prefix in "${SHM_PREFIXES[@]}"; do
    out+=(--prefix "$prefix")
  done
  if [ -n "$MANIFEST" ]; then
    out+=(--manifest "$MANIFEST")
  fi
  printf '%s\n' "${out[@]}"
}

PROBE_EXTRA=()
while IFS= read -r arg; do
  [ -n "$arg" ] && PROBE_EXTRA+=("$arg")
done < <(probe_args)

run_probe() {
  "$PROBE" "$@" "${PROBE_EXTRA[@]}"
}

if [ "$MATRIX_ONLY" -eq 1 ]; then
  run_probe abi --matrix
  exit 0
fi

if ! command -v "$PROBE" >/dev/null 2>&1 && [ ! -x "$PROBE" ]; then
  die "probe not found: $PROBE (pass --probe)"
fi

# --- pre-flight ABI check ---------------------------------------------------
# Returns 0 when the host is upgrade-safe, 2 when it must be refused, and
# prints the incompatible pages into MISMATCH_LINES.
MISMATCH_LINES=()
ABI_STATE="unknown"

preflight_abi() {
  local rc=0
  local output
  set +e
  output="$(run_probe abi --mismatches 2>&1)"
  rc=$?
  set -e
  MISMATCH_LINES=()
  if [ -n "$output" ]; then
    while IFS= read -r line; do
      [ -n "$line" ] && MISMATCH_LINES+=("$line")
    done <<< "$output"
  fi
  case "$rc" in
    0)
      ABI_STATE="clean"
      log "  ABI pre-flight: every page matches this build"
      return 0
      ;;
    2)
      ABI_STATE="empty"
      log "  ABI pre-flight: no thyme SHM pages found (nothing running)"
      return 0
      ;;
    3)
      ABI_STATE="mismatch"
      log "  ABI pre-flight: pages from a different build are present:"
      local line name expected actual
      for line in "${MISMATCH_LINES[@]}"; do
        log "    $line"
        name="${line%% *}"
        expected="${line#*expected=}"
        expected="${expected%% *}"
        actual="${line#*actual=}"
        actual="${actual%% *}"
        if [ "$actual" -gt "$expected" ] 2>/dev/null; then
          if [ -n "$ROLLBACK_DIR" ]; then
            log "    note: $name is newer than the restored build; --purge-shm"
            log "    (or the fresh SHM session) replaces it"
          elif [ "$ALLOW_DOWNGRADE" -eq 1 ]; then
            log "    warning: $name is newer than this build (--allow-downgrade)"
          else
            log "    refusing: $name is v$actual, this build expects v$expected."
            log "    That is a downgrade; pass --allow-downgrade to override."
            return 2
          fi
        fi
      done
      if [ -n "$ROLLBACK_DIR" ]; then
        log "  these pages belong to the build being rolled back; the restored"
        log "  build recreates them in a fresh SHM session"
      else
        log "  these pages are old peers: stop everything, install, and let the"
        log "  restarting producers create fresh SHM (names embed the session pid)"
      fi
      return 0
      ;;
    *)
      if [ "$SKIP_ABI" -eq 1 ]; then
        log "  ABI pre-flight failed (rc=$rc); continuing because --skip-abi-check"
        return 0
      fi
      log "  ABI pre-flight failed (rc=$rc): $output"
      return 2
      ;;
  esac
}

if [ "$CHECK_ONLY" -eq 1 ]; then
  step "pre-flight ABI check (check-only)"
  if ! preflight_abi; then
    exit 2
  fi
  exit 0
fi

if [ "$APPLY" -eq 0 ]; then
  log "DRY RUN — no unit is stopped, no file is copied, no SHM is unlinked."
  log "Pass --apply to execute the plan."
fi

# --- rollback / upgrade source selection ------------------------------------
if [ -n "$ROLLBACK_DIR" ] && [ -n "$PACKAGE" ]; then
  usage_error "--rollback and --package are mutually exclusive"
fi
if [ -z "$ROLLBACK_DIR" ] && [ -z "$PACKAGE" ]; then
  usage_error "one of --package or --rollback is required (or --check-only)"
fi
if [ -n "$PACKAGE" ] && [ ! -d "$PACKAGE" ]; then
  die "package directory not found: $PACKAGE"
fi
if [ -n "$ROLLBACK_DIR" ] && [ ! -d "$ROLLBACK_DIR" ]; then
  die "rollback directory not found: $ROLLBACK_DIR"
fi

# Liveness check copied from the OPS-02 rules: a segment mapped or held open
# by any process is LIVE and is never unlinked by this script.
shm_is_live() {
  local name="$1"
  local path="/dev/shm/$name"
  if grep -F -q -- "$path" /proc/[0-9]*/maps 2>/dev/null; then
    return 0
  fi
  if ls -l /proc/[0-9]*/fd 2>/dev/null | grep -F -q -- "$path"; then
    return 0
  fi
  return 1
}

unlink_shm_page() {
  local name="$1"
  if shm_is_live "$name"; then
    log "  skip /dev/shm/$name: mapped or open by a live process"
    return 0
  fi
  run rm -f -- "/dev/shm/$name"
}

purge_incompatible_shm() {
  if [ "$PURGE_SHM" -ne 1 ]; then
    log "  SHM purge not requested: restarted producers create new pid-named"
    log "  segments; stale pages are left for OPS-02 / docs/guides/runbook.md section 2."
    return 0
  fi
  if [ "${#MISMATCH_LINES[@]}" -eq 0 ]; then
    log "  --purge-shm: no incompatible pages reported by the pre-flight check"
    return 0
  fi
  local line name
  for line in "${MISMATCH_LINES[@]}"; do
    name="${line%% *}"
    name="${name#/}"
    unlink_shm_page "$name"
  done
}

units_stopped=()

stop_unit() {
  local unit="$1"
  run "$UNIT_CTL" stop "$unit"
  units_stopped+=("$unit")
}

start_unit() {
  local unit="$1"
  run "$UNIT_CTL" start "$unit"
}

stop_all() {
  local unit
  if [ "${#PRODUCER_UNITS[@]}" -gt 0 ] || [ "${#CONSUMER_UNITS[@]}" -gt 0 ]; then
    for unit in "${PRODUCER_UNITS[@]}"; do
      step "stop producer unit: $unit"
      stop_unit "$unit"
    done
    for unit in "${CONSUMER_UNITS[@]}"; do
      step "stop consumer unit: $unit"
      stop_unit "$unit"
    done
  else
    step "stop unit: $UNIT (producers and consumers together)"
    stop_unit "$UNIT"
  fi
}

start_all() {
  local unit
  if [ "${#PRODUCER_UNITS[@]}" -gt 0 ] || [ "${#CONSUMER_UNITS[@]}" -gt 0 ]; then
    for unit in "${PRODUCER_UNITS[@]}"; do
      step "start producer unit: $unit"
      start_unit "$unit"
    done
    for unit in "${CONSUMER_UNITS[@]}"; do
      step "start consumer unit: $unit"
      start_unit "$unit"
    done
  else
    step "start unit: $UNIT"
    start_unit "$UNIT"
  fi
}

# --- plan -------------------------------------------------------------------
if [ -n "$ROLLBACK_DIR" ]; then
  step "rollback plan"
  log "  source: $ROLLBACK_DIR -> prefix $PREFIX"
  log "  pre-flight ABI check is skipped: the restored binaries define the"
  log "  expected versions; the post-restore check below reports new pages."
else
  step "upgrade plan"
  log "  source: $PACKAGE -> prefix $PREFIX"
  step "pre-flight ABI check"
  if ! preflight_abi; then
    exit 2
  fi
fi

# --- execute ----------------------------------------------------------------
stop_all

if [ -n "$BACKUP_DIR" ]; then
  step "backup current prefix: $PREFIX -> $BACKUP_DIR"
  if [ "$APPLY" -eq 1 ]; then
    mkdir -p -- "$BACKUP_DIR"
    if [ -d "$PREFIX" ]; then
      run cp -a -- "$PREFIX/." "$BACKUP_DIR/"
    else
      log "  prefix $PREFIX does not exist; nothing to back up"
    fi
  else
    log "  (dry-run) would save $PREFIX to $BACKUP_DIR"
  fi
fi

if [ -n "$ROLLBACK_DIR" ]; then
  step "restore binaries from $ROLLBACK_DIR"
  if [ "$APPLY" -eq 1 ]; then
    mkdir -p -- "$PREFIX"
  fi
  run cp -a -- "$ROLLBACK_DIR/." "$PREFIX/"
  # The restored build defines the expected versions; re-check with the probe
  # the operator pointed at (normally the restored thyme_probe).
  step "post-restore ABI check"
  if ! preflight_abi; then
    log "  post-restore check failed; fix the probe/prefix before starting"
    exit 2
  fi
else
  step "install package: $PACKAGE -> $PREFIX"
  if [ "$APPLY" -eq 1 ]; then
    mkdir -p -- "$PREFIX"
  fi
  run cp -a -- "$PACKAGE/." "$PREFIX/"
fi

step "SHM handling"
purge_incompatible_shm

start_all

# --- verify -----------------------------------------------------------------
step "verify"
if [ "$APPLY" -eq 1 ] && [ "$VERIFY_WAIT_MS" -gt 0 ]; then
  sleep "$(printf '%d.%03d' $((VERIFY_WAIT_MS / 1000)) $((VERIFY_WAIT_MS % 1000)))"
fi
verify_rc=0
set +e
run_probe abi >/dev/null 2>&1
verify_rc=$?
set -e
if [ "$verify_rc" -eq 0 ]; then
  log "  post-start ABI check: ok"
elif [ "$verify_rc" -eq 2 ]; then
  log "  post-start ABI check: no pages yet (producers may still be starting)"
else
  log "  post-start ABI check reported incompatible pages:"
  run_probe abi --mismatches || true
  # A mismatch that is not mapped by any process is a stale segment, not an
  # active mixed-version session.
  live_bad=0
  mismatch_out="$(run_probe abi --mismatches 2>/dev/null || true)"
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    name="${line%% *}"
    name="${name#/}"
    if shm_is_live "$name"; then
      log "  LIVE mismatch: /dev/shm/$name"
      live_bad=1
    else
      log "  stale mismatch: /dev/shm/$name (no live process maps it)"
    fi
  done <<< "$mismatch_out"
  if [ "$live_bad" -ne 0 ]; then
    log "  verification FAILED: live peers disagree with this build"
    exit 4
  fi
  log "  only stale segments mismatched; restarted producers are compatible"
fi

if [ -n "$SMOKE_CMD" ]; then
  step "smoke: $SMOKE_CMD"
  if [ "$APPLY" -eq 1 ]; then
    if ! bash -c "$SMOKE_CMD"; then
      log "  smoke command FAILED; roll back with --rollback <backup-dir>"
      exit 3
    fi
    log "  smoke: ok"
  else
    log "  (dry-run) would run: bash -c '$SMOKE_CMD'"
  fi
fi

if [ -n "$DELETE_PACKAGE" ]; then
  step "delete old package: $DELETE_PACKAGE"
  case "$DELETE_PACKAGE" in
    ""|"/"|"."|"..") die "refusing to delete '$DELETE_PACKAGE'" ;;
  esac
  if [ ! -d "$DELETE_PACKAGE" ]; then
    log "  not a directory; nothing to delete"
  else
    run rm -rf -- "$DELETE_PACKAGE"
  fi
fi

step "summary"
if [ "$APPLY" -eq 1 ]; then
  log "  applied: $([ -n "$ROLLBACK_DIR" ] && printf 'rollback' || printf 'upgrade')"
  log "  units handled: ${units_stopped[*]:-none}"
  log "  ABI state before: $ABI_STATE"
  log "  rollback: re-run with --rollback <backup-dir> (created by --backup-dir)"
else
  log "  dry run complete; re-run with --apply to execute the plan"
fi
