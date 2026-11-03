// OVERLAY_MAIN_MARKER: this file replaces the built-in scaffold src/main.cpp
// when the project overlay ships its own (overlay wins over the base).
#include "generated/wiring.h"
#include "thyme/runtime/app.h"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
  auto p = Pipeline::Create();
  p->Start();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!thyme::App::stop_requested() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  p->RequestStop();
  p->Join();
  return 0;
}
