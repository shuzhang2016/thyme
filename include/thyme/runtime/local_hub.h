#pragma once

#include "thyme/contracts/frame.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/write_section.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace thyme {

template <typename T, std::size_t Cap, std::size_t MaxOutputs>
class LocalFrameHub final : public Waitable {
  static_assert(Cap > 0, "LocalFrameHub capacity must be positive");
  static constexpr std::size_t kStorageOutputs = MaxOutputs == 0 ? 1 : MaxOutputs;

 public:
  using Pipe = NotifPipe<Frame<T>, Cap>;

  LocalFrameHub() = default;
  LocalFrameHub(const LocalFrameHub&) = delete;
  LocalFrameHub& operator=(const LocalFrameHub&) = delete;

  Pipe& input() { return input_; }
  const Pipe& input() const { return input_; }

  Pipe& output(std::size_t index) { return outputs_[index]; }
  const Pipe& output(std::size_t index) const { return outputs_[index]; }

  bool BindOutput(std::size_t index, HealthSlot* health) {
    if (index >= MaxOutputs) {
      return false;
    }
    output_health_[index] = health;
    return true;
  }

  std::size_t Dispatch() {
    std::size_t dispatched = 0;
    Frame<T> frame{};
    while (input_.TryPop(frame)) {
      ++dispatched;
      for (std::size_t i = 0; i < MaxOutputs; ++i) {
        WriteSection<Pipe> section(outputs_[i]);
        if (!section) {
          drops_[i].fetch_add(1, std::memory_order_relaxed);
          if (output_health_[i] != nullptr) {
            output_health_[i]->AddDrops(1);
          }
          continue;
        }
        *section = frame;
        section.commit();
      }
    }
    return dispatched;
  }

  uint64_t output_drop_count(std::size_t index) const {
    return index < MaxOutputs ? drops_[index].load(std::memory_order_relaxed) : 0;
  }

  bool empty() const override { return input_.empty(); }
  void SetNotify(Waitable* target) override { input_.SetNotify(target); }
  void WaitUntil(uint64_t deadline_ns) override { input_.WaitUntil(deadline_ns); }
  void Notify() override { input_.Notify(); }
  std::atomic<uint32_t>* NotifyWakeWord() const override {
    return input_.NotifyWakeWord();
  }
  uint64_t Depth() const override { return input_.Depth(); }

 private:
  Pipe input_;
  Pipe outputs_[kStorageOutputs];
  HealthSlot* output_health_[kStorageOutputs]{};
  std::atomic<uint64_t> drops_[kStorageOutputs]{};
};

}  // namespace thyme
