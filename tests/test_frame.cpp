#include "thyme/contracts/frame.h"
#include "thyme/check.h"

#include <cstdint>
#include <cstring>
#include <memory>

using namespace thyme;

namespace {

constexpr uint32_t kTestMagic = 0x54535448u; /* "TSTH" */
constexpr uint16_t kTestVersion = 7;

using Bytes64 = uint8_t[64];

// Compile-time contract pins: Frame<POD> stays SHM-safe, matches the
// payload-member wire convention, and reports hdr + payload wire bytes.
static_assert(std::is_standard_layout<Frame<Bytes64>>::value,
              "Frame must be standard layout");
static_assert(std::is_trivially_copyable<Frame<Bytes64>>::value,
              "Frame must be trivially copyable");
static_assert(FramePodHasPayload<Frame<Bytes64>>::value,
              "Frame<P> must match the payload-member convention");
static_assert(FramePodWireBytes<Frame<Bytes64>> == sizeof(FrameHeader) + 64,
              "Frame wire bytes must be header + payload");
static_assert(offsetof(Frame<Bytes64>, payload) == sizeof(FrameHeader),
              "byte-array payload must sit right after the header");

// Hand-written equivalent of Frame<Bytes64>, mirroring test_frame_header.cpp's
// BlobPod, for the byte-identical wire compatibility proof.
struct BlobPod {
  FrameHeader hdr;
  uint8_t payload[64];
};
static_assert(sizeof(BlobPod) == sizeof(Frame<Bytes64>), "same object size");

// 8-byte payload: the 40 B header is 8-aligned, so there is no padding and
// sizeof(Frame<T>) matches the wire image.
struct Align8Payload {
  uint64_t words[8];
};
static_assert(offsetof(Frame<Align8Payload>, payload) == sizeof(FrameHeader),
              "40 B header leaves an 8-aligned payload adjacent");
static_assert(sizeof(Frame<Align8Payload>) ==
                  FramePodWireBytes<Frame<Align8Payload>>,
              "align<=8 payload has no object/wire size mismatch");

// Over-aligned payload: padding exists in the object but never reaches the
// wire; FramePodWireBytes remains the source of truth.
struct Align16Payload {
  alignas(16) uint64_t words[8];
};
static_assert(offsetof(Frame<Align16Payload>, payload) > sizeof(FrameHeader),
              "16-aligned payload forces padding after the 40 B header");
static_assert(FramePodWireBytes<Frame<Align16Payload>> ==
                  sizeof(FrameHeader) + sizeof(Align16Payload),
              "wire bytes exclude padding");

// Large frame: 60 KB payload — well inside the uint16_t hdr.size range, on
// the heap (way over the default thread stack).
struct BigPayload {
  uint8_t data[60000];
};

}  // namespace

int main() {
  // Init/seal/verify roundtrip.
  Frame<Bytes64> msg{};
  FrameInit(msg, 42u, 123456789u, 7u);
  CHECK(msg.hdr.sequence == 42);
  CHECK(msg.hdr.mono_ns == 123456789u);
  CHECK(msg.hdr.session_id == 7u);
  CHECK(msg.hdr.size == FramePodWireBytes<Frame<Bytes64>>);
  // ABI-05: FrameInit leaves the frame data-invalid until the payload is
  // complete; FrameSeal (below) flips it to 1.
  CHECK(msg.hdr.valid == 0);
  for (std::size_t i = 0; i < 64; ++i) {
    msg.payload[i] = static_cast<uint8_t>(i * 3u);
  }
  FrameSeal(msg, kTestMagic, kTestVersion);
  CHECK(msg.hdr.valid == 1);
  CHECK(msg.hdr.magic == kTestMagic);
  CHECK(msg.hdr.version == kTestVersion);
  CHECK(FrameVerify(msg, kTestMagic, kTestVersion));

  // ABI-05: a channel that does not seal (no CRC) marks data-valid explicitly
  // after the payload is written; FrameVerify stays false without a seal.
  {
    Frame<Bytes64> raw{};
    FrameInit(raw, 44u, 123456791u, 9u);
    CHECK(raw.hdr.valid == 0);
    raw.payload[0] = 0x11;
    FrameMarkValid(raw);
    CHECK(raw.hdr.valid == 1);
    CHECK(!FrameVerify(raw, kTestMagic, kTestVersion));
  }

  // ABI-12: explicit no-CRC seal. Default FrameSeal above remains CRC-on.
  {
    Frame<Bytes64> nc{};
    FrameInit(nc, 43u, 123456790u, 8u);
    for (std::size_t i = 0; i < 64; ++i) nc.payload[i] = 0x5A;
    FrameSealNoCrc(nc, kTestMagic, kTestVersion);
    CHECK(nc.hdr.crc32 == 0);
    CHECK(nc.hdr.valid == 1);  // ABI-05: data-valid is independent of CRC
    CHECK(FrameCrcDisabled(nc.hdr));
    CHECK(FrameVerify(nc, kTestMagic, kTestVersion));
    // No CRC means payload tamper is intentionally not detected.
    nc.payload[0] ^= 0xFFu;
    CHECK(FrameVerify(nc, kTestMagic, kTestVersion));
    // Re-seal with CRC restores the default contract.
    FrameSeal(nc, kTestMagic, kTestVersion);
    CHECK(!FrameCrcDisabled(nc.hdr));
    CHECK(FrameVerify(nc, kTestMagic, kTestVersion));
  }

  // CRC tamper detection on both header and payload bytes.
  {
    Frame<Bytes64> tampered = msg;
    tampered.payload[10] ^= 0xFFu;
    CHECK(!FrameVerify(tampered, kTestMagic, kTestVersion));
  }
  {
    Frame<Bytes64> tampered = msg;
    tampered.hdr.sequence ^= 1u;
    CHECK(!FrameVerify(tampered, kTestMagic, kTestVersion));
  }

  // magic/version mismatch.
  CHECK(!FrameVerify(msg, kTestMagic ^ 1u, kTestVersion));
  CHECK(!FrameVerify(msg, kTestMagic, static_cast<uint16_t>(kTestVersion + 1u)));

  // Byte-identical wire image vs the hand-written BlobPod path.
  BlobPod ref{};
  std::memset(&ref, 0, sizeof(ref));
  ref.hdr.sequence = 42;
  ref.hdr.mono_ns = 123456789u;
  ref.hdr.session_id = 7;
  ref.hdr.size = static_cast<uint16_t>(sizeof(FrameHeader) + 64);
  for (std::size_t i = 0; i < 64; ++i) {
    ref.payload[i] = static_cast<uint8_t>(i * 3u);
  }
  FrameHeaderSeal(ref.hdr, ref.payload, sizeof(ref.payload), kTestMagic,
                  kTestVersion);
  CHECK(ref.hdr.valid == 1);  // seal sets data-valid, matching FrameInit+Seal
  CHECK(std::memcmp(&msg, &ref, sizeof(BlobPod)) == 0);

  // Aligned payload: seal/verify address the member, padding stays off-wire.
  Frame<Align8Payload> am{};
  FrameInit(am, 1u, 2u, 3u);
  am.payload.words[0] = 0xDEADBEEFu;
  FrameSeal(am, kTestMagic, kTestVersion);
  CHECK(FrameVerify(am, kTestMagic, kTestVersion));
  {
    Frame<Align8Payload> tampered = am;
    tampered.payload.words[7] ^= 1u;
    CHECK(!FrameVerify(tampered, kTestMagic, kTestVersion));
  }

  // size-field tampering must reject even when the CRC is recomputed over the
  // tampered header (attacker-consistent CRC): the size/span consistency
  // check in FrameHeaderOk is what rejects it.
  {
    Frame<Bytes64> msg2{};
    FrameInit(msg2, 5u, 6u, 7u);
    FrameSeal(msg2, kTestMagic, kTestVersion);
    Frame<Bytes64> shrunk = msg2;
    shrunk.hdr.size -= 1;  // one byte shorter than the payload span
    shrunk.hdr.crc32 = 0;
    shrunk.hdr.crc32 = Crc32HeaderPayload(shrunk.hdr, shrunk.payload, 64);
    CHECK(!FrameVerify(shrunk, kTestMagic, kTestVersion));
    Frame<Bytes64> grown = msg2;
    grown.hdr.size += 1;
    grown.hdr.crc32 = 0;
    grown.hdr.crc32 = Crc32HeaderPayload(grown.hdr, grown.payload, 64);
    CHECK(!FrameVerify(grown, kTestMagic, kTestVersion));
  }

  // Large frame: 60 KB payload passes seal/verify (uint16 size range is
  // fine); the static_assert in FrameInit guards the compile-time overflow.
  {
    auto big = std::make_unique<Frame<BigPayload>>();
    FrameInit(*big, 9u, 10u, 11u);
    CHECK(big->hdr.size == sizeof(FrameHeader) + 60000);
    for (uint32_t i = 0; i < 60000; ++i) {
      big->payload.data[i] = static_cast<uint8_t>(i * 7);
    }
    FrameSeal(*big, kTestMagic, kTestVersion);
    CHECK(FrameVerify(*big, kTestMagic, kTestVersion));
    big->payload.data[59999] ^= 1u;
    CHECK(!FrameVerify(*big, kTestMagic, kTestVersion));
  }

  return 0;
}
