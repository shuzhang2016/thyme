#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "generated/record_manifest.h"
#include "generated/wiring.h"
#include "support/replay_runner.h"
#include "thyme/check.h"
#include "thyme/contracts/frame.h"
#include "thyme/record/thyme_bin.h"

namespace {

constexpr uint16_t kSourceStructId = 7;
constexpr uint64_t kRecordedSession = 0x1111;
constexpr uint32_t kFrameMagic = 0x70426B21u;
constexpr uint16_t kFrameVersion = 3;
constexpr uint64_t kMonoNs[3] = {100, 200, 300};
constexpr uint32_t kValues[3] = {11, 22, 33};

struct Row {
  uint16_t struct_id = kSourceStructId;
  uint64_t mono_ns = 0;
  uint32_t value = 0;
};

thyme::Frame<Foo> MakeFrame(uint32_t value, uint64_t mono_ns,
                            uint32_t sequence) {
  thyme::Frame<Foo> frame{};
  thyme::FrameInit(frame, sequence, mono_ns, kRecordedSession);
  frame.payload.value = value;
  thyme::FrameSeal(frame, kFrameMagic, kFrameVersion);
  return frame;
}

thyme::ThymeStructDesc MakeDesc(uint16_t struct_id, const char* channel,
                                uint16_t contract_version = 1) {
  thyme::ThymeStructDesc desc{};
  desc.struct_id = struct_id;
  desc.contract_version = contract_version;
  desc.payload_size = sizeof(thyme::Frame<Foo>);
  std::snprintf(desc.name, sizeof(desc.name), "Foo");
  std::snprintf(desc.channel, sizeof(desc.channel), "%s", channel);
  std::snprintf(desc.record_key, sizeof(desc.record_key), "%s", channel);
  return desc;
}

void WriteTbin(const std::string& path,
               const std::vector<thyme::ThymeStructDesc>& descs,
               const std::vector<Row>& rows, bool indexed) {
  thyme::ThymeBinFileSink sink(path.c_str());
  CHECK(sink.ok());
  thyme::ThymeBinEncoder encoder(&sink);
  const thyme::ThymeBinManifestBlock manifest =
      thyme::MakeThymeBinManifestBlock(kRecordManifestHash,
                                       thyme::kThymeBinManifestBlockVersion);
  CHECK(encoder.WriteHeader(99, rows.front().mono_ns, descs.data(),
                            descs.size(), &manifest));
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const Row& row = rows[i];
    const thyme::Frame<Foo> frame =
        MakeFrame(row.value, row.mono_ns, static_cast<uint32_t>(40 + i));
    CHECK(encoder.WriteData(i, row.struct_id, row.mono_ns,
                            static_cast<uint32_t>(40 + i), &frame,
                            sizeof(frame)));
  }
  if (indexed) {
    CHECK(encoder.Finish());
  }
  CHECK(sink.Sync());
}

struct Observed {
  uint64_t mono_ns = 0;
  uint32_t value = 0;
  uint32_t sequence = 0;
};

struct Result {
  ReplayStats stats;
  std::vector<Observed> rows;
};

Result Replay(const std::string& path, uint64_t from_mono_ns = 0,
              uint64_t to_mono_ns = 0) {
  auto pipeline = Pipeline::Create();
  ReplayOptions options;
  options.as_fast = true;
  options.inject_all = true;
  options.from_mono_ns = from_mono_ns;
  options.to_mono_ns = to_mono_ns;
  Result result;
  result.stats = RunReplay(*pipeline, path.c_str(), options);
  uint32_t index = 0;
  uint32_t generation = 0;
  while (pipeline->foo_ring.try_pop(index, generation)) {
    const thyme::Frame<Foo>& frame = pipeline->foo_pool.at(index);
    CHECK(thyme::FrameVerify(frame, kFrameMagic, kFrameVersion));
    CHECK(frame.hdr.session_id != kRecordedSession);
    result.rows.push_back(
        Observed{frame.hdr.mono_ns, frame.payload.value, frame.hdr.sequence});
    CHECK(pipeline->foo_pool.release(index, generation));
  }
  CHECK(pipeline->foo_ring.empty());
  return result;
}

void CheckRows(const std::vector<Observed>& rows,
               const std::vector<Observed>& expected) {
  CHECK(rows.size() == expected.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    CHECK(rows[i].mono_ns == expected[i].mono_ns);
    CHECK(rows[i].value == expected[i].value);
    CHECK(rows[i].sequence == expected[i].sequence);
  }
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 2);
  const std::string root = argv[1];
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const std::vector<Row> rows = {
      {kSourceStructId, kMonoNs[0], kValues[0]},
      {kSourceStructId, kMonoNs[1], kValues[1]},
      {kSourceStructId, kMonoNs[2], kValues[2]},
  };
  const std::vector<thyme::ThymeStructDesc> mapped = {
      MakeDesc(kSourceStructId, "payload")};
  const std::string indexed = root + "/indexed.tbin";
  const std::string unindexed = root + "/unindexed.tbin";
  WriteTbin(indexed, mapped, rows, true);
  WriteTbin(unindexed, mapped, rows, false);

  const Result full = Replay(indexed);
  CHECK(full.stats.entries_read == 3);
  CHECK(full.stats.entries_injected == 3);
  CHECK(full.stats.entries_skipped == 0);
  CHECK(full.stats.inject_failures == 0);
  CheckRows(full.rows, {{kMonoNs[0], kValues[0], 0},
                        {kMonoNs[1], kValues[1], 1},
                        {kMonoNs[2], kValues[2], 2}});
  std::vector<Observed> expected_suffix = {full.rows[1], full.rows[2]};
  expected_suffix[0].sequence = 0;
  expected_suffix[1].sequence = 1;

  const Result indexed_suffix = Replay(indexed, kMonoNs[1], kMonoNs[2]);
  CHECK(indexed_suffix.stats.entries_injected == 2);
  CHECK(indexed_suffix.stats.entries_skipped == 1);
  CHECK(indexed_suffix.stats.inject_failures == 0);
  CheckRows(indexed_suffix.rows, expected_suffix);
  const Result unindexed_suffix = Replay(unindexed, kMonoNs[1], kMonoNs[2]);
  CHECK(unindexed_suffix.stats.entries_injected == 2);
  CHECK(unindexed_suffix.stats.entries_skipped == 1);
  CHECK(unindexed_suffix.stats.inject_failures == 0);
  CheckRows(unindexed_suffix.rows, expected_suffix);

  const std::string mismatch = root + "/mismatch.tbin";
  WriteTbin(mismatch,
            {MakeDesc(kSourceStructId, "payload"), MakeDesc(8, "payload", 2)},
            {rows[0]}, true);
  const Result rejected = Replay(mismatch);
  CHECK(rejected.stats.entries_read == 0);
  CHECK(rejected.stats.entries_injected == 0);
  CHECK(rejected.stats.inject_failures == 0);
  CHECK(rejected.rows.empty());

  const std::string missing = root + "/missing.tbin";
  WriteTbin(missing, {MakeDesc(9, "not_in_this_graph")}, {rows[0]}, true);
  const Result skipped = Replay(missing);
  CHECK(skipped.stats.entries_read == 1);
  CHECK(skipped.stats.entries_injected == 0);
  CHECK(skipped.stats.entries_skipped == 1);
  CHECK(skipped.stats.inject_failures == 0);
  CHECK(skipped.rows.empty());

  std::filesystem::remove_all(root);
  std::printf("generated replay behavior: OK\n");
  return 0;
}
