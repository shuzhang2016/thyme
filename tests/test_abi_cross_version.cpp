// test_abi_cross_version.cpp — ABI-04 cross-version reader/writer matrix.
//
// Every versioned wire/SHM format in thyme has an explicit reject rule
// (docs/reference/abi-compatibility.md). This test drives the *documented*
// old/new combinations through the current implementation and asserts the exact
// result, so a layout/version bump cannot silently start accepting a peer
// this build does not understand.
//
// Old readers that no longer exist are reconstructed from the documented
// layouts (byte images / header structs with static_asserts on the old
// offsets), never by keeping a dead code path alive:
//   * trigger protocol V1 (88 B) / V2 (96 B) requests: real structs still
//     shipped for recognition, exercised through Center::ProcessRequest and
//     through a real AF_UNIX datagram socket (reply-size check).
//   * PinnedSlotPageHeader v2 (192 B): synthetic v2 header image; a v3
//     reader/writer must reject it, and a reconstructed v2 probe must reject
//     the v3 header.
//   * .thyme.bin v1 (unindexed): a genuine v1 image produced by the encoder
//     on a non-seekable sink (the documented v1 writer path).
//   * ThymeStructDesc / FrameHeader v1: unknown version/flags must be
//     rejected explicitly instead of being interpreted as today's layout.
//
// Each rejection case asserts that *no partial data* is delivered: no reply,
// no accepted trigger, no reader header/lease, no record.

#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/contracts/struct_desc.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_client.h"
#include "thyme/record/trigger_config.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

// ---- version constants: one place that pins the whole matrix ---------------
static_assert(trigger_proto::kTriggerVersion == 4,
              "trigger protocol is v4 (TRG-19)");
static_assert(trigger_proto::kTriggerVersionV3 == 3,
              "trigger protocol v3 remains supported");
static_assert(trigger_proto::kTriggerVersionV2 == 2, "v2 request is 96 B");
static_assert(trigger_proto::kTriggerVersionLegacy == 1, "v1 request is 88 B");
static_assert(PinnedSlotPageHeader::kVersion == 4, "channel v4 (ARCH-11)");
static_assert(kPinnedSlotLeaseVersion == 6, "lease v6 (ARCH-11)");
static_assert(kShmSpscRingVersion == 4, "spsc ring v4 (ARCH-11)");
static_assert(kShmSpmcRingVersion == 4, "spmc ring v4 (ARCH-11)");
static_assert(kThymeBinFileAbiVersion == 4,
              "v4 = ABI-11 keyed registry + manifest block");
static_assert(kThymeBinFileAbiVersionManifest == 3,
              "v3 = record-manifest block (ABI-10)");
static_assert(kThymeBinFileAbiVersionIndexed == 2,
              "v2 = footer index, no manifest");
static_assert(kThymeBinFileAbiVersionUnindexed == 1, "streams stay v1");
static_assert(kThymeBinIndexVersion == 1, "footer index is v1");
static_assert(sizeof(ThymeStructDesc) == 128, "ABI-11 registry entry is 128 B");
static_assert(kThymeStructDescLegacySize == 40, "legacy entry is 40 B");
static_assert(sizeof(PinnedSlotPageHeader) == 384, "v4 header is 384 B");
static_assert(sizeof(trigger_proto::TriggerRequest) == 104, "v3 request");
static_assert(sizeof(trigger_proto::TriggerRequestV2) == 96, "v2 request");
static_assert(sizeof(trigger_proto::TriggerRequestV1) == 88, "v1 request");
static_assert(sizeof(trigger_proto::TriggerResponse) == 32, "v3 response");
static_assert(sizeof(trigger_proto::TriggerResponseV2) == 24, "v2 response");
static_assert(sizeof(trigger_proto::TriggerResponseV1) == 16, "v1 response");

// Response prefix compatibility: the shorter layouts are prefixes of the
// widest struct by construction, so a V1/V2 peer only ever receives bytes it
// knows (FillResponse writes the widest struct, the socket sends the prefix).
static_assert(offsetof(trigger_proto::TriggerResponse, code) +
                      sizeof(uint16_t) <=
                  sizeof(trigger_proto::TriggerResponseV1),
              "V1 response prefix covers code");
static_assert(offsetof(trigger_proto::TriggerResponse, trigger_id) +
                      sizeof(uint64_t) <=
                  sizeof(trigger_proto::TriggerResponseV1),
              "V1 response prefix covers trigger_id");
static_assert(offsetof(trigger_proto::TriggerResponse, flags) +
                      sizeof(uint32_t) <=
                  sizeof(trigger_proto::TriggerResponseV2),
              "V2 response prefix covers flags");

// ---- trigger center fixture ------------------------------------------------

struct HookState {
  bool active = false;
  int start_calls = 0;
  int preempt_calls = 0;
};

bool HookReady(void*) { return true; }
bool HookActive(void* ctx) {
  return static_cast<HookState*>(ctx)->active;
}
bool HookStart(void* ctx, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  auto* s = static_cast<HookState*>(ctx);
  ++s->start_calls;
  s->active = true;
  return true;
}
bool HookPreempt(void* ctx) {
  auto* s = static_cast<HookState*>(ctx);
  ++s->preempt_calls;
  s->active = false;
  return true;
}

int g_fixture_id = 0;

trigger_center::PeerCred OwnPeer() {
  trigger_center::PeerCred p;
  p.uid = geteuid();
  p.pid = getpid();
  return p;
}

// A Center with only the "manual" source. Legacy compat flags are per-case so
// one test can cover both the fail-closed default and the documented opt-in.
struct CenterFixture {
  CenterFixture(bool allow_v1, bool allow_v2) {
    char base[64];
    std::snprintf(base, sizeof(base), "/tmp/abi04cv_%d_%d",
                  static_cast<int>(getpid()), ++g_fixture_id);
    sock = std::string(base) + ".sock";
    audit = std::string(base) + ".audit";
    ::unlink(sock.c_str());
    ::unlink(audit.c_str());

    trigger_cfg::CenterConfig cfg;
    cfg.socket_path = sock;
    cfg.audit_log_path = audit;
    cfg.audit_max_bytes = 1u << 20;
    cfg.allow_legacy_unsequenced = allow_v1;
    cfg.allow_legacy_v2 = allow_v2;
    trigger_cfg::SourceRule manual;
    manual.name = "manual";
    manual.priority = 10;
    manual.quota_class = "manual";
    manual.before_ns = manual.after_ns = 1000000000ULL;
    cfg.sources = {manual};
    cfg.quota_limits = {{"manual", 0}};

    trigger_center::Hooks hooks;
    hooks.ctx = &hook;
    hooks.recorder_ready = HookReady;
    hooks.capture_active = HookActive;
    hooks.start_capture = HookStart;
    hooks.preempt_capture = HookPreempt;
    CHECK(center.Init(cfg, hooks));
  }

  ~CenterFixture() {
    center.Shutdown();
    ::unlink(sock.c_str());
    ::unlink(audit.c_str());
    ::unlink((audit + ".1").c_str());
    ::unlink((audit + ".2").c_str());
    ::unlink((sock + ".fsm").c_str());
  }

  CenterFixture(const CenterFixture&) = delete;
  CenterFixture& operator=(const CenterFixture&) = delete;

  trigger_center::Center center;
  HookState hook;
  std::string sock;
  std::string audit;
};

trigger_proto::TriggerRequest MakeV3(uint16_t flags = 0, uint64_t ts = 0) {
  trigger_proto::TriggerRequest req;
  req.magic = trigger_proto::kTriggerMagic;
  req.version = trigger_proto::kTriggerVersionV3;
  req.flags = flags;
  req.priority_hint = -1;
  req.timestamp_ns = ts;
  std::snprintf(req.source, sizeof(req.source), "%s", "manual");
  req.sequence = trigger_client::NextSequence();
  req.response_timeout_ms = trigger_proto::kDefaultResponseTimeoutMs;
  trigger_proto::StampRequestCrc(&req);
  return req;
}

trigger_proto::TriggerRequestV2 MakeV2(uint16_t flags = 0,
                                       uint64_t sequence = 0) {
  trigger_proto::TriggerRequestV2 req;
  req.magic = trigger_proto::kTriggerMagic;
  req.version = trigger_proto::kTriggerVersionV2;
  req.flags = flags;
  req.priority_hint = -1;
  std::snprintf(req.source, sizeof(req.source), "%s", "manual");
  req.sequence = sequence != 0 ? sequence : trigger_client::NextSequence();
  return req;
}

trigger_proto::TriggerRequestV1 MakeV1(uint16_t flags = 0) {
  trigger_proto::TriggerRequestV1 req;
  req.magic = trigger_proto::kTriggerMagic;
  req.version = trigger_proto::kTriggerVersionLegacy;
  req.flags = flags;
  req.priority_hint = -1;
  std::snprintf(req.source, sizeof(req.source), "%s", "manual");
  return req;
}

struct Reply {
  bool replied = false;
  trigger_proto::TriggerResponse resp{};
};

Reply SendRequest(trigger_center::Center& c, const void* buf, std::size_t n) {
  Reply r;
  r.replied = c.ProcessRequest(static_cast<const uint8_t*>(buf), n, OwnPeer(),
                               &r.resp);
  return r;
}

// Expected on-wire response size for the layout version the server selected.
std::size_t ResponseBytes(uint16_t version) {
  if (version == trigger_proto::kTriggerVersionLegacy) {
    return sizeof(trigger_proto::TriggerResponseV1);
  }
  if (version == trigger_proto::kTriggerVersionV2) {
    return sizeof(trigger_proto::TriggerResponseV2);
  }
  CHECK(version == trigger_proto::kTriggerVersionV3);
  return sizeof(trigger_proto::TriggerResponse);
}

// A malformed datagram: no reply, exactly one malformed counter, no accept.
void ExpectMalformed(trigger_center::Center& c, const void* buf, std::size_t n,
                     uint64_t* malformed) {
  const uint64_t accepted = c.stats().accepted;
  const Reply r = SendRequest(c, buf, n);
  CHECK(!r.replied);
  *malformed += 1;
  CHECK(c.stats().malformed_datagrams == *malformed);
  CHECK(c.stats().accepted == accepted);
}

// ---- trigger matrix --------------------------------------------------------

// v3 server × v3 client, plus unknown/future shapes that must not be recorded.
void TestTriggerV3Reader() {
  CenterFixture f(/*allow_v1=*/false, /*allow_v2=*/false);
  auto& c = f.center;

  // v3 × v3: accepted with the v3 layout (32 B, CRC trailer).
  {
    const auto req = MakeV3();
    const Reply r = SendRequest(c, &req, sizeof(req));
    CHECK(r.replied);
    CHECK(r.resp.version == trigger_proto::kTriggerVersionV3);
    CHECK(ResponseBytes(r.resp.version) ==
          sizeof(trigger_proto::TriggerResponse));
    CHECK(r.resp.code ==
          static_cast<uint16_t>(trigger_proto::TriggerCode::kAccepted));
    CHECK(r.resp.trigger_id != 0);
    CHECK(r.resp.reserved0 == 0);
    CHECK(r.resp.crc32 == trigger_proto::ResponseCrc(r.resp));
    CHECK(c.stats().accepted == 1);
    CHECK(c.stats().malformed_datagrams == 0);
    CHECK(c.stats().legacy_requests == 0);
    CHECK(c.stats().legacy_v2_requests == 0);
    CHECK(f.hook.start_calls == 0);  // T2: start happens in Tick, not here
  }

  uint64_t mal = c.stats().malformed_datagrams;
  const uint64_t accepted = c.stats().accepted;

  // v3 with an unknown flag bit (a future opcode inside the v3 layout) must be
  // malformed, never recorded as a plain trigger.
  {
    auto req = MakeV3(static_cast<uint16_t>(1u << 4));
    ExpectMalformed(c, &req, sizeof(req), &mal);
    CHECK(c.stats().accepted == accepted);
  }

  // Same, with the unknown bit next to a known one.
  {
    auto req = MakeV3(
        static_cast<uint16_t>(trigger_proto::kFlagHasTimestamp | (1u << 5)));
    req.timestamp_ns = 1;
    trigger_proto::StampRequestCrc(&req);
    ExpectMalformed(c, &req, sizeof(req), &mal);
  }

  // Right size, future version -> version mismatch (not parsed with v3 rules).
  {
    auto req = MakeV3();
    req.version = 4;
    trigger_proto::StampRequestCrc(&req);
    ExpectMalformed(c, &req, sizeof(req), &mal);
  }

  // Truncated / short datagrams: every size that is not a known layout is
  // malformed; 88/96 are the V1/V2 sizes, and the v3 version field makes them
  // a version mismatch there too.
  {
    const auto req = MakeV3();
    const std::size_t sizes[] = {0, 1, 8, 16, 50, 87, 88, 95, 96, 103, 105};
    for (std::size_t n : sizes) {
      ExpectMalformed(c, &req, n, &mal);
    }
  }

  // Oversized datagram.
  {
    uint8_t garbage[512];
    std::memset(garbage, 0xA5, sizeof(garbage));
    ExpectMalformed(c, garbage, sizeof(garbage), &mal);
  }

  // None of the rejected shapes touched the FSM or the trigger id.
  CHECK(c.stats().accepted == accepted);
  CHECK(accepted == 1);
}

// v3 server × v2 client: fail closed by default, accepted only with the
// documented opt-in; V2 has no CRC and no cancel semantics.
void TestTriggerLegacyV2() {
  {
    CenterFixture f(/*allow_v1=*/false, /*allow_v2=*/false);
    auto& c = f.center;
    const auto req = MakeV2();
    const Reply r = SendRequest(c, &req, sizeof(req));
    // Explicit kUnsupportedVersion in the V2 layout — not a silent timeout.
    CHECK(r.replied);
    CHECK(r.resp.version == trigger_proto::kTriggerVersionV2);
    CHECK(ResponseBytes(r.resp.version) ==
          sizeof(trigger_proto::TriggerResponseV2));
    CHECK(r.resp.code == static_cast<uint16_t>(
                             trigger_proto::TriggerCode::kUnsupportedVersion));
    CHECK(r.resp.trigger_id == 0);
    CHECK(r.resp.crc32 == 0);  // V2 has no CRC trailer (prefix only)
    CHECK(c.stats().accepted == 0);
    CHECK(c.stats().legacy_v2_requests == 0);
    CHECK(c.stats().malformed_datagrams == 1);
    CHECK(c.state() == trigger_center::State::kIdle);
    CHECK(f.hook.start_calls == 0);
  }

  {
    CenterFixture f(/*allow_v1=*/false, /*allow_v2=*/true);
    auto& c = f.center;
    const auto req = MakeV2();
    const Reply r = SendRequest(c, &req, sizeof(req));
    CHECK(r.replied);
    CHECK(r.resp.version == trigger_proto::kTriggerVersionV2);
    CHECK(r.resp.code ==
          static_cast<uint16_t>(trigger_proto::TriggerCode::kAccepted));
    CHECK(r.resp.trigger_id != 0);
    CHECK(r.resp.reserved0 == 0);  // V3-only tail stays 0 in the widest struct
    CHECK(r.resp.crc32 == 0);      // ...and is not transmitted for V2
    CHECK(c.stats().accepted == 1);
    CHECK(c.stats().legacy_v2_requests == 1);
    CHECK(c.stats().legacy_requests == 0);
    CHECK(c.stats().malformed_datagrams == 0);
    CHECK(c.state() == trigger_center::State::kCapturing);

    // V2 has no cancel/notify semantics: those flag bits are rejected as
    // malformed even with the legacy opt-in (never misread as a trigger).
    uint64_t mal = c.stats().malformed_datagrams;
    const uint64_t accepted = c.stats().accepted;
    auto cancel = MakeV2(static_cast<uint16_t>(
        trigger_proto::kFlagCancel | trigger_proto::kFlagHasTimestamp));
    cancel.timestamp_ns = 1;
    ExpectMalformed(c, &cancel, sizeof(cancel), &mal);
    auto wait = MakeV2(static_cast<uint16_t>(trigger_proto::kFlagWaitOutcome |
                                             trigger_proto::kFlagHasTimestamp));
    wait.timestamp_ns = 1;
    ExpectMalformed(c, &wait, sizeof(wait), &mal);
    // Sequence 0 is reserved and explicitly rejected in the V2 layout.
    auto unsequenced = MakeV2(0, trigger_client::NextSequence());
    unsequenced.sequence = 0;
    const Reply rej = SendRequest(c, &unsequenced, sizeof(unsequenced));
    CHECK(rej.replied);
    CHECK(rej.resp.version == trigger_proto::kTriggerVersionV2);
    CHECK(rej.resp.code ==
          static_cast<uint16_t>(trigger_proto::TriggerCode::kReplayRejected));
    CHECK(c.stats().accepted == accepted);
  }
}

// v3 server × v1 client: fail closed by default (no replay protection), the
// documented opt-in accepts it.
void TestTriggerLegacyV1() {
  {
    CenterFixture f(false, false);
    auto& c = f.center;
    const auto req = MakeV1();
    const Reply r = SendRequest(c, &req, sizeof(req));
    CHECK(r.replied);
    CHECK(r.resp.version == trigger_proto::kTriggerVersionLegacy);
    CHECK(ResponseBytes(r.resp.version) ==
          sizeof(trigger_proto::TriggerResponseV1));
    CHECK(r.resp.code == static_cast<uint16_t>(
                             trigger_proto::TriggerCode::kUnsupportedVersion));
    CHECK(r.resp.trigger_id == 0);
    CHECK(c.stats().accepted == 0);
    CHECK(c.stats().legacy_requests == 0);
    CHECK(c.stats().malformed_datagrams == 1);
    CHECK(f.hook.start_calls == 0);
  }

  {
    CenterFixture f(/*allow_v1=*/true, /*allow_v2=*/false);
    auto& c = f.center;
    const auto req = MakeV1();
    const Reply r = SendRequest(c, &req, sizeof(req));
    CHECK(r.replied);
    CHECK(r.resp.version == trigger_proto::kTriggerVersionLegacy);
    CHECK(r.resp.code ==
          static_cast<uint16_t>(trigger_proto::TriggerCode::kAccepted));
    CHECK(r.resp.trigger_id != 0);
    CHECK(c.stats().accepted == 1);
    CHECK(c.stats().legacy_requests == 1);
    CHECK(c.stats().legacy_v2_requests == 0);

    // Unknown flag bits fail closed on V1 too.
    uint64_t mal = c.stats().malformed_datagrams;
    const auto unknown = MakeV1(static_cast<uint16_t>(1u << 4));
    ExpectMalformed(c, &unknown, sizeof(unknown), &mal);
    CHECK(c.stats().accepted == 1);
  }
}

// The reconstructed old readers: a V1/V2 peer receives a 104-byte v3
// datagram. It reads its own layout size and must see a version mismatch.
// The current handler implements exactly that policy, so the case is run
// through it with real byte prefixes of a v3 datagram.
void TestOldReaderVsV3Datagram() {
  CenterFixture f(/*allow_v1=*/true, /*allow_v2=*/true);
  auto& c = f.center;
  uint64_t mal = 0;

  const auto req = MakeV3();
  // V2 reader: 96-byte recv buffer, sees version=3 -> mismatch (malformed).
  ExpectMalformed(c, &req, sizeof(trigger_proto::TriggerRequestV2), &mal);
  // V1 reader: 88-byte recv buffer, sees version=3 -> mismatch (malformed).
  ExpectMalformed(c, &req, sizeof(trigger_proto::TriggerRequestV1), &mal);
  // A v3 sender that edited version=2 but kept the 104-byte datagram: the
  // size selects V3 and the version must match (no cross-layout parse).
  {
    auto edited = MakeV3();
    edited.version = trigger_proto::kTriggerVersionV2;
    trigger_proto::StampRequestCrc(&edited);
    ExpectMalformed(c, &edited, sizeof(edited), &mal);
  }
  // A 96-byte V2 image claiming v3: also a version mismatch, never a mixed
  // parse of a v3 prefix as V2.
  {
    auto v2 = MakeV2();
    v2.version = trigger_proto::kTriggerVersionV3;
    ExpectMalformed(c, &v2, sizeof(v2), &mal);
  }
  CHECK(c.stats().accepted == 0);
  CHECK(c.stats().legacy_requests == 0);
  CHECK(c.stats().legacy_v2_requests == 0);
  CHECK(c.state() == trigger_center::State::kIdle);
}

// ---- socket path: the reply size follows the peer's layout -----------------

struct RawClient {
  RawClient(const std::string& server_path, const std::string& self_path)
      : self_path_(self_path) {
    fd_ = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(fd_ >= 0);
    ::unlink(self_path_.c_str());
    self_.sun_family = AF_UNIX;
    std::snprintf(self_.sun_path, sizeof(self_.sun_path), "%s",
                  self_path_.c_str());
    CHECK(bind(fd_, reinterpret_cast<sockaddr*>(&self_), sizeof(self_)) == 0);
    server_.sun_family = AF_UNIX;
    std::snprintf(server_.sun_path, sizeof(server_.sun_path), "%s",
                  server_path.c_str());
  }
  ~RawClient() {
    if (fd_ >= 0) ::close(fd_);
    ::unlink(self_path_.c_str());
  }
  void Send(const void* buf, std::size_t n) {
    CHECK(sendto(fd_, buf, n, 0, reinterpret_cast<sockaddr*>(&server_),
                 sizeof(server_)) == static_cast<ssize_t>(n));
  }
  ssize_t Recv(void* buf, std::size_t cap) {
    return recv(fd_, buf, cap, MSG_DONTWAIT);
  }

 private:
  int fd_ = -1;
  sockaddr_un self_{};
  sockaddr_un server_{};
  std::string self_path_;
};

void TestTriggerSocketReplySizes() {
  CenterFixture f(/*allow_v1=*/false, /*allow_v2=*/false);
  auto& c = f.center;
  const std::string self_path = f.sock + ".cli";
  RawClient raw(f.sock, self_path);

  // V3 peer: accepted, 32-byte reply with a valid CRC.
  {
    const auto req = MakeV3();
    raw.Send(&req, sizeof(req));
    c.Tick();
    trigger_proto::TriggerResponse resp{};
    const ssize_t n = raw.Recv(&resp, sizeof(resp));
    CHECK(n == static_cast<ssize_t>(sizeof(trigger_proto::TriggerResponse)));
    CHECK(resp.version == trigger_proto::kTriggerVersionV3);
    CHECK(resp.code ==
          static_cast<uint16_t>(trigger_proto::TriggerCode::kAccepted));
    CHECK(resp.crc32 == trigger_proto::ResponseCrc(resp));
  }
  // V2 peer (legacy disabled): exactly the 24-byte V2 layout, explicit
  // kUnsupportedVersion — an old client must never receive a 32-byte datagram
  // (it would fail its size check) nor wait for a timeout.
  {
    const auto req = MakeV2();
    raw.Send(&req, sizeof(req));
    c.Tick();
    uint8_t buf[sizeof(trigger_proto::TriggerResponse) + 8];
    std::memset(buf, 0xEE, sizeof(buf));
    const ssize_t n = raw.Recv(buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(sizeof(trigger_proto::TriggerResponseV2)));
    trigger_proto::TriggerResponseV2 resp{};
    std::memcpy(&resp, buf, sizeof(resp));
    CHECK(resp.magic == trigger_proto::kTriggerMagic);
    CHECK(resp.version == trigger_proto::kTriggerVersionV2);
    CHECK(resp.code == static_cast<uint16_t>(
                           trigger_proto::TriggerCode::kUnsupportedVersion));
    CHECK(resp.trigger_id == 0);
    // Bytes past the V2 prefix were not written into the client buffer.
    CHECK(buf[sizeof(trigger_proto::TriggerResponseV2)] == 0xEE);
  }
  // V1 peer: exactly the 16-byte V1 layout.
  {
    const auto req = MakeV1();
    raw.Send(&req, sizeof(req));
    c.Tick();
    uint8_t buf[sizeof(trigger_proto::TriggerResponse) + 8];
    std::memset(buf, 0xEE, sizeof(buf));
    const ssize_t n = raw.Recv(buf, sizeof(buf));
    CHECK(n == static_cast<ssize_t>(sizeof(trigger_proto::TriggerResponseV1)));
    trigger_proto::TriggerResponseV1 resp{};
    std::memcpy(&resp, buf, sizeof(resp));
    CHECK(resp.magic == trigger_proto::kTriggerMagic);
    CHECK(resp.version == trigger_proto::kTriggerVersionLegacy);
    CHECK(resp.code == static_cast<uint16_t>(
                           trigger_proto::TriggerCode::kUnsupportedVersion));
    CHECK(buf[sizeof(trigger_proto::TriggerResponseV1)] == 0xEE);
  }
}

// ---- PinnedSlotStore: v3 current, v2 old -----------------------------------

// Documented v2 header layout (docs/reference/abi-compatibility.md + the
// pre-REP-10 header): identical offsets for the geometry block, 192 bytes
// total, no ProducerGate/gate_aborted tail. Kept as a byte-image type only.
struct alignas(64) PinnedSlotPageHeaderV2 {
  std::atomic<uint64_t> magic{0};
  std::atomic<uint32_t> version{0};
  std::atomic<uint32_t> slot_count{0};
  std::atomic<uint32_t> slot_bytes{0};
  std::atomic<uint32_t> slot_stride{0};
  std::atomic<uint32_t> meta_blob_bytes{0};
  std::atomic<uint32_t> flags{0};
  std::atomic<uint64_t> dropped_oversize{0};
  alignas(64) std::atomic<uint64_t> head{0};
  alignas(64) std::atomic<uint32_t> epoch{0};
};
static_assert(sizeof(PinnedSlotPageHeaderV2) == 192,
              "v2 channel header is 3 cache lines");
static_assert(offsetof(PinnedSlotPageHeaderV2, version) == 8,
              "v2 version offset is channel ABI");
static_assert(offsetof(PinnedSlotPageHeaderV2, slot_count) == 12,
              "v2 geometry offsets match v3");
static_assert(offsetof(PinnedSlotPageHeaderV2, epoch) == 128,
              "v2 epoch offset");
// v3 grew the tail (gate); v4 moved every offset by one 64 B super-header.
static_assert(offsetof(PinnedSlotPageHeader, epoch) == 192,
              "v4 epoch offset follows the super-header");

enum class V2Attach {
  kOk,
  kNotPresent,
  kIncompatible,
  kBadGeometry,
};

// Reconstructed v2 reader probe: the documented v2 attach path (probe the v2
// header size, require magic, then an exact version match).
V2Attach ProbeAsV2Reader(const char* name) {
  ShmRegion region;
  if (!region.OpenRetry(name, sizeof(PinnedSlotPageHeaderV2), 0,
                        /*read_only=*/true)) {
    return V2Attach::kNotPresent;
  }
  const auto *h = static_cast<const PinnedSlotPageHeaderV2 *>(region.data());
  if (h->magic.load(std::memory_order_acquire) !=
      PinnedSlotPageHeader::kMagic) {
    return V2Attach::kNotPresent;
  }
  if (h->version.load(std::memory_order_acquire) != 2) {
    return V2Attach::kIncompatible;
  }
  if (h->slot_count.load(std::memory_order_acquire) == 0 ||
      h->slot_bytes.load(std::memory_order_acquire) == 0) {
    return V2Attach::kBadGeometry;
  }
  return V2Attach::kOk;
}

std::size_t V2RegionBytes(uint32_t slot_bytes, uint32_t slot_count,
                          uint32_t meta_blob_bytes) {
  return sizeof(PinnedSlotPageHeaderV2) +
         PinnedSlotWriter::RoundUp64(meta_blob_bytes) +
         static_cast<std::size_t>(slot_count) *
             PinnedSlotWriter::SlotStride(slot_bytes);
}

void TestPinnedSlotV2Header() {
  char name[96];
  std::snprintf(name, sizeof(name), "/abi04bf_%d", static_cast<int>(getpid()));
  const std::string lease = PinnedSlotWriter::LeaseShmName(name);
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(lease.c_str());

  constexpr uint32_t kSlotBytes = 256;
  constexpr uint32_t kSlotCount = 2;
  constexpr uint32_t kMetaBytes = 64;

  // Synthetic v2 channel image (the documented v2 layout; a v2 writer's bytes).
  {
    ShmRegion region;
    CHECK(region.Create(name, V2RegionBytes(kSlotBytes, kSlotCount,
                                            kMetaBytes)));
    auto* h = new (region.data()) PinnedSlotPageHeaderV2();
    h->slot_bytes.store(kSlotBytes);
    h->slot_count.store(kSlotCount);
    h->slot_stride.store(
        static_cast<uint32_t>(PinnedSlotWriter::SlotStride(kSlotBytes)));
    h->meta_blob_bytes.store(kMetaBytes);
    h->version.store(2);
    h->magic.store(PinnedSlotPageHeader::kMagic, std::memory_order_release);

    // v4 reader × v2 header: rejected, and nothing (header/lease) is exposed.
    PinnedSlotReader r;
    CHECK(!r.Open(name, 0));
    CHECK(!r.IsOpen());
    CHECK(r.lease() == nullptr);

    // v2 reader × v2 header: the reconstruction is faithful (sanity check for
    // the probe used below); a v2 peer would attach here.
    CHECK(ProbeAsV2Reader(name) == V2Attach::kOk);
    region.Unmap();
  }

  // v4 writer × v2 channel: explicit kIncompatible, not a silent
  // unlink+rebuild.
  PinnedSlotWriter w;
  {
    CHECK(!w.Create(name, kSlotBytes, kSlotCount, kMetaBytes, 60000000000ULL));
    CHECK(w.create_status() == PinnedSlotWriter::CreateStatus::kIncompatible);
    CHECK(!w.last_error().empty());
    // The v2 object was not touched by the refusal.
    CHECK(ProbeAsV2Reader(name) == V2Attach::kOk);

    // force_replace only overrides the version refusal (documented upgrade
    // path); the new object is a real v3 channel.
    CHECK(w.Create(name, kSlotBytes, kSlotCount, kMetaBytes, 60000000000ULL,
                   /*force_replace=*/true));
    CHECK(w.create_status() == PinnedSlotWriter::CreateStatus::kOk);
    CHECK(w.slot_count() == kSlotCount);
    CHECK(w.slot_bytes() == kSlotBytes);
  }

  // v2 reader × v4 header: the reconstructed v2 probe rejects the v4 channel.
  CHECK(ProbeAsV2Reader(name) == V2Attach::kIncompatible);
  // The v3 header is larger, so even without the version check the v2 reader
  // would read the wrong meta/slot offsets — the version gate is mandatory.
  CHECK(sizeof(PinnedSlotPageHeaderV2) != sizeof(PinnedSlotPageHeader));
  CHECK(PinnedSlotWriter::MetaOffset() != sizeof(PinnedSlotPageHeaderV2));

  // Sanity: v4 × v4 still works end to end (no over-rejection).
  {
    PinnedSlotReader r;
    CHECK(r.Open(name, 1000000000ULL));
    CHECK(r.IsOpen());
    CHECK(r.lease() != nullptr);
    CHECK(r.lease()->super.abi_version.load(
              std::memory_order_acquire) == kPinnedSlotLeaseVersion);

    PinnedSlotWriter::BeginView bv = w.Begin(16);
    CHECK(bv.payload != nullptr);
    std::memset(bv.payload, 0x5A, 16);
    w.Commit();
    {
      PinnedSlotReader::View v;
      CHECK(r.TryRead(v));
      CHECK(v.data_len == 16);
      CHECK(r.ReadFrame(0, v) == PinnedSlotReader::ReadResult::kOk);
    }
    r.Close();
  }
  w.Close();

  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(lease.c_str());
}

// v6 reader × v5/v4/v3 lease images: rejected.
void TestPinnedSlotLeaseOldVersions() {
  char name[96];
  std::snprintf(name, sizeof(name), "/abi04lease_%d",
                static_cast<int>(getpid()));
  const std::string lease = PinnedSlotWriter::LeaseShmName(name);
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(lease.c_str());

  constexpr uint32_t kSlotBytes = 128;
  constexpr uint32_t kSlotCount = 2;
  PinnedSlotWriter w;
  CHECK(w.Create(name, kSlotBytes, kSlotCount, 0, 60000000000ULL));

  // Channel header v2 and lease v5/v4/v3 are all rejected at attach.
  {
    ShmRegion rw;
    CHECK(rw.OpenRetry(name, PinnedSlotWriter::RegionBytes(kSlotBytes,
                                                         kSlotCount, 0),
                       0, false));
    auto* h = static_cast<PinnedSlotPageHeader*>(rw.data());
    h->super.abi_version.store(2, std::memory_order_release);
    PinnedSlotReader v2;
    CHECK(!v2.Open(name, 0));
    CHECK(!v2.IsOpen());
    CHECK(v2.lease() == nullptr);
    h->super.abi_version.store(PinnedSlotPageHeader::kVersion,
                               std::memory_order_release);

    ShmRegion lrw;
    CHECK(lrw.OpenRetry(lease.c_str(),
                        PinnedSlotWriter::LeaseRegionBytes(kSlotCount), 0,
                        false));
    auto* lh = static_cast<PinnedSlotLeaseHeader*>(lrw.data());
    for (uint32_t old_version = 3; old_version <= 5; ++old_version) {
      lh->super.abi_version.store(old_version, std::memory_order_release);
      PinnedSlotReader old;
      CHECK(!old.Open(name, 0));
      CHECK(!old.IsOpen());
      CHECK(old.lease() == nullptr);
    }

    // Restoring the version restores attach (no sticky failure).
    lh->super.abi_version.store(kPinnedSlotLeaseVersion,
                               std::memory_order_release);
    PinnedSlotReader ok;
    CHECK(ok.Open(name, 1000000000ULL));
    CHECK(ok.lease() != nullptr);
    ok.Close();
    rw.Unmap();
    lrw.Unmap();
  }

  w.Close();
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(lease.c_str());
}

// ---- ring pages: old version words are rejected at attach ------------------

void TestRingVersions() {
  char name[96];
  std::snprintf(name, sizeof(name), "/abi04spsc_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  {
    using Ring = ShmSpscRing<uint32_t, 8>;
    Ring ring;
    CHECK(ring.Create(name));
    ring.Close();
    // v2 image (PERF-14 added epoch_waiters): the v3 opener must refuse.
    ShmRegion rw;
    CHECK(rw.OpenRetry(name, Ring::RegionBytes(), 0, false));
    auto* layout = static_cast<Ring::Layout*>(rw.data());
    CHECK(layout->header.abi_version.load() == kShmSpscRingVersion);
    layout->header.abi_version.store(2);
    Ring old;
    CHECK(!old.Open(name, 0));
    layout->header.abi_version.store(kShmSpscRingVersion);
    Ring ok;
    CHECK(ok.Open(name, 0));
    ok.Close();
    rw.Unmap();
  }
  ShmRegion::UnlinkName(name);

  std::snprintf(name, sizeof(name), "/abi04spmc_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  {
    using Ring = ShmSpmcRing<uint32_t, 8>;
    Ring ring;
    CHECK(ring.Create(name));
    ring.Close();
    // v2 image (no per-reader drop slots): the v3 opener must refuse.
    ShmRegion rw;
    CHECK(rw.OpenRetry(name, Ring::RegionBytes(), 0, false));
    auto* layout = static_cast<Ring::Layout*>(rw.data());
    layout->header.abi_version.store(2);
    Ring old;
    CHECK(!old.Open(name, 0));
    layout->header.abi_version.store(kShmSpmcRingVersion);
    Ring ok;
    CHECK(ok.Open(name, 0));
    ok.Close();
    rw.Unmap();
  }
  ShmRegion::UnlinkName(name);

  std::snprintf(name, sizeof(name), "/abi04mirror_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  {
    ThymeStructDesc desc{};
    desc.contract_version = 1;
    desc.payload_size = 8;
    std::snprintf(desc.name, sizeof(desc.name), "Mirror");
    const RecordMirrorRingSpec spec{8, 2};
    record_mirror::RingClient current;
    CHECK(current.Create(name, &desc, &spec, 1, 1));

    ShmRegion rw;
    CHECK(rw.OpenRetry(name, RecordMirrorRingPage::RequiredBytes(&desc, &spec, 1),
                       0, false));
    auto* page = static_cast<RecordMirrorRingPage*>(rw.data());
    page->super.abi_version.store(5, std::memory_order_release);
    page->header.version.store(5, std::memory_order_release);
    record_mirror::RingClient old;
    CHECK(!old.AttachReadOnly(name, 0));
    CHECK(old.Page() == nullptr);

    page->super.abi_version.store(RecordMirrorRingPage::kVersion,
                                  std::memory_order_release);
    page->header.version.store(RecordMirrorRingPage::kVersion,
                               std::memory_order_release);
    record_mirror::RingClient ok;
    CHECK(ok.AttachReadOnly(name, 0));
    CHECK(ok.Page()->Validate());
    ok.Shutdown(false);
    rw.Unmap();
    current.Shutdown(true);
  }
}

// ---- .thyme.bin: v1 (unindexed) packages -----------------------------------

ThymeStructDesc MakeDesc(uint16_t id, uint16_t version, uint32_t payload_size,
                         const char* name) {
  ThymeStructDesc d{};
  d.struct_id = id;
  d.contract_version = version;
  d.payload_size = payload_size;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

std::string TempPath(const char* tag) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "/tmp/abi04_%s_%d.tbin", tag,
                static_cast<int>(getpid()));
  return std::string(buf);
}

// A genuine v1 image: the encoder picks version 1 when the sink cannot seek,
// which is the documented writer path for TCP/fd recordings. A socketpair is
// the smallest non-seekable sink, so the encoder's own v1 branch produces the
// bytes; they are then spilled to a file for the v2 reader to consume.
void WriteV1Package(const std::string& path, const ThymeStructDesc& desc,
                    bool with_data) {
  int fds[2] = {-1, -1};
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  {
    ThymeBinFdSink sink(fds[0], /*owned=*/true);
    ThymeBinEncoder enc(&sink);
    CHECK(enc.ok());
    CHECK(enc.WriteHeader(11, 1000, &desc, 1));
    if (with_data) {
      const uint8_t payload[16] = {1, 2, 3, 4, 5, 6, 7, 8,
                                   9, 10, 11, 12, 13, 14, 15, 16};
      CHECK(
          enc.WriteData(1, desc.struct_id, 2000, 7, payload, sizeof(payload)));
    }
    CHECK(enc.Finish());
    CHECK(!enc.has_index());  // stream sink: no footer index
  }
  fds[0] = -1;  // owned by the sink

  std::vector<uint8_t> image;
  uint8_t buf[512];
  for (;;) {
    const ssize_t n = ::recv(fds[1], buf, sizeof(buf), 0);
    CHECK(n >= 0);
    if (n == 0)
      break;
    image.insert(image.end(), buf, buf + n);
  }
  CHECK(::close(fds[1]) == 0);
  CHECK(!image.empty());

  std::FILE* f = std::fopen(path.c_str(), "wb");
  CHECK(f != nullptr);
  CHECK(std::fwrite(image.data(), 1, image.size(), f) == image.size());
  std::fclose(f);
}

void WriteRawHeader(const std::string& path, uint16_t file_version,
                    uint32_t struct_count, const ThymeStructDesc* descs,
                    std::size_t descs_to_write) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  CHECK(f != nullptr);
  ThymeBinFileHeader h{};
  std::memcpy(h.magic, kThymeBinMagic, sizeof(h.magic));
  h.file_abi_version = file_version;
  h.struct_count = struct_count;
  CHECK(std::fwrite(&h, sizeof(h), 1, f) == 1);
  for (std::size_t i = 0; i < descs_to_write; ++i) {
    const ThymeBinLegacyStructDesc legacy = ToThymeBinLegacyDesc(descs[i]);
    CHECK(std::fwrite(&legacy, sizeof(legacy), 1, f) == 1);
  }
  std::fclose(f);
}

void TestThymeBinV1() {
  const std::string path = TempPath("v1");
  const ThymeStructDesc desc = MakeDesc(0, 1, 16, "Demo");
  WriteV1Package(path, desc, /*with_data=*/true);

  ThymeBinReader r(path.c_str());
  CHECK(r.ok());
  CHECK(r.header().file_abi_version == kThymeBinFileAbiVersionUnindexed);
  CHECK(r.registry().size() == 1);
  CHECK(std::strcmp(r.registry()[0].name, "Demo") == 0);
  // No footer index -> explicit kAbsent with sequential fallback, not kCorrupt.
  CHECK(r.index_status() == ThymeBinReader::IndexStatus::kAbsent);
  CHECK(!r.HasIndex());
  CHECK(r.index_block_count() == 0);
  CHECK(r.indexed_record_count() == 0);
  CHECK(!r.SeekToMonoNs(0));  // no index to seek with
  CHECK(r.first_mono_ns() == 0);
  CHECK(r.last_mono_ns() == 0);

  ThymeBinDataEntry e{};
  CHECK(r.ReadNext(e) == ThymeBinReader::ReadResult::kOk);
  CHECK(e.type == ThymeBinEntryType::kData);
  CHECK(e.record_sequence == 1);
  CHECK(e.mono_ns == 2000);
  CHECK(e.source_sequence == 7);
  CHECK(e.payload_size == 16);
  CHECK(e.payload.size() == 16);
  CHECK(e.payload[0] == 1 && e.payload[15] == 16);
  ThymeBinDataEntry eof{};
  CHECK(r.ReadNext(eof) == ThymeBinReader::ReadResult::kEof);

  std::remove(path.c_str());
}

// Header version/flags that a current reader must reject outright.
void TestThymeBinVersions() {
  const ThymeStructDesc desc = MakeDesc(0, 1, 16, "Demo");

  // Version 0 is undefined (no writer ever emitted it).
  {
    const std::string path = TempPath("v0");
    WriteRawHeader(path, 0, 1, &desc, 1);
    ThymeBinReader r(path.c_str());
    CHECK(!r.ok());
    std::remove(path.c_str());
  }
  // Future version (> 4): reject, never parse with the v4 layout. A v2
  // reader applies the same rule to a v3/v4 file
  // (docs/reference/abi-compatibility.md).
  {
    const std::string path = TempPath("v5");
    WriteRawHeader(path, 5, 1, &desc, 1);
    ThymeBinReader r(path.c_str());
    CHECK(!r.ok());
    std::remove(path.c_str());
  }
  // Registry larger than the ABI ceiling.
  {
    const std::string path = TempPath("bigreg");
    WriteRawHeader(path, kThymeBinFileAbiVersionIndexed,
                   kThymeBinMaxStructCount + 1, &desc, 1);
    ThymeBinReader r(path.c_str());
    CHECK(!r.ok());
    std::remove(path.c_str());
  }
  // Registry truncated inside an entry.
  {
    const std::string path = TempPath("truncreg");
    WriteRawHeader(path, kThymeBinFileAbiVersionIndexed, 2, &desc, 1);
    ThymeBinReader r(path.c_str());
    CHECK(!r.ok());
    std::remove(path.c_str());
  }
  // v2 header with no footer (crash-truncated / non-seekable capture): the
  // reader falls back to sequential scanning instead of inventing an index.
  {
    const std::string path = TempPath("v2nofooter");
    WriteRawHeader(path, kThymeBinFileAbiVersionIndexed, 1, &desc, 1);
    ThymeBinReader r(path.c_str());
    CHECK(r.ok());
    CHECK(r.header().file_abi_version == kThymeBinFileAbiVersionIndexed);
    CHECK(r.index_status() == ThymeBinReader::IndexStatus::kAbsent);
    CHECK(!r.HasIndex());
    ThymeBinDataEntry e{};
    CHECK(r.ReadNext(e) == ThymeBinReader::ReadResult::kEof);
    std::remove(path.c_str());
  }
  // A record claiming a payload above the codec ceiling is explicitly
  // kCorrupt at the record offset (not a silent short read).
  {
    const std::string path = TempPath("oversize");
    WriteRawHeader(path, kThymeBinFileAbiVersionUnindexed, 1, &desc, 1);
    std::FILE* f = std::fopen(path.c_str(), "ab");
    CHECK(f != nullptr);
    uint8_t rec[32] = {};
    rec[0] = static_cast<uint8_t>(ThymeBinEntryType::kData);
    const uint64_t seq = 1;
    const uint64_t mono = 3000;
    const uint32_t src_seq = 0;
    const uint32_t payload_size = kThymeBinMaxPayloadSize + 1;
    std::memcpy(rec + 4, &seq, sizeof(seq));
    std::memcpy(rec + 16, &mono, sizeof(mono));
    std::memcpy(rec + 24, &src_seq, sizeof(src_seq));
    std::memcpy(rec + 28, &payload_size, sizeof(payload_size));
    CHECK(std::fwrite(rec, sizeof(rec), 1, f) == 1);
    std::fclose(f);

    ThymeBinReader r(path.c_str());
    CHECK(r.ok());
    ThymeBinDataEntry e{};
    CHECK(r.ReadNext(e) == ThymeBinReader::ReadResult::kCorrupt);
    CHECK(r.corrupt_offset() ==
          sizeof(ThymeBinFileHeader) + kThymeStructDescLegacySize);
    CHECK(e.payload.empty());  // nothing partially delivered
    std::remove(path.c_str());
  }
}

// ---- contract registry (ThymeStructDesc) -----------------------------------

void TestContractRegistry() {
  static_assert(sizeof(ThymeStructDesc) == 128, "wire entry");
  static_assert(offsetof(ThymeStructDesc, contract_version) == 2, "offset");
  static_assert(offsetof(ThymeStructDesc, payload_size) == 4, "offset");
  static_assert(offsetof(ThymeStructDesc, name) == 8, "offset");
  static_assert(offsetof(ThymeStructDesc, layout_hash) == 40, "ABI-11 offset");
  static_assert(offsetof(ThymeStructDesc, channel) == 48, "ABI-11 offset");
  static_assert(offsetof(ThymeStructDesc, record_key) == 80, "ABI-11 offset");

  // An unknown *contract* version is metadata the consumer must see verbatim
  // (the reader has no local contract table to compare against), so nothing
  // is zeroed or truncated. Unknown *file* versions are rejected instead.
  {
    const std::string path = TempPath("desc7");
    const ThymeStructDesc desc = MakeDesc(3, 7, 16, "Future");
    WriteV1Package(path, desc, /*with_data=*/true);
    ThymeBinReader r(path.c_str());
    CHECK(r.ok());
    CHECK(r.registry().size() == 1);
    CHECK(r.registry()[0].struct_id == 3);
    CHECK(r.registry()[0].contract_version == 7);
    CHECK(r.registry()[0].payload_size == 16);
    CHECK(std::strcmp(r.registry()[0].name, "Future") == 0);
    ThymeBinDataEntry e{};
    CHECK(r.ReadNext(e) == ThymeBinReader::ReadResult::kOk);
    CHECK(e.payload_size == 16);
    CHECK(e.payload.size() == 16);
    std::remove(path.c_str());
  }
  // contract_version 0 = unset: no writer emits it, the reader rejects the
  // registry instead of handing out an unversioned entry.
  {
    const std::string path = TempPath("desc0");
    const ThymeStructDesc desc = MakeDesc(0, 0, 16, "Unset");
    WriteV1Package(path, desc, /*with_data=*/false);
    ThymeBinReader r(path.c_str());
    CHECK(!r.ok());
    CHECK(r.registry().empty());
    std::remove(path.c_str());
  }
}

// ---- FrameHeader v1: version + reserved-bit flags --------------------------

void TestFrameHeaderCrossVersion() {
  constexpr uint32_t kMagic = 0x41424934u;  // "ABI4"
  const uint8_t body[4] = {1, 2, 3, 4};
  FrameHeader h{};
  h.sequence = 1;
  h.size = static_cast<uint16_t>(sizeof(FrameHeader) + sizeof(body));
  h.valid = 1;
  FrameHeaderSeal(h, body, sizeof(body), kMagic, 1);
  CHECK(FrameHeaderOk(h, body, sizeof(body), kMagic, 1));

  // Future frame version: a v1 reader rejects it (exact version match).
  CHECK(!FrameHeaderOk(h, body, sizeof(body), kMagic, 2));

  // ABI-12 no-CRC frame: the flag says "skip CRC"; an old reader that does not
  // know the flag computes the CRC and rejects (crc32 is 0, real CRC is not).
  {
    FrameHeader nc{};
    nc.sequence = 1;
    nc.size = static_cast<uint16_t>(sizeof(FrameHeader));
    nc.valid = 1;
    FrameHeaderSealNoCrc(nc, kMagic, 1);
    CHECK(FrameCrcDisabled(nc));
    CHECK(nc.crc32 == 0);
    CHECK(FrameHeaderOk(nc, nullptr, 0, kMagic, 1));  // current reader
    CHECK(Crc32HeaderPayload(nc, nullptr, 0) != nc.crc32);  // pre-ABI-12 reader
  }

  // ABI-05: valid == 1 is the data-valid boundary, independent of the CRC
  // policy. A header whose payload was never completed is structurally
  // inspectable but not readable as data, even with a recomputed CRC.
  {
    FrameHeader not_ready = h;
    not_ready.valid = 0;
    not_ready.crc32 = 0;
    not_ready.crc32 = Crc32HeaderPayload(not_ready, body, sizeof(body));
    CHECK(FrameHeaderStructurallyOk(not_ready, kMagic, 1));
    CHECK(!FrameHeaderOk(not_ready, body, sizeof(body), kMagic, 1));
  }

  // reserved[0] bit0 is the only defined bit; any other reserved bit is a
  // future flag this build does not implement -> rejected (rule 3), not
  // interpreted with today's semantics.
  {
    FrameHeader future = h;
    future.reserved[0] |= static_cast<uint8_t>(1u << 1);
    // Re-seal so the CRC is valid for the mutated header (a real future
    // sender would do that): only the flag check can reject it.
    FrameHeaderSeal(future, body, sizeof(body), kMagic, 1);
    CHECK(!FrameHeaderOk(future, body, sizeof(body), kMagic, 1));

    FrameHeader future_tail = h;
    future_tail.reserved[1] = 0x01;
    FrameHeaderSeal(future_tail, body, sizeof(body), kMagic, 1);
    CHECK(!FrameHeaderOk(future_tail, body, sizeof(body), kMagic, 1));

    FrameHeader future_tail2 = h;
    future_tail2.reserved[2] = 0x80;
    FrameHeaderSeal(future_tail2, body, sizeof(body), kMagic, 1);
    CHECK(!FrameHeaderOk(future_tail2, body, sizeof(body), kMagic, 1));
  }
}

}  // namespace

int main() {
  TestTriggerV3Reader();
  TestTriggerLegacyV2();
  TestTriggerLegacyV1();
  TestOldReaderVsV3Datagram();
  TestTriggerSocketReplySizes();
  TestPinnedSlotV2Header();
  TestPinnedSlotLeaseOldVersions();
  TestRingVersions();
  TestThymeBinV1();
  TestThymeBinVersions();
  TestContractRegistry();
  TestFrameHeaderCrossVersion();
  return 0;
}
