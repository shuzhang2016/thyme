#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace thyme {

// TRG-15 slice: the drain thread is the only thread that runs Tick and Call.
// Call posts one job and waits until that thread finishes it. Calling Call
// from the tick callback, or from a job, deadlocks. Stop joins the thread; it
// does not shut the recorder down.
class RecorderDrain {
 public:
  using TickFn = void (*)(void*);

  RecorderDrain(TickFn tick, void* ctx, uint64_t period_ns);
  ~RecorderDrain();

  RecorderDrain(const RecorderDrain&) = delete;
  RecorderDrain& operator=(const RecorderDrain&) = delete;

  void Start();
  void Stop();
  // True while another thread holds the drain mutex (Tick or a job).
  bool Contended();

  template <class F>
  auto Call(F&& fn) -> decltype(fn()) {
    using Result = decltype(fn());
    if constexpr (std::is_void<Result>::value) {
      auto* target = &fn;
      Submit([](void* p) { (*static_cast<F*>(p))(); }, target);
    } else {
      struct Box {
        F* fn;
        Result result;
      };
      Box box{&fn, Result{}};
      Submit(
          [](void* p) {
            auto* slot = static_cast<Box*>(p);
            slot->result = (*slot->fn)();
          },
          &box);
      return box.result;
    }
  }

 private:
  void Loop();
  void Submit(void (*run)(void*), void* arg);

  TickFn tick_;
  void* ctx_;
  uint64_t period_ns_;
  std::mutex mu_;
  std::condition_variable cv_;
  void (*job_)(void*) = nullptr;
  void* job_arg_ = nullptr;
  bool job_ready_ = false;
  bool job_done_ = false;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace thyme
