// test_enospc_fs.cpp — REC-14 real small-filesystem ENOSPC (not a write hook).
//
// The test is launched under `unshare --map-root-user --mount` so it can
// mount a size-capped tmpfs without host root. That tmpfs is a real kernel
// filesystem: fwrite/ENOSPC come from the VFS, not Config::fault_inject_write.
// A loop-backed ext4 image needs /dev/loop-control (not available inside a
// user namespace); tmpfs `size=` is the unprivileged equivalent of a tiny
// loop device and produces the same errno 28.
#include "thyme/check.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace thyme;

namespace {

constexpr uint32_t kSlotBytes = 64u * 1024u;
constexpr uint32_t kSlotCount = 16;
constexpr uint32_t kFrameCount = 24;  // 1.5 MiB of payload > 768 KiB fs

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

bool PathExists(const std::string& path) {
  return ::access(path.c_str(), F_OK) == 0;
}

}  // namespace

int main() {
  if (geteuid() != 0) {
    std::fprintf(stderr,
                 "test_enospc_fs: skip (not root in a mount namespace; "
                 "run via unshare --map-root-user --mount)\n");
    return 77;
  }

  const int me = static_cast<int>(getpid());
  char mnt[96];
  char chan[96];
  std::snprintf(mnt, sizeof(mnt), "/tmp/thyme_enospc_%d", me);
  std::snprintf(chan, sizeof(chan), "/thyme_enospc_%d", me);
  ::rmdir(mnt);
  CHECK(::mkdir(mnt, 0700) == 0);
  if (::mount("tmpfs", mnt, "tmpfs", 0, "size=768k") != 0) {
    std::fprintf(stderr, "test_enospc_fs: skip (mount tmpfs: %s)\n",
                 std::strerror(errno));
    ::rmdir(mnt);
    return 77;
  }

  ShmRegion::UnlinkName(chan);
  PinnedSlotWriter w;
  CHECK(w.Create(chan, kSlotBytes, kSlotCount, 0));
  for (uint32_t f = 0; f < kFrameCount; ++f) {
    PinnedSlotWriter::BeginView bv = w.Begin(kSlotBytes);
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, static_cast<int>(0xA0 + (f % 16)), kSlotBytes);
    w.Commit(NowNs());
    SleepNs(50000);
  }

  triggered::Config cfg;
  triggered::SourceConfig sc;
  sc.kind = triggered::SourceConfig::Kind::kPinnedSlot;
  sc.shm_name = chan;
  sc.channel_name = "cloud_1";
  cfg.sources = {sc};
  cfg.out_dir = mnt;
  cfg.post_grace_ns = 2000000ULL;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));
  CHECK(rec.Trigger(1, 60000000000ULL, 0));
  const uint64_t deadline = NowNs() + 15000000000ULL;
  while (rec.phase() != triggered::Phase::kArmed) {
    rec.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(100000);
  }

  CHECK(rec.disk_full());
  CHECK(rec.stats().disk_errors >= 1);
  CHECK(!rec.Trigger(2, 60000000000ULL, 0));
  const std::string pkg = rec.last_package_dir();
  CHECK(!triggered::TriggeredRecorder::PackageComplete(pkg));
  CHECK(PathExists(pkg + ".incomplete") || PathExists(pkg));
  if (PathExists(pkg + ".incomplete")) {
    CHECK(!triggered::TriggeredRecorder::PackageComplete(pkg + ".incomplete"));
  }

  rec.Shutdown();
  w.Close();
  ShmRegion::UnlinkName(chan);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(chan).c_str());
  ::umount(mnt);
  ::rmdir(mnt);
  std::printf("test_enospc_fs: OK (real ENOSPC on size-capped tmpfs)\n");
  return 0;
}
