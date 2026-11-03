#include "replay_repair.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "replay_cli.h"
#include "replay_json.h"
#include "thyme/record/thyme_bin.h"

namespace thyme {
namespace replaycli {
namespace {

bool GetFileSize(const std::string& path, uint64_t* size) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0 || st.st_size < 0) {
    return false;
  }
  *size = static_cast<uint64_t>(st.st_size);
  return true;
}

uint64_t CorruptTrailerOffset(const std::string& path, uint64_t input_bytes) {
  const uint64_t footer_size = sizeof(ThymeBinIndexFooter);
  if (input_bytes < footer_size) {
    return input_bytes;
  }
  const uint64_t footer_offset = input_bytes - footer_size;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return footer_offset;
  }
  ThymeBinIndexFooter footer{};
  const bool read =
      std::fseek(file, static_cast<long>(footer_offset), SEEK_SET) == 0 &&
      std::fread(&footer, 1, sizeof(footer), file) == sizeof(footer);
  std::fclose(file);
  if (read && footer.entries_offset <= footer_offset &&
      footer_offset - footer.entries_offset ==
          static_cast<uint64_t>(footer.block_count) *
              sizeof(ThymeBinIndexEntry)) {
    return footer.entries_offset;
  }
  return footer_offset;
}

bool SamePath(const std::string& input, const std::string& output) {
  if (input == output) {
    return true;
  }
  std::error_code in_ec;
  std::error_code out_ec;
  const std::filesystem::path in_path =
      std::filesystem::weakly_canonical(input, in_ec);
  const std::filesystem::path out_path =
      std::filesystem::weakly_canonical(output, out_ec);
  if (in_ec == std::error_code{} && out_ec == std::error_code{} &&
      in_path == out_path) {
    return true;
  }
  std::error_code equivalent_ec;
  return std::filesystem::equivalent(input, output, equivalent_ec) &&
         equivalent_ec == std::error_code{};
}

std::string ErrnoMessage(const char* operation, const std::string& path) {
  return std::string(operation) + " " + path + ": " + std::strerror(errno);
}

class TempFile {
 public:
  ~TempFile() {
    if (!path_.empty()) {
      (void)std::remove(path_.c_str());
    }
  }

  bool Create(const std::string& output, std::string* error) {
    std::string pattern = output + ".repair.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    if (fd < 0) {
      *error = ErrnoMessage("cannot create temporary output beside", output);
      return false;
    }
    path_ = name.data();
    if (close(fd) != 0) {
      *error = ErrnoMessage("cannot close temporary output", path_);
      return false;
    }
    return true;
  }

  const std::string& path() const { return path_; }

  void Release() { path_.clear(); }

 private:
  std::string path_;
};

void RenderRepair(const RepairSummary& summary, bool json, std::ostream& out) {
  if (json) {
    Json j(out);
    j.BeginObject();
    j.KeyStr("tool", "thyme-replay");
    j.KeyStr("command", "repair");
    j.KeyBool("ok", summary.ok);
    j.KeyStr("input", summary.input);
    j.KeyStr("output", summary.output);
    if (summary.error.empty()) {
      j.KeyNull("error");
    } else {
      j.KeyStr("error", summary.error);
    }
    j.KeyBool("input_corrupt", summary.input_corrupt);
    j.KeyU64("recovered_records", summary.recovered_records);
    j.KeyU64("corrupt_offset", summary.corrupt_offset);
    j.KeyU64("discarded_tail_bytes", summary.discarded_tail_bytes);
    j.KeyU64("output_bytes", summary.output_bytes);
    j.KeyBool("partial", summary.partial);
    j.EndObject();
    out << '\n';
    return;
  }
  if (!summary.ok) {
    out << "repair  : FAILED\n";
    out << "error   : " << summary.error << '\n';
    return;
  }
  out << "repair  : "
      << (summary.partial ? "PARTIAL (valid prefix only)" : "OK") << '\n';
  out << "input   : " << summary.input << '\n';
  out << "output  : " << summary.output << '\n';
  out << "records : " << summary.recovered_records << " recovered\n";
  if (summary.partial) {
    out << "corrupt : offset " << summary.corrupt_offset << ", discarded "
        << summary.discarded_tail_bytes << " tail byte(s); no resync\n";
  }
  out << "bytes   : " << summary.output_bytes << '\n';
}

int Fail(RepairSummary* summary, const std::string& error, bool json,
         std::ostream& out) {
  summary->ok = false;
  summary->error = error;
  RenderRepair(*summary, json, out);
  return kExitFailure;
}

bool WriteRecovered(ThymeBinEncoder* encoder, const ThymeBinDataEntry& entry) {
  switch (entry.type) {
    case ThymeBinEntryType::kData:
      return encoder->WriteData(
          entry.record_sequence, entry.struct_id, entry.mono_ns,
          entry.source_sequence,
          entry.payload.empty() ? nullptr : entry.payload.data(),
          entry.payload_size);
    case ThymeBinEntryType::kGap:
      return encoder->WriteGap(entry.struct_id, entry.mono_ns, entry.reserved,
                               entry.source_sequence);
    case ThymeBinEntryType::kTrigger:
      return encoder->WriteTrigger(entry.record_sequence, entry.mono_ns,
                                   entry.pre_roll_ns, entry.post_roll_ns);
    case ThymeBinEntryType::kSession:
      return encoder->WriteSession(entry.record_sequence, entry.reserved);
  }
  return false;
}

bool VerifyOutput(const std::string& path, uint64_t expected_records,
                  std::string* error) {
  ThymeBinReader reader(path.c_str());
  if (!reader.ok()) {
    *error = "full verification cannot open temporary output";
    return false;
  }
  if (!reader.HasIndex()) {
    *error = "full verification found no usable output footer index";
    return false;
  }
  uint64_t count = 0;
  ThymeBinDataEntry entry{};
  ThymeBinReader::ReadResult result = ThymeBinReader::ReadResult::kOk;
  while ((result = reader.ReadNext(entry)) == ThymeBinReader::ReadResult::kOk) {
    ++count;
  }
  if (result != ThymeBinReader::ReadResult::kEof) {
    *error = "full verification found a corrupt output record at offset " +
             std::to_string(reader.corrupt_offset());
    return false;
  }
  if (count != expected_records ||
      reader.indexed_record_count() != expected_records) {
    *error = "full verification record/footer count mismatch";
    return false;
  }
  return true;
}

}  // namespace

int RunRepair(const std::string& input, const std::string& output, bool json,
              std::ostream& out, RepairSummary* summary) {
  RepairSummary local;
  RepairSummary* result = summary != nullptr ? summary : &local;
  *result = RepairSummary{};
  result->input = input;
  result->output = output;

  if (SamePath(input, output)) {
    return Fail(result, "input and output must be different paths", json, out);
  }
  struct stat output_stat {};
  if (lstat(output.c_str(), &output_stat) == 0) {
    return Fail(result, "output already exists; refusing to overwrite", json,
                out);
  }
  if (errno != ENOENT) {
    return Fail(result, ErrnoMessage("cannot inspect output", output), json,
                out);
  }

  uint64_t input_bytes = 0;
  ThymeBinReader reader(input.c_str());
  if (!reader.ok() || !GetFileSize(input, &input_bytes)) {
    return Fail(result,
                "cannot validate input header/registry/manifest: " + input,
                json, out);
  }

  TempFile temp;
  std::string error;
  if (!temp.Create(output, &error)) {
    return Fail(result, error, json, out);
  }

  {
    ThymeBinFileSink sink(temp.path().c_str());
    ThymeBinEncoder encoder(&sink);
    ThymeBinManifestBlock manifest{};
    const ThymeBinManifestBlock* manifest_ptr = nullptr;
    if (reader.manifest_state() == ThymeBinReader::ManifestState::kOk) {
      manifest = MakeThymeBinManifestBlock(reader.manifest_hash(),
                                           reader.manifest_version());
      manifest_ptr = &manifest;
    }
    if (!encoder.ok() ||
        !encoder.WriteHeader(reader.header().session_id,
                             reader.header().start_mono_ns,
                             reader.registry().data(), reader.registry().size(),
                             manifest_ptr, reader.header().device_id)) {
      return Fail(result, "cannot write temporary output header", json, out);
    }

    ThymeBinDataEntry entry{};
    ThymeBinReader::ReadResult read_result = ThymeBinReader::ReadResult::kOk;
    while ((read_result = reader.ReadNext(entry)) ==
           ThymeBinReader::ReadResult::kOk) {
      if (!WriteRecovered(&encoder, entry)) {
        return Fail(result,
                    "cannot write recovered record " +
                        std::to_string(result->recovered_records),
                    json, out);
      }
      ++result->recovered_records;
    }
    if (read_result == ThymeBinReader::ReadResult::kCorrupt) {
      result->input_corrupt = true;
      result->partial = true;
      result->corrupt_offset = reader.corrupt_offset();
    } else if (read_result == ThymeBinReader::ReadResult::kEof) {
      if (reader.index_status() == ThymeBinReader::IndexStatus::kCorrupt) {
        result->input_corrupt = true;
        result->partial = true;
        result->corrupt_offset = CorruptTrailerOffset(input, input_bytes);
      }
    } else {
      return Fail(result, "unexpected input read result", json, out);
    }
    result->discarded_tail_bytes =
        result->partial && result->corrupt_offset <= input_bytes
            ? input_bytes - result->corrupt_offset
            : 0;
    if (!encoder.Finish() || !sink.Sync()) {
      return Fail(result, "cannot finish and sync temporary output", json, out);
    }
  }

  if (!VerifyOutput(temp.path(), result->recovered_records, &error) ||
      !GetFileSize(temp.path(), &result->output_bytes)) {
    return Fail(result, error.empty() ? "cannot size temporary output" : error,
                json, out);
  }
  if (link(temp.path().c_str(), output.c_str()) != 0) {
    return Fail(result, ErrnoMessage("cannot atomically publish", output), json,
                out);
  }
  if (std::remove(temp.path().c_str()) != 0) {
    (void)unlink(output.c_str());
    return Fail(result,
                ErrnoMessage("cannot remove temporary output", temp.path()),
                json, out);
  }
  temp.Release();
  result->ok = true;
  RenderRepair(*result, json, out);
  return kExitOk;
}

}  // namespace replaycli
}  // namespace thyme
