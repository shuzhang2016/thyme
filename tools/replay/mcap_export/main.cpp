// thyme_bin_to_mcap — export a .thyme.bin recording to MCAP (REC-06).
//
// Usage:
//   thyme_bin_to_mcap <in.thyme.bin> <out.mcap> [--no-events] [--no-summary]
//                     [--no-header-session] [--crc]
//
// Exit codes: 0 ok, 2 usage, 3 input open failed, 4 corrupt input,
//             5 data entry references an unregistered struct, 6 MCAP write
//             failed. A failed export removes the partial output file.

#include "thyme/record/mcap_export.h"

#include <cstdio>
#include <cstring>

namespace {

void Usage() {
  std::fprintf(stderr,
               "usage: thyme_bin_to_mcap <in.thyme.bin> <out.mcap> "
               "[--no-events] [--no-summary] [--no-header-session] [--crc]\n");
}

int CodeFor(thyme::ThymeBinExportStatus status) {
  using S = thyme::ThymeBinExportStatus;
  switch (status) {
    case S::kOk: return 0;
    case S::kInvalidArgument: return 2;
    case S::kInputOpen: return 3;
    case S::kInputCorrupt: return 4;
    case S::kUnknownStruct: return 5;
    case S::kWriterError: return 6;
  }
  return 6;
}

const char* NameFor(thyme::ThymeBinExportStatus status) {
  using S = thyme::ThymeBinExportStatus;
  switch (status) {
    case S::kOk: return "ok";
    case S::kInvalidArgument: return "invalid argument";
    case S::kInputOpen: return "cannot open input";
    case S::kInputCorrupt: return "corrupt input record";
    case S::kUnknownStruct: return "unregistered struct id";
    case S::kWriterError: return "mcap write failed";
  }
  return "unknown error";
}

}  // namespace

int main(int argc, char** argv) {
  const char* in_path = nullptr;
  const char* out_path = nullptr;
  thyme::ThymeBinToMcapOptions options{};

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (std::strcmp(arg, "--no-events") == 0) {
      options.emit_events = false;
      options.emit_header_session = false;
    } else if (std::strcmp(arg, "--no-header-session") == 0) {
      options.emit_header_session = false;
    } else if (std::strcmp(arg, "--no-summary") == 0) {
      options.include_summary = false;
    } else if (std::strcmp(arg, "--crc") == 0) {
      options.data_section_crc = true;
    } else if (std::strcmp(arg, "-h") == 0 ||
               std::strcmp(arg, "--help") == 0) {
      Usage();
      return 0;
    } else if (arg[0] == '-') {
      std::fprintf(stderr, "thyme_bin_to_mcap: unknown option %s\n", arg);
      Usage();
      return 2;
    } else if (in_path == nullptr) {
      in_path = arg;
    } else if (out_path == nullptr) {
      out_path = arg;
    } else {
      Usage();
      return 2;
    }
  }
  if (in_path == nullptr || out_path == nullptr) {
    Usage();
    return 2;
  }

  thyme::ThymeBinToMcapStats stats{};
  const thyme::ThymeBinExportStatus status =
      thyme::ThymeBinToMcap(in_path, out_path, options, &stats);
  if (status != thyme::ThymeBinExportStatus::kOk) {
    std::fprintf(stderr, "thyme_bin_to_mcap: %s (%s)\n", NameFor(status),
                 in_path);
    if (status == thyme::ThymeBinExportStatus::kInputCorrupt) {
      std::fprintf(stderr,
                   "  record offset %llu, crc expected 0x%08x actual "
                   "0x%08x\n",
                   static_cast<unsigned long long>(stats.corrupt_offset),
                   static_cast<unsigned>(stats.corrupt_crc_expected),
                   static_cast<unsigned>(stats.corrupt_crc_actual));
    }
    if (stats.writer_status != thyme::McapStatus::kOk) {
      std::fprintf(stderr, "  writer: mcap status %u\n",
                   static_cast<unsigned>(stats.writer_status));
    }
    if (stats.output_opened) {
      if (std::remove(out_path) == 0) {
        std::fprintf(stderr, "  removed partial output %s\n", out_path);
      }
    }
    return CodeFor(status);
  }

  std::printf(
      "wrote %s: %llu data, %llu gap, %llu trigger, %llu session messages, "
      "%u channels, %llu records\n",
      out_path, static_cast<unsigned long long>(stats.data_messages),
      static_cast<unsigned long long>(stats.gap_messages),
      static_cast<unsigned long long>(stats.trigger_messages),
      static_cast<unsigned long long>(stats.session_messages), stats.channels,
      static_cast<unsigned long long>(stats.input_records));
  return 0;
}
