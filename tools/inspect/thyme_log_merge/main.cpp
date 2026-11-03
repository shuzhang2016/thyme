// thyme_log_merge — OBS-06: merge per-process observability logs (.fsm /
// generic JSONL with "ts" mono_ns fields, and T9P1 perf dump binaries) into
// one interleaved timeline ordered by absolute CLOCK_MONOTONIC ns.
//
// Usage:
//   thyme_log_merge <inputs...> [--from ns] [--to ns] [--json] [-o out]
//
// Exit codes: 0 ok, 2 usage / bad arguments, 3 an input failed to open,
// 4 record-manifest mismatch (ABI-10: inputs produced under different
// RecordStructId assignments are never merged silently).

#include "log_merge.h"

#include "thyme/record/record_schema.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void Usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s <inputs...> [--from ns] [--to ns] [--json] "
               "[--hist] [-o out]\n"
               "  inputs: .fsm/JSONL logs (\"ts\" = mono_ns) and/or T9P1 perf "
               "dump binaries\n"
               "  output: text (default) or JSONL (--json), stdout or -o "
               "file\n"
               "  --hist: skip the timeline; merge OBS-09 histogram blocks "
               "across processes\n"
               "          and print p50/p90/p99/p999/max per (node, series)\n"
               "  --sys: emit only OBS-04 process/resource series rows "
               "(kind \"sys\")\n"
               "  --expect-hash <0x...>: require every input's ABI-10 record "
               "manifest\n"
               "          to match (exit 4 otherwise; legacy logs without a\n"
               "          manifest block fail closed)\n",
               argv0);
}

bool ParseU64(const char* s, uint64_t* out) {
  if (s == nullptr || *s == '\0') return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (end == s || *end != '\0') return false;
  *out = static_cast<uint64_t>(v);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> inputs;
  thyme::logmerge::Options opt;
  const char* out_path = nullptr;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
      if (!ParseU64(argv[++i], &opt.from_ns)) {
        Usage(argv[0]);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
      if (!ParseU64(argv[++i], &opt.to_ns)) {
        Usage(argv[0]);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--json") == 0) {
      opt.json = true;
    } else if (std::strcmp(argv[i], "--hist") == 0) {
      opt.hist = true;
    } else if (std::strcmp(argv[i], "--sys") == 0) {
      opt.sys_only = true;
    } else if (std::strcmp(argv[i], "--expect-hash") == 0 && i + 1 < argc) {
      if (!thyme::schema::ParseManifestHash(argv[++i], &opt.expect_hash)) {
        std::fprintf(stderr,
                     "thyme_log_merge: --expect-hash wants 0x<16 hex digits>\n");
        return 2;
      }
      opt.have_expect_hash = true;
    } else if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      out_path = argv[++i];
    } else if (argv[i][0] == '-') {
      Usage(argv[0]);
      return 2;
    } else {
      inputs.emplace_back(argv[i]);
    }
  }
  if (inputs.empty()) {
    Usage(argv[0]);
    return 2;
  }

  FILE* out = stdout;
  if (out_path != nullptr) {
    out = std::fopen(out_path, "w");
    if (out == nullptr) {
      std::fprintf(stderr, "thyme_log_merge: cannot open output %s\n",
                   out_path);
      return 3;
    }
  }
  const int rc = thyme::logmerge::Merge(inputs, opt, out, stderr);
  if (out_path != nullptr) std::fclose(out);
  return rc;
}
