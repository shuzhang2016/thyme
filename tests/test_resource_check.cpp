// test_resource_check.cpp — OPS-07: the startup resource self-check.
//
// Pure library test (getrlimit/statvfs are read-only, no threads, no files
// created). Covers the severity policy, the environment knobs, the
// "unverifiable" paths, the bounded renderings, and the shared
// THYME_NO_MLOCK predicate.

#include "thyme/check.h"
#include "thyme/runtime/resource_check.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace thyme;
using resource::Level;
using resource::Limits;
using resource::Report;

namespace {

void ClearResourceEnv() {
  unsetenv("THYME_RESOURCE_CHECK");
  unsetenv("THYME_RESOURCE_BASELINE_FILES");
  unsetenv("THYME_RESOURCE_FILES_PER_CHANNEL");
  unsetenv("THYME_RESOURCE_SHM_PLANNED_MB");
  unsetenv("THYME_RESOURCE_SHM_RESERVE_MB");
  unsetenv("THYME_RESOURCE_MEMLOCK_HEADROOM_MB");
  unsetenv("THYME_RESOURCE_DISK_RESERVE_MB");
  unsetenv("THYME_NO_MLOCK");
}

uint64_t SoftNofile() {
  struct rlimit rl {};
  if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return 0;
  if (rl.rlim_cur == RLIM_INFINITY) return UINT64_MAX;
  return static_cast<uint64_t>(rl.rlim_cur);
}

void TestLevels() {
  CHECK(std::strcmp(resource::LevelName(Level::kOk), "ok") == 0);
  CHECK(std::strcmp(resource::LevelName(Level::kWarn), "warn") == 0);
  CHECK(std::strcmp(resource::LevelName(Level::kFatal), "fatal") == 0);
  CHECK(resource::Worst(Level::kOk, Level::kWarn) == Level::kWarn);
  CHECK(resource::Worst(Level::kWarn, Level::kOk) == Level::kWarn);
  CHECK(resource::Worst(Level::kWarn, Level::kFatal) == Level::kFatal);
  CHECK(resource::Worst(Level::kFatal, Level::kFatal) == Level::kFatal);
}

void TestMlockOptOut() {
  CHECK(!resource::MlockOptOut(nullptr));
  CHECK(resource::MlockOptOut("1"));
  CHECK(resource::MlockOptOut("true"));
  CHECK(resource::MlockOptOut("TRUE"));
  CHECK(resource::MlockOptOut("yes"));
  CHECK(resource::MlockOptOut("On"));
  CHECK(!resource::MlockOptOut("0"));
  CHECK(!resource::MlockOptOut("false"));
  CHECK(!resource::MlockOptOut(""));
  CHECK(!resource::MlockOptOut("off"));

  ClearResourceEnv();
  CHECK(resource::MlockConfiguredFromEnv());
  CHECK(setenv("THYME_NO_MLOCK", "1", 1) == 0);
  CHECK(!resource::MlockConfiguredFromEnv());
  CHECK(setenv("THYME_NO_MLOCK", "0", 1) == 0);
  CHECK(resource::MlockConfiguredFromEnv());
  ClearResourceEnv();
}

void TestPlanFromEnv() {
  ClearResourceEnv();
  const resource::Plan def =
      resource::PlanFromEnv(3, "/tmp", 100ULL * 1024 * 1024, 0);
  CHECK(def.enabled);
  CHECK(def.enforce);
  CHECK(def.limits.channels == 3);
  CHECK(def.limits.files_per_channel == 2);
  CHECK(def.limits.baseline_files == 64);
  CHECK(def.limits.mlock_configured);
  // Default plan: 8 MiB per channel, + 64 MiB mlock headroom.
  CHECK(def.limits.shm_planned_bytes == 24ULL * 1024 * 1024);
  CHECK(def.limits.shm_reserve_bytes == 16ULL * 1024 * 1024);
  CHECK(def.limits.memlock_required_bytes ==
        24ULL * 1024 * 1024 + 64ULL * 1024 * 1024);
  CHECK(def.limits.disk_path != nullptr);
  CHECK(std::strcmp(def.limits.disk_path, "/tmp") == 0);
  CHECK(def.limits.disk_quota_bytes == 100ULL * 1024 * 1024);
  CHECK(def.limits.disk_reserve_bytes == 0);

  CHECK(setenv("THYME_RESOURCE_FILES_PER_CHANNEL", "7", 1) == 0);
  CHECK(setenv("THYME_RESOURCE_BASELINE_FILES", "10", 1) == 0);
  CHECK(setenv("THYME_RESOURCE_SHM_PLANNED_MB", "100", 1) == 0);
  CHECK(setenv("THYME_RESOURCE_SHM_RESERVE_MB", "2", 1) == 0);
  CHECK(setenv("THYME_RESOURCE_MEMLOCK_HEADROOM_MB", "8", 1) == 0);
  CHECK(setenv("THYME_RESOURCE_DISK_RESERVE_MB", "1", 1) == 0);
  const resource::Plan tuned =
      resource::PlanFromEnv(2, "/tmp", 0, 0);
  CHECK(tuned.limits.files_per_channel == 7);
  CHECK(tuned.limits.baseline_files == 10);
  CHECK(tuned.limits.shm_planned_bytes == 100ULL * 1024 * 1024);
  CHECK(tuned.limits.shm_reserve_bytes == 2ULL * 1024 * 1024);
  CHECK(tuned.limits.memlock_required_bytes ==
        108ULL * 1024 * 1024);
  CHECK(tuned.limits.disk_reserve_bytes == 1ULL * 1024 * 1024);

  // Invalid numbers fall back instead of becoming zero/garbage.
  CHECK(setenv("THYME_RESOURCE_BASELINE_FILES", "12x", 1) == 0);
  CHECK(resource::PlanFromEnv(1, nullptr, 0, 0).limits.baseline_files == 64);

  CHECK(setenv("THYME_RESOURCE_CHECK", "warn", 1) == 0);
  const resource::Plan warn = resource::PlanFromEnv(1, nullptr, 0, 0);
  CHECK(warn.enabled);
  CHECK(!warn.enforce);

  CHECK(setenv("THYME_RESOURCE_CHECK", "off", 1) == 0);
  CHECK(!resource::PlanFromEnv(1, nullptr, 0, 0).enabled);

  ClearResourceEnv();
  CHECK(setenv("THYME_NO_MLOCK", "yes", 1) == 0);
  CHECK(!resource::PlanFromEnv(1, nullptr, 0, 0).limits.mlock_configured);
  ClearResourceEnv();
}

void TestNofile() {
  Limits limits;
  // An impossible plan: channels x files_per_channel dwarfs any real soft
  // limit. A host with RLIM_INFINITY can never be fatal by definition.
  limits.channels = 0xFFFFFFFFu;
  limits.files_per_channel = 0xFFFFFFFFu;
  limits.baseline_files = 1;
  const Report r = resource::Check(limits);
  CHECK(r.nofile_checked);
  CHECK(r.nofile_required > 1);
  if (SoftNofile() != UINT64_MAX && SoftNofile() < r.nofile_required) {
    CHECK(r.nofile == Level::kFatal);
    CHECK(r.fatal);
    CHECK(r.worst() == Level::kFatal);
  } else {
    CHECK(r.nofile == Level::kOk);
  }

  // A small plan passes on any normal host.
  Limits small;
  small.channels = 1;
  small.files_per_channel = 2;
  small.baseline_files = 8;
  const Report ok = resource::Check(small);
  CHECK(ok.nofile_required == 10);
  CHECK(ok.nofile != Level::kFatal);
}

void TestMemlock() {
  Limits off;
  off.mlock_configured = false;
  off.memlock_required_bytes = UINT64_MAX;
  const Report off_r = resource::Check(off);
  CHECK(!off_r.memlock_configured);
  CHECK(off_r.memlock == Level::kOk);
  CHECK(!off_r.memlock_checked);

  Limits on;
  on.mlock_configured = true;
  on.memlock_required_bytes = UINT64_MAX;
  const Report on_r = resource::Check(on);
  CHECK(on_r.memlock_configured);
  CHECK(on_r.memlock_checked);
  // mlockall failure is non-fatal by policy: warn unless the limit is
  // unlimited (then the requirement fits).
  if (on_r.memlock_soft == UINT64_MAX) {
    CHECK(on_r.memlock == Level::kOk);
  } else {
    CHECK(on_r.memlock == Level::kWarn);
    CHECK(!on_r.fatal);
    CHECK(on_r.warnings >= 1);
  }
}

void TestDiskAndShm() {
  char dir[] = "/tmp/thyme_rescheck_XXXXXX";
  CHECK(mkdtemp(dir) != nullptr);

  Limits disk;
  disk.disk_path = dir;
  disk.disk_quota_bytes = UINT64_MAX;  // free < quota -> warn
  const Report disk_warn = resource::Check(disk);
  CHECK(disk_warn.disk_checked);
  CHECK(disk_warn.disk == Level::kWarn);
  CHECK(!disk_warn.fatal);

  Limits disk_fatal;
  disk_fatal.disk_path = dir;
  disk_fatal.disk_reserve_bytes = UINT64_MAX;  // impossible reserve
  const Report df = resource::Check(disk_fatal);
  CHECK(df.disk_checked);
  CHECK(df.disk == Level::kFatal);
  CHECK(df.fatal);

  // Zero quota and zero reserve: not configured, so not checked.
  Limits disk_off;
  disk_off.disk_path = dir;
  const Report doff = resource::Check(disk_off);
  CHECK(!doff.disk_checked);
  CHECK(doff.disk == Level::kOk);

  // Unreadable path with a configured quota: unverifiable -> warn.
  Limits disk_missing;
  disk_missing.disk_path = "/nonexistent/thyme/rescheck";
  disk_missing.disk_quota_bytes = 1;
  const Report dm = resource::Check(disk_missing);
  CHECK(!dm.disk_checked);
  CHECK(dm.disk == Level::kWarn);

  Limits shm;
  shm.shm_path = dir;
  shm.shm_planned_bytes = UINT64_MAX;
  const Report shm_fatal = resource::Check(shm);
  CHECK(shm_fatal.shm_checked);
  CHECK(shm_fatal.shm == Level::kFatal);
  CHECK(shm_fatal.fatal);

  Limits shm_warn;
  shm_warn.shm_path = dir;
  shm_warn.shm_planned_bytes = 1;
  shm_warn.shm_reserve_bytes = UINT64_MAX;  // plan fits, headroom does not
  const Report sw = resource::Check(shm_warn);
  CHECK(sw.shm_checked);
  CHECK(sw.shm == Level::kWarn);

  Limits shm_off;
  shm_off.shm_path = dir;
  const Report soff = resource::Check(shm_off);
  CHECK(!soff.shm_checked);
  CHECK(soff.shm == Level::kOk);

  CHECK(rmdir(dir) == 0);
}

void TestRendering() {
  Limits limits;
  limits.channels = 4;
  limits.files_per_channel = 2;
  limits.baseline_files = 8;
  limits.disk_path = "/tmp";
  limits.disk_quota_bytes = UINT64_MAX;
  limits.shm_path = "/dev/shm";
  limits.shm_planned_bytes = 8ULL * 1024 * 1024;
  limits.shm_reserve_bytes = 1ULL * 1024 * 1024;
  const Report r = resource::Check(limits);
  CHECK(!r.fatal);

  char line[512];
  CHECK(resource::FormatSummary(r, line, sizeof(line)));
  CHECK(std::strstr(line, "resources:") != nullptr);
  CHECK(std::strstr(line, "nofile=") != nullptr);
  CHECK(std::strstr(line, "warnings=") != nullptr);

  char json[1024];
  CHECK(resource::FormatJson(r, json, sizeof(json)));
  CHECK(json[0] == '{');
  CHECK(json[std::strlen(json) - 1] == '}');
  CHECK(std::strstr(json, "\"checked\":true") != nullptr);
  CHECK(std::strstr(json, "\"fatal\":false") != nullptr);
  CHECK(std::strstr(json, "\"nofile\":{") != nullptr);
  CHECK(std::strstr(json, "\"shm\":{") != nullptr);
  CHECK(std::strstr(json, "\"disk\":{") != nullptr);

  // A too-small buffer truncates, reports failure, and stays terminated.
  char tiny[8];
  CHECK(!resource::FormatSummary(r, tiny, sizeof(tiny)));
  CHECK(std::strlen(tiny) < sizeof(tiny));
  CHECK(!resource::FormatJson(r, tiny, sizeof(tiny)));
  CHECK(std::strlen(tiny) < sizeof(tiny));
}

}  // namespace

int main() {
  TestLevels();
  TestMlockOptOut();
  TestPlanFromEnv();
  TestNofile();
  TestMemlock();
  TestDiskAndShm();
  TestRendering();
  std::printf("test_resource_check: OK\n");
  return 0;
}
