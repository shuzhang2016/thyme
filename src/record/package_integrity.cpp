#include "thyme/record/package_integrity.h"

#include "sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace thyme {
namespace integrity {

namespace {

constexpr uint64_t kMaxManifestBytes = 256ull * 1024 * 1024;
constexpr std::size_t kHashChunk = std::size_t{64} * 1024;

void PutU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void PutU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

void PutU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint16_t GetU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t GetU32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

uint64_t GetU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

bool ReadFile(const std::string& path, uint64_t max_bytes, std::string* out,
              std::string* err) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (err != nullptr) *err = "cannot open " + path + ": " + std::strerror(errno);
    return false;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    if (err != nullptr) *err = "not a regular file: " + path;
    return false;
  }
  if (static_cast<uint64_t>(st.st_size) > max_bytes) {
    close(fd);
    if (err != nullptr) *err = "file too large: " + path;
    return false;
  }
  out->clear();
  out->resize(static_cast<std::size_t>(st.st_size));
  std::size_t done = 0;
  while (done < out->size()) {
    const ssize_t n = read(fd, &(*out)[done], out->size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      close(fd);
      if (err != nullptr) *err = "read failed: " + path;
      return false;
    }
    if (n == 0) break;
    done += static_cast<std::size_t>(n);
  }
  close(fd);
  out->resize(done);
  return true;
}

bool WriteAll(int fd, const void* data, std::size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (n != 0) {
    const ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= static_cast<std::size_t>(w);
  }
  return true;
}

void SyncDirFd(const std::string& dir) {
  const int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return;
  fsync(fd);
  close(fd);
}

// Writes dir/name through dir/name.tmp + fsync + rename so a crash never
// leaves a partially-written manifest that still parses.
bool WriteFileAtomic(const std::string& dir, const char* name,
                     const void* data, std::size_t n, std::string* err) {
  const std::string final_path = dir + "/" + name;
  const std::string tmp_path = final_path + ".tmp";
  const int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                      0644);
  if (fd < 0) {
    if (err != nullptr) {
      *err = "cannot create " + tmp_path + ": " + std::strerror(errno);
    }
    return false;
  }
  bool ok = WriteAll(fd, data, n);
  if (ok && fsync(fd) != 0) ok = false;
  close(fd);
  if (!ok) {
    unlink(tmp_path.c_str());
    if (err != nullptr) *err = "write failed: " + tmp_path;
    return false;
  }
  if (rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    unlink(tmp_path.c_str());
    if (err != nullptr) {
      *err = "rename failed: " + final_path + ": " + std::strerror(errno);
    }
    return false;
  }
  SyncDirFd(dir);
  return true;
}

bool IsManifestName(const std::string& name) {
  return name == kManifestName || name == kSignatureName ||
         name == PackageHashState::kCheckpointName ||
         name.rfind(PackageHashState::kCheckpointTempPrefix, 0) == 0 ||
         name == std::string(kManifestName) + ".tmp" ||
         name == std::string(kSignatureName) + ".tmp";
}

bool CollectFiles(const std::string& dir, const std::string& prefix,
                  std::vector<std::string>* out, std::string* err) {
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) {
    if (err != nullptr) *err = "cannot list " + dir + ": " + std::strerror(errno);
    return false;
  }
  std::vector<std::string> names;
  while (dirent* e = readdir(d)) {
    if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
      continue;
    }
    names.emplace_back(e->d_name);
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    const std::string rel = prefix.empty() ? name : prefix + "/" + name;
    const std::string full = dir + "/" + name;
    struct stat st {};
    if (lstat(full.c_str(), &st) != 0) {
      if (err != nullptr) {
        *err = "cannot stat " + full + ": " + std::strerror(errno);
      }
      return false;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!CollectFiles(full, rel, out, err)) return false;
    } else if (S_ISREG(st.st_mode)) {
      if (prefix.empty() && IsManifestName(name)) continue;
      out->push_back(rel);
    }
    // Symlinks, sockets and devices are not package evidence: ignoring them
    // keeps the manifest clean, and verification flags extras on disk.
  }
  return true;
}

bool HashFile(const std::string& path, uint64_t* size, uint8_t out[32],
              std::string* err) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (err != nullptr) *err = "cannot open " + path;
    return false;
  }
  sha256_detail::Ctx c{};
  sha256_detail::Init(&c);
  uint8_t buf[kHashChunk];
  uint64_t total = 0;
  for (;;) {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      close(fd);
      if (err != nullptr) *err = "read failed: " + path;
      return false;
    }
    if (n == 0) break;
    sha256_detail::Update(&c, buf, static_cast<std::size_t>(n));
    total += static_cast<uint64_t>(n);
  }
  close(fd);
  sha256_detail::Final(&c, out);
  *size = total;
  return true;
}

void ChainStep(const uint8_t prev[32], const std::string& path, uint64_t size,
               const uint8_t sha[32], uint8_t out[32]) {
  uint8_t sz[8];
  PutU64(sz, size);
  sha256_detail::Ctx c{};
  sha256_detail::Init(&c);
  sha256_detail::Update(&c, prev, 32);
  sha256_detail::Update(&c, path.data(), path.size());
  sha256_detail::Update(&c, sz, sizeof(sz));
  sha256_detail::Update(&c, sha, 32);
  sha256_detail::Final(&c, out);
}

void GenesisRoot(uint8_t out[32]) {
  // chain[-1] is sha256(genesis tag): a fixed 32-byte root, so every
  // ChainStep above it has the same shape.
  sha256_detail::Ctx c{};
  sha256_detail::Init(&c);
  sha256_detail::Update(&c, kGenesisTag, sizeof(kGenesisTag) - 1);
  sha256_detail::Final(&c, out);
}

bool SafeRelativePath(const std::string& p) {
  if (p.empty() || p.size() > kManifestMaxPathBytes) return false;
  if (p[0] == '/') return false;
  std::size_t start = 0;
  for (;;) {
    const std::size_t slash = p.find('/', start);
    const std::string seg =
        p.substr(start, slash == std::string::npos ? std::string::npos
                                                   : slash - start);
    if (seg.empty() || seg == "." || seg == "..") return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

struct Entry {
  std::string path;
  uint64_t size = 0;
  uint8_t sha[32] = {};
  bool hash_checked = true;
};

bool IsZeroDigest(const uint8_t sha[32]) {
  for (std::size_t i = 0; i < 32; ++i) {
    if (sha[i] != 0) return false;
  }
  return true;
}

bool IsRawPath(const std::string& path) {
  return path.size() > 11 && path.compare(0, 7, "frames/") == 0 &&
         path.compare(path.size() - 4, 4, ".raw") == 0;
}

ManifestMode EntryMode(const PackageInfo& info, const std::string& path) {
  if (info.manifest_mode == ManifestMode::kOff) return ManifestMode::kOff;
  if (!IsRawPath(path)) return ManifestMode::kFull;
  const std::size_t begin = 7;
  const std::size_t end = path.size() - 4;
  const std::string channel = path.substr(begin, end - begin);
  for (const auto& item : info.bulk_modes) {
    if (item.first == channel) return item.second;
  }
  return info.manifest_mode == ManifestMode::kMetadata ? ManifestMode::kMetadata
                                                       : ManifestMode::kFull;
}

}  // namespace

std::string HexEncodeBytes(const uint8_t* data, std::size_t n) { return HexEncode(data, n); }

const char* ManifestModeName(ManifestMode mode) {
  switch (mode) {
    case ManifestMode::kFull: return "full";
    case ManifestMode::kMetadata: return "metadata";
    case ManifestMode::kOff: return "off";
    case ManifestMode::kMixed: return "mixed";
  }
  return "unknown";
}

bool ParseManifestMode(const std::string& text, ManifestMode* out) {
  if (out == nullptr || text == "mixed") return false;
  if (text == "full") {
    *out = ManifestMode::kFull;
  } else if (text == "metadata") {
    *out = ManifestMode::kMetadata;
  } else if (text == "off") {
    *out = ManifestMode::kOff;
  } else {
    return false;
  }
  return true;
}

bool LoadSigningKey(const std::string& path, std::string* key, std::string* err) {
  if (key == nullptr) return false;
  std::string data;
  if (!ReadFile(path, 4096, &data, err)) return false;
  while (!data.empty()) {
    const unsigned char c = static_cast<unsigned char>(data.back());
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    data.pop_back();
  }
  if (data.empty()) {
    if (err != nullptr) *err = "signing key file is empty: " + path;
    return false;
  }
  *key = data;
  return true;
}

namespace {

bool WriteManifestEntries(const std::string& package_dir,
                          const PackageInfo& info,
                          const std::string& signing_key_path,
                          const std::vector<Entry>& entries, std::string* err) {
  if (entries.size() > kManifestMaxEntries) {
    if (err != nullptr) *err = "too many files for an integrity manifest";
    return false;
  }
  if (static_cast<uint8_t>(info.manifest_mode) >
      static_cast<uint8_t>(ManifestMode::kMixed)) {
    if (err != nullptr) *err = "invalid integrity manifest mode";
    return false;
  }

  const uint8_t zero_digest[32] = {};
  uint8_t chain[32];
  GenesisRoot(chain);
  for (const Entry& e : entries) {
    const bool hashed = EntryMode(info, e.path) == ManifestMode::kFull;
    const uint8_t* digest = hashed ? e.sha : zero_digest;
    uint8_t next[32];
    ChainStep(chain, e.path, e.size, digest, next);
    std::memcpy(chain, next, sizeof(chain));
  }

  std::string key;
  const bool sign = !signing_key_path.empty();
  if (sign && !LoadSigningKey(signing_key_path, &key, err)) return false;

  std::vector<uint8_t> buf(kManifestHeaderBytes);
  std::memcpy(&buf[0], "THYINTG1", 8);
  PutU16(&buf[8], static_cast<uint16_t>(kManifestVersion));
  buf[10] = static_cast<uint8_t>(info.kind);
  buf[11] = static_cast<uint8_t>((sign ? kFlagSigned : 0) | (info.partial ? kFlagPartial : 0) |
                                 (static_cast<uint8_t>(info.manifest_mode) << kFlagModeShift));
  PutU32(&buf[12], static_cast<uint32_t>(entries.size()));
  PutU64(&buf[16], info.seal_time_ns);
  PutU64(&buf[24], info.session_id);
  PutU64(&buf[32], info.trigger_id);
  PutU64(&buf[40], info.t0_ns);
  PutU64(&buf[48], info.before_ns);
  PutU64(&buf[56], info.after_ns);
  PutU64(&buf[64], info.slice_seq);
  PutU64(&buf[72], info.begin_ns);
  PutU64(&buf[80], info.end_ns);
  std::memcpy(&buf[88], chain, sizeof(chain));

  for (const Entry& e : entries) {
    const bool hashed = EntryMode(info, e.path) == ManifestMode::kFull;
    const uint8_t* digest = hashed ? e.sha : zero_digest;
    const std::size_t base = buf.size();
    buf.resize(base + 44 + e.path.size());
    PutU32(&buf[base], static_cast<uint32_t>(e.path.size()));
    PutU64(&buf[base + 4], e.size);
    std::memcpy(&buf[base + 12], digest, 32);
    std::memcpy(&buf[base + 44], e.path.data(), e.path.size());
  }

  if (!WriteFileAtomic(package_dir, kManifestName, buf.data(), buf.size(),
                       err)) {
    return false;
  }
  if (sign) {
    uint8_t mac[32];
    HmacSha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
               buf.data(), buf.size(), mac);
    const std::string hex = HexEncode(mac, sizeof(mac)) + "\n";
    if (!WriteFileAtomic(package_dir, kSignatureName, hex.data(), hex.size(),
                         err)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool WritePackageManifest(const std::string& package_dir,
                          const PackageInfo& info,
                          const std::string& signing_key_path,
                          std::string* err) {
  std::vector<std::string> files;
  if (!CollectFiles(package_dir, "", &files, err)) return false;
  std::sort(files.begin(), files.end());

  std::vector<Entry> entries;
  entries.reserve(files.size());
  for (const std::string& rel : files) {
    Entry e;
    e.path = rel;
    if (!HashFile(package_dir + "/" + rel, &e.size, e.sha, err)) return false;
    entries.push_back(e);
  }
  return WriteManifestEntries(package_dir, info, signing_key_path, entries, err);
}

bool WritePackageManifestFromHashes(const std::string& package_dir,
                                    const PackageInfo& info,
                                    const std::string& signing_key_path,
                                    const PackageHashState& hashes,
                                    std::string* err,
                                    const std::string& uncommitted_path,
                                    const void* uncommitted_data,
                                    std::size_t uncommitted_size) {
  std::vector<std::string> files;
  if (!CollectFiles(package_dir, "", &files, err)) return false;
  std::sort(files.begin(), files.end());
  const std::vector<PackageFileDigest> digests = hashes.DigestFiles(
      uncommitted_path, uncommitted_data, uncommitted_size);
  bool same_files = files.size() == digests.size();
  for (std::size_t i = 0; same_files && i < files.size(); ++i) {
    same_files = files[i] == digests[i].path;
  }
  if (!same_files) {
    if (err != nullptr) {
      *err = "package file set differs from incrementally hashed file set";
    }
    return false;
  }
  for (std::size_t i = 0; i < files.size(); ++i) {
    struct stat st {};
    const std::string path = package_dir + "/" + files[i];
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
      if (err != nullptr) *err = "cannot stat packaged file " + files[i];
      return false;
    }
    if (static_cast<uint64_t>(st.st_size) != digests[i].size) {
      if (err != nullptr) {
        *err = "package file size differs from incrementally hashed size: " +
               files[i];
      }
      return false;
    }
  }

  std::vector<Entry> entries;
  entries.reserve(digests.size());
  for (const PackageFileDigest& digest : digests) {
    Entry e;
    e.path = digest.path;
    e.size = digest.size;
    std::memcpy(e.sha, digest.sha256, sizeof(e.sha));
    entries.push_back(e);
  }
  return WriteManifestEntries(package_dir, info, signing_key_path, entries, err);
}

bool VerifyResult::intact() const {
  if (state != ManifestState::kOk) return false;
  if (!chain_ok) return false;
  if (files_missing != 0 || files_mismatch != 0 || files_unlisted != 0) {
    return false;
  }
  if (signed_manifest && !(signature_checked && signature_ok)) return false;
  return true;
}

bool VerifyResult::ok() const { return intact() && !partial; }

VerifyResult VerifyPackageManifest(const std::string& package_dir,
                                   const std::string& signing_key_path) {
  VerifyResult out;
  std::string raw;
  std::string err;
  if (!ReadFile(package_dir + "/" + kManifestName, kMaxManifestBytes, &raw,
                &err)) {
    // Distinguish absent from unreadable/unparsable.
    struct stat st {};
    if (lstat((package_dir + "/" + kManifestName).c_str(), &st) != 0 &&
        errno == ENOENT) {
      out.state = ManifestState::kAbsent;
      out.error = "no integrity manifest (pre-SEC-03 package, or a "
                  "pre-REC-18/failed partial manifest on an .incomplete "
                  "package)";
    } else {
      out.state = ManifestState::kInvalid;
      out.error = err;
    }
    return out;
  }
  if (raw.size() < kManifestHeaderBytes) {
    out.state = ManifestState::kInvalid;
    out.error = "manifest shorter than its header";
    return out;
  }
  const uint8_t* p = reinterpret_cast<const uint8_t*>(raw.data());
  if (std::memcmp(p, "THYINTG1", 8) != 0) {
    out.state = ManifestState::kInvalid;
    out.error = "bad manifest magic";
    return out;
  }
  if (GetU16(p + 8) != kManifestVersion) {
    out.state = ManifestState::kInvalid;
    out.error = "unsupported manifest version";
    return out;
  }
  out.info.kind = static_cast<PackageKind>(p[10]);
  if (out.info.kind != PackageKind::kTrigger &&
      out.info.kind != PackageKind::kContinuous) {
    out.state = ManifestState::kInvalid;
    out.error = "bad manifest kind";
    return out;
  }
  const uint8_t flags = p[11];
  if ((flags & ~kFlagKnownMask) != 0) {
    out.state = ManifestState::kInvalid;
    out.error = "unknown manifest flags";
    return out;
  }
  out.signed_manifest = (flags & kFlagSigned) != 0;
  out.partial = (flags & kFlagPartial) != 0;
  out.info.manifest_mode = static_cast<ManifestMode>((flags & kFlagModeMask) >> kFlagModeShift);
  const uint32_t count = GetU32(p + 12);
  if (count > kManifestMaxEntries) {
    out.state = ManifestState::kInvalid;
    out.error = "manifest file count out of range";
    return out;
  }
  out.info.seal_time_ns = GetU64(p + 16);
  out.info.session_id = GetU64(p + 24);
  out.info.trigger_id = GetU64(p + 32);
  out.info.t0_ns = GetU64(p + 40);
  out.info.before_ns = GetU64(p + 48);
  out.info.after_ns = GetU64(p + 56);
  out.info.slice_seq = GetU64(p + 64);
  out.info.begin_ns = GetU64(p + 72);
  out.info.end_ns = GetU64(p + 80);
  uint8_t stored_chain[32];
  std::memcpy(stored_chain, p + 88, sizeof(stored_chain));

  std::vector<Entry> entries;
  entries.reserve(count);
  std::size_t off = kManifestHeaderBytes;
  for (uint32_t i = 0; i < count; ++i) {
    if (off + 44 > raw.size()) {
      out.state = ManifestState::kInvalid;
      out.error = "truncated manifest entry";
      return out;
    }
    const uint32_t path_len = GetU32(p + off);
    if (path_len == 0 || path_len > kManifestMaxPathBytes ||
        off + 44 + path_len > raw.size()) {
      out.state = ManifestState::kInvalid;
      out.error = "bad manifest entry path length";
      return out;
    }
    Entry e;
    e.path.assign(reinterpret_cast<const char*>(p + off + 44), path_len);
    e.size = GetU64(p + off + 4);
    std::memcpy(e.sha, p + off + 12, 32);
    e.hash_checked = !IsZeroDigest(e.sha);
    if (!SafeRelativePath(e.path)) {
      out.state = ManifestState::kInvalid;
      out.error = "unsafe path in manifest: " + e.path;
      return out;
    }
    bool digest_shape_ok = true;
    switch (out.info.manifest_mode) {
      case ManifestMode::kFull: digest_shape_ok = e.hash_checked; break;
      case ManifestMode::kMetadata: digest_shape_ok = e.hash_checked == !IsRawPath(e.path); break;
      case ManifestMode::kOff: digest_shape_ok = !e.hash_checked; break;
      case ManifestMode::kMixed: digest_shape_ok = IsRawPath(e.path) || e.hash_checked; break;
    }
    if (!digest_shape_ok) {
      out.state = ManifestState::kInvalid;
      out.error = "manifest digest does not match its integrity mode: " + e.path;
      return out;
    }
    if (!entries.empty() && !(entries.back().path < e.path)) {
      out.state = ManifestState::kInvalid;
      out.error = "manifest entries are not strictly sorted: " + e.path;
      return out;
    }
    entries.push_back(e);
    off += 44 + path_len;
  }
  if (off != raw.size()) {
    out.state = ManifestState::kInvalid;
    out.error = "trailing bytes after manifest entries";
    return out;
  }

  // Self-consistency: recompute the chain from the declared entries. A
  // mismatch means the manifest itself was edited (or corrupted).
  uint8_t chain[32];
  GenesisRoot(chain);
  for (const Entry& e : entries) {
    uint8_t next[32];
    ChainStep(chain, e.path, e.size, e.sha, next);
    std::memcpy(chain, next, sizeof(chain));
  }
  if (std::memcmp(chain, stored_chain, sizeof(chain)) != 0) {
    out.state = ManifestState::kInvalid;
    out.error = "manifest chain root mismatch (manifest modified/corrupted)";
    return out;
  }

  // Signature before per-file work: a forged manifest should not be trusted
  // even for reporting.
  if (out.signed_manifest) {
    std::string sig_raw;
    out.signature_present = ReadFile(package_dir + "/" + kSignatureName, 256,
                                     &sig_raw, nullptr);
    if (signing_key_path.empty()) {
      out.error = "signed manifest; --key <signing-key> is required";
    } else if (!out.signature_present) {
      out.error = "manifest is signed but integrity.hmac is missing";
    } else {
      std::string key;
      if (!LoadSigningKey(signing_key_path, &key, &out.error)) {
        // error already set
      } else {
        uint8_t mac[32];
        HmacSha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                   raw.data(), raw.size(), mac);
        const std::string expect = HexEncode(mac, sizeof(mac));
        std::string got = sig_raw;
        while (!got.empty() &&
               (got.back() == '\n' || got.back() == '\r' ||
                got.back() == ' ')) {
          got.pop_back();
        }
        out.signature_checked = true;
        out.signature_ok = got == expect;
        if (!out.signature_ok) out.error = "HMAC-SHA256 signature mismatch";
      }
    }
  } else if (!signing_key_path.empty()) {
    // Key supplied for an unsigned manifest: nothing to check. Verification
    // reports the manifest as unsigned rather than failing the package.
  }

  // Per-file status.
  std::vector<std::string> actual;
  std::string walk_err;
  if (!CollectFiles(package_dir, "", &actual, &walk_err)) {
    out.state = ManifestState::kInvalid;
    out.error = walk_err;
    return out;
  }
  std::sort(actual.begin(), actual.end());
  std::size_t ai = 0;
  for (const Entry& e : entries) {
    FileStatus fs;
    fs.path = e.path;
    fs.manifest_size = e.size;
    fs.hash_checked = e.hash_checked;
    const std::string full = package_dir + "/" + e.path;
    struct stat st {};
    if (lstat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      fs.present = true;
      fs.actual_size = static_cast<uint64_t>(st.st_size);
      fs.size_ok = fs.actual_size == e.size;
      if (e.hash_checked) {
        uint8_t sha[32];
        std::string herr;
        if (HashFile(full, &fs.actual_size, sha, &herr)) {
          std::memcpy(fs.actual_sha256, sha, sizeof(sha));
          fs.sha256_hex = HexEncode(sha, sizeof(sha));
          fs.size_ok = fs.actual_size == e.size;
          fs.hash_ok = std::memcmp(sha, e.sha, 32) == 0;
        }
      } else {
        ++out.files_unverified;
      }
    }
    if (!fs.present) {
      ++out.files_missing;
    } else if (!fs.size_ok || (fs.hash_checked && !fs.hash_ok)) {
      ++out.files_mismatch;
    } else {
      ++out.files_ok;
    }
    out.files.push_back(fs);
    // Advance the on-disk cursor past listed paths to find extras.
    while (ai < actual.size() && actual[ai] < e.path) {
      out.unlisted.push_back(actual[ai]);
      ++ai;
    }
    if (ai < actual.size() && actual[ai] == e.path) ++ai;
  }
  while (ai < actual.size()) {
    out.unlisted.push_back(actual[ai]);
    ++ai;
  }
  out.files_unlisted = out.unlisted.size();

  // Chain over the declared verification scope. Unchecked payload entries
  // contribute their manifested zero digest, while size remains actual.
  bool all_present = true;
  uint8_t actual_chain[32];
  GenesisRoot(actual_chain);
  for (const FileStatus& fs : out.files) {
    if (!fs.present) {
      all_present = false;
      break;
    }
    uint8_t next[32];
    ChainStep(actual_chain, fs.path, fs.actual_size, fs.actual_sha256, next);
    std::memcpy(actual_chain, next, sizeof(actual_chain));
  }
  out.chain_ok =
      all_present && std::memcmp(actual_chain, stored_chain, 32) == 0;
  if (!out.chain_ok && out.error.empty()) {
    out.error = "hash chain mismatch (package content modified)";
  }
  out.state = ManifestState::kOk;
  return out;
}

}  // namespace integrity
}  // namespace thyme
