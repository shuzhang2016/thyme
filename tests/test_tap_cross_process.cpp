// test_tap_cross_process.cpp — cross-process tap chain (design doc 4.1/4.2):
// the CHILD process is the producer: it creates the mirror ring page and an
// ShmSpmcRing shm ring, arms the tap, then publishes ONLY through
// ring.Push() (no recording API calls). The PARENT plays two roles at once:
//   * recorder: AttachReadOnly the mirror ring, HarvestWindow the samples and
//     verify content/timestamps/order;
//   * plain consumer: Open the SPMC ring and drain it with SpmcReader,
//     proving business reads and recording do not interfere.
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>

using namespace thyme;

namespace {

struct Pod {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};

constexpr uint64_t kFrames = 100;
constexpr uint32_t kDepth = 128;  // deeper than kFrames: everything retained

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

}  // namespace

int main() {
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = sizeof(Pod);
  std::snprintf(desc.name, sizeof(desc.name), "XpTap");
  RecordMirrorRingSpec spec{static_cast<uint32_t>(sizeof(Pod)), kDepth};
  const uint64_t hash = RecordMirrorRingPage::HashConfig(&desc, &spec, 1);

  char page_name[64];
  char ring_name[64];
  std::snprintf(page_name, sizeof(page_name), "/c9xtap_page_%d",
                static_cast<int>(getpid()));
  std::snprintf(ring_name, sizeof(ring_name), "/c9xtap_ring_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);

  int sync_pipe[2];
  CHECK(pipe(sync_pipe) == 0);

  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    // ---- producer child: node-oblivious publishing ----
    close(sync_pipe[0]);
    record_mirror::RingClient mirror;
    CHECK(mirror.Create(page_name, &desc, &spec, 1, 5));
    ShmSpmcRing<Pod, 8> ring;
    CHECK(ring.Create(ring_name));
    ring.SetRecordTap(mirror.Page(), 0);
    CHECK(write(sync_pipe[1], "C", 1) == 1);  // "created" signal
    for (uint64_t i = 0; i < kFrames; ++i) {
      Pod v{i, static_cast<uint32_t>(i * 2), static_cast<uint32_t>(i * 3)};
      ring.Push(v);  // the ONLY publish call; tap fires inside Push
      SleepNs(1000000);  // 1 ms: the 8-deep SPMC ring must never overrun the
                         // parent consumer even on a heavily loaded CI box
    }
    ring.Close();
    mirror.Shutdown(false);  // leave the shm objects behind for the reader
    _exit(0);
  }

  // ---- parent: recorder + business consumer ----
  close(sync_pipe[1]);
  char b = 0;
  CHECK(read(sync_pipe[0], &b, 1) == 1 && b == 'C');

  record_mirror::RingClient ro;
  CHECK(ro.AttachReadOnly(page_name, 2000000000ULL, hash));
  const RecordMirrorRingPage* page = ro.Page();
  CHECK(page != nullptr && page->Validate());
  CHECK(page->header.session_id.load(std::memory_order_acquire) == 5);

  ShmSpmcRing<Pod, 8> ring;
  CHECK(ring.Open(ring_name, 2000000000ULL));
  SpmcReader<Pod, 8> reader;
  CHECK(reader.Bind(&ring, SpmcRead::Follow));

  // Consume the business ring while the mirror fills; both paths must make
  // progress independently.
  uint64_t consumed = 0;
  const uint64_t deadline = NowNs() + 10000000000ULL;
  while ((page->RingHead(0) < kFrames || consumed < kFrames) &&
         NowNs() < deadline) {
    Pod v{};
    while (reader.TryPop(v)) {
      CHECK(v.a == consumed);
      CHECK(v.b == consumed * 2);
      CHECK(v.c == consumed * 3);
      ++consumed;
    }
    SleepNs(100000);  // 100 us poll: 10x the producer's 1 ms cadence
  }
  CHECK(consumed == kFrames);        // business path saw every push, in order
  CHECK(reader.drop_count() == 0);   // ...and never fell behind
  CHECK(page->RingHead(0) == kFrames);  // recorder path saw every push too

  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  // ---- recorder verification: full-window harvest ----
  RecordMirrorRingSampleDesc harvest[kDepth];
  const uint32_t n = page->HarvestWindow(0, 0, ~0ULL, harvest, kDepth);
  CHECK(n == kFrames);
  for (uint64_t i = 0; i < kFrames; ++i) {
    CHECK(harvest[i].publish_index == i);            // exact order
    CHECK(harvest[i].payload_size == sizeof(Pod));
    if (i > 0) CHECK(harvest[i].mono_ns >= harvest[i - 1].mono_ns);
  }

  // Sample content matches what the business consumer saw.
  for (uint64_t i : {0UL, 1UL, 50UL, 99UL}) {
    Pod v{};
    uint32_t sz = 0;
    uint64_t mono = 0;
    uint32_t sseq = 0;
    CHECK(page->ReadRingSample(0, i, reinterpret_cast<uint8_t*>(&v),
                               sizeof(v), sz, mono, sseq));
    CHECK(sz == sizeof(Pod));
    CHECK(v.a == i && v.b == i * 2 && v.c == i * 3);
    CHECK(sseq == static_cast<uint32_t>(i));  // SPMC publish index
    CHECK(mono == harvest[i].mono_ns);        // harvest/read agree
  }

  // Sub-window harvest crops exactly to [t_lo, t_hi].
  const uint64_t t_lo = harvest[40].mono_ns;
  const uint64_t t_hi = harvest[80].mono_ns;
  CHECK(t_lo < t_hi);  // 100 us spacing must yield distinct timestamps
  const uint32_t n2 = page->HarvestWindow(0, t_lo, t_hi, harvest, kDepth);
  CHECK(n2 == 41);
  CHECK(harvest[0].publish_index == 40);
  CHECK(harvest[n2 - 1].publish_index == 80);
  for (uint32_t i = 0; i < n2; ++i) {
    CHECK(harvest[i].mono_ns >= t_lo && harvest[i].mono_ns <= t_hi);
  }

  // Empty window outside the recorded range.
  CHECK(page->HarvestWindow(0, 1, 2, harvest, kDepth) == 0);

  ro.Shutdown(true);
  ring.Close();
  ShmRegion::UnlinkName(ring_name);
  return 0;
}
