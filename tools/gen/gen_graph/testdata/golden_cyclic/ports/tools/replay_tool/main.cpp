#include "generated/wiring.h"
#include "support/cli_parse.h"
#include "support/replay_runner.h"
#include "thyme/app/replay_mode.h"
#include "thyme/record/thyme_bin.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void PrintUsage() {
  std::fprintf(stderr,
              "usage: replay_tool SESSION.bin [--validate]\n"
              "       [--rate R] [--as-fast]\n"
              "       [--topics A,B] [--exclude-topics X,Y]\n"
              "       [--from-mono NS] [--to-mono NS]\n"
              "       [--inject-stage NODE] [--inject-all]\n");
}

}  // namespace

int main(int argc, char** argv) {
  const char* bin_path = nullptr;
  bool validate_only = false;
  ReplayOptions opt{};

  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-' && bin_path == nullptr) {
      bin_path = argv[i];
    } else if (std::strcmp(argv[i], "--validate") == 0) {
      validate_only = true;
    } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
      opt.rate = std::atof(argv[++i]);
      if (opt.rate <= 0.0) {
        std::fprintf(stderr, "replay_tool: --rate must be > 0 (got %g)\n", opt.rate);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--as-fast") == 0) {
      opt.as_fast = true;
    } else if (std::strcmp(argv[i], "--topics") == 0 && i + 1 < argc) {
      cli::SplitCsv(argv[++i], opt.include_topics);
    } else if (std::strcmp(argv[i], "--exclude-topics") == 0 && i + 1 < argc) {
      cli::SplitCsv(argv[++i], opt.exclude_topics);
    } else if (std::strcmp(argv[i], "--from-mono") == 0 && i + 1 < argc) {
      opt.from_mono_ns = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--to-mono") == 0 && i + 1 < argc) {
      opt.to_mono_ns = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--inject-stage") == 0 && i + 1 < argc) {
      opt.inject_stage = argv[++i];
    } else if (std::strcmp(argv[i], "--inject-all") == 0) {
      opt.inject_all = true;
    } else {
      PrintUsage();
      return 2;
    }
  }

  if (bin_path == nullptr) {
    PrintUsage();
    return 2;
  }

  thyme::ThymeBinReader reader(bin_path);
  if (!reader.ok()) {
    std::fprintf(stderr, "replay_tool: invalid THYM bin %s\n", bin_path);
    return 1;
  }

  std::size_t data_rows = 0;
  thyme::ThymeBinDataEntry e{};
  while (reader.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
    if (e.type == thyme::ThymeBinEntryType::kData) ++data_rows;
  }

  std::printf("replay_tool: session=%llu structs=%u data_entries=%zu\n",
              static_cast<unsigned long long>(reader.header().session_id),
              reader.header().struct_count, data_rows);

  if (validate_only) return data_rows > 0 ? 0 : 1;

  thyme::app::replay_mode::SetActive(true);
  auto p = Pipeline::Create();
  p->Start();
  const ReplayStats stats = RunReplay(*p, bin_path, opt);
  p->RequestStop();
  p->Join();

  std::printf(
      "replay_tool: read=%zu injected=%zu skipped=%zu failed=%zu\n",
      stats.entries_read, stats.entries_injected, stats.entries_skipped,
      stats.inject_failures);

  return stats.entries_injected > 0 ? 0 : 1;
}
