// test_abi_layout.cpp — RT-07 / ABI-02: compile-time lock on every SHM page
// layout (sizeof + field offsets) and on the wire byte order, plus runtime
// checks that the structures really are written little-endian with the
// documented field order.
//
// The header static_asserts are the primary gate; this test is the TU that
// includes every page header at once, so a layout change anywhere breaks the
// build here even if the owning component has no test of its own. A failure
// means the layout changed: bump the version constant
// (docs/reference/abi-compatibility.md), never relax the assert.
#include "thyme/app/mp_control.h"
#include "thyme/check.h"
#include "thyme/contracts/abi.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/contracts/struct_desc.h"
#include "thyme/record/trigger_protocol.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/record_mirror.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"

#include <cstddef>
#include <cstdint>
#include <new>

using namespace thyme;
using namespace thyme::trigger_proto;

namespace {

struct Payload8 {
  uint8_t b[8];
};
struct Payload24 {
  uint8_t b[24];
};
struct alignas(16) Payload16 {
  uint8_t b[16];
};
struct Payload100 {
  uint8_t b[100];
};

using SpscLayout8 = ShmSpscRing<Payload8, 4>::Layout;
using SpscLayout100 = ShmSpscRing<Payload100, 8>::Layout;
using SpmcLayout8 = ShmSpmcRing<Payload8, 4>::Layout;
using SpmcLayout24 = ShmSpmcRing<Payload24, 16>::Layout;
using SpmcLayout16 = ShmSpmcRing<Payload16, 4>::Layout;

// Magic literals spell their documented name MSB-first (the memory image is
// the byte-reverse on a little-endian host; probes compare the numeric value).
constexpr uint64_t AsciiMsb(const char (&s)[9]) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | static_cast<uint8_t>(s[i]);
  }
  return v;
}
static_assert(kPinnedSlotStoreMagic == AsciiMsb("THYMBFC1"),
              "PinnedSlotStore magic is the documented name");
static_assert(kShmSpscRingMagic == AsciiMsb("THYMSSR1"),
              "SpscRing magic is the documented name");
static_assert(kShmSpmcRingMagic == AsciiMsb("THYMSPM1"),
              "SpmcRing magic is the documented name");
static_assert(kPinnedSlotFooterMagic == AsciiMsb("THYMFOOT"),
              "slot footer magic is the documented name");
static_assert(kPinnedSlotLeaseMagic == AsciiMsb("THYMLSE1"),
              "lease magic is the documented name");
static_assert(RecordMirrorRingPage::kMagic == AsciiMsb("THYMERRG"),
              "ring page magic is the documented name");

// ---- version constants (ABI matrix, docs/reference/abi-compatibility.md)
// ----------
static_assert(kShmSpscRingVersion == 4, "SpscRing v4 adds the ARCH-11 super-header");
static_assert(kShmSpmcRingVersion == 4, "SpmcRing v4 adds the ARCH-11 super-header");
static_assert(PinnedSlotPageHeader::kVersion == 4,
              "PinnedSlot channel v4 (ARCH-11 super-header)");
static_assert(kPinnedSlotStoreVersion == PinnedSlotPageHeader::kVersion,
              "channel version constant is single-sourced");
static_assert(kPinnedSlotLeaseVersion == 6, "lease ABI v6 (ARCH-11)");
static_assert(RecordMirrorPage::kVersion == 4, "mirror page v4 (ARCH-11 super-header)");
static_assert(RecordMirrorRingPage::kVersion == 6,
              "mirror ring page v6 (per-struct ProducerGate)");
static_assert(SharedHealthPage::kVersion == 3,
              "health page v3 (ARCH-11 super-header + RT-23 u64 drops)");
static_assert(app::MpControl::kMpControlAbiVersion == 3,
              "MpControl v3 (ARCH-11 super-header + explicit magic)");
static_assert(kTriggerVersion == 4, "trigger socket v4");
static_assert(kTriggerVersionV3 == 3, "trigger socket v3 remains readable");
static_assert(sizeof(trigger_proto::TriggerRequestV4) == 120, "v4 request");
static_assert(sizeof(trigger_proto::TriggerResponseV4) == 48, "v4 response");

// Distinct page magics: a probe or attacher must not confuse two ABIs.
static_assert(kShmSpscRingMagic != kShmSpmcRingMagic, "ring magics differ");
static_assert(kPinnedSlotStoreMagic != kPinnedSlotFooterMagic &&
                  kPinnedSlotStoreMagic != kPinnedSlotLeaseMagic &&
                  kPinnedSlotFooterMagic != kPinnedSlotLeaseMagic,
              "channel/footer/lease magics are distinct");
static_assert(RecordMirrorPage::kMagic != RecordMirrorRingPage::kMagic,
              "mirror v2 and ring v3 pages are distinct ABIs");
static_assert(SharedHealthPage::kMagic != kPinnedSlotStoreMagic,
              "health page and pinned_slot channel are distinct");
static_assert(app::MpControl::kMagic != SharedHealthPage::kMagic,
              "control page and health page are distinct");
static_assert(app::MpControl::kMagic == AsciiMsb("THYME_MP"),
              "MpControl magic is the documented name");

// ---- SHM page sizes (what region sizing / manifests encode) -------------
static_assert(sizeof(FrameHeader) == 40, "FrameHeader wire size");
static_assert(offsetof(FrameHeader, reserved) == 29, "reserved bits are ABI");
static_assert(offsetof(FrameHeader, reserved2) == 36,
              "explicit tail keeps the header 40 B");
static_assert(sizeof(ThymeStructDesc) == 128,
              "record registry entry size (ABI-11 keys)");

static_assert(sizeof(HealthSlot) == 64, "one health slot per cache line");
static_assert(offsetof(HealthSlot, drop_cnt) == 24,
              "RT-23 drop counter offset");
static_assert(sizeof(HealthSlot::drop_cnt) == 8,
              "RT-23 drop counter is 64-bit");
static_assert(offsetof(HealthSlot, memory_flags) == 40,
              "PERF-04 memory flags occupy health padding");
static_assert(sizeof(ShmSuperHeader) == 64, "ARCH-11 super-header size");
static_assert(sizeof(SharedHealthPage::Header) == 64, "health identity cell");
static_assert(offsetof(SharedHealthPage, super) == 0,
              "health page magic/version stay at 0/8");
static_assert(offsetof(SharedHealthPage, header) == 64,
              "health fields follow the super-header");
static_assert(offsetof(SharedHealthPage, slots) == 128,
              "health slots are cacheline aligned after the header block");
static_assert(sizeof(SharedHealthPage) == 128u + 64u * kAbiMaxThreads,
              "health page size (v3): 128 B identity block + one 64 B slot per "
              "worker; ABI-13 keeps this valid for any capacity profile");
static_assert(sizeof(app::MpControl) == 320, "MpControl page size (v3)");
static_assert(offsetof(app::MpControl, stop) == 64,
              "MpControl counters follow the super-header");
static_assert(offsetof(app::MpControl, proc_loops) == 96,
              "MpControl proc_loops offset");
static_assert(offsetof(app::MpControl, cross_frames) == 224,
              "MpControl cross_frames offset");
static_assert(sizeof(app::MpControl::super) == 64, "control super-header");
static_assert(SharedHealthPage::Spec().magic == SharedHealthPage::kMagic &&
                  SharedHealthPage::Spec().abi_version ==
                      SharedHealthPage::kVersion &&
                  SharedHealthPage::Spec().layout_hash ==
                      SharedHealthPageLayoutHash() &&
                  SharedHealthPage::Spec().page_bytes ==
                      sizeof(SharedHealthPage),
              "health page spec is single-sourced");
static_assert(app::MpControl::Spec().magic == app::MpControl::kMagic &&
                  app::MpControl::Spec().abi_version ==
                      app::MpControl::kMpControlAbiVersion &&
                  app::MpControl::Spec().layout_hash ==
                      app::MpControlLayoutHash() &&
                  app::MpControl::Spec().page_bytes == sizeof(app::MpControl),
              "control page spec is single-sourced");

static_assert(sizeof(PinnedSlotHeader) == 64, "channel slot header");
static_assert(sizeof(PinnedSlotFooter) == 64, "channel slot footer");
static_assert(sizeof(PinnedSlotPageHeader) == 384, "channel header (v4)");
static_assert(offsetof(PinnedSlotPageHeader, gate) == 256, "gate cache line");
static_assert(offsetof(PinnedSlotPageHeader, gate_aborted) == 320,
              "gate_aborted offset");
static_assert(sizeof(PinnedSlotLeaseHeader) == 128, "lease header (v6)");
static_assert(sizeof(PinnedSlotLeaseCell) == 64, "lease cell");

static_assert(sizeof(RecordMirrorSlotHeader) == 64, "mirror slot metadata");
static_assert(sizeof(RecordMirrorPage::Header) == 64, "mirror page header");
static_assert(sizeof(RecordMirrorPage) ==
                  128u + 192u * kRecordMirrorMaxSlots +
                      kRecordMirrorMaxArenaBytes,
              "mirror page size: 128 B header + (128 B descriptor + 64 B slot) "
              "per struct + fixed arena; ABI-13 keeps this valid for any "
              "capacity profile");

static_assert(sizeof(RecordMirrorRingSlotHeader) == 64, "ring slot header");
static_assert(sizeof(RecordMirrorRingDesc) == 128, "ring descriptor");
static_assert(offsetof(RecordMirrorRingDesc, gate) == 64, "ring gate offset");
static_assert(sizeof(RecordMirrorRingSampleDesc) == 24, "harvest descriptor");
static_assert(sizeof(RecordMirrorRingPage::Header) == 128, "ring page header");
static_assert(sizeof(RecordMirrorRingPage) ==
                  192u + 256u * kRecordMirrorRingMaxStructs,
              "ring page fixed tables: 192 B header + (128 B descriptor + "
              "128 B ring descriptor) per struct; ABI-13 keeps this valid");

// Ring pages: header offsets are T-independent, cell stride is
// RoundUp64(sizeof(T)).
static_assert(offsetof(SpscLayout8, header) == 0 &&
                  offsetof(SpscLayout8, cap) == 64 &&
                  offsetof(SpscLayout8, epoch_waiters) == 260 &&
                  offsetof(SpscLayout8, slots) == 320,
              "spsc v4 header layout");
static_assert(sizeof(SpscLayout8) == 320 + 4 * 64, "spsc page size (8B slots)");
static_assert(sizeof(SpscLayout100) == 320 + 8 * 128,
              "spsc page size (100B slots)");
static_assert(offsetof(SpmcLayout8, epoch_waiters) == 196 &&
                  offsetof(SpmcLayout8, reader_slot_mask) == 208 &&
                  offsetof(SpmcLayout8, reader_drop_slots) == 256 &&
                  offsetof(SpmcLayout8, slots) == 320,
              "spmc v4 header layout");
static_assert(sizeof(SpmcLayout8) == 320 + 4 * 64, "spmc page size (8B slots)");
static_assert(sizeof(SpmcLayout24) == 320 + 16 * 64,
              "spmc page size (24B slots)");
// Over-aligned payload: the seqlock is 8 B, so the payload starts at the next
// multiple of alignof(T) and the cell still rounds up to one cache line.
static_assert(offsetof(SpmcLayout16::Cell, value) == 16,
              "over-aligned payload follows the 8 B seqlock at 16");
static_assert(sizeof(SpmcLayout16) == 320 + 4 * 64,
              "spmc page size (16B aligned slots)");

// LatestSlot layout v2 (RT-03): 2 payload cells + the 64 B tail. Instantiating
// LatestSlotLayout<T> is what runs its offset/size asserts.
static_assert(detail::LatestSlotLayout<Payload24>::kCellBytes == 64 &&
                  detail::LatestSlotLayout<Payload24>::kPageBytes == 192,
              "LatestSlot<24B> page is 192 B");
static_assert(detail::LatestSlotLayout<Payload100>::kCellBytes == 128 &&
                  detail::LatestSlotLayout<Payload100>::kPageBytes == 320,
              "LatestSlot<100B> page is 320 B");

// ---- ABI-02: host byte order --------------------------------------------
static_assert(kAbiHostIsLittleEndian,
              "thyme wire/SHM formats are little-endian");
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(kAbiHostEndianChecked,
              "GCC/Clang must prove the little-endian assertion");
#endif

// Byte image of a little-endian uint64 containing `v` (LSB first).
void ExpectLe64(const uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    CHECK(p[i] == static_cast<uint8_t>(v >> (8 * i)));
  }
}

}  // namespace

int main() {
  // FrameHeader is memcpy'd verbatim onto the wire: its byte image must be
  // little-endian and match the 40 B layout documented in
  // docs/reference/abi-compatibility.md.
  FrameHeader h{};
  h.magic = 0x11223344u;
  h.version = 0x0102u;
  h.size = 36;
  h.session_id = 0x0102030405060708ULL;
  h.mono_ns = 0x1112131415161718ULL;
  h.sequence = 0xAABBCCDDu;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&h);
  CHECK(bytes[0] == 0x44 && bytes[1] == 0x33 && bytes[2] == 0x22 &&
        bytes[3] == 0x11);
  CHECK(bytes[4] == 0x02 && bytes[5] == 0x01);
  CHECK(bytes[6] == 36 && bytes[7] == 0);
  ExpectLe64(bytes + 8, h.session_id);
  ExpectLe64(bytes + 16, h.mono_ns);
  CHECK(bytes[24] == 0xDD && bytes[27] == 0xAA);
  CHECK(bytes[28] == h.valid);

  // Sealing writes version in LE at offset 4 and magic at offset 0.
  const uint8_t payload[4] = {1, 2, 3, 4};
  FrameHeader sealed{};
  sealed.size = static_cast<uint16_t>(sizeof(FrameHeader) + sizeof(payload));
  FrameHeaderSeal(sealed, payload, sizeof(payload), 0x5448594Du, 1);
  const auto* sb = reinterpret_cast<const uint8_t*>(&sealed);
  CHECK(sb[0] == 0x4D && sb[1] == 0x59 && sb[2] == 0x48 && sb[3] == 0x54);
  CHECK(sb[4] == 1 && sb[5] == 0);
  CHECK(sealed.size == sizeof(FrameHeader) + sizeof(payload));
  CHECK(FrameHeaderOk(sealed, payload, sizeof(payload), 0x5448594Du, 1));
  CHECK(!FrameHeaderOk(sealed, payload, sizeof(payload), 0x5448594Du, 2));

  // Page magics spell their documented names MSB-first in the numeric
  // literal (the memory byte image is the reverse on a little-endian host;
  // src/probe/page_reader.cpp compares the numeric value, never the bytes).

  // PinnedSlot geometry helpers are the region-size ABI the manifest records;
  // verify they compose from the pinned sizeofs (no shm object needed).
  CHECK(PinnedSlotWriter::MetaOffset() == 384);
  CHECK(PinnedSlotWriter::SlotsOffset(64) == 384 + 64);
  CHECK(PinnedSlotWriter::SlotStride(512) == 64 + 512 + 64);
  CHECK(PinnedSlotWriter::SlotStride(100) == 64 + 128 + 64);
  CHECK(PinnedSlotWriter::RegionBytes(512, 4, 64) ==
        (384 + 64) + 4 * (64 + 512 + 64));
  CHECK(PinnedSlotWriter::LeaseRegionBytes(4) == 128 + 4 * 64);
  static_assert(PinnedSlotWriter::MetaOffset() == 384,
                "meta offset is constexpr ABI");
  static_assert(PinnedSlotWriter::SlotStride(100) == 256,
                "slot stride is constexpr ABI");

  // A synthetic channel header (as a peer would find it in shm) is readable
  // with the documented LE field offsets.
  alignas(PinnedSlotPageHeader) uint8_t raw_header[sizeof(
      PinnedSlotPageHeader)] = {};
  auto* synthesized = new (raw_header) PinnedSlotPageHeader{};
  synthesized->super.magic.store(PinnedSlotPageHeader::kMagic,
                                 std::memory_order_relaxed);
  synthesized->super.abi_version.store(PinnedSlotPageHeader::kVersion,
                                       std::memory_order_relaxed);
  synthesized->slot_count.store(3, std::memory_order_relaxed);
  synthesized->slot_bytes.store(100, std::memory_order_relaxed);
  synthesized->slot_stride.store(
      static_cast<uint32_t>(PinnedSlotWriter::SlotStride(100)),
      std::memory_order_relaxed);
  CHECK(raw_header[8] == PinnedSlotPageHeader::kVersion);
  CHECK(raw_header[9] == 0 && raw_header[10] == 0 && raw_header[11] == 0);
  CHECK(raw_header[76] == 3);
  CHECK(raw_header[80] == 100);
  CHECK(raw_header[84] == 0 && raw_header[85] == 1);  // 256 LE
  CHECK(synthesized->slot_stride.load(std::memory_order_relaxed) == 256);

  // Trigger socket request/response: the v3 trailer offsets are the wire ABI.
  uint8_t raw_req[sizeof(TriggerRequest)] = {};
  auto* req = new (raw_req) TriggerRequest{};
  req->magic = kTriggerMagic;
  req->version = kTriggerVersionV3;
  req->response_timeout_ms = 500;
  StampRequestCrc(req);
  CHECK(sizeof(TriggerRequest) == 104);
  CHECK(offsetof(TriggerRequest, response_timeout_ms) == 96);
  CHECK(offsetof(TriggerRequest, crc32) == 100);
  CHECK(raw_req[0] == static_cast<uint8_t>(kTriggerMagic & 0xFF));
  CHECK(raw_req[4] == kTriggerVersionV3);
  CHECK(raw_req[96] == static_cast<uint8_t>(500u & 0xFF));
  CHECK(raw_req[97] == static_cast<uint8_t>((500u >> 8) & 0xFF));
  CHECK(RequestCrc(*req) == req->crc32);

  uint8_t raw_resp[sizeof(TriggerResponse)] = {};
  auto* resp = new (raw_resp) TriggerResponse{};
  resp->magic = kTriggerMagic;
  resp->version = kTriggerVersionV3;
  resp->effective_priority = 7;
  StampResponseCrc(resp);
  CHECK(sizeof(TriggerResponse) == 32);
  CHECK(offsetof(TriggerResponse, reserved0) == 24);
  CHECK(offsetof(TriggerResponse, crc32) == 28);
  CHECK(raw_resp[16] == 7 && raw_resp[17] == 0);
  CHECK(ResponseCrc(*resp) == resp->crc32);

  // Ring header version words are LE at offset 8 in every page.
  alignas(SpscLayout8) uint8_t raw_ring[sizeof(SpscLayout8)] = {};
  auto* spsc = new (raw_ring) SpscLayout8{};
  spsc->header.abi_version.store(kShmSpscRingVersion,
                                 std::memory_order_relaxed);
  CHECK(raw_ring[8] == kShmSpscRingVersion && raw_ring[9] == 0);
  CHECK(offsetof(SpscLayout8, header) == 0);
  CHECK(offsetof(SpscLayout8, cap) == 64);
  return 0;
}
