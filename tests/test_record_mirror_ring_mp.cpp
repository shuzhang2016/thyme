// test_record_mirror_ring_mp.cpp — multi-process mirror-ring behavior:
//   1. stress: one writer process hammers several struct rings while the
//      parent and a second child read concurrently (HarvestWindow +
//      ReadRingSample), verifying content integrity, per-struct monotonic
//      publish order, and window bounds under races;
//   2. producer restart: a writer exits (no unlink), a new producer Creates
//      the same shm name with a new session_id; the recorder detects the
//      session change, re-verifies config_hash, and never mistakes stale
//      data for the new session;
//   3. cross-process config_hash mismatch rejection on attach.
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

constexpr uint32_t kStructs = 3;
constexpr uint32_t kDepth = 4096;
constexpr uint64_t kTotal = 5000;  // per struct; > kDepth to force eviction

struct StressPayload {
  uint64_t index;
  uint64_t mono;
  uint32_t sid;
  uint32_t pad;
  uint8_t fill[96];
};

void FillPayload(StressPayload& p, uint32_t sid, uint64_t idx) {
  p.index = idx;
  p.mono = idx * 10;
  p.sid = sid;
  p.pad = 0;
  for (uint32_t i = 0; i < sizeof(p.fill); ++i) {
    p.fill[i] = static_cast<uint8_t>(idx + i * 7u + sid);
  }
}

void CheckPayload(const StressPayload& p, uint32_t sid, uint64_t idx) {
  CHECK(p.index == idx);
  CHECK(p.mono == idx * 10);
  CHECK(p.sid == sid);
  for (uint32_t i = 0; i < sizeof(p.fill); ++i) {
    CHECK(p.fill[i] == static_cast<uint8_t>(idx + i * 7u + sid));
  }
}

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

void MakeDescs(ThymeStructDesc* descs, RecordMirrorRingSpec* specs,
               uint32_t payload) {
  for (uint32_t i = 0; i < kStructs; ++i) {
    descs[i] = {};
    descs[i].struct_id = static_cast<uint16_t>(i);
    descs[i].contract_version = 1;
    descs[i].payload_size = payload;
    std::snprintf(descs[i].name, sizeof(descs[i].name), "Mp%u", i);
    specs[i] = {payload, kDepth};
  }
}

// One reader pass over all structs: verifies descriptor self-consistency
// (mono == publish_index * 10, ascending per struct) and spot-checks payload
// content. Torn reads are counted into the caller-owned counter. Returns
// full reads performed.
uint64_t ReaderPass(const RecordMirrorRingPage* page,
                    std::vector<RecordMirrorRingSampleDesc>& buf,
                    uint64_t& torn_drops) {
  uint64_t reads = 0;
  for (uint32_t sid = 0; sid < kStructs; ++sid) {
    const uint32_t n = page->HarvestWindow(static_cast<uint16_t>(sid), 0,
                                           ~0ULL, buf.data(),
                                           static_cast<uint32_t>(buf.size()),
                                           &torn_drops);
    for (uint32_t i = 0; i < n; ++i) {
      CHECK(buf[i].mono_ns == buf[i].publish_index * 10);
      CHECK(buf[i].payload_size == sizeof(StressPayload));
      if (i > 0) CHECK(buf[i].publish_index > buf[i - 1].publish_index);
      if (buf[i].publish_index % 97 == 0) {
        StressPayload p{};
        uint32_t sz = 0;
        uint64_t mono = 0;
        uint32_t sseq = 0;
        if (page->ReadRingSample(static_cast<uint16_t>(sid),
                                 buf[i].publish_index,
                                 reinterpret_cast<uint8_t*>(&p), sizeof(p), sz,
                                 mono, sseq, nullptr, &torn_drops)) {
          // Any successfully read sample must be byte-consistent.
          CheckPayload(p, sid, buf[i].publish_index);
          CHECK(sz == sizeof(StressPayload));
          CHECK(mono == buf[i].mono_ns);
          ++reads;
        }
        // else: raced an eviction between harvest and read — a legitimate
        // overrun hole, not an error.
      }
    }
  }
  return reads;
}

void RunStress(const char* name) {
  ThymeStructDesc descs[kStructs];
  RecordMirrorRingSpec specs[kStructs];
  MakeDescs(descs, specs, sizeof(StressPayload));
  const uint64_t hash =
      RecordMirrorRingPage::HashConfig(descs, specs, kStructs);

  ShmRegion::UnlinkName(name);
  record_mirror::RingClient owner;
  CHECK(owner.Create(name, descs, specs, kStructs, 9));

  const pid_t writer = fork();
  CHECK(writer >= 0);
  if (writer == 0) {
    record_mirror::RingClient w;
    CHECK(w.AttachReadWrite(name, 2000000000ULL, hash));
    StressPayload p{};
    for (uint64_t i = 0; i < kTotal; ++i) {
      for (uint32_t s = 0; s < kStructs; ++s) {
        FillPayload(p, s, i);
        CHECK(w.Page()->TryWriteRing(static_cast<uint16_t>(s), &p,
                                     sizeof(p), i * 10,
                                     static_cast<uint32_t>(i)));
      }
      // Pace slightly: an un-paced writer cycles the 4096-deep ring faster
      // than a 1 ms-cadence reader can spot-read, which is a valid race but
      // makes the "reads > 0" witness flaky. The race window stays wide.
      SleepNs(2000);
    }
    w.Shutdown(false);
    _exit(0);
  }

  const pid_t reader = fork();
  CHECK(reader >= 0);
  if (reader == 0) {
    record_mirror::RingClient r;
    CHECK(r.AttachReadOnly(name, 2000000000ULL, hash));
    std::vector<RecordMirrorRingSampleDesc> buf(512);
    uint64_t reads = 0;
    uint64_t torn = 0;
    const uint64_t deadline = NowNs() + 60000000000ULL;
    while (r.Page()->RingHead(kStructs - 1) < kTotal) {
      // Wedged writer must not hang the test; the 1 ms cadence keeps the
      // spin readers from starving the writer on a fully loaded CI box.
      CHECK(NowNs() < deadline);
      reads += ReaderPass(r.Page(), buf, torn);
      SleepNs(1000000);
    }
    // Final pass once the writer is done: exact window math, no racing.
    std::vector<RecordMirrorRingSampleDesc> full(kDepth);
    for (uint32_t sid = 0; sid < kStructs; ++sid) {
      CHECK(r.Page()->RingHead(static_cast<uint16_t>(sid)) == kTotal);
      const uint32_t n =
          r.Page()->HarvestWindow(static_cast<uint16_t>(sid), 0, ~0ULL,
                                  full.data(), kDepth);
      CHECK(n == kDepth);  // retained window is exactly full
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t expect = kTotal - kDepth + i;
        CHECK(full[i].publish_index == expect);
        CHECK(full[i].mono_ns == expect * 10);
      }
      // Sub-window fully inside the retained range: exact count and bounds.
      const uint32_t nw =
          r.Page()->HarvestWindow(static_cast<uint16_t>(sid), 1000 * 10,
                                  2000 * 10, full.data(), kDepth);
      CHECK(nw == 1001);
      CHECK(full[0].publish_index == 1000);
      CHECK(full[nw - 1].publish_index == 2000);
      // Window entirely below the retained range harvests nothing.
      CHECK(r.Page()->HarvestWindow(static_cast<uint16_t>(sid), 0, 10,
                                    full.data(), kDepth) == 0);
      // Newest sample readable; the sample just beyond the retained window
      // is an overrun hole.
      StressPayload p{};
      uint32_t sz = 0;
      uint64_t mono = 0;
      uint32_t sseq = 0;
      CHECK(r.Page()->ReadRingSample(static_cast<uint16_t>(sid), kTotal - 1,
                                     reinterpret_cast<uint8_t*>(&p),
                                     sizeof(p), sz, mono, sseq));
      CheckPayload(p, sid, kTotal - 1);
      CHECK(!r.Page()->ReadRingSample(static_cast<uint16_t>(sid),
                                      kTotal - kDepth - 1,
                                      reinterpret_cast<uint8_t*>(&p),
                                      sizeof(p), sz, mono, sseq));
    }
    CHECK(reads > 0);  // the racing phase actually read payloads
    _exit(0);
  }

  // Parent reads concurrently too (lighter cadence).
  uint64_t torn_race = 0;
  {
    std::vector<RecordMirrorRingSampleDesc> buf(256);
    uint64_t reads = 0;
    const uint64_t deadline = NowNs() + 60000000000ULL;
    while (owner.Page()->RingHead(kStructs - 1) < kTotal) {
      CHECK(NowNs() < deadline);
      reads += ReaderPass(owner.Page(), buf, torn_race);
      SleepNs(1000000);
    }
    CHECK(reads > 0);
  }

  int st = 0;
  CHECK(waitpid(writer, &st, 0) == writer);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  CHECK(waitpid(reader, &st, 0) == reader);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  // Torn reads are legal under races (counted caller-side, never blocking);
  // after the writer stopped, one more pass must be tear-free.
  std::vector<RecordMirrorRingSampleDesc> buf(512);
  uint64_t torn_after = 0;
  CHECK(ReaderPass(owner.Page(), buf, torn_after) > 0);
  CHECK(torn_after == 0);
  std::fprintf(stderr, "stress: torn reads during race phase: %llu\n",
               static_cast<unsigned long long>(torn_race));

  owner.Shutdown(true);
}

void RunRestart(const char* name) {
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 8;
  std::snprintf(desc.name, sizeof(desc.name), "Restart");
  RecordMirrorRingSpec spec{8, 16};
  const uint64_t hash = RecordMirrorRingPage::HashConfig(&desc, &spec, 1);

  ShmRegion::UnlinkName(name);

  // First producer: session 11, 10 samples tagged 11xxx; exits WITHOUT
  // unlinking the shm object.
  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    record_mirror::RingClient c1;
    CHECK(c1.Create(name, &desc, &spec, 1, 11));
    for (uint64_t i = 0; i < 10; ++i) {
      const uint64_t v = 11000 + i;
      CHECK(c1.Page()->TryWriteRing(0, &v, sizeof(v), i * 10,
                                    static_cast<uint32_t>(i)));
    }
    c1.Shutdown(false);
    _exit(0);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  // Recorder attaches to session 11 and reads it.
  record_mirror::RingClient ro1;
  CHECK(ro1.AttachReadOnly(name, 2000000000ULL, hash));
  CHECK(ro1.Page()->header.session_id.load(std::memory_order_acquire) == 11);
  CHECK(ro1.Page()->RingHead(0) == 10);
  {
    uint64_t v = 0;
    uint32_t sz = 0;
    uint64_t mono = 0;
    uint32_t sseq = 0;
    CHECK(ro1.Page()->ReadRingSample(0, 5, reinterpret_cast<uint8_t*>(&v),
                                     sizeof(v), sz, mono, sseq));
    CHECK(v == 11005);
  }

  // Cross-process config-hash mismatch rejection (scenario 5): a recorder
  // configured for a different ring shape must be refused.
  {
    RecordMirrorRingSpec other{8, 17};  // different depth -> different hash
    const uint64_t bad_hash =
        RecordMirrorRingPage::HashConfig(&desc, &other, 1);
    CHECK(bad_hash != hash);
    record_mirror::RingClient bad;
    CHECK(!bad.AttachReadOnly(name, 100000000ULL, bad_hash));
    record_mirror::RingClient bad_rw;
    CHECK(!bad_rw.AttachReadWrite(name, 100000000ULL, bad_hash));
  }

  // Second producer: same shm name, NEW session 22. Create unlinks and
  // re-creates the object, so the recorder's old mapping still points at the
  // stale session-11 page.
  pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    record_mirror::RingClient c2;
    // The parent retains the old mapping to prove stale-session detection;
    // this is an explicit known-dead-session replacement.
    ShmRegion::UnlinkName(name);
    CHECK(c2.Create(name, &desc, &spec, 1, 22));
    for (uint64_t i = 0; i < 7; ++i) {
      const uint64_t v = 22000 + i;
      CHECK(c2.Page()->TryWriteRing(0, &v, sizeof(v), i * 10,
                                    static_cast<uint32_t>(i)));
    }
    c2.Shutdown(false);
    _exit(0);
  }
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  // The stale mapping still shows session 11: the recorder MUST re-attach on
  // session change; this pins that the old page is never silently reused.
  CHECK(ro1.Page()->header.session_id.load(std::memory_order_acquire) == 11);

  // Re-attach: session changed, config identical, data is the new session's.
  record_mirror::RingClient ro2;
  CHECK(ro2.AttachReadOnly(name, 2000000000ULL, hash));
  CHECK(ro2.Page()->header.session_id.load(std::memory_order_acquire) == 22);
  CHECK(ro2.Page()->ValidateConfig(hash));
  CHECK(ro2.Page()->RingHead(0) == 7);
  for (uint64_t i = 0; i < 7; ++i) {
    uint64_t v = 0;
    uint32_t sz = 0;
    uint64_t mono = 0;
    uint32_t sseq = 0;
    CHECK(ro2.Page()->ReadRingSample(0, i, reinterpret_cast<uint8_t*>(&v),
                                     sizeof(v), sz, mono, sseq));
    CHECK(v == 22000 + i);  // never stale session-11 data
  }

  ro1.Shutdown(false);
  ro2.Shutdown(true);
}

}  // namespace

int main() {
  char stress_name[64];
  std::snprintf(stress_name, sizeof(stress_name), "/c9rmp_stress_%d",
                static_cast<int>(getpid()));
  RunStress(stress_name);

  char restart_name[64];
  std::snprintf(restart_name, sizeof(restart_name), "/c9rmp_restart_%d",
                static_cast<int>(getpid()));
  RunRestart(restart_name);
  return 0;
}
