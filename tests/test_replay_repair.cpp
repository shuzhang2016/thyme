#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "replay_cli.h"
#include "replay_repair.h"
#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"

using namespace thyme;

namespace {

constexpr uint64_t kSessionId = 0x123456789ULL;
constexpr uint64_t kStartMonoNs = 0x102030405ULL;
constexpr uint32_t kDeviceId = 0x89ABCDEFu;
constexpr uint64_t kManifestHash = 0x9914C81F74037D59ULL;
constexpr uint16_t kManifestVersion = 7;
constexpr uint64_t kBaseMonoNs = 1000000;

std::string Root() {
  return "/tmp/thyme_replay_repair_" + std::to_string(getpid());
}

std::vector<ThymeStructDesc> MakeRegistry() {
  std::vector<ThymeStructDesc> descs(2);
  descs[0].struct_id = 7;
  descs[0].contract_version = 3;
  descs[0].payload_size = 4;
  descs[0].layout_hash = 0x1111222233334444ULL;
  std::snprintf(descs[0].name, sizeof(descs[0].name), "Alpha");
  std::snprintf(descs[0].channel, sizeof(descs[0].channel), "small");
  std::snprintf(descs[0].record_key, sizeof(descs[0].record_key), "alpha.key");
  descs[1].struct_id = 9;
  descs[1].contract_version = 5;
  descs[1].payload_size = 4;
  descs[1].layout_hash = 0x5555666677778888ULL;
  std::snprintf(descs[1].name, sizeof(descs[1].name), "Beta");
  std::snprintf(descs[1].channel, sizeof(descs[1].channel), "aux");
  std::snprintf(descs[1].record_key, sizeof(descs[1].record_key), "beta.key");
  return descs;
}

ThymeBinDataEntry MakeEntry(uint64_t i) {
  ThymeBinDataEntry entry{};
  entry.mono_ns = kBaseMonoNs + i * 100;
  if (i == 0 || i == 64) {
    entry.type = ThymeBinEntryType::kSession;
    entry.record_sequence = 0x5000 + i;
    entry.reserved = static_cast<uint16_t>(2 + i);
    entry.mono_ns = 0;
    return entry;
  }
  switch (i % 4) {
    case 1:
      entry.type = ThymeBinEntryType::kTrigger;
      entry.record_sequence = 0x6000 + i;
      entry.pre_roll_ns = 1000 + i;
      entry.post_roll_ns = 2000 + i;
      break;
    case 2:
      entry.type = ThymeBinEntryType::kGap;
      entry.struct_id = 9;
      entry.reserved = static_cast<uint16_t>(3 + i);
      entry.source_sequence = static_cast<uint32_t>(10 + i);
      break;
    default:
      entry.type = ThymeBinEntryType::kData;
      entry.record_sequence = 0x7000 + i;
      entry.struct_id = i % 8 == 0 ? 7 : 9;
      entry.source_sequence = static_cast<uint32_t>(0x9000 + i);
      entry.payload_size = 4;
      entry.payload = {static_cast<uint8_t>(i), 0xA5, 0x5A,
                       static_cast<uint8_t>(i * 3)};
      break;
  }
  return entry;
}

struct Synthetic {
  std::string path;
  std::vector<ThymeBinDataEntry> expected;
  std::vector<uint64_t> offsets;
};

Synthetic WriteSynthetic(const std::string& path, uint64_t count = 70,
                         bool finish = true) {
  Synthetic synthetic;
  synthetic.path = path;
  const std::vector<ThymeStructDesc> descs = MakeRegistry();
  ThymeBinManifestBlock manifest =
      MakeThymeBinManifestBlock(kManifestHash, kManifestVersion);
  ThymeBinFileSink sink(path.c_str());
  CHECK(sink.ok());
  ThymeBinEncoder encoder(&sink);
  CHECK(encoder.WriteHeader(kSessionId, kStartMonoNs, descs.data(),
                            descs.size(), &manifest, kDeviceId));
  for (uint64_t i = 0; i < count; ++i) {
    const int64_t offset = sink.Tell();
    CHECK(offset >= 0);
    synthetic.offsets.push_back(static_cast<uint64_t>(offset));
    ThymeBinDataEntry entry = MakeEntry(i);
    switch (entry.type) {
      case ThymeBinEntryType::kData:
        CHECK(encoder.WriteData(entry.record_sequence, entry.struct_id,
                                entry.mono_ns, entry.source_sequence,
                                entry.payload.data(), entry.payload_size));
        break;
      case ThymeBinEntryType::kGap:
        CHECK(encoder.WriteGap(entry.struct_id, entry.mono_ns, entry.reserved,
                               entry.source_sequence));
        break;
      case ThymeBinEntryType::kTrigger:
        CHECK(encoder.WriteTrigger(entry.record_sequence, entry.mono_ns,
                                   entry.pre_roll_ns, entry.post_roll_ns));
        break;
      case ThymeBinEntryType::kSession:
        CHECK(encoder.WriteSession(entry.record_sequence, entry.reserved));
        break;
    }
    synthetic.expected.push_back(entry);
  }
  if (finish) {
    CHECK(encoder.Finish());
  }
  return synthetic;
}

std::vector<uint8_t> ReadBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  CHECK(in.good());
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
}

void CopyFile(const std::string& source, const std::string& output) {
  const std::vector<uint8_t> bytes = ReadBytes(source);
  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  CHECK(out.good());
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  CHECK(out.good());
}

void Poke(const std::string& path, uint64_t offset, uint8_t value) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  CHECK(file.good());
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(reinterpret_cast<const char*>(&value), 1);
  CHECK(file.good());
}

void CheckEntry(const ThymeBinDataEntry& got,
                const ThymeBinDataEntry& expected) {
  CHECK(got.type == expected.type);
  CHECK(got.record_sequence == expected.record_sequence);
  CHECK(got.struct_id == expected.struct_id);
  CHECK(got.reserved == expected.reserved);
  CHECK(got.mono_ns == expected.mono_ns);
  CHECK(got.source_sequence == expected.source_sequence);
  CHECK(got.pre_roll_ns == expected.pre_roll_ns);
  CHECK(got.post_roll_ns == expected.post_roll_ns);
  CHECK(got.payload_size == expected.payload_size);
  CHECK(got.payload == expected.payload);
}

void CheckOutput(const std::string& path,
                 const std::vector<ThymeBinDataEntry>& expected) {
  ThymeBinReader reader(path.c_str());
  CHECK(reader.ok());
  CHECK(reader.header().device_id == kDeviceId);
  CHECK(reader.header().session_id == kSessionId);
  CHECK(reader.header().start_mono_ns == kStartMonoNs);
  CHECK(reader.manifest_state() == ThymeBinReader::ManifestState::kOk);
  CHECK(reader.manifest_hash() == kManifestHash);
  CHECK(reader.manifest_version() == kManifestVersion);
  const std::vector<ThymeStructDesc> registry = MakeRegistry();
  CHECK(reader.registry().size() == registry.size());
  for (std::size_t i = 0; i < registry.size(); ++i) {
    CHECK(std::memcmp(&reader.registry()[i], &registry[i],
                      sizeof(ThymeStructDesc)) == 0);
  }
  CHECK(reader.HasIndex());
  CHECK(reader.indexed_record_count() == expected.size());
  CHECK(reader.SeekToMonoNs(kBaseMonoNs));
  ThymeBinDataEntry entry{};
  for (const ThymeBinDataEntry& wanted : expected) {
    CHECK(reader.ReadNext(entry) == ThymeBinReader::ReadResult::kOk);
    CheckEntry(entry, wanted);
  }
  CHECK(reader.ReadNext(entry) == ThymeBinReader::ReadResult::kEof);
}

ThymeBinIndexFooter ReadFooter(const std::string& path) {
  const std::vector<uint8_t> bytes = ReadBytes(path);
  CHECK(bytes.size() >= sizeof(ThymeBinIndexFooter));
  ThymeBinIndexFooter footer{};
  std::memcpy(&footer, bytes.data() + bytes.size() - sizeof(footer),
              sizeof(footer));
  return footer;
}

ThymeBinIndexEntry ReadIndexEntry(const std::string& path, uint64_t index) {
  const ThymeBinIndexFooter footer = ReadFooter(path);
  CHECK(index < footer.block_count);
  std::ifstream in(path, std::ios::binary);
  CHECK(in.good());
  in.seekg(static_cast<std::streamoff>(footer.entries_offset +
                                       index * sizeof(ThymeBinIndexEntry)));
  ThymeBinIndexEntry entry{};
  in.read(reinterpret_cast<char*>(&entry), sizeof(entry));
  CHECK(in.good());
  return entry;
}

bool Exists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

bool NoTemps() {
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(Root())) {
    if (entry.path().filename().string().find(".repair.") !=
        std::string::npos) {
      return false;
    }
  }
  return true;
}

std::vector<ThymeBinDataEntry> Prefix(const Synthetic& synthetic,
                                      std::size_t count) {
  return std::vector<ThymeBinDataEntry>(
      synthetic.expected.begin(),
      synthetic.expected.begin() + static_cast<std::ptrdiff_t>(count));
}

void TestCleanAndSessionIndex(const std::string& root) {
  const Synthetic source = WriteSynthetic(root + "/clean.tbin");
  const std::vector<uint8_t> original = ReadBytes(source.path);
  const std::string output = root + "/clean-out.tbin";
  replaycli::RepairSummary summary;
  std::ostringstream out;
  CHECK(replaycli::RunRepair(source.path, output, false, out, &summary) ==
        replaycli::kExitOk);
  CHECK(summary.ok && !summary.partial && !summary.input_corrupt);
  CHECK(summary.recovered_records == 70);
  CHECK(summary.discarded_tail_bytes == 0);
  CHECK(summary.output_bytes == ReadBytes(output).size());
  CHECK(out.str().find("repair  : OK") != std::string::npos);
  CheckOutput(output, source.expected);
  CHECK(ReadBytes(source.path) == original);
  ThymeBinReader reader(output.c_str());
  CHECK(reader.index_block_count() == 2);
  CHECK(ReadIndexEntry(output, 0).mono_ns == 0);
  CHECK(ReadIndexEntry(output, 1).mono_ns == kBaseMonoNs + 63 * 100);
  CHECK(NoTemps());
}

void TestCrcStopsWithoutResync(const std::string& root) {
  const Synthetic source = WriteSynthetic(root + "/crc-source.tbin", 12);
  const std::string input = root + "/crc.tbin";
  CopyFile(source.path, input);
  Poke(input, source.offsets[3] + 32, 0xFF);
  const std::string output = root + "/crc-out.tbin";
  replaycli::RepairSummary summary;
  std::ostringstream out;
  std::ostringstream err;
  CHECK(replaycli::RunCli({"repair", input, "--out", output, "--json"}, out,
                          err, nullptr) == replaycli::kExitOk);
  const std::string json = out.str();
  CHECK(json.find("\"input_corrupt\":true") != std::string::npos);
  CHECK(json.find("\"recovered_records\":3") != std::string::npos);
  CHECK(json.find("\"corrupt_offset\":" + std::to_string(source.offsets[3])) !=
        std::string::npos);
  CHECK(json.find("\"discarded_tail_bytes\":") != std::string::npos);
  CHECK(json.find("\"output_bytes\":") != std::string::npos);
  CHECK(json.find("\"partial\":true") != std::string::npos);
  CheckOutput(output, Prefix(source, 3));
  CHECK(NoTemps());

  const std::string text_output = root + "/crc-text-out.tbin";
  const std::string text_input = root + "/crc-text.tbin";
  CopyFile(input, text_input);
  std::ostringstream text;
  CHECK(replaycli::RunRepair(text_input, text_output, false, text, &summary) ==
        replaycli::kExitOk);
  CHECK(summary.input_corrupt && summary.partial);
  CHECK(summary.corrupt_offset == source.offsets[3]);
  CHECK(summary.discarded_tail_bytes ==
        ReadBytes(input).size() - source.offsets[3]);
  CHECK(text.str().find("PARTIAL (valid prefix only)") != std::string::npos);
}

void TestTruncationAndFirstCorrupt(const std::string& root) {
  const Synthetic source = WriteSynthetic(root + "/trunc-source.tbin", 12);
  const std::string input = root + "/trunc.tbin";
  CopyFile(source.path, input);
  std::filesystem::resize_file(input, source.offsets[4] + 36);
  const std::string output = root + "/trunc-out.tbin";
  replaycli::RepairSummary summary;
  std::ostringstream out;
  CHECK(replaycli::RunRepair(input, output, false, out, &summary) ==
        replaycli::kExitOk);
  CHECK(summary.partial && summary.input_corrupt);
  CHECK(summary.recovered_records == 4);
  CHECK(summary.corrupt_offset == source.offsets[4]);
  CHECK(summary.discarded_tail_bytes > 0);
  CheckOutput(output, Prefix(source, 4));

  const std::string first = root + "/first.tbin";
  CopyFile(source.path, first);
  Poke(first, source.offsets[0] + 16, 0xFF);
  const std::string first_output = root + "/first-out.tbin";
  CHECK(replaycli::RunRepair(first, first_output, false, out, &summary) ==
        replaycli::kExitOk);
  CHECK(summary.partial && summary.recovered_records == 0);
  CHECK(summary.corrupt_offset == source.offsets[0]);
  CheckOutput(first_output, {});
}

void TestAbsentAndBadFooter(const std::string& root) {
  const Synthetic absent = WriteSynthetic(root + "/absent.tbin", 12, false);
  replaycli::RepairSummary summary;
  std::ostringstream out;
  CHECK(replaycli::RunRepair(absent.path, root + "/absent-out.tbin", false, out,
                             &summary) == replaycli::kExitOk);
  CHECK(!summary.partial && !summary.input_corrupt);
  CHECK(summary.recovered_records == 12);
  CheckOutput(root + "/absent-out.tbin", absent.expected);

  const Synthetic source = WriteSynthetic(root + "/footer-source.tbin", 12);
  const ThymeBinIndexFooter footer = ReadFooter(source.path);
  const std::string input = root + "/bad-footer.tbin";
  CopyFile(source.path, input);
  const std::vector<uint8_t> bytes = ReadBytes(input);
  Poke(input,
       bytes.size() - sizeof(ThymeBinIndexFooter) +
           offsetof(ThymeBinIndexFooter, crc32),
       0xFF);
  CHECK(replaycli::RunRepair(input, root + "/bad-footer-out.tbin", false, out,
                             &summary) == replaycli::kExitOk);
  CHECK(summary.partial && summary.input_corrupt);
  CHECK(summary.recovered_records == 12);
  CHECK(summary.corrupt_offset == footer.entries_offset);
  CheckOutput(root + "/bad-footer-out.tbin", source.expected);

  const std::string bad_index = root + "/bad-index.tbin";
  CopyFile(source.path, bad_index);
  ThymeBinIndexEntry entry = ReadIndexEntry(source.path, 0);
  entry.offset = bytes.size() + 4096;
  entry.crc32 =
      Crc32Update(0xFFFFFFFFu, reinterpret_cast<const uint8_t*>(&entry),
                  offsetof(ThymeBinIndexEntry, crc32)) ^
      0xFFFFFFFFu;
  std::fstream index_file(bad_index,
                          std::ios::binary | std::ios::in | std::ios::out);
  CHECK(index_file.good());
  index_file.seekp(static_cast<std::streamoff>(footer.entries_offset));
  index_file.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
  CHECK(index_file.good());
  index_file.close();
  CHECK(replaycli::RunRepair(bad_index, root + "/bad-index-out.tbin", false,
                             out, &summary) == replaycli::kExitOk);
  CHECK(summary.partial && summary.input_corrupt);
  CHECK(summary.recovered_records == 12);
  CHECK(summary.corrupt_offset == footer.entries_offset);
  CHECK(summary.discarded_tail_bytes == bytes.size() - footer.entries_offset);
  CheckOutput(root + "/bad-index-out.tbin", source.expected);
}

void CheckFatalNoOutput(const std::string& input, const std::string& output) {
  replaycli::RepairSummary summary;
  std::ostringstream out;
  CHECK(replaycli::RunRepair(input, output, false, out, &summary) ==
        replaycli::kExitFailure);
  CHECK(!summary.ok && !summary.error.empty());
  CHECK(!Exists(output));
  CHECK(NoTemps());
}

void TestHeaderRegistryManifestFatal(const std::string& root) {
  const Synthetic source = WriteSynthetic(root + "/fatal-source.tbin", 2);
  const std::string header = root + "/bad-header.tbin";
  CopyFile(source.path, header);
  Poke(header, 0, 'X');
  CheckFatalNoOutput(header, root + "/bad-header-out.tbin");

  const std::string registry = root + "/bad-registry.tbin";
  CopyFile(source.path, registry);
  const uint16_t invalid_version = 0;
  std::fstream file(registry, std::ios::binary | std::ios::in | std::ios::out);
  CHECK(file.good());
  file.seekp(
      static_cast<std::streamoff>(sizeof(ThymeBinFileHeader) +
                                  offsetof(ThymeStructDesc, contract_version)));
  file.write(reinterpret_cast<const char*>(&invalid_version),
             sizeof(invalid_version));
  CHECK(file.good());
  file.close();
  CheckFatalNoOutput(registry, root + "/bad-registry-out.tbin");

  const std::string manifest = root + "/bad-manifest.tbin";
  CopyFile(source.path, manifest);
  Poke(manifest,
       sizeof(ThymeBinFileHeader) + 2 * sizeof(ThymeStructDesc) +
           offsetof(ThymeBinManifestBlock, manifest_hash),
       0xFF);
  CheckFatalNoOutput(manifest, root + "/bad-manifest-out.tbin");
}

void TestUsageExistingAndSamePath(const std::string& root) {
  const Synthetic source = WriteSynthetic(root + "/usage.tbin", 3);
  std::ostringstream out;
  std::ostringstream err;
  CHECK(replaycli::RunCli({"repair", source.path}, out, err, nullptr) ==
        replaycli::kExitUsage);
  CHECK(err.str().find("repair needs --out") != std::string::npos);

  const std::string existing = root + "/existing.tbin";
  {
    std::ofstream file(existing, std::ios::binary);
    file << "keep";
  }
  replaycli::RepairSummary summary;
  CHECK(replaycli::RunRepair(source.path, existing, false, out, &summary) ==
        replaycli::kExitFailure);
  CHECK(ReadBytes(existing) == std::vector<uint8_t>({'k', 'e', 'e', 'p'}));

  const std::vector<uint8_t> original = ReadBytes(source.path);
  CHECK(replaycli::RunRepair(source.path, source.path, false, out, &summary) ==
        replaycli::kExitFailure);
  CHECK(summary.error.find("different paths") != std::string::npos);
  CHECK(ReadBytes(source.path) == original);

  const std::string alias = root + "/alias.tbin";
  CHECK(link(source.path.c_str(), alias.c_str()) == 0);
  CHECK(replaycli::RunRepair(source.path, alias, false, out, &summary) ==
        replaycli::kExitFailure);
  CHECK(summary.error.find("different paths") != std::string::npos);
  CHECK(NoTemps());
}

void TestAtomicPublishRace(const std::string& root) {
  const Synthetic source = WriteSynthetic(root + "/race.tbin");
  const std::string output = root + "/race-out.tbin";
  std::atomic<bool> start{false};
  int rc1 = -1;
  int rc2 = -1;
  replaycli::RepairSummary summary1;
  replaycli::RepairSummary summary2;
  std::ostringstream out1;
  std::ostringstream out2;
  auto run = [&](int* rc, replaycli::RepairSummary* summary,
                 std::ostringstream* out) {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    *rc = replaycli::RunRepair(source.path, output, false, *out, summary);
  };
  std::thread first(run, &rc1, &summary1, &out1);
  std::thread second(run, &rc2, &summary2, &out2);
  start.store(true, std::memory_order_release);
  first.join();
  second.join();
  CHECK((rc1 == replaycli::kExitOk) != (rc2 == replaycli::kExitOk));
  CHECK((rc1 == replaycli::kExitFailure) != (rc2 == replaycli::kExitFailure));
  CHECK(summary1.ok != summary2.ok);
  CheckOutput(output, source.expected);
  CHECK(NoTemps());
}

}  // namespace

int main() {
  const std::string root = Root();
  std::filesystem::remove_all(root);
  CHECK(std::filesystem::create_directory(root));
  TestCleanAndSessionIndex(root);
  TestCrcStopsWithoutResync(root);
  TestTruncationAndFirstCorrupt(root);
  TestAbsentAndBadFooter(root);
  TestHeaderRegistryManifestFatal(root);
  TestUsageExistingAndSamePath(root);
  TestAtomicPublishRace(root);
  CHECK(NoTemps());
  std::filesystem::remove_all(root);
  std::puts("test_replay_repair: OK");
  return 0;
}
