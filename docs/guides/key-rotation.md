# Evidence signing-key rotation (SEC-08)

This procedure applies to the optional HMAC-SHA256 package signature described
in [Evidence-package integrity](../security/evidence-integrity.md). Signing is
disabled unless `[security].signing_key` is configured. There is no certificate
or Ed25519 key store in the current implementation.

The manifest has no key id. Rotation therefore preserves the old key as a
separate file; the operator selects that file when verifying an old package.
No manifest, record layout, SHM object, or online ABI changes during rotation.

## Storage and permissions

Keep the active and archived keys outside every package directory. Restrict
their parent directories to the service account (and, where required, the
offline verification account). The rotation tool creates both active and
archived key files with mode `0600`; it never prints key material.

Example initial provisioning, run as the account that owns the recorder key:

```bash
install -d -m 700 /etc/thyme/keys /var/lib/thyme/key-archive
tools/ops/rotate_signing_key.sh \
  --key /etc/thyme/keys/evidence.key \
  --archive-dir /var/lib/thyme/key-archive
```

Configure the active path, not an archived path:

```ini
[security]
signing_key = /etc/thyme/keys/evidence.key
```

The generated key is 32 random bytes encoded as 64 hexadecimal characters plus
a newline. This is raw key material for the existing HMAC implementation, not
a password and not a public/private key pair. Anyone who can read an HMAC key
can both verify and forge packages.

If the file must be owned by a different service account, pass an explicit
owner, for example `--owner thyme:thyme`, while running the tool with suitable
privileges. Do not loosen the file mode to solve ownership mistakes.

## Rotate

Keep the path in `trigger_config.ini` unchanged and run:

```bash
tools/ops/rotate_signing_key.sh \
  --key /etc/thyme/keys/evidence.key \
  --archive-dir /var/lib/thyme/key-archive
```

The tool performs these steps:

1. open or create `<PATH>.lock` with mode `0600` and acquire an exclusive
   Linux `flock` on its file descriptor;
2. generate a new key in a temporary file in the active key's directory;
3. write and sync the complete key, then set mode `0600`;
4. copy the current key to a uniquely named, mode-`0600` archive file;
5. atomically rename the new file over the active path.

The lock file is a separate inode that is never renamed over the active key.
Concurrent rotations of the same active path therefore serialize the complete
archive-and-replace critical section, and every previous generation is archived
exactly once. The lock contains no key material. Closing its descriptor on
failure or a signal releases the lock; an operator does not need to delete the
lock file before retrying.

The temporary file and active path are on the same filesystem, so readers see
either the complete old key or the complete new key, never a partial file. A
failure before replacement leaves the active key unchanged and removes the
tool's temporary files.

The recorder reads the key path when it writes each manifest and does not wait
for the rotation lock. Consequently, an in-flight seal can use the old key and
later seals use the new key without changing the configuration; both keys are
retained as described above. Replacing key contents at the configured path does
not require a restart. Changing `[security].signing_key` to another path does
require a restart; SIGHUP deliberately keeps the running path.

## Verify after rotation

Use the archived key reported by the tool for packages sealed before rotation,
and the active key for packages sealed afterwards:

```bash
thyme-replay integrity /data/high/OLD_PACKAGE \
  --key /var/lib/thyme/key-archive/ARCHIVED_KEY
thyme-replay integrity /data/high/NEW_PACKAGE \
  --key /etc/thyme/keys/evidence.key
```

Both commands must print `integrity: OK` and exit zero for full, sealed
packages. A wrong key must produce `integrity: FAILED` and a non-zero exit.
Since the package does not identify its key, try only keys from the controlled
archive and record which archive belongs to each retention period; do not
weaken verification by accepting an unverified manifest.

Retain every archived key until all packages signed by it have been verified,
exported, or deleted under the retention policy. Deleting an old key makes its
packages impossible to authenticate. Archiving does not provide anti-rollback:
an attacker holding any retained HMAC key can still forge a package.

## Test

The focused test uses real manifest writing and verification, temporary key
directories, two successive rotations, and four concurrent rotations:

```bash
ctest --test-dir build -R '^test_signing_key_rotation$' --output-on-failure
```

It checks old/new package verification, wrong-key rejection, unique archives,
mode `0600`, and failure without active-key replacement or temporary-file
leftovers.
