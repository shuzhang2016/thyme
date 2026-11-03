#include "thyme/check.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace thyme;

// Probe's constructor and destructor have observable side effects, so a
// placement-new really must run: ftruncate's zero-fill (the old
// "zero-fill as construction" pattern) or a memcpy would never bump `alive`
// nor set the constructor values.
struct Probe {
  static std::atomic<int> alive;
  uint32_t a;
  uint32_t b;

  Probe() : a(0xDEADBEEFu), b(0xCAFEBABEu) { alive.fetch_add(1, std::memory_order_relaxed); }
  Probe(uint32_t x, uint32_t y) : a(x), b(y) { alive.fetch_add(1, std::memory_order_relaxed); }
  ~Probe() { alive.fetch_sub(1, std::memory_order_relaxed); }
};
std::atomic<int> Probe::alive{0};

static void UnlinkIfPresent(const char* name) {
  const int fd = shm_open(name, O_RDONLY, 0600);
  if (fd >= 0) {
    close(fd);
    ShmRegion::UnlinkName(name);
  }
}

int main() {
  char name[64];

  // 1. Ctor args forwarded, ctor really ran (side effect), and the
  //    constructed object is visible through a second, independently mapped
  //    region (cross-process SHM semantics).
  std::snprintf(name, sizeof(name), "/c5t1_%d", static_cast<int>(getpid()));
  UnlinkIfPresent(name);
  ShmRegion a;
  Probe* p = a.Create<Probe>(name, sizeof(Probe), 42u, 24u);
  CHECK(p != nullptr);
  CHECK(Probe::alive.load(std::memory_order_relaxed) == 1);  // placement new ran
  CHECK(p->a == 42u && p->b == 24u);  // args forwarded, not zeros

  ShmRegion b;
  CHECK(b.OpenRetry(name, sizeof(Probe), 1000000000ULL));
  auto* q = static_cast<Probe*>(b.data());
  CHECK(q->a == 42u && q->b == 24u);  // visible through the other mapping

  // Documented lifecycle: call the destructor before Unmap().
  p->~Probe();
  CHECK(Probe::alive.load(std::memory_order_relaxed) == 0);
  a.Unmap();
  b.Unmap();
  ShmRegion::UnlinkName(name);

  // 2. bytes defaults to sizeof(T); the default ctor must produce its ctor
  //    values -- a plain static_cast over the zero-filled region would read
  //    0/0 instead of 0xDEADBEEF/0xCAFEBABE, proving construction ran.
  std::snprintf(name, sizeof(name), "/c5t2_%d", static_cast<int>(getpid()));
  UnlinkIfPresent(name);
  ShmRegion c;
  Probe* r = c.Create<Probe>(name);
  CHECK(r != nullptr);
  CHECK(r->a == 0xDEADBEEFu && r->b == 0xCAFEBABEu);
  CHECK(Probe::alive.load(std::memory_order_relaxed) == 1);
  r->~Probe();
  c.Unmap();
  ShmRegion::UnlinkName(name);
  CHECK(Probe::alive.load(std::memory_order_relaxed) == 0);

  // 3. bytes < sizeof(T) is rejected, and no half-created object is left
  //    behind in /dev/shm.
  std::snprintf(name, sizeof(name), "/c5t3_%d", static_cast<int>(getpid()));
  UnlinkIfPresent(name);
  ShmRegion d;
  CHECK(d.Create<Probe>(name, 4u) == nullptr);
  CHECK(d.data() == nullptr);  // region was unmapped on failure
  const int leftover = shm_open(name, O_RDONLY, 0600);
  if (leftover >= 0) close(leftover);
  CHECK(leftover < 0 && errno == ENOENT);
  ShmRegion::UnlinkName(name);

  // 4. Over-aligned T (alignof(T) > page size) is rejected and cleaned up
  //    the same way.
  const long ps = sysconf(_SC_PAGESIZE);
  CHECK(ps > 0 && ps < 8192);  // guards the alignas(8192) choice below
  struct alignas(8192) OverAligned {
    uint32_t x;
  };
  static_assert(alignof(OverAligned) > 4096, "test requires over-aligned T");
  std::snprintf(name, sizeof(name), "/c5t4_%d", static_cast<int>(getpid()));
  UnlinkIfPresent(name);
  ShmRegion e;
  CHECK(e.Create<OverAligned>(name, sizeof(OverAligned)) == nullptr);
  CHECK(e.data() == nullptr);
  const int leftover2 = shm_open(name, O_RDONLY, 0600);
  if (leftover2 >= 0) close(leftover2);
  CHECK(leftover2 < 0 && errno == ENOENT);
  ShmRegion::UnlinkName(name);

  // 5. A 64-byte-aligned T (well within page alignment) constructs fine.
  struct alignas(64) Slot64 {
    uint32_t v = 7u;
  };
  static_assert(alignof(Slot64) == 64, "Slot64 must be 64-byte aligned");
  std::snprintf(name, sizeof(name), "/c5t5_%d", static_cast<int>(getpid()));
  UnlinkIfPresent(name);
  ShmRegion f;
  Slot64* s = f.Create<Slot64>(name, sizeof(Slot64));
  CHECK(s != nullptr && s->v == 7u);
  f.Unmap();
  ShmRegion::UnlinkName(name);

  return 0;
}
