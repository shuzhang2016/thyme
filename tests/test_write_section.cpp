// test_write_section.cpp — ARCH-12 RAII write section.
//
// Commit publishes; destructor without commit aborts (four abort classes).
// A failed BeginWrite (full pipe) is a no-op destructor. Primitive
// BeginWrite/CommitWrite/AbortWrite remain the hooks and stay tested
// separately.
#include "thyme/check.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/pinned_latest_store.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"
#include "thyme/runtime/write_section.h"

#include <cstdint>
#include <cstdio>
#include <unistd.h>

using namespace thyme;

namespace {

struct Payload {
  uint64_t a;
  uint32_t b;
};

}  // namespace

int main() {
  {
    LatestSlot<Payload> slot;
    slot.Write(Payload{1, 1});
    {
      WriteSection<LatestSlot<Payload>> section(slot);
      CHECK(section);
      section->a = 99;
      section->b = 99;
    }
    Payload out{};
    CHECK(slot.Read(out));
    CHECK(out.a == 1 && out.b == 1);
    {
      WriteSection<LatestSlot<Payload>> section(slot);
      *section = Payload{2, 2};
      section.commit();
    }
    CHECK(slot.Read(out));
    CHECK(out.a == 2 && out.b == 2);
  }

  {
    NotifPipe<Payload, 2> pipe;
    CHECK(pipe.Push(Payload{1, 1}));
    CHECK(pipe.Push(Payload{2, 2}));
    {
      WriteSection<NotifPipe<Payload, 2>> full(pipe);
      CHECK(!full);
    }
    Payload out{};
    CHECK(pipe.TryPop(out) && out.a == 1);
    {
      WriteSection<NotifPipe<Payload, 2>> section(pipe);
      CHECK(section);
      *section = Payload{3, 3};
    }
    CHECK(pipe.Push(Payload{4, 4}));
    CHECK(pipe.TryPop(out) && out.a == 2);
    CHECK(pipe.TryPop(out) && out.a == 4);
    CHECK(!pipe.TryPop(out));
  }

  {
    PinnedLatestStore<Payload, 2> store;
    CHECK(store.Publish(Payload{7, 7}));
    {
      WriteSection<PinnedLatestStore<Payload, 2>> section(store);
      CHECK(section);
      *section = Payload{99, 99};
    }
    PinnedLatestStore<Payload, 2>::Borrowed view;
    CHECK(store.TryRead(&view));
    CHECK(view->a == 7 && view->b == 7);
    CHECK(store.Publish(Payload{8, 8}));
    CHECK(store.TryRead(&view));
    CHECK(view->a == 8);
  }

  char spsc[64];
  char spmc[64];
  std::snprintf(spsc, sizeof(spsc), "/c9wsec_s_%d", static_cast<int>(getpid()));
  std::snprintf(spmc, sizeof(spmc), "/c9wsec_m_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(spsc);
  ShmRegion::UnlinkName(spmc);

  {
    ShmSpscRing<Payload, 2> w;
    CHECK(w.Create(spsc));
    ShmSpscRing<Payload, 2> r;
    CHECK(r.Open(spsc, 1000000000ULL));
    CHECK(w.Push(Payload{1, 1}));
    CHECK(w.Push(Payload{2, 2}));
    {
      WriteSection<ShmSpscRing<Payload, 2>> full(w);
      CHECK(!full);
    }
    Payload out{};
    CHECK(r.TryPop(out) && out.a == 1);
    {
      WriteSection<ShmSpscRing<Payload, 2>> section(w);
      CHECK(section);
      *section = Payload{3, 3};
    }
    CHECK(w.Push(Payload{4, 4}));
    CHECK(r.TryPop(out) && out.a == 2);
    CHECK(r.TryPop(out) && out.a == 4);
    w.Close();
    r.Close();
  }

  {
    ShmSpmcRing<Payload, 4> w;
    CHECK(w.Create(spmc));
    ShmSpmcRing<Payload, 4> r;
    CHECK(r.Open(spmc, 1000000000ULL));
    SpmcReader<Payload, 4> follow;
    CHECK(follow.Bind(&r, SpmcRead::Follow));
    w.Push(Payload{1, 1});
    {
      WriteSection<ShmSpmcRing<Payload, 4>> section(w);
      CHECK(section);
      *section = Payload{99, 99};
    }
    w.Push(Payload{2, 2});
    Payload out{};
    CHECK(follow.TryPop(out) && out.a == 1);
    CHECK(follow.TryPop(out) && out.a == 2);
    w.Close();
    r.Close();
  }

  ShmRegion::UnlinkName(spsc);
  ShmRegion::UnlinkName(spmc);
  std::printf("test_write_section: OK\n");
  return 0;
}
