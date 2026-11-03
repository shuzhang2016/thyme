#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/local_hub.h"
#include "thyme/runtime/shm_region.h"
#include "test_wait.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <type_traits>
#include <unistd.h>

using namespace thyme;

namespace {

struct Payload {
  uint64_t sequence;
  uint32_t value;
  uint32_t mirror;
};

using TestFrame = thyme::Frame<Payload>;

template <std::size_t Cap>
using Pipe = thyme::NotifPipe<TestFrame, Cap>;

TestFrame MakeFrame(uint64_t sequence) {
  TestFrame frame{};
  frame.hdr.sequence = static_cast<uint32_t>(sequence);
  frame.hdr.mono_ns = sequence * 1000;
  frame.payload.sequence = sequence;
  frame.payload.value = static_cast<uint32_t>(sequence * 10);
  frame.payload.mirror = static_cast<uint32_t>(sequence * 10);
  return frame;
}

void CheckFrame(const TestFrame& frame, uint64_t sequence) {
  CHECK(frame.hdr.sequence == static_cast<uint32_t>(sequence));
  CHECK(frame.hdr.mono_ns == sequence * 1000);
  CHECK(frame.payload.sequence == sequence);
  CHECK(frame.payload.value == static_cast<uint32_t>(sequence * 10));
  CHECK(frame.payload.mirror == frame.payload.value);
}

template <std::size_t Cap>
void CheckFrames(Pipe<Cap>& pipe, uint64_t first, uint64_t count) {
  TestFrame frame{};
  for (uint64_t i = 0; i < count; ++i) {
    CHECK(pipe.TryPop(frame));
    CheckFrame(frame, first + i);
  }
  CHECK(!pipe.TryPop(frame));
  CHECK(pipe.empty());
}

struct CountingWaitable final : thyme::Waitable {
  bool empty() const override { return true; }
  void WaitUntil(uint64_t) override {}
  void Notify() override { count.fetch_add(1, std::memory_order_relaxed); }

  std::atomic<uint32_t> count{0};
};

void TestOutputCountsAndOrdering() {
  thyme::LocalFrameHub<Payload, 4, 0> no_outputs;
  CHECK(no_outputs.input().Push(MakeFrame(1)));
  CHECK(no_outputs.input().Push(MakeFrame(2)));
  CHECK(no_outputs.Dispatch() == 2);
  CHECK(no_outputs.empty());
  CHECK(no_outputs.output_drop_count(0) == 0);
  CHECK(!no_outputs.BindOutput(0, nullptr));

  thyme::LocalFrameHub<Payload, 4, 1> one_output;
  auto& only = one_output.output(0);
  HealthSlot only_health{};
  CHECK(!one_output.BindOutput(1, &only_health));
  CHECK(one_output.BindOutput(0, &only_health));
  for (uint64_t i = 1; i <= 4; ++i) {
    CHECK(one_output.input().Push(MakeFrame(i)));
  }
  CHECK(one_output.Dispatch() == 4);
  CheckFrames(only, 1, 4);
  CHECK(one_output.output_drop_count(0) == 0);
  CHECK(only_health.drop_cnt.load(std::memory_order_relaxed) == 0);

  thyme::LocalFrameHub<Payload, 4, 3> many_outputs;
  auto& first = many_outputs.output(0);
  auto& second = many_outputs.output(1);
  auto& third = many_outputs.output(2);
  for (uint64_t i = 1; i <= 3; ++i) {
    CHECK(many_outputs.input().Push(MakeFrame(i)));
  }
  CHECK(many_outputs.Dispatch() == 3);
  CheckFrames(first, 1, 3);
  CheckFrames(second, 1, 3);
  CheckFrames(third, 1, 3);
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(many_outputs.output_drop_count(i) == 0);
  }
}

void TestFullOutputIsIndependent() {
  thyme::LocalFrameHub<Payload, 4, 3> hub;
  auto& full = hub.output(0);
  auto& partially_full = hub.output(1);
  auto& empty = hub.output(2);
  HealthSlot full_health{};
  HealthSlot partially_full_health{};
  HealthSlot empty_health{};
  CHECK(hub.BindOutput(0, &full_health));
  CHECK(hub.BindOutput(1, &partially_full_health));
  CHECK(hub.BindOutput(2, &empty_health));

  for (uint64_t i = 100; i < 104; ++i) {
    CHECK(full.Push(MakeFrame(i)));
  }
  CHECK(partially_full.Push(MakeFrame(200)));
  for (uint64_t i = 1; i <= 3; ++i) {
    CHECK(hub.input().Push(MakeFrame(i)));
  }
  CHECK(hub.Dispatch() == 3);
  CHECK(hub.output_drop_count(0) == 3);
  CHECK(hub.output_drop_count(1) == 0);
  CHECK(hub.output_drop_count(2) == 0);
  CHECK(full_health.drop_cnt.load(std::memory_order_relaxed) == 3);
  CHECK(partially_full_health.drop_cnt.load(std::memory_order_relaxed) == 0);
  CHECK(empty_health.drop_cnt.load(std::memory_order_relaxed) == 0);
  CheckFrames(full, 100, 4);

  TestFrame frame{};
  CHECK(partially_full.TryPop(frame));
  CheckFrame(frame, 200);
  CheckFrames(partially_full, 1, 3);
  CheckFrames(empty, 1, 3);

  CHECK(hub.input().Push(MakeFrame(4)));
  CHECK(hub.Dispatch() == 1);
  CheckFrames(full, 4, 1);
  CheckFrames(partially_full, 4, 1);
  CheckFrames(empty, 4, 1);
  CHECK(hub.output_drop_count(0) == 3);
  CHECK(hub.output_drop_count(1) == 0);
  CHECK(hub.output_drop_count(2) == 0);
  CHECK(full_health.drop_cnt.load(std::memory_order_relaxed) == 3);
}

void TestWaitableAndRaii() {
  thyme::LocalFrameHub<Payload, 4, 1> hub;
  auto& output = hub.output(0);
  CountingWaitable target;
  hub.SetNotify(&target);
  CHECK(hub.empty());
  CHECK(hub.Depth() == 0);
  CHECK(hub.NotifyWakeWord() == hub.input().NotifyWakeWord());

  CHECK(hub.input().Push(MakeFrame(1)));
  CHECK(target.count.load(std::memory_order_relaxed) == 1);
  CHECK(!hub.empty());
  CHECK(hub.Depth() == 1);
  CHECK(hub.Dispatch() == 1);

  std::thread waiter([&] {
    hub.WaitUntil(thyme_test::MonoNs() + 5000000000ULL);
  });
  CHECK(thyme_test::WaitFor([&] { return hub.input().waiter_count() > 0; }));
  CHECK(hub.input().Push(MakeFrame(2)));
  waiter.join();
  CHECK(target.count.load(std::memory_order_relaxed) == 2);
  CHECK(hub.Dispatch() == 1);

  std::thread notified([&] {
    hub.WaitUntil(thyme_test::MonoNs() + 5000000000ULL);
  });
  CHECK(thyme_test::WaitFor([&] { return hub.input().waiter_count() > 0; }));
  hub.Notify();
  notified.join();
  CHECK(target.count.load(std::memory_order_relaxed) == 2);

  TestFrame* slot = hub.input().BeginWrite();
  CHECK(slot != nullptr);
  *slot = MakeFrame(99);
  hub.input().AbortWrite();
  CHECK(hub.empty());
  CHECK(target.count.load(std::memory_order_relaxed) == 2);
  slot = hub.input().BeginWrite();
  CHECK(slot != nullptr);
  *slot = MakeFrame(42);
  hub.input().CommitWrite();
  CHECK(target.count.load(std::memory_order_relaxed) == 3);
  CHECK(hub.Dispatch() == 1);
  hub.SetNotify(nullptr);

  TestFrame frame{};
  CHECK(output.TryPop(frame));
  CheckFrame(frame, 1);
  CHECK(output.TryPop(frame));
  CheckFrame(frame, 2);
  CHECK(output.TryPop(frame));
  CheckFrame(frame, 42);
  CHECK(!output.TryPop(frame));
}

class SharedHubWorker final : public thyme::EventThread {
 public:
  using Hub = thyme::LocalFrameHub<Payload, 4, 1>;
  using thyme::EventThread::EventThread;

  void Dispatch() override {
    dispatched.fetch_add(static_cast<uint64_t>(first->Dispatch() + second->Dispatch()),
                         std::memory_order_relaxed);
  }

  Hub* first = nullptr;
  Hub* second = nullptr;
  std::atomic<uint64_t> dispatched{0};
};

void TestSharedEventThread() {
  SharedHubWorker::Hub first;
  SharedHubWorker::Hub second;
  auto& first_output = first.output(0);
  auto& second_output = second.output(0);

  thyme::HealthSlot health{};
  SharedHubWorker worker("local_hub_test", &health, 100);
  worker.first = &first;
  worker.second = &second;
  thyme::Waitable* inputs[] = {&first, &second};
  worker.SetWaitInputs(inputs, 2);
  CHECK(worker.Start());
  CHECK(first.input().Push(MakeFrame(11)));
  CHECK(second.input().Push(MakeFrame(22)));
  CHECK(thyme_test::WaitFor([&] {
    return worker.dispatched.load(std::memory_order_relaxed) == 2;
  }));
  worker.RequestStop();
  worker.Join();
  CheckFrames(first_output, 11, 1);
  CheckFrames(second_output, 22, 1);
}

void TestInputRecordTapOnly() {
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = static_cast<uint32_t>(sizeof(TestFrame));
  std::snprintf(desc.name, sizeof(desc.name), "LocalHubFrame");
  RecordMirrorRingSpec spec{static_cast<uint32_t>(sizeof(TestFrame)), 8};

  char page_name[64];
  std::snprintf(page_name, sizeof(page_name), "/local_hub_tap_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(page_name);
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page_name, &desc, &spec, 1, 1));
  RecordMirrorRingPage* page = mirror.Page();
  CHECK(page != nullptr && page->Validate());

  {
    thyme::LocalFrameHub<Payload, 4, 2> hub;
    auto& first = hub.output(0);
    auto& second = hub.output(1);
    hub.input().SetRecordTap(page, 0);
    CHECK(hub.input().record_tap_armed());
    CHECK(!first.record_tap_armed());
    CHECK(!second.record_tap_armed());

    TestFrame* slot = hub.input().BeginWrite();
    CHECK(slot != nullptr);
    *slot = MakeFrame(99);
    hub.input().AbortWrite();
    CHECK(page->RingHead(0) == 0);

    for (uint64_t i = 1; i <= 3; ++i) {
      CHECK(hub.input().Push(MakeFrame(i)));
    }
    CHECK(hub.Dispatch() == 3);
    CHECK(page->RingHead(0) == 3);
    CheckFrames(first, 1, 3);
    CheckFrames(second, 1, 3);
    CHECK(page->RingHead(0) == 3);

    for (uint64_t i = 0; i < 3; ++i) {
      TestFrame frame{};
      uint32_t size = 0;
      uint64_t mono_ns = 0;
      uint32_t source_sequence = 0;
      CHECK(page->ReadRingSample(0, i, reinterpret_cast<uint8_t*>(&frame), sizeof(frame),
                                 size, mono_ns, source_sequence));
      CHECK(size == sizeof(TestFrame));
      CheckFrame(frame, i + 1);
      CHECK(source_sequence == i);
    }

    hub.input().ClearRecordTap();
    CHECK(hub.input().Push(MakeFrame(4)));
    CHECK(hub.Dispatch() == 1);
    CHECK(page->RingHead(0) == 3);
  }
  mirror.Shutdown(true);
}

}  // namespace

int main() {
  static_assert(std::is_base_of<thyme::Waitable,
                                thyme::LocalFrameHub<Payload, 4, 3>>::value,
                "LocalFrameHub must be waitable");
  TestOutputCountsAndOrdering();
  TestFullOutputIsIndependent();
  TestWaitableAndRaii();
  TestSharedEventThread();
  TestInputRecordTapOnly();
  std::printf("test_local_hub ok\n");
  return 0;
}
