#include "thyme/record/recorder_drain.h"

#include "thyme/check.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace {

struct Ctx {
  std::atomic<int> entered{0};
  std::atomic<int> hold{1};
  std::atomic<int> trying{0};
  std::atomic<int> call_ran{0};
  std::atomic<uint64_t> ticks{0};
  std::thread::id drain_id{};
};

void Tick(void* p) {
  auto* ctx = static_cast<Ctx*>(p);
  ctx->drain_id = std::this_thread::get_id();
  ctx->ticks.fetch_add(1, std::memory_order_relaxed);
  ctx->entered.store(1, std::memory_order_release);
  while (ctx->hold.load(std::memory_order_acquire) != 0) {
  }
}

void WaitUntil(const std::atomic<int>& flag) {
  for (int i = 0; i < 1000000 && flag.load(std::memory_order_acquire) == 0;
       ++i) {
    std::this_thread::yield();
  }
  CHECK(flag.load(std::memory_order_acquire) == 1);
}

void TestDrainThreadIsNotTheCaller() {
  Ctx ctx;
  thyme::RecorderDrain drain(&Tick, &ctx, 1000000ull);
  drain.Start();
  WaitUntil(ctx.entered);
  CHECK(ctx.drain_id != std::this_thread::get_id());
  CHECK(drain.Contended());

  std::thread::id call_id{};
  std::thread caller([&] {
    ctx.trying.store(1, std::memory_order_release);
    drain.Call([&] {
      call_id = std::this_thread::get_id();
      ctx.call_ran.store(1, std::memory_order_release);
    });
  });
  WaitUntil(ctx.trying);
  for (int i = 0; i < 10000; ++i) std::this_thread::yield();
  CHECK(ctx.call_ran.load(std::memory_order_acquire) == 0);
  CHECK(drain.Contended());

  ctx.hold.store(0, std::memory_order_release);
  caller.join();
  CHECK(ctx.call_ran.load(std::memory_order_acquire) == 1);
  CHECK(call_id == ctx.drain_id);
  drain.Stop();
  CHECK(ctx.ticks.load(std::memory_order_relaxed) >= 1);
}

}  // namespace

int main() {
  TestDrainThreadIsNotTheCaller();
  return 0;
}
