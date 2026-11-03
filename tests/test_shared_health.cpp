#include "thyme/check.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/shm_region.h"

#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

using namespace thyme;


int main() {
  char name[32];
  std::snprintf(name, sizeof(name), "/c9hp_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);

  ShmRegion a;
  CHECK(a.Create(name, sizeof(SharedHealthPage)));
  auto* page = static_cast<SharedHealthPage*>(a.data());
  CHECK(page->Init(1, 2));
  CHECK(page->Validate());
  CHECK(page->worker_count() == 2);
  CHECK(page->session_id() == 1);
  CHECK(page->magic() == SharedHealthPage::kMagic);
  CHECK(page->abi_version() == SharedHealthPage::kVersion);
  CHECK(page->layout_hash() == SharedHealthPageLayoutHash());
  CHECK(page->page_bytes() == sizeof(SharedHealthPage));
  CHECK(page->feature_bits() == SharedHealthPage::kFeatureBits);

  CHECK(!page->Init(1, 99));
  CHECK(!page->Validate());
  CHECK(page->Init(1, 2));
  CHECK(page->Validate());

  // Worker ceiling: L4 topologies need headroom beyond the original 16
  // (multi-LiDAR front-ends, per-sensor pipelines).
  CHECK(SharedHealthPage::kMaxWorkers >= 64);
  CHECK(page->Init(1, SharedHealthPage::kMaxWorkers));
  CHECK(!page->Init(1, SharedHealthPage::kMaxWorkers + 1));
  CHECK(page->Init(1, 2));

  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    page->slots[0].alive_counter.fetch_add(1, std::memory_order_release);
    _exit(0);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  CHECK(page->slots[0].alive_counter.load(std::memory_order_acquire) >= 1);

  page->super.magic.store(0, std::memory_order_release);
  CHECK(!page->Validate());
  page->super.magic.store(SharedHealthPage::kMagic,
                          std::memory_order_release);
  CHECK(page->Validate());

  ShmRegion ro;
  CHECK(ro.OpenRetry(name, sizeof(SharedHealthPage), 1000000000ULL, true));
  auto* rpage = static_cast<const SharedHealthPage*>(ro.data());
  CHECK(rpage->Validate());
  CHECK(rpage->slots[0].alive_counter.load(std::memory_order_acquire) >= 1);

  a.Unmap();
  ro.Unmap();
  ShmRegion::UnlinkName(name);
  return 0;
}
