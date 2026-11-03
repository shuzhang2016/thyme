// fuzz_trigger_config.cpp — TST-03 structured fuzz driver for the INI config
// parser (trigger_cfg::ParseCenterConfig: section dispatch, key/value
// validation, numeric conversion, cross-section checks). Deterministic
// splitmix64 mutations.
//
// Contract asserted per input: no crash, and a defined result — a rejection
// always carries an error message, and every accepted config round-trips
// through DumpConfig (TRG-17: the effective dump must re-parse).
//
// Usage: fuzz_trigger_config [iterations] [seed]
//   env THYME_FUZZ_ITERS / THYME_FUZZ_SEED override argv.

#include "thyme/check.h"
#include "thyme/record/trigger_config.h"

#include "fuzz_harness.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using thyme_fuzz::SplitMix64;

constexpr std::size_t kMaxInput = 64u << 10;  // 64 KiB of INI is plenty
constexpr uint64_t kDefaultIters = 50000;
constexpr uint64_t kDefaultSeed = 0x494E4931u;  // "INI1"

const char kMinimalIni[] = R"INI([trigger_center]
socket = /tmp/thyme_trigger.sock
max_concurrent_captures = 1
default_source = manual
crash_priority = 100

[source.manual]
before_s = 10
after_s = 5
priority = 10
mute_s = 0
quota_class = manual

[source.crash]
before_s = 10
after_s = 5
priority = 100
flush_policy = realtime
quota_class = safety

[quota.manual]
max_triggers_per_hour = 10

[quota.safety]
max_triggers_per_hour = 0
)INI";

const char kFullIni[] = R"INI([trigger_center]
socket = /tmp/thyme_trigger.sock
max_concurrent_captures = 1
default_source = manual
crash_priority = 100

[recorder]
output_dir = /opt/usr/triggered_data
pending_bytes_quota = 67108864
flush_policy = idle
idle_hold_s = 30
idle_fallback = always_busy
io_rate_limit_mbps = 20
disk_quota_mb = 20000
high_reserved_mb = 512
incomplete_policy = keep
incomplete_max_age_s = 3600

[security]
allow_root = false
require_peer_pid = true
max_priority_hint = 1000
allow_legacy_unsequenced = false
allow_legacy_v2 = false
audit_max_bytes = 4194304
audit_rotate_keep = 2
audit_fsync_interval_s = 0

[channel./h264_front]
kind = h264_stream
name = h264_front
bitrate_kbps = 4000

[channel./thyme_recr_can]
kind = mirror_ring
name = can_chassis
rate_hz = 50
avg_frame_bytes = 128

[source.manual]
channels = /h264_front, /thyme_recr_can
before_s = 10
after_s = 5
priority = 10
mute_s = 0
quota_class = manual

[source.crash]
channels = /h264_front
before_s = 10
after_s = 5
priority = 100
flush_policy = realtime
quota_class = safety

[quota.manual]
max_triggers_per_hour = 10

[quota.safety]
max_triggers_per_hour = 0

[continuous]
auto_arm = false
channels = small_struct:*
slice_mb = 512
slice_s = 0
wait_idle = false
rate_check = warn

[chassis]
page_shm = /thyme_chassis
struct_id = 3
epb_offset = 8
speed_offset = 16
speed_threshold = 0.5
)INI";

// The fuzz target proper.
void RunOneInput(const uint8_t* data, std::size_t size) {
  if (size > kMaxInput) return;
  const std::string text(reinterpret_cast<const char*>(data), size);
  thyme::trigger_cfg::CenterConfig cfg;
  std::string err;
  std::string warn;
  const bool ok = thyme::trigger_cfg::ParseCenterConfig(text, &cfg, &err,
                                                        &warn);
  if (!ok) {
    CHECK(!err.empty());  // rejection always says why (line N: ...)
    return;
  }
  // TRG-17 contract: the rendered effective config must re-parse.
  const std::string dumped = thyme::trigger_cfg::DumpConfig(cfg);
  thyme::trigger_cfg::CenterConfig cfg2;
  std::string err2;
  if (!thyme::trigger_cfg::ParseCenterConfig(dumped, &cfg2, &err2)) {
    // Keep the offending input for triage before aborting the run.
    if (FILE* f = std::fopen("/tmp/thyme_fuzz_cfg_fail.ini", "wb")) {
      std::fwrite(text.data(), 1, text.size(), f);
      std::fclose(f);
    }
    std::fprintf(stderr,
                 "fuzz_trigger_config: DumpConfig round-trip failed: %s\n"
                 "(input saved to /tmp/thyme_fuzz_cfg_fail.ini)\n",
                 err2.c_str());
    CHECK(false);
  }
}

std::vector<std::string> SplitLines(const std::string& s) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos <= s.size()) {
    const std::size_t nl = s.find('\n', pos);
    out.push_back(s.substr(pos, nl - pos));
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  return out;
}

std::string JoinLines(const std::vector<std::string>& lines) {
  std::string out;
  for (const std::string& l : lines) {
    out += l;
    out.push_back('\n');
  }
  return out;
}

// INI-aware mutations: operate on whole lines/sections/keys/values so the
// parser's structure checks (not just the byte sink) get exercised.
std::string MutateIni(std::string text, SplitMix64* rng) {
  std::vector<std::string> lines = SplitLines(text);
  const int ops = 1 + static_cast<int>(rng->Below(3));
  for (int op = 0; op < ops; ++op) {
    if (lines.empty()) break;
    switch (rng->Below(12)) {
      case 0:  // swap two lines (section/key shuffling)
      case 1: {
        const std::size_t a = rng->Below(lines.size());
        const std::size_t b = rng->Below(lines.size());
        std::swap(lines[a], lines[b]);
        break;
      }
      case 2:  // duplicate a line (duplicate section / duplicate key)
        lines.insert(lines.begin() +
                         static_cast<std::ptrdiff_t>(rng->Below(lines.size())),
                     rng->Pick(lines));
        break;
      case 3:  // drop a line
        lines.erase(lines.begin() +
                    static_cast<std::ptrdiff_t>(rng->Below(lines.size())));
        break;
      case 4:  // truncate mid-line
        lines.resize(rng->Below(lines.size() + 1));
        if (!lines.empty() && rng->Coin(2)) {
          std::string& last = lines.back();
          last.resize(rng->Below(last.size() + 1));
        }
        break;
      case 5: {  // hostile numeric values into an existing assignment
        static const char* const kNumbers[] = {
            "-1",        "0",          "1",
            "2147483647", "2147483648", "4294967295",
            "4294967296", "18446744073709551615", "18446744073709551616",
            "999999999999999999999999", "1e9",   "1e999",
            "nan",       "inf",        "0x10",
            "1.5.6",     " 12 ",       "12abc"};
        for (std::string& l : lines) {
          if (rng->Coin(3)) continue;
          const std::size_t eq = l.find('=');
          if (eq == std::string::npos) continue;
          l = l.substr(0, eq + 1) + " " +
              rng->Pick(std::vector<std::string>{
                  kNumbers[0],  kNumbers[1],  kNumbers[2],  kNumbers[3],
                  kNumbers[4],  kNumbers[5],  kNumbers[6],  kNumbers[7],
                  kNumbers[8],  kNumbers[9],  kNumbers[10], kNumbers[11],
                  kNumbers[12], kNumbers[13], kNumbers[14], kNumbers[15],
                  kNumbers[16], kNumbers[17]});
          break;
        }
        break;
      }
      case 6: {  // case-mangle or corrupt a section header
        for (std::string& l : lines) {
          if (l.empty() || l.front() != '[' || rng->Coin(3)) continue;
          const std::size_t at = 1 + rng->Below(l.size() - 1);
          l[at] = static_cast<char>(rng->Byte() | 0x20u);
          if (rng->Coin(3)) l.insert(at, 1, '.');
          break;
        }
        break;
      }
      case 7: {  // rename a key to a plausible typo / unknown key
        static const char* const kKeys[] = {
            "socket",    "sockt",     "priority",   "priorty",
            "before_s",  "before",    "channels",   "quota_class",
            "use_cgroup", "gop_s",    "enable",     "max_triggers_per_hour",
            "rate_hz",   "kind",      "unknown_key"};
        for (std::string& l : lines) {
          if (rng->Coin(3)) continue;
          const std::size_t eq = l.find('=');
          if (eq == std::string::npos) continue;
          l = std::string(rng->Pick(std::vector<std::string>{
                  kKeys[0], kKeys[1], kKeys[2], kKeys[3], kKeys[4], kKeys[5],
                  kKeys[6], kKeys[7], kKeys[8], kKeys[9], kKeys[10], kKeys[11],
                  kKeys[12], kKeys[13], kKeys[14]})) +
              " = " + (eq + 1 < l.size() ? l.substr(eq + 1) : "x");
          break;
        }
        break;
      }
      case 8:  // inject structural characters / non-UTF8 / NUL
        if (!lines.empty()) {
          std::string& l = lines[rng->Below(lines.size())];
          static const char kInject[] = {'#', '=', '[', ']', '\r', '\t',
                                         ',', '*', '@', '!', ';'};
          const char c = rng->Coin(4)
                             ? static_cast<char>(rng->Byte() | 0x80u)
                             : (rng->Coin(4) ? '\0'
                                             : rng->Pick(std::vector<char>{
                                                   kInject[0], kInject[1],
                                                   kInject[2], kInject[3],
                                                   kInject[4], kInject[5],
                                                   kInject[6], kInject[7],
                                                   kInject[8], kInject[9],
                                                   kInject[10]}));
          l.insert(rng->Below(l.size() + 1), 1, c);
        }
        break;
      case 9: {  // long line (512B+ stresses fixed buffers downstream)
        std::string long_line = "socket = /tmp/";
        long_line.append(300 + rng->Below(1500), 'a');
        lines.insert(lines.begin() +
                         static_cast<std::ptrdiff_t>(rng->Below(lines.size())),
                     long_line);
        break;
      }
      case 10: {  // channel selector / continuous rule games
        static const char* const kSels[] = {
            "channels = *",
            "channels = pinned_slot:*",
            "channels = pinned_slot:/x@dec=3",
            "channels = pinned_slot:/x@dec=0",
            "channels = pinned_slot:/x@dec=9999999",
            "channels = !small_struct:/x",
            "channels = /h264_*",
            "channels = ,",
            "channels = *@dec=2",
            "auto_arm = true",
            "wait_idle = true",
            "demo_idle = true"};
        lines.insert(lines.begin() +
                         static_cast<std::ptrdiff_t>(rng->Below(lines.size())),
                     rng->Pick(std::vector<std::string>{
                         kSels[0], kSels[1], kSels[2], kSels[3], kSels[4],
                         kSels[5], kSels[6], kSels[7], kSels[8], kSels[9],
                         kSels[10], kSels[11]}));
        break;
      }
      default:  // generic byte flips on the joined text happen after join
        break;
    }
  }
  text = JoinLines(lines);
  // ...plus raw byte flips on the joined text (1 in 3 structured runs).
  if (rng->Coin(3) && !text.empty()) {
    for (int f = 0; f < 1 + static_cast<int>(rng->Below(8)); ++f) {
      text[rng->Below(text.size())] =
          static_cast<char>(rng->Byte());
    }
  }
  return text;
}

std::vector<uint8_t> Mutate(std::vector<uint8_t> in, SplitMix64* rng) {
  // Half structured (line-level), half byte-level.
  if (rng->Coin(2)) {
    const std::string text(reinterpret_cast<const char*>(in.data()),
                           in.size());
    const std::string mutated = MutateIni(text, rng);
    return std::vector<uint8_t>(mutated.begin(), mutated.end());
  }
  const int ops = 1 + static_cast<int>(rng->Below(3));
  for (int op = 0; op < ops && in.size() <= kMaxInput; ++op) {
    switch (rng->Below(6)) {
      case 0:
        thyme_fuzz::FlipRandomBytes(&in, rng, 1 + rng->Below(8));
        break;
      case 1:
        thyme_fuzz::Truncate(&in, rng);
        break;
      case 2:
        thyme_fuzz::ExtendRandom(&in, rng, kMaxInput);
        break;
      case 3:
        thyme_fuzz::DeleteSpan(&in, rng);
        break;
      case 4:
        thyme_fuzz::DuplicateSpan(&in, rng, kMaxInput);
        break;
      default:
        thyme_fuzz::FillRandomSpan(&in, rng, rng->Coin(2));
        break;
    }
  }
  return in;
}

}  // namespace

#if defined(THYME_FUZZ_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  RunOneInput(data, size);
  return 0;
}
#else
int main(int argc, char** argv) {
  const thyme_fuzz::RunConfig cfg =
      thyme_fuzz::ParseRunConfig(argc, argv, kDefaultIters, kDefaultSeed);

  std::vector<std::vector<uint8_t>> corpus =
      thyme_fuzz::LoadCorpusDir(THYME_FUZZ_CORPUS_DIR);
  const auto add = [&corpus](const char* s) {
    corpus.emplace_back(reinterpret_cast<const uint8_t*>(s),
                        reinterpret_cast<const uint8_t*>(s) +
                            std::strlen(s));
  };
  add(kMinimalIni);
  add(kFullIni);
  add("");
  CHECK(!corpus.empty());

  for (const auto& s : corpus) RunOneInput(s.data(), s.size());

  // The in-code seeds must be valid (harness self-check).
  {
    thyme::trigger_cfg::CenterConfig c;
    std::string err;
    CHECK(thyme::trigger_cfg::ParseCenterConfig(kMinimalIni, &c, &err));
    CHECK(thyme::trigger_cfg::ParseCenterConfig(kFullIni, &c, &err));
  }

  uint64_t accepted = 0;
  for (uint64_t i = 0; i < cfg.iterations; ++i) {
    SplitMix64 rng(thyme_fuzz::IterSeed(cfg.seed, i));
    std::vector<uint8_t> in = corpus[rng.Below(corpus.size())];
    in = Mutate(std::move(in), &rng);
    RunOneInput(in.data(), in.size());
    ++accepted;
  }
  std::printf("fuzz_trigger_config: %llu inputs, seed=%llu, corpus=%zu, OK\n",
              static_cast<unsigned long long>(accepted),
              static_cast<unsigned long long>(cfg.seed), corpus.size());
  return 0;
}
#endif
