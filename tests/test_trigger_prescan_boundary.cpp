// test_trigger_prescan_boundary.cpp — P2-a regression: a ring sample tapped
// between the trigger's t0 stamp and the post-cursor snapshot must be
// packaged EXACTLY once. Pre/post harvest partition strictly by
// publish_index (pre: [.., post_next), post: [post_next, ..)); the time
// filter only excludes mono > t0 + after_ns. Before the fix the pre harvest
// filtered at t0 and post_next was snapped after HarvestPre, so such a
// sample fell into the gap and was lost. The Config::test_hook_after_t0 hook
// injects the boundary sample deterministically.
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
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
#include <vector>

using namespace thyme;

namespace {

constexpr uint64_t kTag = 0xB0DA21ULL;  // tag value of the boundary sample

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

void RmTree(const std::string& path) {
  DIR* d = opendir(path.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string sub = path + "/" + e->d_name;
    if (e->d_type == DT_DIR) RmTree(sub);
    else std::remove(sub.c_str());
  }
  closedir(d);
  rmdir(path.c_str());
}

std::vector<ThymeBinDataEntry> ReadAll(const std::string& path) {
  ThymeBinReader rd(path.c_str());
  CHECK(rd.ok());
  std::vector<ThymeBinDataEntry> out;
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
    out.push_back(e);
  }
  return out;
}

struct HookContext {
  RecordMirrorRingPage* page;
  uint64_t t0;
  uint64_t now_ns = 0;
  uint64_t mono_ns = 0;
  uint64_t index = 0;
  uint64_t head = 0;
  uint32_t slot_seq = 0;
};

// injection hook: tap one boundary sample after the t0 stamp and before the post-cursor snapshot.
void HookTap(void* ctx) {
  auto* hook = static_cast<HookContext*>(ctx);
  uint64_t v = kTag;
  hook->now_ns = NowNs();
  // Trigger() stamps t0 before its package I/O. A fresh clock reading here can
  // be beyond t0 + after_ns under load, which tests window rejection instead
  // of the pre/post handoff. Keep the injected stamp tied to the trigger's t0.
  hook->mono_ns = hook->t0 + 1000;
  hook->index = hook->page->RingHead(0);
  CHECK(hook->page->TryWriteRing(0, &v, sizeof(v), hook->mono_ns, 0xBEEF));
  hook->head = hook->page->RingHead(0);
  RecordMirrorRingSampleDesc desc;
  CHECK(hook->page->PeekRingSample(0, hook->index, desc));
  hook->slot_seq = desc.slot_seq;
}

}  // namespace

int main() {
  const int me = static_cast<int>(getpid());
  char page_name[96], out[96];
  std::snprintf(page_name, sizeof(page_name), "/c9boundary_%d", me);
  std::snprintf(out, sizeof(out), "/tmp/c9boundary_%d", me);
  ShmRegion::UnlinkName(page_name);
  RmTree(out);

  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 16;
  std::snprintf(desc.name, sizeof(desc.name), "Pod16");
  RecordMirrorRingSpec spec{16, 256};
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page_name, &desc, &spec, 1, 9));

  HookContext hook{mirror.Page(), 0};
  triggered::Config cfg;
  cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""}};
  cfg.out_dir = out;
  cfg.reattach_scan_ns = 1000000;
  cfg.post_grace_ns = 50000000ULL;
  cfg.fault_ctx = &hook;
  cfg.test_hook_after_t0 = &HookTap;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));
  // Let ScanSources finish attaching.
  const uint64_t attach_end = NowNs() + 100000000ULL;
  while (NowNs() < attach_end) {
    rec.Tick();
    SleepNs(1000000);
  }

  // 20 background samples, all inside the pre window.
  for (uint64_t i = 0; i < 20; ++i) {
    CHECK(mirror.Page()->TryWriteRing(0, &i, sizeof(i), NowNs(),
                                      static_cast<uint32_t>(i)));
    SleepNs(200000);
  }

  constexpr uint64_t kBefore = 30000000ULL;
  constexpr uint64_t kAfter = 15000000ULL;
  const uint64_t t0 = NowNs();
  hook.t0 = t0;
  CHECK(rec.Trigger(1, kBefore, kAfter, t0));  // hook injects the 21st
  const uint64_t deadline = NowNs() + 15000000000ULL;
  while (rec.phase() != triggered::Phase::kArmed) {
    rec.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(100000);
  }
  CHECK(rec.stats().captures_done == 1);

  const std::string dir = rec.last_package_dir();
  CHECK(triggered::TriggeredRecorder::PackageComplete(dir));
  const std::vector<ThymeBinDataEntry> meta = ReadAll(dir + "/meta.tbin");
  CHECK(meta[0].mono_ns == t0);

  const std::vector<ThymeBinDataEntry> small = ReadAll(dir + "/small.tbin");
  uint64_t tagged = 0;
  uint64_t gaps = 0;
  for (const auto& e : small) {
    if (e.type == ThymeBinEntryType::kGap) {
      ++gaps;
      continue;
    }
    CHECK(e.type == ThymeBinEntryType::kData);
    CHECK(e.mono_ns >= t0 - kBefore && e.mono_ns <= t0 + kAfter);
    uint64_t v = 0;
    std::memcpy(&v, e.payload.data(), sizeof(v));
    if (v == kTag) ++tagged;
  }

  // independent check: harvest the same time window directly from the page; the count must match.
  RecordMirrorRingSampleDesc buf[256];
  uint64_t indep_torn = 0;
  const uint32_t indep = mirror.Page()->HarvestWindow(
      0, t0 - kBefore, t0 + kAfter, buf, 256, &indep_torn);
  std::fprintf(
      stderr,
      "prescan boundary: packaged=%zu tagged=%llu indep=%u gaps=%llu "
      "t0=%llu window_end=%llu hook_now=%llu hook_mono=%llu "
      "hook_index=%llu hook_head=%llu slot_seq=%u torn=%llu retries=%llu "
      "indep_torn=%llu\n",
      small.size(), static_cast<unsigned long long>(tagged), indep,
      static_cast<unsigned long long>(gaps),
      static_cast<unsigned long long>(t0),
      static_cast<unsigned long long>(t0 + kAfter),
      static_cast<unsigned long long>(hook.now_ns),
      static_cast<unsigned long long>(hook.mono_ns),
      static_cast<unsigned long long>(hook.index),
      static_cast<unsigned long long>(hook.head), hook.slot_seq,
      static_cast<unsigned long long>(rec.stats().torn_reads),
      static_cast<unsigned long long>(rec.stats().harvest_retries),
      static_cast<unsigned long long>(indep_torn));
  CHECK(gaps == 0);
  CHECK(tagged == 1);         // boundary sample exactly once (no loss, no duplication)
  CHECK(small.size() == 21);  // 20 background + 1 boundary
  CHECK(indep == small.size());
  rec.Shutdown();
  mirror.Shutdown(true);
  ShmRegion::UnlinkName(page_name);
  RmTree(out);
  return 0;
}
