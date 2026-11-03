#include "generated/wiring.h"
#include "support/ad_stack_args.h"
#include "support/record_mirror_ctx.h"
#include "support/replay_runner.h"
#include "thyme/app/replay_mode.h"
#include "thyme/runtime/app.h"
#include "mp/mp_entry.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

int main(int argc, char** argv) {
  AdStackArgs args;
  {
    std::string err;
    if (!ParseAdStackArgs(argc, argv, args, &err)) {
      std::fprintf(stderr, "ad_stack: %s\n%s", err.c_str(), AdStackUsage());
      return 2;
    }
  }
  const char* role = args.role;
  const char* shm_prefix = args.shm_prefix;
  const bool mp = args.mp;
  bool record = args.record;
  const char* record_shm = args.record_shm;
  const char* relay_recorder_socket = args.relay_recorder_socket;
  const char* replay_bin = args.replay_bin;
  ReplayOptions replay_opt = args.replay;
  const int seconds = args.seconds;

  if (replay_bin != nullptr) {
    record = false;
    thyme::app::replay_mode::SetActive(true);
  }

  if (mp && role != nullptr) {
    std::fprintf(stderr, "ad_stack: --mp and --role are mutually exclusive\n");
    return 2;
  }

  if (role != nullptr && shm_prefix == nullptr) {
    std::fprintf(stderr, "ad_stack: --role requires --shm-prefix\n");
    return 2;
  }

  if (replay_bin != nullptr && mp) {
    std::fprintf(stderr, "ad_stack: --replay and --mp are mutually exclusive\n");
    return 2;
  }

  if (role != nullptr && shm_prefix != nullptr) {
    return RunRole(shm_prefix, role, record_shm, args.proc_idx);
  }

  if (mp) {
    return RunSupervisor(argv[0], seconds, record, record_shm,
                         relay_recorder_socket);
  }

  std::puts("ad_stack v1");

#ifdef THYME_GRAPH_HAS_RECORD
  std::string mirror_name = record_shm != nullptr ? record_shm : "";
  if (record && mirror_name.empty()) mirror_name = record_mirror::DefaultShmName();
  if (record) {
    if (!record_mirror::Create(mirror_name.c_str())) {
      std::fprintf(stderr, "ad_stack: record mirror Create(%s) failed\n",
                   mirror_name.c_str());
      return 1;
    }
    std::printf("record mirror: %s\n", mirror_name.c_str());
  }
#endif

  auto p = Pipeline::Create();
  p->Start();

  ReplayStats replay_stats{};
  if (replay_bin != nullptr) {
    if (replay_opt.as_fast) {
      std::printf("replay: %s (as-fast)\n", replay_bin);
    } else {
      std::printf("replay: %s (rate=%g)\n", replay_bin, replay_opt.rate);
    }
    replay_stats = RunReplay(*p, replay_bin, replay_opt);
  } else {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!thyme::App::stop_requested() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  p->RequestStop();
  p->Join();

  if (record) record_mirror::Shutdown(true);

  // Loop-count summary: iterate the generated node table and dispatch to each
  // thread member by id (no per-node member literals here).
  bool ok = true;
  for (std::size_t i = 0; i < kGraphNodesCount; ++i) {
    const char* id = kGraphNodes[i].id;
    thyme::CyclicThread* ct = p->ThreadByNodeId(id);
    if (ct != nullptr) {
      std::printf("%-12s loops=%llu\n", id,
                  static_cast<unsigned long long>(ct->loop_count()));
      if (ct->loop_count() < 1) ok = false;
      continue;
    }
#ifdef THYME_GRAPH_HAS_EVENT_NODES
    thyme::EventThread* et = p->EventThreadByNodeId(id);
    if (et != nullptr) {
      std::printf("%-12s handled=%llu\n", id,
                  static_cast<unsigned long long>(et->handled_count()));
      // An event node with zero handled events is not a smoke failure:
      // it is alive and wired, just idle (stub producers push nothing).
      continue;
    }
#endif
    std::fprintf(stderr, "ad_stack: no thread for node %s\n", id);
    ok = false;
  }

  if (replay_bin != nullptr) {
    std::printf("replay injected=%zu failed=%zu\n",
                replay_stats.entries_injected, replay_stats.inject_failures);
    return replay_stats.entries_injected > 0 ? 0 : 1;
  }

  return ok ? 0 : 1;
}
