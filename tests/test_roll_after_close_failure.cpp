// test_roll_after_close_failure.cpp — W2: when CloseSlice fails (seal-path
// fault), RollSlice must NOT open a new slice: no orphan directory, the
// failed slice is invalidated (.incomplete + abort row), continuous mode
// stops, and a re-arm after ClearDiskFull works again.
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace thyme;

namespace {

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}

int g_seal_fault = 0;
int SealFaultInject(void*) { return g_seal_fault; }

int CountSliceDirs(const std::string& out) {
  int n = 0;
  DIR* d = opendir(out.c_str());
  if (d == nullptr) return -1;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    if (e->d_type == DT_DIR &&
        std::strncmp(e->d_name, "continuous_", 11) == 0) {
      ++n;
    }
  }
  closedir(d);
  return n;
}

void TickFor(triggered::TriggeredRecorder& rec, uint64_t ns) {
  const uint64_t end = NowNs() + ns;
  while (NowNs() < end) {
    rec.Tick();
    SleepNs(200000);
  }
}

}  // namespace

int main() {
  const int me = static_cast<int>(getpid());
  char page_name[96], out_dir[96];
  std::snprintf(page_name, sizeof(page_name), "/c9roll_page_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9roll_%d", me);
  ShmRegion::UnlinkName(page_name);

  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 64;
  std::snprintf(desc.name, sizeof(desc.name), "Pod64");
  RecordMirrorRingSpec spec{64, 256};
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page_name, &desc, &spec, 1, 1));

  triggered::Config ecfg;
  ecfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name,
                   ""}};
  ecfg.out_dir = out_dir;
  ecfg.fault_inject_seal = SealFaultInject;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(ecfg));
  triggered::ContinuousConfig ccfg;
  ccfg.slice_bytes = 1000;  // ~10 samples/roll
  CHECK(rec.ArmContinuous(ccfg));

  // Drive one byte-triggered roll; the seal fault makes CloseSlice fail.
  g_seal_fault = 2;
  for (uint64_t i = 0; i < 25; ++i) {
    const uint64_t v = i;
    CHECK(mirror.Page()->TryWriteRing(0, &v, sizeof(v), NowNs(),
                                      static_cast<uint32_t>(i)));
    rec.Tick();
    SleepNs(200000);
  }
  TickFor(rec, 10000000ULL);
  CHECK(!rec.continuous_armed());       // continuous capture stopped
  CHECK(rec.disk_full());
  CHECK(rec.stats().disk_errors >= 1);
  // no orphan directories: only the invalidated continuous_0_*.incomplete, no continuous_1_*.
  CHECK(CountSliceDirs(out_dir) == 1);
  struct stat st {};
  DIR* d = opendir(out_dir);
  CHECK(d != nullptr);
  bool saw_incomplete = false;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    if (std::strstr(e->d_name, ".incomplete") != nullptr) saw_incomplete = true;
  }
  closedir(d);
  CHECK(saw_incomplete);
  CHECK(stat((std::string(out_dir) + "/continuous_1_0").c_str(), &st) != 0);

  // Recovery: clear fault + disk_full, re-arm, and roll/seal normally.
  g_seal_fault = 0;
  rec.ClearDiskFull();
  CHECK(rec.ArmContinuous(ccfg));
  for (uint64_t i = 25; i < 60; ++i) {
    const uint64_t v = i;
    CHECK(mirror.Page()->TryWriteRing(0, &v, sizeof(v), NowNs(),
                                      static_cast<uint32_t>(i)));
    rec.Tick();
    SleepNs(200000);
  }
  TickFor(rec, 10000000ULL);
  rec.DisarmContinuous();
  CHECK(rec.continuous_stats().slices_done >= 1);
  // Directories sorted by seq; the wal of every new slice is sealed.
  d = opendir(out_dir);
  CHECK(d != nullptr);
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    if (e->d_type != DT_DIR) continue;
    if (std::strncmp(e->d_name, "continuous_", 11) != 0) continue;
    if (std::strstr(e->d_name, ".incomplete") != nullptr) continue;
    CHECK(triggered::TriggeredRecorder::PackageComplete(
        std::string(out_dir) + "/" + e->d_name));
  }
  closedir(d);

  mirror.Shutdown(true);
  rec.Shutdown();
  return 0;
}
