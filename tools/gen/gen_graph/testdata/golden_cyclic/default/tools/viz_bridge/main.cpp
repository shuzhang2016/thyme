#include "generated/record_registry.h"
#include "support/record_mirror_ctx.h"
#include "support/viz_poll.h"

#include "thyme/viz/foxglove_ws.h"
#include "thyme/viz/viz_config.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

void PrintUsage() {
  std::fprintf(stderr,
              "usage: viz_bridge [--config PATH] [--shm NAME] [--bind HOST]\n"
              "                  [--port PORT] [--hz RATE] [--seconds N]\n");
}

struct Cli {
  const char* config = nullptr;
  const char* shm = nullptr;
  const char* bind = nullptr;
  const char* port = nullptr;
  const char* hz = nullptr;
  const char* seconds = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
  Cli cli{};
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      cli.config = argv[++i];
    } else if (std::strcmp(argv[i], "--shm") == 0 && i + 1 < argc) {
      cli.shm = argv[++i];
    } else if (std::strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
      cli.bind = argv[++i];
    } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      cli.port = argv[++i];
    } else if (std::strcmp(argv[i], "--hz") == 0 && i + 1 < argc) {
      cli.hz = argv[++i];
    } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      cli.seconds = argv[++i];
    } else {
      PrintUsage();
      return 2;
    }
  }

  thyme::VizBridgeConfig cfg{};
  if (cli.config != nullptr) {
    if (!LoadVizConfig(cli.config, &cfg)) return 2;
  }

  const char* shm = cli.shm != nullptr ? cli.shm : cfg.shm.c_str();
  const char* bind_host =
      cli.bind != nullptr ? cli.bind : cfg.bind.c_str();
  int port = cli.port != nullptr ? std::atoi(cli.port) : cfg.port;
  int hz = cli.hz != nullptr ? std::atoi(cli.hz) : cfg.hz;
  int seconds =
      cli.seconds != nullptr ? std::atoi(cli.seconds) : cfg.seconds;

  if (shm == nullptr || shm[0] == '\0') {
    PrintUsage();
    return 2;
  }
  if (hz <= 0) hz = 10;
  // 1000/hz would be 0 ms above 1000 Hz (busy loop); reject before attaching.
  if (hz > 1000) {
    std::fprintf(stderr, "viz_bridge: hz %d too high (max 1000)\n", hz);
    return 2;
  }
  if (port <= 0 || port > 65535) {
    std::fprintf(stderr, "viz_bridge: invalid port %d\n", port);
    return 2;
  }

  bool slot_enabled[kRecordRegistryCount]{};
  const std::vector<std::string>* whitelist =
      cfg.channels.empty() ? nullptr : &cfg.channels;
  if (whitelist != nullptr) {
    std::string err;
    if (!viz_poll::ResolveChannelFilter(cfg.channels, slot_enabled, &err)) {
      std::fprintf(stderr, "viz_bridge: %s\n", err.c_str());
      return 2;
    }
  }

  if (!record_mirror::AttachReadOnly(shm)) {
    std::fprintf(stderr, "viz_bridge: cannot attach %s\n", shm);
    return 1;
  }
  thyme::RecordMirrorRingPage* page = record_mirror::Page();
  if (page == nullptr || !page->Validate()) {
    std::fprintf(stderr, "viz_bridge: invalid mirror page\n");
    return 1;
  }

  const auto channels = viz_poll::BuildChannels(whitelist);
  if (channels.empty() && whitelist != nullptr) {
    std::fprintf(stderr, "viz_bridge: no channels to advertise\n");
    return 2;
  }

  thyme::FoxgloveWsServer server;
  server.SetSessionId("thyme-viz-1");
  if (!server.Listen(bind_host, static_cast<uint16_t>(port))) {
    std::fprintf(stderr, "viz_bridge: listen %s:%d failed\n", bind_host, port);
    return 1;
  }
  server.Advertise(channels);

  std::printf("viz_bridge: ws://%s:%d mirror=%s hz=%d channels=%zu",
              bind_host, port, shm, hz, channels.size());
  if (cli.config != nullptr) {
    std::printf(" config=%s", cli.config);
  }
  if (!cfg.layout_path.empty()) {
    std::printf(" layout=%s", cfg.layout_path.c_str());
  }
  std::printf(" (no file I/O)\n");

  viz_poll::SlotCursor cur{};
  viz_poll::Stats stats{};
  const bool* enabled_mask = whitelist != nullptr ? slot_enabled : nullptr;
  const auto period = std::chrono::milliseconds(1000 / hz);
  const bool run_forever = seconds <= 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

  while (run_forever || std::chrono::steady_clock::now() < deadline) {
    server.Poll();
    if (server.HasSubscribers()) {
      thyme::RecordMirrorRingPage* live = record_mirror::Page();
      if (live != nullptr && live->Validate()) {
        viz_poll::PollOnce(*live, cur, server, &stats, enabled_mask);
      }
    }
    std::this_thread::sleep_for(period);
  }

  server.Close();
  uint64_t drops = 0;
  uint64_t oversize_drops = 0;
  thyme::RecordMirrorRingPage* live = record_mirror::Page();
  if (live != nullptr && live->Validate()) {
    drops = live->mirror_odd_skip_count() + live->oversize_drop_count();
    oversize_drops = live->oversize_drop_count();
  }
  record_mirror::Shutdown(false);
  std::printf("viz_bridge: sent=%zu polled=%zu mirror_drops=%llu oversize_drops=%llu\n",
              stats.messages_sent, stats.slots_polled,
              static_cast<unsigned long long>(drops),
              static_cast<unsigned long long>(oversize_drops));
  std::fflush(stdout);
  return 0;
}
