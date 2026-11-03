#include "thyme/check.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/shm_region.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <csignal>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace thyme;


int main() {
  char name[32];
  std::snprintf(name, sizeof(name), "/c9t_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmRegion a;
  CHECK(a.Create(name, 4096));
  auto* slot = static_cast<LatestSlot<uint32_t>*>(a.data());
  slot->Write(42u);
  ShmRegion b;
  CHECK(b.OpenRetry(name, 4096, 1000000000ULL));
  uint32_t v = 0;
  CHECK(static_cast<LatestSlot<uint32_t>*>(b.data())->Read(v) && v == 42u);
  CHECK(a.size() >= 4096);
  CHECK(a.size() % 4096 == 0 ||
        a.size() % static_cast<std::size_t>(sysconf(_SC_PAGESIZE)) == 0);

  ShmRegion ro;
  CHECK(ro.OpenRetry(name, 4096, 1000000000ULL, true));
  uint32_t v2 = 0;
  CHECK(static_cast<LatestSlot<uint32_t>*>(ro.data())->Read(v2) && v2 == 42u);
  ro.Unmap();

  a.Unmap();
  b.Unmap();

  char n2[32];
  std::snprintf(n2, sizeof(n2), "/c9t2_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(n2);
  ShmRegion d;
  CHECK(d.Create(n2, 100));
  CHECK(d.size() >= 100);
  long ps = sysconf(_SC_PAGESIZE);
  CHECK(ps > 0);
  CHECK(d.size() % static_cast<std::size_t>(ps) == 0);
  d.Unmap();
  ShmRegion::UnlinkName(n2);

  ShmRegion::UnlinkName(name);
  ShmRegion c;
  CHECK(!c.OpenRetry(name, 4096, 50000000ULL));

  // A Create() that fails after shm_open (here: ftruncate hits RLIMIT_FSIZE)
  // must not leave the shm object behind in /dev/shm.
  char n4[32];
  std::snprintf(n4, sizeof(n4), "/c9t4_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(n4);
  std::signal(SIGXFSZ, SIG_IGN);
  rlimit rl{};
  CHECK(getrlimit(RLIMIT_FSIZE, &rl) == 0);
  const rlim_t saved = rl.rlim_cur;
  rl.rlim_cur = 4096;
  CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0);
  ShmRegion e;
  CHECK(!e.Create(n4, 8192));  // rounds up past the 4096-byte file-size cap
  rl.rlim_cur = saved;
  CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0);
  const int leftover = shm_open(n4, O_RDONLY, 0600);
  if (leftover >= 0) close(leftover);
  CHECK(leftover < 0 && errno == ENOENT);
  ShmRegion::UnlinkName(n4);
  return 0;
}
