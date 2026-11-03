#include "support/replay_runner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "thyme/app/replay_mode.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/replay/small_struct_injector.h"
#include "thyme/replay/topology_map.h"
#include "thyme/runtime/app.h"

namespace replay_mode = thyme::app::replay_mode;

#ifdef THYME_GRAPH_HAS_RECORD

#include "generated/record_registry.h"
#include "generated/record_table.h"
#include "generated/replay_inject.h"
#include "generated/replay_stages.h"

namespace {

bool NameMatch(const char* name, const std::vector<std::string>& names) {
  for (const auto& n : names) {
    if (std::strcmp(name, n.c_str()) == 0) return true;
  }
  return false;
}

bool TopicAllowed(uint16_t struct_id, const ReplayOptions& opt) {
  if (struct_id >= kRecordRegistryCount) return false;
  const RecordStructMeta& m = kRecordRegistry[struct_id];
  if (opt.inject_all) {
    // Full replay: inject every recorded topic (HealthSnapshot excluded below).
  } else if (opt.inject_stage != nullptr) {
    if (!ReplayStageIncludes(struct_id, opt.inject_stage)) return false;
  } else if (!ReplayDefaultInjectAllowed(struct_id)) {
    return false;
  }
  if (!opt.include_topics.empty()) {
    return NameMatch(m.channel, opt.include_topics) ||
           NameMatch(m.cpp_type, opt.include_topics);
  }
  if (NameMatch(m.channel, opt.exclude_topics) ||
      NameMatch(m.cpp_type, opt.exclude_topics)) {
    return false;
  }
  if (std::strcmp(m.cpp_type, "HealthSnapshot") == 0) return false;
  return true;
}

bool GeneratedPublish(void* ctx, const void* payload, uint32_t size,
                      uint64_t mono_ns, uint32_t source_sequence,
                      uint16_t target_struct_id) {
  (void)mono_ns;
  (void)source_sequence;
  return ReplayInjectOne(*static_cast<Pipeline*>(ctx), target_struct_id,
                         payload, size);
}

const thyme::replay::TopologyMapping* FindMapping(
    const std::vector<thyme::replay::TopologyMapping>& mappings,
    uint16_t source_struct_id) {
  for (const auto& mapping : mappings) {
    if (mapping.source_struct_id == source_struct_id) {
      return &mapping;
    }
  }
  return nullptr;
}

void ResetReplayMode() {
  replay_mode::SetInjectStage(nullptr);
  replay_mode::SetFullInject(false);
  replay_mode::SetPaused(false);
  replay_mode::ClearStop();
  replay_mode::SetActive(false);
}

}  // namespace

ReplayStats RunReplay(Pipeline& pipeline, const char* bin_path,
                      const ReplayOptions& opt) {
  ReplayStats stats{};
  if (!opt.as_fast && opt.rate <= 0.0) {
    std::fprintf(stderr, "replay: --rate must be > 0 (got %g)\n", opt.rate);
    return stats;
  }
  replay_mode::ClearStop();
  replay_mode::SetPaused(false);
  replay_mode::SetActive(true);
  replay_mode::SetFullInject(opt.inject_all);
  replay_mode::SetInjectStage(opt.inject_stage);
  thyme::ThymeBinReader reader(bin_path);
  if (!reader.ok()) {
    std::fprintf(stderr, "replay: cannot open %s\n", bin_path);
    ResetReplayMode();
    return stats;
  }

  // A standalone .tbin is one explicit local channel. Package replay keeps its
  // own channel numbering in ReplayPackage and does not use this overload.
  thyme::replay::ReplayChannel source_channel;
  source_channel.id = 0;
  source_channel.kind = thyme::replay::ReplayChannelKind::kSmall;
  source_channel.name = "small";
  source_channel.registry = reader.registry();
  thyme::ThymeStructDesc generated_descs[thyme::kRecordMirrorRingMaxStructs]{};
  ThymeRecordFillStructDescs(generated_descs,
                             thyme::kRecordMirrorRingMaxStructs);
  std::vector<thyme::replay::TopologyMapping> mappings;
  std::vector<thyme::replay::SmallStructTarget> targets;
  mappings.reserve(reader.registry().size());
  targets.reserve(reader.registry().size());
  for (const thyme::ThymeStructDesc& source : reader.registry()) {
    thyme::replay::TopologyMapping mapping;
    const thyme::replay::TopologyMapStatus status =
        thyme::replay::ResolveTopologyMapping(source, generated_descs,
                                              kRecordRegistryCount, &mapping);
    if (!thyme::replay::TopologyMapStatusMapped(status) &&
        status != thyme::replay::TopologyMapStatus::kMissing) {
      std::fprintf(
          stderr, "replay: topology mapping failed for struct %u (%s): %s\n",
          static_cast<unsigned>(source.struct_id),
          thyme::replay::TopologyMapStatusName(status), mapping.detail.c_str());
      ResetReplayMode();
      return stats;
    }
    if (status == thyme::replay::TopologyMapStatus::kMissing) {
      mappings.push_back(mapping);
      continue;
    }
    thyme::replay::SmallStructTarget target;
    target.channel = source_channel.id;
    target.struct_id = source.struct_id;
    target.target_struct_id = mapping.target_struct_id;
    target.publish = &GeneratedPublish;
    target.publish_ctx = &pipeline;
    target.payload_capacity =
        generated_descs[mapping.target_struct_id].payload_size;
    targets.push_back(target);
    mappings.push_back(mapping);
  }

  thyme::replay::SmallStructInjector injector;
  if (injector.Open(source_channel, targets.data(),
                    static_cast<uint32_t>(targets.size())) !=
      thyme::replay::ReplayStatus::kOk) {
    std::fprintf(stderr, "replay: cannot open small-struct injector: %s\n",
                 injector.error().c_str());
    ResetReplayMode();
    return stats;
  }

  if (opt.from_mono_ns != 0 && !reader.SeekToMonoNs(opt.from_mono_ns) &&
      reader.HasIndex()) {
    std::fprintf(stderr, "replay: cannot seek %s to mono_ns=%llu\n", bin_path,
                 static_cast<unsigned long long>(opt.from_mono_ns));
    ResetReplayMode();
    return stats;
  }

  struct Row {
    uint64_t mono_ns = 0;
    thyme::ThymeBinDataEntry entry;
  };
  std::vector<Row> rows;
  thyme::ThymeBinDataEntry e{};
  while (reader.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
    ++stats.entries_read;
    if (e.type != thyme::ThymeBinEntryType::kData) continue;
    if (opt.from_mono_ns != 0 && e.mono_ns < opt.from_mono_ns) {
      ++stats.entries_skipped;
      continue;
    }
    if (opt.to_mono_ns != 0 && e.mono_ns > opt.to_mono_ns) {
      ++stats.entries_skipped;
      continue;
    }
    const thyme::replay::TopologyMapping* mapping =
        FindMapping(mappings, e.struct_id);
    if (mapping == nullptr ||
        !thyme::replay::TopologyMapStatusMapped(mapping->status)) {
      ++stats.entries_skipped;
      continue;
    }
    if (!TopicAllowed(mapping->target_struct_id, opt)) {
      ++stats.entries_skipped;
      continue;
    }
    rows.push_back(Row{e.mono_ns, e});
  }
  if (rows.empty()) {
    ResetReplayMode();
    return stats;
  }

  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.mono_ns < b.mono_ns; });

  const double rate = opt.rate > 0.0 ? opt.rate : 1.0;
  const uint64_t t0_mono = rows.front().mono_ns;
  const auto wall_t0 = std::chrono::steady_clock::now();

  for (std::size_t i = 0; i < rows.size(); ++i) {
    while (replay_mode::Paused() && !thyme::App::stop_requested() &&
           !replay_mode::StopRequested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (thyme::App::stop_requested() || replay_mode::StopRequested()) break;

    if (!opt.as_fast) {
      const uint64_t delta_ns = rows[i].mono_ns - t0_mono;
      const auto target = wall_t0 + std::chrono::nanoseconds(
                                        ReplayScaledDelayNs(delta_ns, rate));
      while (std::chrono::steady_clock::now() < target &&
             !thyme::App::stop_requested() && !replay_mode::StopRequested()) {
        if (replay_mode::Paused()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    if (thyme::App::stop_requested() || replay_mode::StopRequested()) break;

    const thyme::ThymeBinDataEntry& row = rows[i].entry;
    thyme::replay::ReplayEvent event;
    event.mono_ns = row.mono_ns;
    event.channel = source_channel.id;
    event.kind = thyme::replay::ReplayKind::kData;
    event.struct_id = row.struct_id;
    event.record_sequence = row.record_sequence;
    event.len = row.payload_size;
    event.payload = row.payload.data();
    event.source_sequence = row.source_sequence;
    if (injector.Inject(event) != thyme::replay::InjectStatus::kOk) {
      ++stats.inject_failures;
      continue;
    }
    ++stats.entries_injected;
  }
  ResetReplayMode();
  return stats;
}

#else  // THYME_GRAPH_HAS_RECORD

ReplayStats RunReplay(Pipeline&, const char*, const ReplayOptions&) {
  std::fprintf(stderr, "replay: graph has no recordable edges\n");
  return ReplayStats{};
}

#endif  // THYME_GRAPH_HAS_RECORD
