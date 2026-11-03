#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace thyme {
namespace integrity {

struct PackageFileDigest {
  std::string path;
  uint64_t size = 0;
  uint8_t sha256[32] = {};
};

// Incremental hashes for the regular files written into one package. Files are
// tracked when opened so empty files are represented too. Append only records
// bytes accepted by the underlying file write; Replace is for atomic sidecars.
// With hash_enabled=false, Append still tracks size but DigestFiles returns a
// zero digest: REC-23 metadata/off manifests deliberately leave payload content
// unverified without losing the file-list/size chain.
class PackageHashState {
 public:
  static constexpr const char* kCheckpointName = "integrity.checkpoint";
  static constexpr const char* kCheckpointTempPrefix =
      ".integrity.checkpoint.tmp.";

  PackageHashState();
  ~PackageHashState();
  PackageHashState(PackageHashState&&) noexcept;
  PackageHashState& operator=(PackageHashState&&) noexcept;
  PackageHashState(const PackageHashState&) = delete;
  PackageHashState& operator=(const PackageHashState&) = delete;

  void Reset();
  void Track(const std::string& relative_path, bool hash_enabled = true);
  uint64_t Size(const std::string& relative_path) const;
  // Append-only: the writer must be positioned at Size(relative_path) before
  // calling this. seek+overwrite must use Replace instead (REC-24).
  void Append(const std::string& relative_path, const void* data,
              std::size_t n);
  void Replace(const std::string& relative_path, const void* data,
               std::size_t n);
  std::vector<PackageFileDigest> DigestFiles() const;
  std::vector<PackageFileDigest> DigestFiles(
      const std::string& uncommitted_path, const void* data,
      std::size_t n) const;

  // The checkpoint preserves incremental SHA-256 contexts. Load validates file
  // type and size with stat, but does not read payload bytes: same-size payload
  // changes are outside its detection scope. wal.log alone may have grown for
  // crash-recovery abort rows and must then be refreshed with RehashFile. CRC32
  // detects checkpoint corruption, not malicious replacement by another process
  // with write access.
  bool SaveCheckpoint(const std::string& package_dir,
                      uint64_t session_id) const;
  bool LoadCheckpoint(const std::string& package_dir,
                      uint64_t expected_session_id);
  // Re-read a small mutable file such as wal.log after loading a checkpoint.
  // Same-size overwrites cannot be detected by the checkpoint's stat checks.
  bool RehashFile(const std::string& package_dir,
                  const std::string& relative_path);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace integrity
}  // namespace thyme
