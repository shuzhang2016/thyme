// test_replay_producer_gate.cpp — REP-10: replay producer gate.
//
// A: a foreign (replay) gate makes a plain writer's Begin() fail with the
//    distinct kGated status, before any slot/head mutation; blocked attempts
//    are counted in gate.rejects.
// B: a writer that owns the gate (the PinnedSlotInjector path) keeps writing;
//    a second acquire is kBusy, and Create() cannot replace the gated channel
//    even with force_replace.
// C: ReleaseProducerGate() restores the plain producer immediately.
// D: a stale lease (no heartbeat) is reclaimed by the next writer.
// E: a crashed holder (fork + SIGKILL) is reclaimed because /proc proves the
//    pid is gone — no permanent gating.
// F: an old (v2) channel header is refused by Create() without force, and the
//    new reader rejects it (mixed ABI never attaches).
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/producer_gate.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/wait.h>
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

std::string Name(const char* tag) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "/thyme_rep10_%s_%ld", tag,
                static_cast<long>(getpid()));
  ShmRegion::UnlinkName(buf);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(buf).c_str());
  return buf;
}

void Cleanup(const std::string& name) {
  ShmRegion::UnlinkName(name.c_str());
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name.c_str()).c_str());
}

bool WriteOne(PinnedSlotWriter& w, uint8_t tag) {
  PinnedSlotWriter::BeginView bv = w.Begin(128);
  if (bv.payload == nullptr) return false;
  std::memset(bv.payload, tag, 128);
  bv.header->flags.store(tag, std::memory_order_relaxed);
  w.Commit(NowNs());
  return true;
}

}  // namespace

int main() {
  // pid-reuse guard is available on Linux and points at this process.
  const uint64_t self_start =
      ProducerGateProcessStart(static_cast<uint64_t>(getpid()));
  CHECK(self_start != 0);
  CHECK(ProducerGateProcessAlive(static_cast<uint64_t>(getpid()), self_start));

  // ---- A/B/C/D: gated writer, owner writes, double acquire, timeout ----
  {
    const std::string name = Name("gate");
    PinnedSlotWriter w;
    CHECK(w.Create(name.c_str(), 512, 4, 0));
    w.SetCrcEnabled(true);
    CHECK(WriteOne(w, 0x11));
    CHECK(w.head() == 1);

    // A: a foreign token plays the replay holder; this writer is a plain
    // producer (token 0).
    const uint64_t replay_token = ProducerGateNewToken();
    CHECK(ProducerGateAcquire(*w.gate(), replay_token, NowNs(),
                              1000000000ULL) == GateResult::kOk);
    CHECK(w.producer_gate_held_by_other());
    PinnedSlotWriter::BeginView gated = w.Begin(128);
    CHECK(gated.payload == nullptr);
    CHECK(gated.header == nullptr);
    CHECK(gated.status == PinnedSlotWriter::BeginStatus::kGated);
    CHECK(w.pending_gated());
    w.Commit();  // no-op
    CHECK(w.head() == 1);  // nothing was published
    CHECK(w.gate_rejects() == 1);
    CHECK(w.dropped_oversize() == 0);  // not misreported as oversize

    // B: a second replay cannot attach to the same gate.
    CHECK(ProducerGateAcquire(*w.gate(), ProducerGateNewToken(), NowNs(),
                              1000000000ULL) == GateResult::kBusy);

    // B: Create() refuses a live-gated channel (double-write prevention),
    // even when force_replace is requested.
    {
      PinnedSlotWriter clobber;
      CHECK(!clobber.Create(name.c_str(), 512, 4, 0,
                            kPinnedSlotLeaseDefaultStealTimeoutNs,
                            /*force_replace=*/true));
      CHECK(clobber.create_status() == PinnedSlotWriter::CreateStatus::kGated);
      CHECK(!clobber.last_error().empty());
    }

    // B: the gate owner (the injector's writer) keeps writing through
    // Begin/Commit. Release the foreign lease first, then take it with the
    // writer itself, exactly as PinnedSlotInjector does.
    CHECK(ProducerGateRelease(*w.gate(), replay_token) == GateResult::kOk);
    CHECK(w.AcquireProducerGateWithNewToken() == GateResult::kOk);
    CHECK(w.owns_producer_gate());
    CHECK(w.gate()->owner_token.load(std::memory_order_acquire) != 0);
    CHECK(WriteOne(w, 0x22));
    CHECK(w.head() == 2);

    // C: replay end releases the gate; the plain producer resumes.
    CHECK(w.ReleaseProducerGate() == GateResult::kOk);
    CHECK(!w.owns_producer_gate());
    CHECK(!w.producer_gate_held_by_other());
    CHECK(WriteOne(w, 0x33));
    CHECK(w.head() == 3);

    // D: a lease with no heartbeat expires and the writer reclaims it. Use a
    // foreign token so this writer is the plain producer that must recover.
    uint64_t stale_token = ProducerGateNewToken();
    CHECK(ProducerGateAcquire(*w.gate(), stale_token, NowNs(),
                              30000000ULL) == GateResult::kOk);
    SleepNs(80000000ULL);
    CHECK(WriteOne(w, 0x44));  // reclaims the expired lease
    CHECK(w.head() == 4);
    CHECK(w.gate_steals() >= 1);
    CHECK(w.gate()->owner_token.load(std::memory_order_acquire) ==
          kProducerGateUnheld);

    w.Close();
    Cleanup(name);
  }

  // ---- E: crash of the replay holder is recovered immediately ----
  {
    const std::string name = Name("crash");
    PinnedSlotWriter w;
    CHECK(w.Create(name.c_str(), 512, 4, 0));
    CHECK(WriteOne(w, 0x55));

    int ready[2];
    CHECK(pipe(ready) == 0);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
      close(ready[0]);
      const uint64_t token = ProducerGateNewToken();
      const GateResult r = ProducerGateAcquire(*w.gate(), token, NowNs(),
                                               60000000000ULL);
      const char msg = (r == GateResult::kOk || r == GateResult::kStolen)
                           ? '1'
                           : '0';
      (void)!write(ready[1], &msg, 1);
      close(ready[1]);
      alarm(10);           // never outlive a failed parent
      for (;;) pause();    // killed by the parent
    }
    close(ready[1]);
    char msg = '0';
    CHECK(read(ready[0], &msg, 1) == 1);
    close(ready[0]);
    CHECK(msg == '1');

    // While the holder is alive the producer is blocked.
    PinnedSlotWriter::BeginView blocked = w.Begin(128);
    CHECK(blocked.status == PinnedSlotWriter::BeginStatus::kGated);
    CHECK(blocked.payload == nullptr);

    CHECK(kill(child, SIGKILL) == 0);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);

    // /proc proves the pid is gone: the very next Begin reclaims the lease.
    CHECK(WriteOne(w, 0x66));
    CHECK(w.head() == 2);
    CHECK(w.gate_steals() >= 1);

    w.Close();
    Cleanup(name);
  }

  // ---- F: old (v2) header is rejected by Create/Open ----
  {
    const std::string name = Name("oldabi");
    PinnedSlotWriter old;
    CHECK(old.Create(name.c_str(), 512, 4, 0));
    CHECK(WriteOne(old, 0x77));

    // Stomp the header version to the pre-REP-10 ABI.
    {
      ShmRegion rw;
      CHECK(rw.OpenRetry(name.c_str(),
                         PinnedSlotWriter::RegionBytes(512, 4, 0),
                         1000000000ULL, /*read_only=*/false));
      auto* h = static_cast<PinnedSlotPageHeader*>(rw.data());
      h->super.abi_version.store(2, std::memory_order_release);
      rw.Unmap();
    }
    old.Close();

    // The new reader rejects a v2 header (no mixed attach).
    PinnedSlotReader reader;
    CHECK(!reader.Open(name.c_str(), 20000000ULL));

    // A new writer refuses to clobber the incompatible channel by default...
    PinnedSlotWriter next;
    CHECK(!next.Create(name.c_str(), 512, 4, 0));
    CHECK(next.create_status() ==
          PinnedSlotWriter::CreateStatus::kIncompatible);
    CHECK(next.last_error().find("incompatible") != std::string::npos);

    // ...but an explicit rebuild (force_replace) succeeds.
    CHECK(next.Create(name.c_str(), 512, 4, 0,
                      kPinnedSlotLeaseDefaultStealTimeoutNs,
                      /*force_replace=*/true));
    CHECK(next.create_status() == PinnedSlotWriter::CreateStatus::kOk);
    CHECK(WriteOne(next, 0x88));
    CHECK(next.head() == 1);
    next.Close();
    Cleanup(name);
  }

  return 0;
}
