#include "thyme/record/retention.h"

#include "thyme/record/file_ptr.h"
#include "thyme/record/package_integrity.h"  // REC-18 partial manifests
#include "thyme/record/record_schema.h"       // ABI-09 partial schema sidecar
#include "thyme/record/triggered_recorder.h"  // TriggeredRecorder::PackageComplete
#include "thyme/runtime/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace thyme {
namespace retention {

namespace {

// lstat everywhere — symbolic links are never followed.
int64_t MtimeNs(const std::string& path) {
  struct stat st {};
  if (lstat(path.c_str(), &st) != 0) return 0;
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL +
         static_cast<int64_t>(st.st_mtim.tv_nsec);
}

int64_t RealtimeNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL +
         static_cast<int64_t>(ts.tv_nsec);
}

bool EndsWith(const std::string& s, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// symlinks (may be null) accumulates the number of skipped symbolic links.
// Beyond kMaxTreeDepth the walk stops descending.
uint64_t DirBytesWalk(const std::string& dir, uint32_t depth,
                      uint64_t* symlinks) {
  if (depth > kMaxTreeDepth) return 0;  // depth cap: corruption/attack, report 0
  uint64_t total = 0;
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return 0;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string sub = dir + "/" + e->d_name;
    struct stat st {};
    if (lstat(sub.c_str(), &st) != 0) continue;
    if (S_ISLNK(st.st_mode)) {
      if (symlinks != nullptr) ++*symlinks;  // not followed, not sized
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      total += DirBytesWalk(sub, depth + 1, symlinks);
    } else if (S_ISREG(st.st_mode)) {
      total += static_cast<uint64_t>(st.st_size);
    }
  }
  closedir(d);
  return total;
}

// Exceeding the depth cap refuses the delete (returns false, package kept);
// a symbolic link only has the link itself unlinked.
bool RemoveTreeWalk(const std::string& dir, uint32_t depth,
                    uint64_t* symlinks) {
  if (depth > kMaxTreeDepth) {
    THYME_LOG_ERROR("retention: refuse to remove %s (depth > %u)",
                    dir.c_str(), kMaxTreeDepth);
    return false;
  }
  bool ok = true;
  DIR* d = opendir(dir.c_str());
  if (d == nullptr) return false;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string sub = dir + "/" + e->d_name;
    struct stat st {};
    if (lstat(sub.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {  // lstat: a symlink never takes this branch
      if (!RemoveTreeWalk(sub, depth + 1, symlinks)) ok = false;
    } else {
      if (S_ISLNK(st.st_mode) && symlinks != nullptr) ++*symlinks;
      if (std::remove(sub.c_str()) != 0) ok = false;
    }
  }
  closedir(d);
  return rmdir(dir.c_str()) == 0 && ok;
}

// REC-18: after the .incomplete rename, manifest the surviving files as a
// partial package. The kind comes from the leaf name; the wal header still
// carries the session id (kContinuousWalMark|seq for slices), so trigger_id
// / slice_seq are recovered when the header parses. The other identity
// fields stay 0 — the sweep does not know the trigger window. Best-effort:
// a failure leaves the renamed package without a manifest (reported absent
// by the verifier), it never blocks the sweep.
void WriteRecoveryManifest(const std::string& dir, const std::string& leaf,
                           const std::string& signing_key_path,
                           const integrity::PackageInfo& manifest_policy) {
  integrity::PackageInfo info = manifest_policy;
  info.partial = true;
  info.seal_time_ns = static_cast<uint64_t>(RealtimeNs());
  info.kind = leaf.compare(0, 11, "continuous_") == 0
                  ? integrity::PackageKind::kContinuous
                  : integrity::PackageKind::kTrigger;
  if (FilePtr f = FilePtr(std::fopen((dir + "/wal.log").c_str(), "rb"))) {
    triggered::WalHeader wh{};
    if (std::fread(&wh, sizeof(wh), 1, f.get()) == 1 &&
        std::memcmp(wh.magic, triggered::kWalMagic,
                    sizeof(triggered::kWalMagic)) == 0) {
      info.session_id = wh.trigger_id;
      if (info.kind == integrity::PackageKind::kContinuous) {
        if ((wh.trigger_id & triggered::kContinuousWalMark) ==
            triggered::kContinuousWalMark) {
          info.slice_seq = wh.trigger_id & ~triggered::kContinuousWalMark;
        }
      } else {
        info.trigger_id = wh.trigger_id;
      }
    }
  }
  integrity::PackageHashState restored;
  const bool have_checkpoint =
      restored.LoadCheckpoint(dir, info.session_id) &&
      restored.RehashFile(dir, "wal.log");
  // ABI-09: the crash-leftover package still describes its record contracts
  // in meta.tbin's struct registry, so write the partial schema sidecar from
  // it before the manifest covers the survivors. Field-level layout is not
  // known here (the sweep has no generated contract table) — the sidecar says
  // so instead of guessing. Best-effort, same as the manifest.
  {
    ThymeBinReader reader((dir + "/meta.tbin").c_str());
    if (reader.ok() && !reader.registry().empty()) {
      schema::RecordManifest manifest;
      const schema::RecordManifest* manifest_ptr = nullptr;
      if (reader.manifest_state() == ThymeBinReader::ManifestState::kOk) {
        manifest.hash = reader.manifest_hash();
        manifest.version = reader.manifest_version();
        manifest_ptr = &manifest;
      }
      std::string serr;
      if (!schema::WritePackageSchema(dir, reader.registry().data(),
                                      reader.registry().size(), manifest_ptr,
                                      /*partial=*/true, &serr,
                                      have_checkpoint ? &restored : nullptr)) {
        THYME_LOG_WARN("retention: partial schema for %s failed: %s",
                       dir.c_str(), serr.c_str());
      }
    }
  }
  std::string err;
  if (have_checkpoint &&
      integrity::WritePackageManifestFromHashes(
          dir, info, signing_key_path, restored, &err)) {
    return;
  }
  if (!integrity::WritePackageManifest(dir, info, signing_key_path, &err)) {
    THYME_LOG_WARN("retention: partial manifest for %s failed: %s",
                   dir.c_str(), err.c_str());
  }
}

}  // namespace

bool ParseIncompletePolicy(const std::string& s, IncompletePolicy* out) {
  if (s == "keep") {
    *out = IncompletePolicy::kKeep;
    return true;
  }
  if (s == "delete_after_age") {
    *out = IncompletePolicy::kDeleteAfterAge;
    return true;
  }
  if (s == "delete_immediately") {
    *out = IncompletePolicy::kDeleteImmediately;
    return true;
  }
  return false;
}

const char* IncompletePolicyName(IncompletePolicy p) {
  switch (p) {
    case IncompletePolicy::kKeep: return "keep";
    case IncompletePolicy::kDeleteAfterAge: return "delete_after_age";
    case IncompletePolicy::kDeleteImmediately: return "delete_immediately";
  }
  return "keep";
}

bool Manager::Init(const Config& cfg) {
  if (cfg.out_dir.empty()) return false;
  cfg_ = cfg;
  struct stat st {};
  ok_ = stat(cfg_.out_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  return ok_;
}

uint64_t Manager::DirBytes(const std::string& dir) {
  return DirBytesWalk(dir, 0, nullptr);
}

bool Manager::RemoveTree(const std::string& dir) {
  struct stat st {};
  if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    return false;  // refuse to delete a symlink/non-directory
  }
  return RemoveTreeWalk(dir, 0, nullptr);
}

std::vector<PackageInfo> Manager::ScanZone(const std::string& zone_dir,
                                           bool high) const {
  std::vector<PackageInfo> out;
  DIR* d = opendir(zone_dir.c_str());
  if (d == nullptr) return out;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string sub = zone_dir + "/" + e->d_name;
    struct stat st {};
    if (lstat(sub.c_str(), &st) != 0) continue;
    if (S_ISLNK(st.st_mode)) {
      ++symlinks_skipped_;  // a symlink is never a package, let alone recursed
      continue;
    }
    if (!S_ISDIR(st.st_mode)) continue;
    PackageInfo p;
    p.dir = sub;
    p.high = high;
    p.incomplete = EndsWith(sub, ".incomplete");
    p.mtime_ns = MtimeNs(sub);
    p.bytes = DirBytesWalk(sub, 1, &symlinks_skipped_);
    out.push_back(p);
  }
  closedir(d);
  std::sort(out.begin(), out.end(),
            [](const PackageInfo& a, const PackageInfo& b) {
              return a.mtime_ns < b.mtime_ns;  // oldest first
            });
  return out;
}

std::vector<PackageInfo> Manager::Scan() const {
  std::vector<PackageInfo> out;
  const std::string high_path = cfg_.out_dir + "/" + cfg_.high_dir;
  for (PackageInfo& p : ScanZone(cfg_.out_dir, false)) {
    if (p.dir == high_path) continue;  // the zone dir itself is not a package
    out.push_back(p);
  }
  std::vector<PackageInfo> hi = ScanZone(high_path, true);
  out.insert(out.end(), hi.begin(), hi.end());
  return out;
}

uint32_t Manager::MarkIncomplete() {
  if (!ok_) return 0;
  uint32_t marked = 0;
  for (bool high : {false, true}) {
    const std::string zone =
        high ? cfg_.out_dir + "/" + cfg_.high_dir : cfg_.out_dir;
    DIR* d = opendir(zone.c_str());
    if (d == nullptr) continue;
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      const std::string sub = zone + "/" + e->d_name;
      struct stat st {};
      if (lstat(sub.c_str(), &st) != 0) continue;
      if (S_ISLNK(st.st_mode)) {
        ++symlinks_skipped_;  // do not rename/follow a symlink
        continue;
      }
      if (!S_ISDIR(st.st_mode)) continue;
      if (EndsWith(sub, ".incomplete")) continue;
      if (!high && e->d_name == cfg_.high_dir) continue;  // the zone itself
      if (!triggered::TriggeredRecorder::PackageComplete(sub)) {
        // A valid non-terminal WAL gets one durable, explicit terminal fact.
        // Malformed/legacy-unwritable journals still follow the existing
        // rename + best-effort partial-manifest recovery policy.
        if (!triggered::AbortPackageAfterCrash(sub)) {
          THYME_LOG_WARN("retention: could not append crash abort for %s",
                         sub.c_str());
        }
        const std::string renamed = sub + ".incomplete";
        if (::rename(sub.c_str(), renamed.c_str()) == 0) {
          ++marked;
          WriteRecoveryManifest(renamed, e->d_name, cfg_.signing_key_path,
                                cfg_.manifest_policy);
        }
      }
    }
    closedir(d);
  }
  return marked;
}

uint64_t Manager::Enforce(const std::vector<std::string>& exclude) {
  if (!ok_) return 0;
  uint64_t freed = 0;
  auto excluded = [&exclude](const std::string& dir) {
    for (const std::string& e : exclude) {
      if (dir == e) return true;
    }
    return false;
  };

  // .incomplete packages are handled only by the policy (the quota sweep never
  // deletes them).
  if (cfg_.incomplete_policy != IncompletePolicy::kKeep) {
    const int64_t now = RealtimeNs();
    for (const PackageInfo& p : Scan()) {
      if (!p.incomplete || excluded(p.dir)) continue;
      const bool expired =
          cfg_.incomplete_policy == IncompletePolicy::kDeleteImmediately ||
          (now > p.mtime_ns &&
           static_cast<uint64_t>(now - p.mtime_ns) >
               cfg_.incomplete_max_age_ns);
      if (expired && RemoveTreeWalk(p.dir, 1, &symlinks_skipped_)) {
        freed += p.bytes;
      }
    }
  }

  // Enforced at the API layer: a directory that is not sealed and not marked
  // .incomplete is an in-flight package (or corrupt) and is never deleted,
  // regardless of the caller's exclude list.
  auto deletable = [this, &excluded](const PackageInfo& p) {
    if (p.incomplete) return false;  // handled exclusively by incomplete_policy
    if (excluded(p.dir)) return false;  // skip active packages/slices
    if (!triggered::TriggeredRecorder::PackageComplete(p.dir)) {
      THYME_LOG_WARN_RL("retention: skip unsealed package %s (in-flight?)",
                        p.dir.c_str());
      return false;
    }
    return true;
  };

  // Total quota: continuous slices go first (6.6: triggered packages are kept
  // in preference), then the oldest triggered packages; high/ is never touched
  // by this sweep.
  if (cfg_.disk_quota_bytes > 0) {
    const std::string high_path = cfg_.out_dir + "/" + cfg_.high_dir;
    std::vector<PackageInfo> low = ScanZone(cfg_.out_dir, false);
    uint64_t total = 0;
    for (const PackageInfo& p : Scan()) total += p.bytes;
    auto sweep = [&](bool continuous_only) {
      for (const PackageInfo& p : low) {
        if (total <= cfg_.disk_quota_bytes) return;
        if (p.dir == high_path) continue;  // high/ is not intruded on
        const std::string leaf = p.dir.substr(cfg_.out_dir.size() + 1);
        const bool is_cont = leaf.compare(0, 11, "continuous_") == 0;
        if (is_cont != continuous_only) continue;
        if (!deletable(p)) continue;
        if (RemoveTreeWalk(p.dir, 1, &symlinks_skipped_)) {
          total -= p.bytes;
          freed += p.bytes;
        }
      }
    };
    sweep(true);   // continuous slices, oldest deleted first
    sweep(false);  // triggered packages, oldest deleted first
  }

  // high/ over its own reservation: delete oldest crash packages inside.
  if (cfg_.high_reserved_bytes > 0) {
    std::vector<PackageInfo> high =
        ScanZone(cfg_.out_dir + "/" + cfg_.high_dir, true);
    uint64_t high_total = 0;
    for (const PackageInfo& p : high) high_total += p.bytes;
    for (const PackageInfo& p : high) {
      if (high_total <= cfg_.high_reserved_bytes) break;
      if (!deletable(p)) continue;
      if (RemoveTreeWalk(p.dir, 1, &symlinks_skipped_)) {
        high_total -= p.bytes;
        freed += p.bytes;
      }
    }
  }
  return freed;
}

}  // namespace retention
}  // namespace thyme
