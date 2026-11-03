#!/usr/bin/env bash
# SEC-08: atomically rotate the optional evidence-package HMAC key.

set -euo pipefail

PROG="$(basename "$0")"
KEY_PATH=""
ARCHIVE_DIR=""
OWNER=""

usage() {
  cat <<EOF
usage: $PROG --key PATH --archive-dir DIR [--owner USER[:GROUP]]

Generate a 256-bit HMAC key, preserve the current key in DIR, and atomically
replace PATH. New and archived key files are mode 0600. PATH may be absent for
initial provisioning; its parent directory and DIR must already exist.
EOF
}

usage_error() {
  printf '%s: %s\n' "$PROG" "$*" >&2
  usage >&2
  exit 1
}

die() {
  printf '%s: %s\n' "$PROG" "$*" >&2
  exit 2
}

need_value() {
  if [ "$#" -lt 2 ] || [ -z "$2" ]; then
    usage_error "missing value for $1"
  fi
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --key) need_value "$@"; KEY_PATH="$2"; shift ;;
    --archive-dir) need_value "$@"; ARCHIVE_DIR="$2"; shift ;;
    --owner) need_value "$@"; OWNER="$2"; shift ;;
    --help|-h) usage; exit 0 ;;
    *) usage_error "unknown option $1" ;;
  esac
  shift
done

[ -n "$KEY_PATH" ] || usage_error "--key is required"
[ -n "$ARCHIVE_DIR" ] || usage_error "--archive-dir is required"

KEY_DIR="$(dirname -- "$KEY_PATH")"
KEY_NAME="$(basename -- "$KEY_PATH")"
[ -d "$KEY_DIR" ] || die "key parent directory does not exist: $KEY_DIR"
[ -d "$ARCHIVE_DIR" ] || die "archive directory does not exist: $ARCHIVE_DIR"
[ "$KEY_NAME" != "." ] && [ "$KEY_NAME" != "/" ] || die "invalid key path: $KEY_PATH"

umask 077
LOCK_PATH="${KEY_PATH}.lock"
if [ -L "$LOCK_PATH" ] || { [ -e "$LOCK_PATH" ] && [ ! -f "$LOCK_PATH" ]; }; then
  die "rotation lock must be a regular file: $LOCK_PATH"
fi
exec {LOCK_FD}>"$LOCK_PATH" || die "could not open rotation lock: $LOCK_PATH"
chmod 600 "$LOCK_PATH"
flock --exclusive "$LOCK_FD" || die "could not acquire rotation lock: $LOCK_PATH"

if [ -L "$KEY_PATH" ]; then
  die "refusing to replace a symbolic link: $KEY_PATH"
fi
if [ -e "$KEY_PATH" ] && { [ ! -f "$KEY_PATH" ] || [ ! -s "$KEY_PATH" ]; }; then
  die "existing key must be a non-empty regular file: $KEY_PATH"
fi

NEW_KEY="$(mktemp "$KEY_DIR/.${KEY_NAME}.new.XXXXXX")"
ARCHIVE_TMP=""
cleanup() {
  rm -f -- "$NEW_KEY"
  if [ -n "$ARCHIVE_TMP" ]; then
    rm -f -- "$ARCHIVE_TMP"
  fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Hex encoding keeps the random key compatible with LoadSigningKey's trailing
# whitespace trimming. No key material is written to stdout or stderr.
if ! dd if=/dev/urandom bs=32 count=1 status=none |
    od -An -N32 -tx1 -v | tr -d ' \n' > "$NEW_KEY"; then
  die "could not generate a random key"
fi
printf '\n' >> "$NEW_KEY"
[ "$(wc -c < "$NEW_KEY")" -eq 65 ] || die "generated key has an invalid length"
chmod 600 "$NEW_KEY"
if [ -n "$OWNER" ]; then
  chown "$OWNER" "$NEW_KEY"
fi
sync -d "$NEW_KEY"

ARCHIVE_PATH=""
if [ -f "$KEY_PATH" ]; then
  ARCHIVE_TMP="$(mktemp "$ARCHIVE_DIR/.${KEY_NAME}.old.XXXXXX")"
  cp -- "$KEY_PATH" "$ARCHIVE_TMP"
  chmod 600 "$ARCHIVE_TMP"
  if [ -n "$OWNER" ]; then
    chown "$OWNER" "$ARCHIVE_TMP"
  fi
  sync -d "$ARCHIVE_TMP"
  ARCHIVE_PATH="$ARCHIVE_DIR/$(date -u +%Y%m%dT%H%M%SZ)-${ARCHIVE_TMP##*/}"
  mv -n -- "$ARCHIVE_TMP" "$ARCHIVE_PATH"
  [ -f "$ARCHIVE_PATH" ] || die "could not archive the previous key"
  ARCHIVE_TMP=""
fi

mv -fT -- "$NEW_KEY" "$KEY_PATH"
printf 'active signing key: %s\n' "$KEY_PATH"
if [ -n "$ARCHIVE_PATH" ]; then
  printf 'previous signing key: %s\n' "$ARCHIVE_PATH"
fi
