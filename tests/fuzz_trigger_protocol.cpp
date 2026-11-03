// fuzz_trigger_protocol.cpp — TST-03 structured fuzz driver for the trigger
// center datagram protocol (Center::ProcessRequest: V1/V2/V3 size dispatch,
// magic/version checks, SEC-04 CRC, SEC-02 replay window, TRG-07 cancel/wait
// flags, FSM submit path). Deterministic splitmix64 mutations; the Center is
// shared across inputs exactly like the production socket loop.
//
// Usage: fuzz_trigger_protocol [iterations] [seed]
//   env THYME_FUZZ_ITERS / THYME_FUZZ_SEED override argv.

#include "thyme/check.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_protocol.h"

#include "fuzz_harness.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using thyme::trigger_center::Center;
using thyme::trigger_center::PeerCred;
using thyme::trigger_proto::TriggerRequest;
using thyme_fuzz::SplitMix64;

constexpr std::size_t kMaxInput = 512;
constexpr uint64_t kDefaultIters = 50000;
constexpr uint64_t kDefaultSeed = 0x54524947u;  // "TRIG"

// ---- Fake recorder hooks (never fail, capture completes on next Tick) -----
bool HookReady(void*) { return true; }
bool HookActive(void*) { return false; }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  return true;
}
bool HookPreempt(void*) { return true; }

Center& TheCenter() {
  static Center center;
  static bool inited = false;
  if (!inited) {
    thyme::trigger_cfg::CenterConfig c;
    char sock[128];
    std::snprintf(sock, sizeof(sock), "/tmp/thyme_fuzz_trg_%ld.sock",
                  static_cast<long>(getpid()));
    c.socket_path = sock;
    c.audit_max_bytes = 64ull * 1024 * 1024;  // no rotation churn during a run
    thyme::trigger_cfg::SourceRule manual;
    manual.name = "manual";
    manual.before_ns = 1000000000ull;
    manual.after_ns = 500000000ull;
    manual.priority = 10;
    manual.quota_class = "manual";
    thyme::trigger_cfg::SourceRule crash;
    crash.name = "crash";
    crash.before_ns = 1000000000ull;
    crash.after_ns = 500000000ull;
    crash.priority = 100;
    crash.quota_class = "safety";
    c.sources = {manual, crash};
    c.quota_limits = {{"manual", 0}, {"safety", 0}};
    thyme::trigger_center::Hooks hooks;
    hooks.recorder_ready = HookReady;
    hooks.capture_active = HookActive;
    hooks.start_capture = HookStart;
    hooks.preempt_capture = HookPreempt;
    CHECK(center.Init(c, hooks));
    inited = true;
  }
  return center;
}

uint64_t g_sequence = 1;  // per-run monotonic anti-replay counter

// Canonical seeds: valid wire requests built with the real structs + CRC.
std::vector<uint8_t> MakeV3(uint16_t flags, uint64_t timestamp_ns,
                            int32_t priority_hint, uint32_t timeout_ms,
                            const char* source) {
  TriggerRequest req{};
  req.magic = thyme::trigger_proto::kTriggerMagic;
  req.version = thyme::trigger_proto::kTriggerVersionV3;
  req.flags = flags;
  req.priority_hint = priority_hint;
  req.timestamp_ns = timestamp_ns;
  if (source != nullptr) {
    std::strncpy(req.source, source, sizeof(req.source) - 1);
  }
  req.sequence = 1;  // re-stamped per send by the structured mutator
  req.response_timeout_ms = timeout_ms;
  thyme::trigger_proto::StampRequestCrc(&req);
  const auto* p = reinterpret_cast<const uint8_t*>(&req);
  return std::vector<uint8_t>(p, p + sizeof(req));
}

std::vector<uint8_t> MakeV2() {
  thyme::trigger_proto::TriggerRequestV2 req{};
  req.magic = thyme::trigger_proto::kTriggerMagic;
  req.version = thyme::trigger_proto::kTriggerVersionV2;
  req.priority_hint = -1;
  std::strncpy(req.source, "manual", sizeof(req.source) - 1);
  req.sequence = 1;
  const auto* p = reinterpret_cast<const uint8_t*>(&req);
  return std::vector<uint8_t>(p, p + sizeof(req));
}

std::vector<uint8_t> MakeV1() {
  thyme::trigger_proto::TriggerRequestV1 req{};
  req.magic = thyme::trigger_proto::kTriggerMagic;
  req.version = thyme::trigger_proto::kTriggerVersionLegacy;
  req.priority_hint = -1;
  std::strncpy(req.source, "manual", sizeof(req.source) - 1);
  const auto* p = reinterpret_cast<const uint8_t*>(&req);
  return std::vector<uint8_t>(p, p + sizeof(req));
}

// The fuzz target proper: one datagram through the real request handler.
// Defined-result contract: a reply carries the protocol magic, a known
// response version, and (V3) a valid CRC trailer.
void RunOneInput(const uint8_t* data, std::size_t size) {
  if (size > kMaxInput) return;
  Center& center = TheCenter();
  PeerCred peer;
  peer.uid = geteuid();
  peer.pid = getpid();
  thyme::trigger_proto::TriggerResponse resp{};
  const bool reply = center.ProcessRequest(data, size, peer, &resp, nullptr);
  if (reply) {
    CHECK(resp.magic == thyme::trigger_proto::kTriggerMagic);
    CHECK(resp.version == thyme::trigger_proto::kTriggerVersionLegacy ||
          resp.version == thyme::trigger_proto::kTriggerVersionV2 ||
          resp.version == thyme::trigger_proto::kTriggerVersionV3);
    if (resp.version == thyme::trigger_proto::kTriggerVersionV3) {
      CHECK(resp.reserved0 == 0);
      CHECK(resp.crc32 == thyme::trigger_proto::ResponseCrc(resp));
    }
  }
  // Drain the FSM periodically so accepted triggers start/complete and the
  // pending queue does not pin every later request at kBusy.
  static uint64_t calls = 0;
  if ((++calls & 0x3F) == 0) center.Tick();
}

void Poke(std::vector<uint8_t>* b, std::size_t at, uint64_t value, int width) {
  if (b->size() < static_cast<std::size_t>(width) ||
      at > b->size() - static_cast<std::size_t>(width)) {
    return;
  }
  for (int i = 0; i < width; ++i) {
    (*b)[at + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

// Structured field mutations on a V3-layout buffer. All offsets come from the
// public wire struct, so a layout change fails at compile time, not as a
// silent fuzz-coverage loss.
void MutateV3Fields(std::vector<uint8_t>* in, SplitMix64* rng) {
  using thyme::trigger_proto::kFlagCancel;
  using thyme::trigger_proto::kFlagForce;
  using thyme::trigger_proto::kFlagHasTimestamp;
  using thyme::trigger_proto::kFlagWaitOutcome;
  switch (rng->Below(8)) {
    case 0:
      Poke(in, offsetof(TriggerRequest, magic), rng->Next(), 4);
      break;
    case 1:
      Poke(in, offsetof(TriggerRequest, version),
           rng->Pick(std::vector<uint64_t>{0, 1, 2, 3, 4, 0xFFFF}), 2);
      break;
    case 2:
      Poke(in, offsetof(TriggerRequest, flags),
         rng->Pick(std::vector<uint64_t>{0, kFlagHasTimestamp, kFlagForce,
                                         kFlagCancel,
               kFlagHasTimestamp | kFlagCancel,
                                         kFlagHasTimestamp | kFlagWaitOutcome,
                                         kFlagCancel | kFlagForce, 0xFFFF}),
           2);
      break;
    case 3:
      Poke(in, offsetof(TriggerRequest, priority_hint),
         rng->Pick(std::vector<uint64_t>{0, 1, 10, 100, 1000, 1001, 0x7FFFFFFFu,
                                         0xFFFFFFFFu}),
           4);
      break;
    case 4:
      Poke(in, offsetof(TriggerRequest, timestamp_ns),
         rng->Pick(std::vector<uint64_t>{
             0, 1, 42, thyme::trigger_proto::kWaitFilterAny, ~0ull - 1}),
           8);
      break;
    case 5:
      Poke(in, offsetof(TriggerRequest, response_timeout_ms),
         rng->Pick(std::vector<uint64_t>{0, 1, 100, 60000, 60001, 0xFFFFFFFFu}),
           4);
      break;
    case 6: {  // source: known / unknown / unterminated / empty
      static const char* const kSources[] = {"manual", "crash", "nope", ""};
      char src[64];
      std::memset(src, rng->Coin(4) ? 'A' : 0, sizeof(src));  // 1/4: no NUL
      const char* s = rng->Pick(std::vector<const char*>{
          kSources[0], kSources[1], kSources[2], kSources[3]});
      if (!rng->Coin(4)) {
        std::strncpy(src, s, sizeof(src) - 1);
      }
      if (in->size() >= offsetof(TriggerRequest, source) + sizeof(src)) {
        std::memcpy(in->data() + offsetof(TriggerRequest, source), src,
                    sizeof(src));
      }
      break;
    }
    default:  // sequence: replay-window edges
      Poke(in, offsetof(TriggerRequest, sequence),
         rng->Pick(std::vector<uint64_t>{
             0, 1, g_sequence, g_sequence > 64 ? g_sequence - 64 : 0}),
           8);
      break;
  }
}

std::vector<uint8_t> Mutate(std::vector<uint8_t> in, SplitMix64* rng) {
  const bool structured_v3 =
      in.size() == sizeof(TriggerRequest) && rng->Coin(2);
  if (structured_v3) {
    const int ops = 1 + static_cast<int>(rng->Below(3));
    for (int i = 0; i < ops; ++i)
      MutateV3Fields(&in, rng);
    if (rng->Coin(2) && in.size() == sizeof(TriggerRequest)) {
      // Deep path: fresh sequence + valid CRC, so the mutated fields reach
      // replay/timeout/flag handling and the FSM.
      TriggerRequest req;
      std::memcpy(&req, in.data(), sizeof(req));
      req.sequence = ++g_sequence;
      thyme::trigger_proto::StampRequestCrc(&req);
      std::memcpy(in.data(), &req, sizeof(req));
    } else if (rng->Coin(2) && in.size() >= 4) {
      // Corrupt-CRC path: the field mutations above stand, CRC stays stale.
      const std::size_t at = in.size() - 1 - rng->Below(4);
      in[at] =
          static_cast<uint8_t>(in[at] ^ static_cast<uint8_t>(rng->Byte() | 1u));
    }
    return in;
  }
  // Byte-level chaos (any layout/size).
  const int ops = 1 + static_cast<int>(rng->Below(3));
  for (int op = 0; op < ops && in.size() <= kMaxInput; ++op) {
    switch (rng->Below(8)) {
      case 0:
        thyme_fuzz::FlipRandomBit(&in, rng);
        break;
      case 1:
        thyme_fuzz::FlipRandomBytes(&in, rng, 1 + rng->Below(8));
        break;
      case 2:
        thyme_fuzz::Truncate(&in, rng);
        break;
      case 3:
        thyme_fuzz::ExtendRandom(&in, rng, kMaxInput);
        break;
      case 4:
        thyme_fuzz::FillRandomSpan(&in, rng, rng->Coin(2));
        break;
      case 5:
        thyme_fuzz::DeleteSpan(&in, rng);
        break;
      case 6:
        thyme_fuzz::DuplicateSpan(&in, rng, kMaxInput);
        break;
      default:  // land exactly on a wire size with random garbage
        in.resize(rng->Pick(std::vector<std::size_t>{
            sizeof(thyme::trigger_proto::TriggerRequestV1),
            sizeof(thyme::trigger_proto::TriggerRequestV2),
            sizeof(TriggerRequest)}));
        rng->Fill(in.data(), in.size());
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

  namespace tp = thyme::trigger_proto;
  std::vector<std::vector<uint8_t>> corpus =
      thyme_fuzz::LoadCorpusDir(THYME_FUZZ_CORPUS_DIR);
  corpus.push_back(MakeV3(0, 0, -1, 100, "manual"));
  corpus.push_back(MakeV3(tp::kFlagHasTimestamp, 1, 50, 100, "crash"));
  corpus.push_back(
      MakeV3(tp::kFlagHasTimestamp | tp::kFlagCancel, 1, -1, 100, nullptr));
  corpus.push_back(MakeV3(tp::kFlagHasTimestamp | tp::kFlagWaitOutcome,
                          tp::kWaitFilterAny, -1, 60000, nullptr));
  corpus.push_back(MakeV2());
  corpus.push_back(MakeV1());
  corpus.push_back(std::vector<uint8_t>{});
  CHECK(!corpus.empty());

  for (const auto &s : corpus)
    RunOneInput(s.data(), s.size());

  // The well-formed submit seed must be accepted (guards against the whole
  // corpus silently degrading to malformed-input coverage only).
  {
    std::vector<uint8_t> submit = MakeV3(0, 0, -1, 100, "manual");
    TriggerRequest req;
    std::memcpy(&req, submit.data(), sizeof(req));
    req.sequence = ++g_sequence;
    tp::StampRequestCrc(&req);
    std::memcpy(submit.data(), &req, sizeof(req));
    Center& center = TheCenter();
    PeerCred peer;
    peer.uid = geteuid();
    peer.pid = getpid();
    tp::TriggerResponse resp{};
    CHECK(center.ProcessRequest(submit.data(), submit.size(), peer, &resp,
                                nullptr));
    CHECK(resp.code == static_cast<uint16_t>(tp::TriggerCode::kAccepted));
    center.Tick();
  }

  for (uint64_t i = 0; i < cfg.iterations; ++i) {
    SplitMix64 rng(thyme_fuzz::IterSeed(cfg.seed, i));
    std::vector<uint8_t> in = corpus[rng.Below(corpus.size())];
    in = Mutate(std::move(in), &rng);
    RunOneInput(in.data(), in.size());
  }
  TheCenter().Shutdown();
  char path[160];
  std::snprintf(path, sizeof(path), "/tmp/thyme_fuzz_trg_%ld.sock.audit",
                static_cast<long>(getpid()));
  std::remove(path);
  std::printf("fuzz_trigger_protocol: %llu inputs, seed=%llu, corpus=%zu, "
              "OK\n",
              static_cast<unsigned long long>(cfg.iterations),
              static_cast<unsigned long long>(cfg.seed), corpus.size());
  return 0;
}
#endif
