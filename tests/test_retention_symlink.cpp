// test_retention_symlink.cpp — retention hardening (roadmap 7.3):
//   * symlink refusal (lstat): symlinks inside the capture directory are never followed or recursed into,
//     and deletion only unlinks the link itself — never outside the capture directory;
//   * recursion depth limit kMaxTreeDepth: exceeding it is treated as corruption/attack and RemoveTree refuses;
//   * active-package exclusion enforced at the API layer: an unsealed package is never deleted even if not in exclude;
//   * .incomplete policy matrix: keep (default) / delete_after_age /
//     delete_immediately.
#include "thyme/check.h"
#include "thyme/record/retention.h"
#include "thyme/record/triggered_recorder.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

int64_t RealtimeNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL +
         static_cast<int64_t>(ts.tv_nsec);
}

bool PathExists(const std::string& p) {  // follows symlinks
  struct stat st {};
  return stat(p.c_str(), &st) == 0;
}

bool PathExistsL(const std::string& p) {  // lstat: the link itself
  struct stat st {};
  return lstat(p.c_str(), &st) == 0;
}

void Mkdir(const std::string& p) { CHECK(mkdir(p.c_str(), 0700) == 0); }

void WriteFile(const std::string& p, uint64_t bytes) {
  FILE* f = std::fopen(p.c_str(), "wb");
  CHECK(f != nullptr);
  std::vector<uint8_t> buf(bytes, 0xAB);
  if (bytes > 0) CHECK(std::fwrite(buf.data(), 1, bytes, f) == bytes);
  CHECK(std::fclose(f) == 0);
}

void TouchMtime(const std::string& p, int64_t mtime_ns) {
  struct timespec ts[2];
  ts[0].tv_sec = static_cast<time_t>(mtime_ns / 1000000000LL);
  ts[0].tv_nsec = static_cast<long>(mtime_ns % 1000000000LL);
  ts[1] = ts[0];
  CHECK(utimensat(AT_FDCWD, p.c_str(), ts, 0) == 0);
}

// Fake package: dir + data.bin(payload_bytes) + wal.log (sealed or not).
// Directory mtime is pinned AFTER file creation.
void MakePkg(const std::string& dir, uint64_t payload_bytes, int64_t mtime_ns,
             bool sealed) {
  Mkdir(dir);
  WriteFile(dir + "/data.bin", payload_bytes);
  FILE* f = std::fopen((dir + "/wal.log").c_str(), "wb");
  CHECK(f != nullptr);
  const triggered::WalHeader wh{{'T', 'R', 'I', 'G', 'W', 'A', 'L', '1'}, 1};
  CHECK(std::fwrite(&wh, sizeof(wh), 1, f) == 1);
  if (sealed) {
    const triggered::WalRow seal{triggered::kWalOpSeal, 0, 0, 0, 0, 0};
    CHECK(std::fwrite(&seal, sizeof(seal), 1, f) == 1);
  }
  CHECK(std::fclose(f) == 0);
  TouchMtime(dir, mtime_ns);
}

}  // namespace

int main() {
  const int me = static_cast<int>(getpid());
  char root[96];
  std::snprintf(root, sizeof(root), "/tmp/c9retsym_%d", me);

  // ---- test_retention_symlink: symlink pointing outside the capture directory, never followed/deleted ----
  {
    const std::string out = std::string(root) + "/symlink";
    const std::string outside = std::string(root) + "/outside";
    Mkdir(root);
    Mkdir(out);
    Mkdir(outside);
    WriteFile(outside + "/sentinel.bin", 4096);  // sentinel outside the capture directory

    // symlink inside the package -> external directory; the package itself is sealed, so it is a legitimate deletion candidate.
    MakePkg(out + "/1_100", 1000, 100000000000LL, true);
    CHECK(symlink(outside.c_str(), (out + "/1_100/escape").c_str()) == 0);
    TouchMtime(out + "/1_100", 100000000000LL);  // creating the symlink touches the dir mtime
    // top-level zone symlink -> external directory: never counted as a package.
    CHECK(symlink(outside.c_str(), (out + "/evil_link").c_str()) == 0);
    MakePkg(out + "/2_200", 1000, 200000000000LL, true);

    retention::Manager rm;
    retention::Config rcfg;
    rcfg.out_dir = out;
    CHECK(rm.Init(rcfg));
    // DirBytes does not count size through symlinks.
    CHECK(rm.DirBytes(out + "/1_100") < 4096);
    const std::vector<retention::PackageInfo> pkgs = rm.Scan();
    CHECK(pkgs.size() == 2);  // evil_link is not a package
    // quota forces deletion of the oldest 1_100 (contains the escape symlink).
    rcfg.disk_quota_bytes = 1500;  // ~2080B total -> after deleting 1_100 ~1040B is under quota
    CHECK(rm.Init(rcfg));
    CHECK(rm.Enforce({}) >= 1000);
    CHECK(!PathExistsL(out + "/1_100"));     // package deleted
    CHECK(PathExists(outside + "/sentinel.bin"));  // sentinel outside the directory intact
    CHECK(PathExistsL(out + "/evil_link"));  // top-level symlink not deleted
    CHECK(PathExists(out + "/2_200"));
    CHECK(rm.symlinks_skipped() >= 2);  // escape + evil_link counted
    retention::Manager::RemoveTree(out);
    retention::Manager::RemoveTree(outside);
  }

  // ---- recursion depth limit: over-limit deletion refused, shallow packages unaffected ----
  {
    const std::string out = std::string(root) + "/depth";
    Mkdir(out);
    MakePkg(out + "/deep_1", 100, 100000000000LL, true);
    // Build a nesting chain deeper than kMaxTreeDepth inside the package, with a sentinel at the bottom.
    std::string deep = out + "/deep_1";
    for (uint32_t i = 0; i < retention::kMaxTreeDepth + 2; ++i) {
      deep += "/a";
      Mkdir(deep);
    }
    WriteFile(deep + "/deep_sentinel.bin", 16);
    MakePkg(out + "/flat_2", 100, 200000000000LL, true);

    retention::Manager rm;
    retention::Config rcfg;
    rcfg.out_dir = out;
    CHECK(rm.Init(rcfg));
    // A direct RemoveTree is refused (returns false, tree preserved).
    CHECK(!retention::Manager::RemoveTree(out + "/deep_1"));
    CHECK(PathExists(deep + "/deep_sentinel.bin"));
    // Under quota pressure: deep_1 cannot be deleted (refused), flat_2 is deleted normally.
    rcfg.disk_quota_bytes = 1;
    CHECK(rm.Init(rcfg));
    rm.Enforce({});
    CHECK(PathExistsL(out + "/deep_1"));
    CHECK(PathExists(deep + "/deep_sentinel.bin"));
    CHECK(!PathExists(out + "/flat_2"));
    // Clean up the deep chain by hand; RemoveTree can then delete the shallow tree normally.
    std::remove((deep + "/deep_sentinel.bin").c_str());
    std::string cur = deep;
    while (cur != out + "/deep_1") {
      CHECK(rmdir(cur.c_str()) == 0);
      cur = cur.substr(0, cur.find_last_of('/'));
    }
    CHECK(retention::Manager::RemoveTree(out + "/deep_1"));
    retention::Manager::RemoveTree(out);
  }

  // ---- active-package exclusion enforced at the API layer: an unsealed package is never deleted even if not in exclude ----
  {
    const std::string out = std::string(root) + "/active";
    Mkdir(out);
    MakePkg(out + "/1_100", 1000, 100000000000LL, true);   // sealed
    MakePkg(out + "/2_200", 1000, 200000000000LL, false);  // being written (unsealed)
    retention::Manager rm;
    retention::Config rcfg;
    rcfg.out_dir = out;
    rcfg.disk_quota_bytes = 1;  // quota pressure maxed out
    CHECK(rm.Init(rcfg));
    rm.Enforce({});  // empty caller active set: the sealed re-check must backstop
    CHECK(!PathExists(out + "/1_100"));   // sealed old package deleted
    CHECK(PathExists(out + "/2_200"));    // in-flight package forcibly kept at the API layer
    // the exclude list still works: a sealed package is skipped when excluded.
    MakePkg(out + "/3_300", 1000, 300000000000LL, true);
    rm.Enforce({out + "/3_300"});
    CHECK(PathExists(out + "/3_300"));
    retention::Manager::RemoveTree(out);
  }

  // ---- .incomplete policy matrix ----
  {
    const std::string out = std::string(root) + "/incomplete";
    Mkdir(out);
    const int64_t now = RealtimeNs();
    // kKeep (default): under quota pressure the incomplete package is kept and the sealed package is deleted.
    MakePkg(out + "/old_1.incomplete", 1000, 100000000000LL, false);
    MakePkg(out + "/sealed_2", 1000, 200000000000LL, true);
    {
      retention::Manager rm;
      retention::Config rcfg;
      rcfg.out_dir = out;
      rcfg.disk_quota_bytes = 1;
      CHECK(rm.Init(rcfg));
      rm.Enforce({});
      CHECK(PathExists(out + "/old_1.incomplete"));  // kept by default
      CHECK(!PathExists(out + "/sealed_2"));
    }
    // kDeleteAfterAge: delete when over age, keep fresh incomplete packages (applies without quota pressure too).
    MakePkg(out + "/young_3.incomplete", 1000, now, false);
    {
      retention::Manager rm;
      retention::Config rcfg;
      rcfg.out_dir = out;
      rcfg.incomplete_policy = retention::IncompletePolicy::kDeleteAfterAge;
      rcfg.incomplete_max_age_ns = 3600000000000ull;  // 1h
      CHECK(rm.Init(rcfg));
      const uint64_t freed = rm.Enforce({});
      CHECK(freed >= 1000);
      CHECK(!PathExists(out + "/old_1.incomplete"));    // over-age deleted
      CHECK(PathExists(out + "/young_3.incomplete"));   // fresh incomplete package kept
    }
    // kDeleteImmediately: clear immediately even without quota pressure.
    {
      retention::Manager rm;
      retention::Config rcfg;
      rcfg.out_dir = out;
      rcfg.incomplete_policy =
          retention::IncompletePolicy::kDeleteImmediately;
      CHECK(rm.Init(rcfg));
      CHECK(rm.Enforce({}) >= 1000);
      CHECK(!PathExists(out + "/young_3.incomplete"));
    }
    retention::Manager::RemoveTree(out);
  }

  CHECK(rmdir(root) == 0);
  return 0;
}
