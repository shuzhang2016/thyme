#pragma once

// package_integrity.h — SEC-03 evidence-package integrity manifest, extended
// by REC-18 with partial manifests for invalidated (.incomplete) packages.
//
// On a clean seal (trigger capture or continuous slice) the recorder writes
//
//   <pkg>/integrity.manifest   binary manifest: header + one entry per file
//   <pkg>/integrity.hmac       optional detached HMAC-SHA256 (hex), present
//                              only when [security].signing_key is configured
//
// REC-18: when a package is actively invalidated (abort row + .incomplete
// rename — seal-path failure, ENOSPC abort) or swept to .incomplete by the
// retention startup pass (crash-leftover), a manifest with the partial flag
// is written over the surviving file set, so forensics can still verify that
// what remains of the package is untouched. Recorder-written files update
// their SHA-256 incrementally; startup recovery hashes the survivors on demand.
//
// Manifest format (little-endian, fixed 120-byte header then file_count
// entries; documented in docs/security/evidence-integrity.md):
//
//   offset  size  field
//   0       8     magic "THYINTG1"
//   8       2     version (1)
//   10      1     kind (1 = trigger, 2 = continuous)
//   11      1     flags (bit0: HMAC-SHA256 signature present,
//                        bit1: partial — package invalidated/aborted, the
//                              manifest covers the surviving file set,
//                        bit2..3: full/metadata/off/mixed content-hash mode)
//   12      4     file_count
//   16      8     seal_time_ns (CLOCK_REALTIME; invalidation/manifest time
//                 for partial manifests)
//   24      8     session_id (meta.tbin/wal.log session id; recovered from
//                 the wal header for sweep-marked packages, 0 if unreadable)
//   32      8     trigger_id (0 for continuous slices)
//   40      8     t0_ns (trigger) / 0
//   48      8     before_ns (trigger) / 0
//   56      8     after_ns (trigger) / 0
//   64      8     slice_seq (continuous) / 0
//   72      8     begin_ns (continuous) / 0
//   80      8     end_ns (continuous) / 0
//   88      32    chain root
//   120     ...   entries, sorted bytewise by relative path:
//                   4  path_len
//                   8  size
//                   32 sha256(file bytes), or zero for size-only entries
//                   N  path (no NUL, '/' separators, relative to <pkg>)
//
// Chain construction (rooted at a fixed genesis tag):
//
//   chain[-1] = sha256("thyme-evidence-package-v1")
//   chain[i]  = sha256(chain[i-1] || path || u64le(size) || sha256)
//   root      = chain[file_count-1]
//
// The chain makes deletion/reordering/duplication of entries detectable even
// if an attacker rewrites the per-file digests; it does not make the manifest
// itself authentic — that is the HMAC's job (detached, so verification works
// offline with the key file and no daemon).
//
// The manifest covers every regular file in the package. integrity.manifest
// and integrity.hmac are not self-covered (and are excluded from the file
// set). A sealed package with no manifest is *absent*: pre-SEC-03 packages
// are reported as such, never as a crash. A .incomplete package with no
// manifest is a crash-leftover from a pre-REC-18 build (or a partial-manifest
// write that itself failed, e.g. ENOSPC) — likewise reported absent.
//
// Signing choice (SEC-03): HMAC-SHA256 with a raw key file. No new
// dependency and no key management beyond "the deployment distributes the
// key". Limitation: HMAC is symmetric — anyone who can verify can also forge,
// and a same-uid/root attacker who can read the key file can re-sign a
// tampered package. Ed25519 would give public-key verification (the recorder
// holds only the secret key, analysts hold the public key) but needs a
// dependency (OpenSSL/libsodium) and is therefore an ADR, not this round.
//
// Encryption is explicitly out of scope this round: packages remain plain
// files inside a 0700 directory. At-rest confidentiality is NOT provided;
// anyone with filesystem access (same uid or root) can read every payload.
// See docs/security/evidence-integrity.md.

#include "thyme/record/package_hash.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace thyme {
namespace integrity {

inline constexpr char kManifestName[] = "integrity.manifest";
inline constexpr char kSignatureName[] = "integrity.hmac";
inline constexpr uint32_t kManifestVersion = 1;
inline constexpr uint32_t kManifestHeaderBytes = 120;
inline constexpr uint32_t kManifestMaxEntries = 1000000;
inline constexpr uint32_t kManifestMaxPathBytes = 4096;
inline constexpr char kGenesisTag[] = "thyme-evidence-package-v1";

enum class PackageKind : uint8_t { kTrigger = 1, kContinuous = 2 };
enum class ManifestMode : uint8_t {
  kFull = 0,
  kMetadata = 1,
  kOff = 2,
  kMixed = 3,
};
const char* ManifestModeName(ManifestMode mode);
bool ParseManifestMode(const std::string& text, ManifestMode* out);

inline constexpr uint8_t kFlagSigned = 1u << 0;
// REC-18: the package was invalidated/aborted (or swept by startup recovery)
// and this manifest covers the surviving file set, not a completed seal.
inline constexpr uint8_t kFlagPartial = 1u << 1;
inline constexpr uint8_t kFlagModeShift = 2;
inline constexpr uint8_t kFlagModeMask = 3u << kFlagModeShift;
inline constexpr uint8_t kFlagKnownMask = kFlagSigned | kFlagPartial | kFlagModeMask;

struct PackageInfo {
  PackageKind kind = PackageKind::kTrigger;
  uint64_t seal_time_ns = 0;  // CLOCK_REALTIME (invalidation time if partial)
  uint64_t session_id = 0;
  uint64_t trigger_id = 0;
  uint64_t t0_ns = 0;
  uint64_t before_ns = 0;
  uint64_t after_ns = 0;
  uint64_t slice_seq = 0;
  uint64_t begin_ns = 0;
  uint64_t end_ns = 0;
  // REC-18: set for .incomplete packages. Fields the writer no longer knows
  // (a startup-recovery sweep has only the wal header) stay 0.
  bool partial = false;
  ManifestMode manifest_mode = ManifestMode::kFull;
  // Effective full/metadata policy for each frames/<channel>.raw. Mixed uses
  // this list to distinguish fully hashed channels from metadata-only ones.
  std::vector<std::pair<std::string, ManifestMode>> bulk_modes;
};

// Reads and trims a signing key file (raw bytes; trailing whitespace is
// ignored so `printf 'secret\n' > key` works). Empty/missing -> false.
bool LoadSigningKey(const std::string& path, std::string* key,
                    std::string* err);

// Hashes every regular file under `package_dir` (recursively, sorted by
// relative path), builds the chain and writes integrity.manifest; when
// signing_key_path is non-empty also writes integrity.hmac. Uses temp files +
// rename + fsync so a crash never leaves a half-written manifest that parses.
// info.partial marks the manifest as covering an invalidated (.incomplete)
// package's surviving files instead of a completed seal.
// Returns false with *err on any I/O failure (on the seal path the caller
// must then treat the seal as failed; an unmanifested package is never
// "clean").
bool WritePackageManifest(const std::string& package_dir,
                          const PackageInfo& info,
                          const std::string& signing_key_path,
                          std::string* err);

// Recorder seal/invalidation variant: uses hashes accumulated by the package
// writers and never re-reads file contents. The on-disk regular-file set must
// exactly match the tracked set, otherwise the write fails closed. An optional
// uncommitted tail is included in the digest without changing the saved state;
// this lets a failed wal seal be replaced by an abort before the partial seal.
bool WritePackageManifestFromHashes(const std::string& package_dir,
                                    const PackageInfo& info,
                                    const std::string& signing_key_path,
                                    const PackageHashState& hashes,
                                    std::string* err,
                                    const std::string& uncommitted_path = {},
                                    const void* uncommitted_data = nullptr,
                                    std::size_t uncommitted_size = 0);

enum class ManifestState {
  kOk,       // parsed
  kAbsent,   // no manifest (aborted/crashed/legacy package)
  kInvalid,  // present but unreadable/inconsistent
};

struct FileStatus {
  std::string path;
  uint64_t manifest_size = 0;
  uint64_t actual_size = 0;
  bool present = false;
  bool size_ok = false;
  bool hash_checked = false;
  bool hash_ok = false;
  uint8_t actual_sha256[32] = {};
  std::string sha256_hex;  // actual digest (empty when absent or unchecked)
};

struct VerifyResult {
  ManifestState state = ManifestState::kAbsent;
  std::string error;
  PackageInfo info;
  bool partial = false;  // REC-18: manifest covers an invalidated package
  bool signed_manifest = false;
  bool signature_present = false;
  bool signature_checked = false;
  bool signature_ok = false;
  bool chain_ok = false;  // recomputed chain over the declared scope
  uint64_t files_ok = 0;
  uint64_t files_missing = 0;
  uint64_t files_mismatch = 0;    // size or checked digest differs
  uint64_t files_unlisted = 0;    // present on disk, absent from the manifest
  uint64_t files_unverified = 0;  // size checked; payload deliberately unchecked
  std::vector<FileStatus> files;
  std::vector<std::string> unlisted;

  // The declared verification scope is intact: the manifest parses, the chain
  // matches, every listed file is present with the declared size and every
  // non-zero digest matches, no extra file exists, and a signed manifest
  // verified with the supplied key. Zero-digest payload is not
  // content-verified.
  bool intact() const;

  // True only when intact() holds AND the package is a complete sealed
  // package (not partial). A partial package is forensic evidence of an
  // aborted/invalidated capture: its contents may verify, but it is not a
  // sealed package, so ok() stays false.
  bool ok() const;
};

// Offline verification (no daemon). `signing_key_path` may be empty: a
// signed manifest then fails with "signature key required" (fail closed).
VerifyResult VerifyPackageManifest(const std::string& package_dir,
                                   const std::string& signing_key_path);

std::string HexEncodeBytes(const uint8_t* data, std::size_t n);

}  // namespace integrity
}  // namespace thyme
