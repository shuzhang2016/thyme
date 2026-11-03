#!/usr/bin/env bash
# install the thyme service for systemd or BusyBox/SysV.
#
# Detects the init system, installs the matching unit/script plus the shared
# exec wrapper and environment file, and (on a live systemd host) reloads and
# enables the unit. Idempotent: unchanged files are left byte-identical and a
# locally edited /etc/thyme/thyme-app.env is never overwritten.
#
# Non-root users must pass --dry-run. --prefix stages everything under a
# directory (packaging, review) and never calls systemctl or edits the host's
# user database.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
DEPLOY_DIR="$REPO_ROOT/deploy"

PREFIX=""
INIT="auto"
APP_BIN="/opt/usr/thyme/bin/thyme-app"
SERVICE_USER="thyme"
SERVICE_GROUP="thyme"
ACCOUNT_EXPLICIT=0
DRY_RUN=0
DO_ENABLE=1
DO_START=0
WITH_INITTAB=0
CHANGED=0

usage() {
  cat <<'EOF'
Usage: install-service.sh [options]

  --dry-run            print actions, change nothing (allowed for non-root)
  --prefix DIR         stage under DIR instead of / (no systemctl, no useradd)
  --init KIND          auto (default), systemd, or busybox
  --app-bin PATH       supervisor executable (default /opt/usr/thyme/bin/thyme-app)
  --user USER          service account for the systemd unit (default thyme)
  --group GROUP        service group for the systemd unit (default thyme)
  --no-enable          install without `systemctl enable`
  --start              also `systemctl restart` after installing
  --with-inittab       BusyBox only: append the respawn line to /etc/inittab
  -h, --help           this help
EOF
}

die() {
  printf 'install-service.sh: %s\n' "$*" >&2
  exit 1
}

log() {
  printf '%s\n' "$*"
}

warn() {
  printf 'install-service.sh: warning: %s\n' "$*" >&2
}

dry_log() {
  printf '[dry-run] %s\n' "$*"
}

run_cmd() {
  if [ "$DRY_RUN" -eq 1 ]; then
    dry_log "$*"
  else
    "$@"
  fi
}

rooted() {
  printf '%s%s' "$PREFIX" "$1"
}

install_file() {
  src=$1
  dst=$2
  mode=$3
  if [ "$DRY_RUN" -eq 1 ]; then
    dry_log "install -D -m $mode $src $dst"
    return 0
  fi
  mkdir -p "$(dirname "$dst")"
  if [ -f "$dst" ] && cmp -s "$src" "$dst"; then
    chmod "$mode" "$dst"
    log "unchanged: $dst"
  else
    install -m "$mode" "$src" "$dst"
    CHANGED=1
    log "installed: $dst"
  fi
}

detect_init() {
  case "$INIT" in
    systemd | busybox)
      printf '%s' "$INIT"
      return
      ;;
  esac
  if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    printf 'systemd'
  elif command -v busybox >/dev/null 2>&1 || [ -e /etc/inittab ]; then
    printf 'busybox'
  elif command -v systemctl >/dev/null 2>&1; then
    printf 'systemd'
  else
    printf 'busybox'
  fi
}

ensure_env_file() {
  dst=$(rooted /etc/thyme/thyme-app.env)
  if [ -e "$dst" ]; then
    log "kept existing: $dst"
    return 0
  fi
  if [ "$DRY_RUN" -eq 1 ]; then
    dry_log "create $dst (THYME_APP_BIN=$APP_BIN)"
    return 0
  fi
  mkdir -p "$(dirname "$dst")"
  sed "s|^THYME_APP_BIN=.*|THYME_APP_BIN=$APP_BIN|" \
    "$DEPLOY_DIR/thyme-app.env.example" >"$dst"
  chmod 0644 "$dst"
  log "installed: $dst"
}

ensure_account() {
  if [ "$INIT_KIND" != "systemd" ]; then
    log "note: SysV path runs the app as ${SERVICE_USER:-the invoking user}"
    return 0
  fi
  if id -u "$SERVICE_USER" >/dev/null 2>&1; then
    return 0
  fi
  if [ -n "$PREFIX" ]; then
    log "staging prefix: not creating user $SERVICE_USER"
    return 0
  fi
  if [ "$ACCOUNT_EXPLICIT" -eq 1 ]; then
    warn "user $SERVICE_USER does not exist; not creating it because --user/--group was explicit"
    return 0
  fi
  if ! command -v useradd >/dev/null 2>&1; then
    warn "useradd not found; create system user $SERVICE_USER manually"
    return 0
  fi
  if [ "$DRY_RUN" -eq 1 ]; then
    dry_log "create system group/user $SERVICE_GROUP/$SERVICE_USER"
    return 0
  fi
  if ! grep -q "^$SERVICE_GROUP:" /etc/group 2>/dev/null; then
    if command -v groupadd >/dev/null 2>&1; then
      groupadd --system "$SERVICE_GROUP"
      log "created group $SERVICE_GROUP"
    else
      warn "groupadd not found; using useradd's default group"
      useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
      log "created user $SERVICE_USER"
      return 0
    fi
  fi
  useradd --system --gid "$SERVICE_GROUP" --no-create-home \
    --shell /usr/sbin/nologin "$SERVICE_USER"
  log "created user $SERVICE_USER"
}

ensure_dirs() {
  data_dir=$(rooted /opt/usr/thyme/triggered_data)
  log_dir=$(rooted /var/log/thyme)
  if [ "$DRY_RUN" -eq 1 ]; then
    dry_log "install -d -m 0750 $data_dir"
    if [ "$INIT_KIND" = "busybox" ]; then
      dry_log "install -d -m 0750 $log_dir"
    fi
    return 0
  fi
  install -d -m 0750 "$data_dir"
  if id "$SERVICE_USER" >/dev/null 2>&1; then
    chown "$SERVICE_USER:$SERVICE_GROUP" "$data_dir" 2>/dev/null || true
  fi
  if [ "$INIT_KIND" = "busybox" ]; then
    install -d -m 0750 "$log_dir"
    if id "$SERVICE_USER" >/dev/null 2>&1; then
      chown "$SERVICE_USER:$SERVICE_GROUP" "$log_dir" 2>/dev/null || true
    fi
  fi
}

install_systemd_unit() {
  dst=$(rooted /etc/systemd/system/thyme-app.service)
  if [ "$DRY_RUN" -eq 1 ]; then
    dry_log "install -D -m 0644 deploy/systemd/thyme-app.service -> $dst (User=$SERVICE_USER Group=$SERVICE_GROUP)"
    return 0
  fi
  tmp=$(mktemp)
  # Keep the shipped unit authoritative; only the account is site-specific.
  sed -e "s|^User=.*|User=$SERVICE_USER|" \
    -e "s|^Group=.*|Group=$SERVICE_GROUP|" \
    -e "s|^ExecStartPre=+/bin/chown .*|ExecStartPre=+/bin/chown $SERVICE_USER:$SERVICE_GROUP /opt/usr/thyme/triggered_data|" \
    "$DEPLOY_DIR/systemd/thyme-app.service" >"$tmp"
  install_file "$tmp" "$dst" 0644
  rm -f "$tmp"
}

install_inittab() {
  inittab=$(rooted /etc/inittab)
  line='::respawn:/etc/init.d/thyme-app run'
  shutdown_line='::shutdown:/etc/init.d/thyme-app stop'
  if [ -f "$inittab" ] && grep -qxF "$line" "$inittab"; then
    log "unchanged: $inittab (respawn line present)"
    return 0
  fi
  if [ ! -f "$inittab" ] && [ -z "$PREFIX" ]; then
    warn "$inittab does not exist; refusing to create a fresh BusyBox inittab on a live root"
    log "add this line manually: $line"
    return 0
  fi
  if [ "$DRY_RUN" -eq 1 ]; then
    dry_log "append '$line' and '$shutdown_line' to $inittab"
    return 0
  fi
  mkdir -p "$(dirname "$inittab")"
  {
    printf '\n# thyme supervisor (BusyBox respawn supervision)\n'
    printf '%s\n%s\n' "$line" "$shutdown_line"
  } >>"$inittab"
  CHANGED=1
  log "updated: $inittab"
}

# ---- arguments -------------------------------------------------------------

while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --prefix)
      [ $# -ge 2 ] || die "--prefix needs a directory"
      PREFIX=$2
      shift
      ;;
    --prefix=*) PREFIX=${1#*=} ;;
    --init)
      [ $# -ge 2 ] || die "--init needs a value"
      INIT=$2
      shift
      ;;
    --init=*) INIT=${1#*=} ;;
    --app-bin)
      [ $# -ge 2 ] || die "--app-bin needs a path"
      APP_BIN=$2
      shift
      ;;
    --app-bin=*)
      APP_BIN=${1#*=}
      ;;
    --user)
      [ $# -ge 2 ] || die "--user needs a name"
      SERVICE_USER=$2
      ACCOUNT_EXPLICIT=1
      shift
      ;;
    --user=*)
      SERVICE_USER=${1#*=}
      ACCOUNT_EXPLICIT=1
      ;;
    --group)
      [ $# -ge 2 ] || die "--group needs a name"
      SERVICE_GROUP=$2
      ACCOUNT_EXPLICIT=1
      shift
      ;;
    --group=*)
      SERVICE_GROUP=${1#*=}
      ACCOUNT_EXPLICIT=1
      ;;
    --no-enable) DO_ENABLE=0 ;;
    --start) DO_START=1 ;;
    --with-inittab) WITH_INITTAB=1 ;;
    -h | --help)
      usage
      exit 0
      ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

case "$INIT" in
  auto | systemd | busybox) ;;
  *) die "--init must be auto, systemd or busybox" ;;
esac
case "$APP_BIN" in
  /*) ;;
  *) die "--app-bin must be an absolute path" ;;
esac
case "$PREFIX" in
  "" | /*) ;;
  *) die "--prefix must be an absolute path" ;;
esac
[ -n "$SERVICE_USER" ] || die "--user must not be empty"
[ -n "$SERVICE_GROUP" ] || die "--group must not be empty"

if [ "$DRY_RUN" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
  die "must run as root; use --dry-run to preview (or --prefix with --dry-run)"
fi

PREFIX=${PREFIX%/}
INIT_KIND=$(detect_init)

log "thyme service installer: init=$INIT_KIND prefix=${PREFIX:-/} dry-run=$DRY_RUN"

# ---- shared files ----------------------------------------------------------

install_file "$DEPLOY_DIR/systemd/thyme-app-run" \
  "$(rooted /usr/libexec/thyme/thyme-app-run)" 0755
install_file "$DEPLOY_DIR/README.md" \
  "$(rooted /usr/share/doc/thyme/deploy/README.md)" 0644
ensure_env_file
ensure_dirs

# ---- init-specific ---------------------------------------------------------

if [ "$INIT_KIND" = "systemd" ]; then
  ensure_account
  install_systemd_unit
  if [ -z "$PREFIX" ] && { [ "$DRY_RUN" -eq 1 ] || [ -d /run/systemd/system ]; }; then
    run_cmd systemctl daemon-reload
    if [ "$DO_ENABLE" -eq 1 ]; then
      run_cmd systemctl enable thyme-app.service
    fi
    if [ "$DO_START" -eq 1 ]; then
      run_cmd systemctl restart thyme-app.service
    fi
  else
    log "systemd not running (or staging prefix): skipping daemon-reload/enable"
  fi
else
  install_file "$DEPLOY_DIR/sysvinit/thyme-app" \
    "$(rooted /etc/init.d/thyme-app)" 0755
  if [ "$WITH_INITTAB" -eq 1 ]; then
    install_inittab
  else
    log "next: add the respawn line from deploy/busybox/inittab to /etc/inittab,"
    log "      or start once with /etc/init.d/thyme-app start (no auto-restart)"
  fi
fi

log ""
log "done. Edit $(rooted /etc/thyme/thyme-app.env), then:"
if [ "$INIT_KIND" = "systemd" ]; then
  log "  systemctl start thyme-app"
  log "  systemctl status thyme-app"
else
  log "  /etc/init.d/thyme-app start"
  log "  /etc/init.d/thyme-app status"
fi
if [ "$CHANGED" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
  log "(no files changed: installation was already up to date)"
fi
