// main.cpp — thin entry point for thyme-replay (REP-08).
//
// All command logic lives in replay_cli.cpp / replay_runner.cpp so it can be
// tested in-process. This file only wires signals: SIGINT/SIGTERM set a
// sig_atomic_t flag, and a monitor thread turns it into PlayControl::
// RequestStop() in normal (non-signal) context, which calls
// ReplaySession::Stop() and lets the run exit cleanly.

#include "replay_cli.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile sig_atomic_t g_signal = 0;

extern "C" void OnSignal(int) { g_signal = 1; }

}  // namespace

int main(int argc, char** argv) {
  struct sigaction sa {};
  sa.sa_handler = &OnSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // no SA_RESTART: a signal interrupts blocking calls
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  thyme::replaycli::PlayControl control;
  std::atomic<bool> done{false};
  std::thread monitor([&control, &done] {
    while (!done.load(std::memory_order_relaxed)) {
      if (g_signal != 0) {
        control.RequestStop();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  const int rc =
      thyme::replaycli::RunCli(args, std::cout, std::cerr, &control);
  done.store(true, std::memory_order_relaxed);
  monitor.join();
  return rc;
}
