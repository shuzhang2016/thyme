#include "thyme/record/recorder_drain.h"

#include <chrono>

namespace thyme {

RecorderDrain::RecorderDrain(TickFn tick, void* ctx, uint64_t period_ns)
    : tick_(tick), ctx_(ctx), period_ns_(period_ns) {}

RecorderDrain::~RecorderDrain() { Stop(); }

void RecorderDrain::Start() {
  if (thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = false;
    job_ready_ = false;
    job_done_ = false;
  }
  thread_ = std::thread([this] { Loop(); });
}

void RecorderDrain::Stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

bool RecorderDrain::Contended() {
  if (!mu_.try_lock()) return true;
  mu_.unlock();
  return false;
}

void RecorderDrain::Submit(void (*run)(void*), void* arg) {
  std::unique_lock<std::mutex> lock(mu_);
  job_ = run;
  job_arg_ = arg;
  job_ready_ = true;
  job_done_ = false;
  cv_.notify_all();
  cv_.wait(lock, [this] { return job_done_; });
}

void RecorderDrain::Loop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    if (job_ready_ && !job_done_) {
      void (*run)(void*) = job_;
      void* arg = job_arg_;
      run(arg);
      job_done_ = true;
      cv_.notify_all();
      continue;
    }
    if (stop_) break;
    if (tick_ != nullptr) tick_(ctx_);
    if (stop_) break;
    cv_.wait_for(lock, std::chrono::nanoseconds(period_ns_), [this] {
      return stop_ || (job_ready_ && !job_done_);
    });
  }
}

}  // namespace thyme
