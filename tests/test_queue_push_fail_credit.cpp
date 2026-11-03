// M4.4: cross-process queue push_fail_count → producer HealthSlot (CreditQueuePushFails).
#include "thyme/check.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spsc_ring.h"
#include <cstdio>
#include <unistd.h>

using namespace thyme;

namespace {

struct Payload {
  uint32_t v;
};

// Standalone fixture mirroring the generated channel's push_fail accounting.
template <typename T, std::size_t Cap>
struct EventPipeRef {
  NotifPipe<T, Cap>* n_ = nullptr;
  ShmSpscRing<T, Cap>* s_ = nullptr;
  EventPipeRef() = default;
  explicit EventPipeRef(NotifPipe<T, Cap>* p) : n_(p) {}
  explicit EventPipeRef(ShmSpscRing<T, Cap>* p) : s_(p) {}
  bool Push(const T& v) { return n_ != nullptr ? n_->Push(v) : s_->Push(v); }
  T* BeginWrite() { return n_ != nullptr ? n_->BeginWrite() : s_->BeginWrite(); }
  void CommitWrite() {
    if (n_ != nullptr) { n_->CommitWrite(); } else { s_->CommitWrite(); }
  }
  void AbortWrite() {
    if (n_ != nullptr) { n_->AbortWrite(); } else { s_->AbortWrite(); }
  }
  bool uses_notif_pipe() const { return n_ != nullptr; }
  uint32_t push_fail_count() const {
    return s_ != nullptr ? s_->push_fail_count() : 0;
  }
};

// Mirrors generated PublishAndFork (RT-16 in-place shape).
template <typename Pipe, typename T>
void PublishAndFork(Pipe& pipe, HealthSlot* health, const T& v) {
  T* slot = pipe.BeginWrite();
  if (slot == nullptr) {
    if (pipe.uses_notif_pipe()) health->AddDrops(1);
    return;
  }
  *slot = v;
  pipe.CommitWrite();
}

template <typename Pipe>
void CreditQueuePushFails(Pipe& pipe, HealthSlot* health, uint32_t& seen) {
  const uint32_t d = pipe.push_fail_count();
  health->AddDrops(d - seen);
  seen = d;
}

}  // namespace

int main() {
  // Notify: immediate AddDrops on full pipe (M4.2).
  {
    HealthSlot h{};
    NotifPipe<Payload, 2> q;
    EventPipeRef<Payload, 2> ref(&q);
    Payload p{1};
    CHECK(ref.Push(p));
    CHECK(ref.Push(p));
    PublishAndFork(ref, &h, Payload{9});
    CHECK(h.drop_cnt.load() == 1);
    CHECK(ref.push_fail_count() == 0);
  }

  // Cross-process queue: defer to CreditQueuePushFails from shared push_fail.
  {
    char name[64];
    std::snprintf(name, sizeof(name), "/c9qpf_%d", static_cast<int>(getpid()));
    ShmRegion::UnlinkName(name);

    ShmSpscRing<Payload, 2> prod;
    CHECK(prod.Create(name));
    ShmSpscRing<Payload, 2> cons;
    CHECK(cons.Open(name, 1000000000ULL));

    HealthSlot h{};
    EventPipeRef<Payload, 2> ref(&prod);
    uint32_t seen = 0;
    Payload p{1};
    CHECK(ref.Push(p));
    CHECK(ref.Push(p));
    PublishAndFork(ref, &h, Payload{9});
    PublishAndFork(ref, &h, Payload{10});
    CHECK(h.drop_cnt.load() == 0);
    CHECK(ref.push_fail_count() == 2);
    CreditQueuePushFails(ref, &h, seen);
    CHECK(h.drop_cnt.load() == 2);
    CHECK(seen == 2);

    Payload out{};
    CHECK(cons.TryPop(out));
    CHECK(prod.Push(Payload{11}));
    CreditQueuePushFails(ref, &h, seen);
    CHECK(h.drop_cnt.load() == 2);

    prod.Close();
    cons.Close();
    ShmRegion::UnlinkName(name);
  }

  return 0;
}
