#pragma once

// ad_stack command-line parsing. Unknown arguments and malformed values are
// errors (caller prints usage and exits 2, same convention as viz_bridge).

#include "support/cli_parse.h"
#include "support/replay_runner.h"

#include <climits>
#include <cstring>
#include <string>

struct AdStackArgs {
  const char* role = nullptr;
  const char* shm_prefix = nullptr;
  int proc_idx = -1;
  bool mp = false;
  bool record = false;
  const char* record_shm = nullptr;
  const char* relay_recorder_socket = nullptr;
  const char* replay_bin = nullptr;
  ReplayOptions replay{};
  int seconds = 2;
};

inline const char* AdStackUsage() {
  return "usage: ad_stack [--mp] [--role R --shm-prefix P] [--record]\n"
         "       [--record-shm NAME] [--relay-recorder-socket PATH]\n"
         "       [--replay BIN [--rate R] [--as-fast] [--inject-all]\n"
         "       [--topics a,b] [--exclude-topics a,b]] [--seconds N]\n";
}

// Fills out from argv; returns false and sets err on unknown arguments or
// malformed values. Semantic checks (flag combinations) stay in main.
inline bool ParseAdStackArgs(int argc, char** argv, AdStackArgs& out,
                             std::string* err) {
  auto need_value = [&](int i) -> bool {
    if (i + 1 < argc) return true;
    if (err != nullptr) *err = std::string("missing value for ") + argv[i];
    return false;
  };
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--role") == 0) {
      if (!need_value(i)) return false;
      out.role = argv[++i];
    } else if (std::strcmp(argv[i], "--shm-prefix") == 0) {
      if (!need_value(i)) return false;
      out.shm_prefix = argv[++i];
    } else if (std::strcmp(argv[i], "--proc-idx") == 0) {
      if (!need_value(i)) return false;
      long v = 0;
      if (!cli::ParseLong(argv[++i], 0, INT_MAX, v)) {
        if (err != nullptr) *err = "--proc-idx must be a non-negative integer";
        return false;
      }
      out.proc_idx = static_cast<int>(v);
    } else if (std::strcmp(argv[i], "--mp") == 0) {
      out.mp = true;
    } else if (std::strcmp(argv[i], "--record") == 0) {
      out.record = true;
    } else if (std::strcmp(argv[i], "--record-shm") == 0) {
      if (!need_value(i)) return false;
      out.record_shm = argv[++i];
      out.record = true;
    } else if (std::strcmp(argv[i], "--relay-recorder-socket") == 0) {
      if (!need_value(i)) return false;
      out.relay_recorder_socket = argv[++i];
    } else if (std::strcmp(argv[i], "--replay") == 0) {
      if (!need_value(i)) return false;
      out.replay_bin = argv[++i];
    } else if (std::strcmp(argv[i], "--rate") == 0) {
      if (!need_value(i)) return false;
      if (!cli::ParseDouble(argv[++i], 0.0, 1e9, out.replay.rate) ||
          out.replay.rate <= 0.0) {
        if (err != nullptr) *err = "--rate must be > 0";
        return false;
      }
    } else if (std::strcmp(argv[i], "--as-fast") == 0) {
      out.replay.as_fast = true;
    } else if (std::strcmp(argv[i], "--inject-all") == 0) {
      out.replay.inject_all = true;
    } else if (std::strcmp(argv[i], "--topics") == 0) {
      if (!need_value(i)) return false;
      cli::SplitCsv(argv[++i], out.replay.include_topics);
    } else if (std::strcmp(argv[i], "--exclude-topics") == 0) {
      if (!need_value(i)) return false;
      cli::SplitCsv(argv[++i], out.replay.exclude_topics);
    } else if (std::strcmp(argv[i], "--seconds") == 0) {
      if (!need_value(i)) return false;
      long v = 0;
      if (!cli::ParseLong(argv[++i], 1, INT_MAX, v)) {
        if (err != nullptr) *err = "--seconds must be a positive integer";
        return false;
      }
      out.seconds = static_cast<int>(v);
    } else {
      if (err != nullptr) *err = std::string("unknown argument: ") + argv[i];
      return false;
    }
  }
  return true;
}
