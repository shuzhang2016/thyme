// ARCH-19: short-window A/B for direct primitives vs the Channel facade.

#include "thyme/runtime/channel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef THYME_BENCH_BUILD_TYPE
#define THYME_BENCH_BUILD_TYPE "unknown"
#endif

namespace {

constexpr std::size_t kRepeats = 5;
constexpr std::size_t kSmallOperations = 200000;
constexpr std::size_t kPinnedSlotOperations = 20000;
constexpr std::uint32_t kMagic = 0x42434E47u;
constexpr std::uint16_t kVersion = 1;
constexpr std::uint64_t kSessionId = 19;

constexpr std::uint32_t kSmallLatestSemantics = thyme::ChannelSemanticsPack(
    thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kLatest,
    thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kOverwrite);
constexpr std::uint32_t kSmallQueueSemantics = thyme::ChannelSemanticsPack(
    thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue,
    thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop);
constexpr std::uint32_t kBigLatestSemantics = thyme::ChannelSemanticsPack(
    thyme::ChannelTransportKind::kPinnedSlot, thyme::ChannelRetention::kLatest,
    thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop);
constexpr std::uint32_t kBigQueueSemantics = thyme::ChannelSemanticsPack(
    thyme::ChannelTransportKind::kPinnedSlot, thyme::ChannelRetention::kQueue,
    thyme::ChannelReaders::kMany, thyme::ChannelOverflow::kDrop);

struct Small {
  std::uint64_t value;
};

struct PinnedSlot {
  std::uint64_t value;
  std::uint8_t bytes[70000 - sizeof(std::uint64_t)];
};

struct PairTiming {
  double direct_ns_per_op = 0.0;
  double facade_ns_per_op = 0.0;
};

struct Comparison {
  const char* name = "";
  const char* path = "";
  std::size_t payload_bytes = 0;
  std::size_t operations = 0;
  PairTiming timing;
  bool ok = false;
};

std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

template <typename Op>
double RunBatch(std::size_t operations, Op& op) {
  const std::uint64_t begin = NowNs();
  for(std::size_t i = 0; i < operations; ++i) {
    op(i);
  }
  const std::uint64_t end = NowNs();
  return static_cast<double>(end - begin) / static_cast<double>(operations);
}

double Median(std::array<double, kRepeats> values) {
  std::sort(values.begin(), values.end());
  return values[kRepeats / 2];
}

template <typename DirectOp, typename FacadeOp>
PairTiming MeasurePair(std::size_t operations, DirectOp&& direct_op,
                       FacadeOp&& facade_op) {
  const std::size_t warmup = std::min<std::size_t>(operations, 256);
  for(std::size_t i = 0; i < warmup; ++i) {
    direct_op(i);
    facade_op(i);
  }

  std::array<double, kRepeats> direct_samples{};
  std::array<double, kRepeats> facade_samples{};
  for(std::size_t round = 0; round < kRepeats; ++round) {
    if(round % 2 == 0) {
      direct_samples[round] = RunBatch(operations, direct_op);
      facade_samples[round] = RunBatch(operations, facade_op);
    } else {
      facade_samples[round] = RunBatch(operations, facade_op);
      direct_samples[round] = RunBatch(operations, direct_op);
    }
  }
  return {Median(direct_samples), Median(facade_samples)};
}

void RunSmallLatest(std::vector<Comparison>* metrics) {
  using Frame = thyme::Frame<Small>;
  using Facade = thyme::Channel<Small, kSmallLatestSemantics,
                                thyme::ChannelBackend::kLocal>;
  thyme::LatestSlot<Frame> direct_slot;
  thyme::LatestSlot<Frame> facade_slot;
  Facade facade({kMagic, kVersion, kSessionId});
  facade.Bind(&facade_slot);

  std::uint64_t direct_sequence = 0;
  std::uint64_t direct_seen = 0;
  bool direct_ok = true;
  auto direct_op = [&](std::size_t) {
    if(!direct_ok) {
      return;
    }
    ++direct_sequence;
    const std::uint64_t mono_ns = NowNs();
    Frame* write = direct_slot.BeginWrite();
    thyme::FrameInit(*write, static_cast<std::uint32_t>(direct_sequence), mono_ns,
                     kSessionId);
    write->payload.value = direct_sequence;
    thyme::FrameSeal(*write, kMagic, kVersion);
    direct_slot.CommitWrite();
    Frame read{};
    if(!direct_slot.ReadIfNew(direct_seen, read) ||
       !thyme::FrameVerify(read, kMagic, kVersion) ||
       read.payload.value != direct_sequence) {
      direct_ok = false;
    }
  };

  std::uint64_t facade_sequence = 0;
  bool facade_ok = true;
  Facade::Sample sample;
  auto facade_op = [&](std::size_t) {
    if(!facade_ok) {
      return;
    }
    ++facade_sequence;
    auto write = facade.BeginWrite();
    if(write.status != thyme::ChannelStatus::kOk) {
      facade_ok = false;
      return;
    }
    write.payload->value = facade_sequence;
    if(facade.CommitWrite(&write) != thyme::ChannelStatus::kOk ||
       facade.TryRead(&sample) != thyme::ChannelStatus::kOk ||
       sample.payload->value != facade_sequence ||
       sample.sequence != facade_sequence) {
      facade_ok = false;
      return;
    }
    sample.Reset();
  };

  Comparison comparison;
  comparison.name = "small_latest_publish_read";
  comparison.path = "latest_slot_frame";
  comparison.payload_bytes = sizeof(Small);
  comparison.operations = kSmallOperations;
  comparison.timing = MeasurePair(kSmallOperations, direct_op, facade_op);
  comparison.ok = direct_ok && facade_ok;
  metrics->push_back(comparison);
}

void RunSmallQueue(std::vector<Comparison>* metrics) {
  using Frame = thyme::Frame<Small>;
  using Facade = thyme::Channel<Small, kSmallQueueSemantics,
                                thyme::ChannelBackend::kLocal>;
  thyme::NotifPipe<Frame, 4> direct_pipe;
  thyme::NotifPipe<Frame, 4> facade_pipe;
  Facade facade({kMagic, kVersion, kSessionId});
  facade.Bind(&facade_pipe);

  std::uint64_t direct_sequence = 0;
  bool direct_ok = true;
  auto direct_op = [&](std::size_t) {
    if(!direct_ok) {
      return;
    }
    ++direct_sequence;
    const std::uint64_t mono_ns = NowNs();
    Frame* write = direct_pipe.BeginWrite();
    if(write == nullptr) {
      direct_ok = false;
      return;
    }
    thyme::FrameInit(*write, static_cast<std::uint32_t>(direct_sequence), mono_ns,
                     kSessionId);
    write->payload.value = direct_sequence;
    thyme::FrameSeal(*write, kMagic, kVersion);
    direct_pipe.CommitWrite();
    Frame read{};
    if(!direct_pipe.TryPop(read) || !thyme::FrameVerify(read, kMagic, kVersion) ||
       read.payload.value != direct_sequence) {
      direct_ok = false;
    }
  };

  std::uint64_t facade_sequence = 0;
  bool facade_ok = true;
  Facade::Sample sample;
  auto facade_op = [&](std::size_t) {
    if(!facade_ok) {
      return;
    }
    ++facade_sequence;
    auto write = facade.BeginWrite();
    if(write.status != thyme::ChannelStatus::kOk) {
      facade_ok = false;
      return;
    }
    write.payload->value = facade_sequence;
    if(facade.CommitWrite(&write) != thyme::ChannelStatus::kOk ||
       facade.TryRead(&sample) != thyme::ChannelStatus::kOk ||
       sample.payload->value != facade_sequence ||
       sample.sequence != facade_sequence) {
      facade_ok = false;
      return;
    }
    sample.Reset();
  };

  Comparison comparison;
  comparison.name = "small_queue_publish_read";
  comparison.path = "notif_pipe_frame";
  comparison.payload_bytes = sizeof(Small);
  comparison.operations = kSmallOperations;
  comparison.timing = MeasurePair(kSmallOperations, direct_op, facade_op);
  comparison.ok = direct_ok && facade_ok;
  metrics->push_back(comparison);
}

void RunBigLatest(std::vector<Comparison>* metrics) {
  using Envelope = thyme::ChannelEnvelope<PinnedSlot>;
  using Store = thyme::PinnedLatestStore<Envelope, 4>;
  using Facade = thyme::Channel<PinnedSlot, kBigLatestSemantics,
                                thyme::ChannelBackend::kLocal>;
  Store direct_store;
  Store facade_store;
  Facade facade;
  facade.Bind(&facade_store);

  std::uint64_t direct_sequence = 0;
  bool direct_ok = true;
  auto direct_op = [&](std::size_t) {
    if(!direct_ok) {
      return;
    }
    ++direct_sequence;
    const std::uint64_t mono_ns = NowNs();
    Envelope* write = direct_store.BeginWrite();
    if(write == nullptr) {
      direct_ok = false;
      return;
    }
    write->mono_ns = mono_ns;
    write->sequence = direct_sequence;
    write->payload.value = direct_sequence;
    direct_store.CommitWrite();
    Store::Borrowed read;
    if(!direct_store.TryRead(&read) || read->sequence != direct_sequence ||
       read->payload.value != direct_sequence) {
      direct_ok = false;
      return;
    }
    read.Reset();
  };

  std::uint64_t facade_sequence = 0;
  bool facade_ok = true;
  Facade::Sample sample;
  auto facade_op = [&](std::size_t) {
    if(!facade_ok) {
      return;
    }
    ++facade_sequence;
    auto write = facade.BeginWrite();
    if(write.status != thyme::ChannelStatus::kOk) {
      facade_ok = false;
      return;
    }
    write.payload->value = facade_sequence;
    if(facade.CommitWrite(&write) != thyme::ChannelStatus::kOk ||
       facade.TryRead(&sample) != thyme::ChannelStatus::kOk ||
       reinterpret_cast<const PinnedSlot*>(sample.data)->value != facade_sequence ||
       sample.sequence != facade_sequence) {
      facade_ok = false;
      return;
    }
    sample.Reset();
  };

  Comparison comparison;
  comparison.name = "pinned_slot_local_latest_lease_publish_read";
  comparison.path = "pinned_latest_lease";
  comparison.payload_bytes = sizeof(PinnedSlot);
  comparison.operations = kPinnedSlotOperations;
  comparison.timing = MeasurePair(kPinnedSlotOperations, direct_op, facade_op);
  comparison.ok = direct_ok && facade_ok;
  metrics->push_back(comparison);
}

void RunBigQueue(std::vector<Comparison>* metrics) {
  using Envelope = thyme::ChannelEnvelope<PinnedSlot>;
  using Ring = thyme::PinnedSpmcRing<Envelope, 4, 8>;
  using Facade = thyme::Channel<PinnedSlot, kBigQueueSemantics,
                                thyme::ChannelBackend::kLocal>;
  Ring direct_ring;
  Ring::Reader direct_reader;
  Ring facade_ring;
  Ring::Reader facade_reader;
  Facade facade;
  const bool bind_ok = direct_reader.Bind(&direct_ring) &&
                       facade_reader.Bind(&facade_ring);
  facade.Bind(&facade_ring);
  facade.BindReader(&facade_reader);

  std::uint64_t direct_sequence = 0;
  bool direct_ok = bind_ok;
  auto direct_op = [&](std::size_t) {
    if(!direct_ok) {
      return;
    }
    ++direct_sequence;
    const std::uint64_t mono_ns = NowNs();
    Envelope* write = direct_ring.BeginWrite();
    if(write == nullptr) {
      direct_ok = false;
      return;
    }
    write->mono_ns = mono_ns;
    write->sequence = direct_sequence;
    write->payload.value = direct_sequence;
    direct_ring.CommitWrite();
    Ring::Borrowed read;
    if(!direct_reader.TryRead(&read, thyme::PinnedReadMode::kFollow) ||
       read->sequence != direct_sequence ||
       read->payload.value != direct_sequence) {
      direct_ok = false;
      return;
    }
    read.Reset();
  };

  std::uint64_t facade_sequence = 0;
  bool facade_ok = bind_ok;
  Facade::Sample sample;
  auto facade_op = [&](std::size_t) {
    if(!facade_ok) {
      return;
    }
    ++facade_sequence;
    auto write = facade.BeginWrite();
    if(write.status != thyme::ChannelStatus::kOk) {
      facade_ok = false;
      return;
    }
    write.payload->value = facade_sequence;
    if(facade.CommitWrite(&write) != thyme::ChannelStatus::kOk ||
       facade.TryRead(&sample) != thyme::ChannelStatus::kOk ||
       reinterpret_cast<const PinnedSlot*>(sample.data)->value != facade_sequence ||
       sample.sequence != facade_sequence) {
      facade_ok = false;
      return;
    }
    sample.Reset();
  };

  Comparison comparison;
  comparison.name = "pinned_slot_local_queue_lease_publish_read";
  comparison.path = "pinned_queue_lease";
  comparison.payload_bytes = sizeof(PinnedSlot);
  comparison.operations = kPinnedSlotOperations;
  comparison.timing = MeasurePair(kPinnedSlotOperations, direct_op, facade_op);
  comparison.ok = direct_ok && facade_ok;
  metrics->push_back(comparison);
}

std::string JsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for(const char c : value) {
    switch(c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out;
}

std::string Toolchain() {
#if defined(__clang__)
  return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("GCC ") + __VERSION__;
#else
  return "unknown";
#endif
}

std::string MakeJson(const std::vector<Comparison>& metrics,
                     const std::string& tag, bool all_ok) {
  char row[1024];
  std::string out = "{\n";
  out += "  \"schema\": 1,\n";
  out += "  \"kind\": \"thyme-bench-channel-facade\",\n";
  out += "  \"task\": \"ARCH-19\",\n";
  out += "  \"runner_tag\": \"" + JsonEscape(tag) + "\",\n";
  out += "  \"toolchain\": \"" + JsonEscape(Toolchain()) + "\",\n";
  out += "  \"build_type\": \"" THYME_BENCH_BUILD_TYPE "\",\n";
  out += "  \"repeats\": " + std::to_string(kRepeats) + ",\n";
  out += "  \"statistic\": \"median_of_alternating_batch_ns_per_op\",\n";
  out += "  \"scope\": \"single_thread_publish_read_and_lease_release\",\n";
  out += "  \"timing_gate\": false,\n";
  out += "  \"target_board_conclusion\": false,\n";
  out += "  \"ok\": " + std::string(all_ok ? "true" : "false") + ",\n";
  out += "  \"benchmarks\": [\n";
  for(std::size_t i = 0; i < metrics.size(); ++i) {
    const Comparison& metric = metrics[i];
    const double direct_ns = metric.timing.direct_ns_per_op;
    const double facade_ns = metric.timing.facade_ns_per_op;
    const double ratio = direct_ns > 0.0 ? facade_ns / direct_ns : 0.0;
    std::snprintf(
        row, sizeof(row),
        "    {\"name\": \"%s\", \"path\": \"%s\", \"payload_bytes\": %llu, "
        "\"operations_per_batch_per_path\": %llu, "
        "\"measured_operations_per_path\": %llu, "
        "\"direct_ns_per_op\": %.4f, \"facade_ns_per_op\": %.4f, "
        "\"overhead_ns_per_op\": %.4f, \"facade_to_direct_ratio\": %.6f, "
        "\"relative_overhead_percent\": %.4f, \"ok\": %s}%s\n",
        metric.name, metric.path,
        static_cast<unsigned long long>(metric.payload_bytes),
        static_cast<unsigned long long>(metric.operations),
        static_cast<unsigned long long>(metric.operations * kRepeats), direct_ns,
        facade_ns, facade_ns - direct_ns, ratio, (ratio - 1.0) * 100.0,
        metric.ok ? "true" : "false", i + 1 < metrics.size() ? "," : "");
    out += row;
  }
  out += "  ]\n}\n";
  return out;
}

bool WriteJson(const std::string& path, const std::string& json) {
  FILE* file = std::fopen(path.c_str(), "w");
  if(file == nullptr) {
    return false;
  }
  const bool write_ok = std::fwrite(json.data(), 1, json.size(), file) == json.size();
  const bool close_ok = std::fclose(file) == 0;
  return write_ok && close_ok;
}

}  // namespace

int main(int argc, char** argv) {
  std::string json_path;
  std::string tag = "local";
  for(int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if(arg == "--json" && i + 1 < argc) {
      json_path = argv[++i];
    } else if(arg == "--tag" && i + 1 < argc) {
      tag = argv[++i];
    } else if(arg == "-h" || arg == "--help") {
      std::printf("usage: bench_channel_facade [--json PATH] [--tag RUNNER_TAG]\n");
      return 0;
    } else {
      std::fprintf(stderr, "bench_channel_facade: unknown or incomplete argument %s\n",
                   arg.c_str());
      return 2;
    }
  }

  std::vector<Comparison> metrics;
  RunSmallLatest(&metrics);
  RunSmallQueue(&metrics);
  RunBigLatest(&metrics);
  RunBigQueue(&metrics);
  bool all_ok = true;
  for(const Comparison& metric : metrics) {
    all_ok = all_ok && metric.ok;
  }

  const std::string json = MakeJson(metrics, tag, all_ok);
  std::fwrite(json.data(), 1, json.size(), stdout);
  if(!json_path.empty() && !WriteJson(json_path, json)) {
    std::fprintf(stderr, "bench_channel_facade: cannot write %s\n", json_path.c_str());
    return 1;
  }
  return all_ok ? 0 : 1;
}
