#include "generated/wiring.h"
#include "generated/mp_roles.h"
#include "thyme/app/replay_mode.h"
#include "thyme/runtime/app.h"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
  // Replay is active with full inject: the generated base Loop must pause the
  // producer (prod is the only data producer) while the consumer and the
  // health reporter keep running. This is the record/replay cross-cutting
  // sunk into the generated base; the node Loop (OnLoop) holds no replay code.
  thyme::app::replay_mode::SetActive(true);
  thyme::app::replay_mode::SetFullInject(true);

  auto p = Pipeline::Create();
  p->Start();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!thyme::App::stop_requested() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  p->RequestStop();
  p->Join();
  // Compile-time exercise of the generated role dispatch: node 0 is the
  // unknown producer, so MakeKnownThread must fall through to nullptr.
  auto locals = RoleLocals::Create();
  thyme::CyclicThread* dispatched =
      MakeKnownThread(0, *locals, p->health.data());
  const bool dispatch_ok = (dispatched == nullptr);
  // The producer's OnLoop adds one drop per execution; under replay it must
  // never run (base Loop guard), while the consumer and the health reporter
  // keep their algorithms running.
  const uint64_t prod_drops = p->health[0].drop_cnt.load();
  const uint64_t consumer_drops = p->health[1].drop_cnt.load();
  const uint64_t reporter_drops = p->health[2].drop_cnt.load();
  const bool prod_paused = (prod_drops == 0);
  const bool consumer_running = (consumer_drops >= 1);
  const bool reporter_running = (reporter_drops >= 1);
  std::fprintf(stderr,
               "drops prod=%llu consumer=%llu reporter=%llu dispatch_ok=%d\n",
               static_cast<unsigned long long>(prod_drops),
               static_cast<unsigned long long>(consumer_drops),
               static_cast<unsigned long long>(reporter_drops),
               dispatch_ok ? 1 : 0);
  return (dispatch_ok && prod_paused && consumer_running && reporter_running)
             ? 0
             : 1;
}
