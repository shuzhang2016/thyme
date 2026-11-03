#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include <cstring>

using namespace thyme;

namespace {
inline constexpr uint32_t kTestMagic = 0x54535448u; /* "TSTH" */
inline constexpr uint16_t kTestVersion = 7;

struct BlobPod {
  FrameHeader hdr;
  uint8_t payload[64];
};

struct StructuredPod {
  FrameHeader hdr;
  uint32_t a;
  uint32_t b;
};

static_assert(FramePodHasPayload<BlobPod>::value, "BlobPod must report a payload");
static_assert(!FramePodHasPayload<StructuredPod>::value,
              "StructuredPod must report no payload");
static_assert(FramePodWireBytes<BlobPod> == sizeof(FrameHeader) + 64,
              "blob wire bytes");
static_assert(FramePodWireBytes<StructuredPod> == sizeof(StructuredPod),
              "structured wire bytes");
}  // namespace

int main() {
  CHECK(sizeof(FrameHeader) == 40);

  FrameHeader h{};
  std::memset(&h, 0, sizeof(h));
  h.magic = kTestMagic;
  h.version = kTestVersion;
  h.size = sizeof(FrameHeader) + 4;  // wire size: header + payload span
  h.session_id = 1;
  h.sequence = 7;
  h.mono_ns = 123;
  // ABI-05: init-time header is structurally valid but NOT data-valid.
  CHECK(h.valid == 0);
  CHECK(FrameHeaderStructurallyOk(h, kTestMagic, kTestVersion));
  CHECK(!FrameHeaderOk(h, reinterpret_cast<const uint8_t*>("abcd"), 4,
                       kTestMagic, kTestVersion));
  h.size = 0;
  FrameHeaderSeal(h, reinterpret_cast<const uint8_t*>("abcd"), 4, kTestMagic,
                  kTestVersion);
  CHECK(h.size == sizeof(FrameHeader) + 4);
  CHECK(h.valid == 1);  // seal marks the payload complete
  CHECK(h.crc32 != 0);
  CHECK(FrameHeaderOk(h, reinterpret_cast<const uint8_t*>("abcd"), 4, kTestMagic,
                      kTestVersion));
  CHECK(!FrameHeaderOk(h, reinterpret_cast<const uint8_t*>("abce"), 4, kTestMagic,
                       kTestVersion));

  // ABI-12: no-CRC seal uses reserved[0] bit0; a zero crc alone is not enough.
  {
    FrameHeader nc = h;
    FrameHeaderSealNoCrc(nc, kTestMagic, kTestVersion);
    CHECK(nc.crc32 == 0);
    CHECK(nc.valid == 1);  // ABI-05: data-valid independent of CRC policy
    CHECK(FrameCrcDisabled(nc));
    CHECK(FrameHeaderOk(nc, reinterpret_cast<const uint8_t*>("abcd"), 4,
                        kTestMagic, kTestVersion));
    // Flag set + non-zero crc is inconsistent.
    FrameHeader bad = nc;
    bad.crc32 = 1;
    CHECK(!FrameHeaderOk(bad, reinterpret_cast<const uint8_t*>("abcd"), 4,
                         kTestMagic, kTestVersion));
    // Re-sealing with CRC clears the flag and restores verification.
    FrameHeaderSeal(nc, reinterpret_cast<const uint8_t*>("abcd"), 4, kTestMagic,
                    kTestVersion);
    CHECK(!FrameCrcDisabled(nc));
    CHECK(nc.crc32 != 0);
    CHECK(FrameHeaderOk(nc, reinterpret_cast<const uint8_t*>("abcd"), 4,
                        kTestMagic, kTestVersion));
    // A zero crc with the flag clear must not be accepted as "disabled".
    FrameHeader zero = h;
    zero.crc32 = 0;
    zero.reserved[0] &= static_cast<uint8_t>(~kFrameFlagCrcDisabled);
    CHECK(!FrameHeaderOk(zero, reinterpret_cast<const uint8_t*>("abcd"), 4,
                         kTestMagic, kTestVersion));
    // Explicit tail padding is reserved and must be zero.
    FrameHeader tail = h;
    tail.reserved2 = 1;
    CHECK(!FrameHeaderOk(tail, reinterpret_cast<const uint8_t*>("abcd"), 4,
                         kTestMagic, kTestVersion));
  }

  // Tampered size field must reject (the size/span consistency check).
  {
    FrameHeader bad_size = h;
    bad_size.size = sizeof(FrameHeader) + 3;  // off by one vs payload span
    CHECK(!FrameHeaderOk(bad_size, reinterpret_cast<const uint8_t*>("abcd"), 4,
                         kTestMagic, kTestVersion));
    bad_size = h;
    bad_size.size = sizeof(FrameHeader) + 5;
    CHECK(!FrameHeaderOk(bad_size, reinterpret_cast<const uint8_t*>("abcd"), 4,
                         kTestMagic, kTestVersion));
  }

  // Wrong magic/version must reject even a structurally-correct CRC.
  FrameHeader wrong = h;
  FrameHeaderSeal(wrong, reinterpret_cast<const uint8_t*>("abcd"), 4, 0xDEADBEEFu,
                  9);
  CHECK(!FrameHeaderOk(wrong, reinterpret_cast<const uint8_t*>("abcd"), 4,
                       kTestMagic, kTestVersion));
  CHECK(!FrameHeaderStructurallyOk(wrong, kTestMagic, kTestVersion));

  // ABI-05: valid is a data-valid boundary, not a header-structure check.
  {
    const uint8_t* payload = reinterpret_cast<const uint8_t*>("abcd");

    // A structurally fine header whose writer never finished the payload.
    // Recompute the CRC after clearing valid so only the valid check rejects.
    FrameHeader not_ready = h;
    not_ready.valid = 0;
    not_ready.crc32 = 0;
    not_ready.crc32 = Crc32HeaderPayload(not_ready, payload, 4);
    CHECK(FrameHeaderStructurallyOk(not_ready, kTestMagic, kTestVersion));
    CHECK(!FrameHeaderOk(not_ready, payload, 4, kTestMagic, kTestVersion));

    // Same for a CRC-disabled frame: crc32 == 0 is consistent, valid still
    // gates the data boundary.
    FrameHeader nc_not_ready{};
    std::memset(&nc_not_ready, 0, sizeof(nc_not_ready));
    nc_not_ready.size = sizeof(FrameHeader) + 4;
    FrameHeaderSealNoCrc(nc_not_ready, kTestMagic, kTestVersion);
    CHECK(nc_not_ready.valid == 1);
    nc_not_ready.valid = 0;
    CHECK(FrameHeaderStructurallyOk(nc_not_ready, kTestMagic, kTestVersion));
    CHECK(!FrameHeaderOk(nc_not_ready, payload, 4, kTestMagic, kTestVersion));

    // valid is boolean: 2..255 is undefined and structurally rejected.
    FrameHeader undefined_valid = h;
    undefined_valid.valid = 2;
    undefined_valid.crc32 = 0;
    undefined_valid.crc32 = Crc32HeaderPayload(undefined_valid, payload, 4);
    CHECK(!FrameHeaderStructurallyOk(undefined_valid, kTestMagic, kTestVersion));
    CHECK(!FrameHeaderOk(undefined_valid, payload, 4, kTestMagic,
                         kTestVersion));

    // Structural sanity: size must at least hold the header.
    FrameHeader tiny = h;
    tiny.size = sizeof(FrameHeader) - 1;
    CHECK(!FrameHeaderStructurallyOk(tiny, kTestMagic, kTestVersion));
  }

  // Structured (payload-less) POD: CRC over the body that follows the header.
  StructuredPod s{};
  std::memset(&s, 0, sizeof(s));
  s.a = 1;
  s.b = 2;
  s.hdr.size = sizeof(s);  // wire size = header + body
  CHECK(s.hdr.valid == 0);
  FrameHeaderSeal(s.hdr, reinterpret_cast<const uint8_t*>(&s) + sizeof(FrameHeader),
                  sizeof(s) - sizeof(FrameHeader), kTestMagic, kTestVersion);
  CHECK(s.hdr.valid == 1);
  CHECK(FrameHeaderOk(s.hdr, reinterpret_cast<const uint8_t*>(&s) + sizeof(FrameHeader),
                      sizeof(s) - sizeof(FrameHeader), kTestMagic, kTestVersion));

  // Blob POD: CRC over just the payload member.
  BlobPod b{};
  std::memset(&b, 0, sizeof(b));
  b.hdr.size = sizeof(FrameHeader) + sizeof(b.payload);
  FrameHeaderSeal(b.hdr, b.payload, sizeof(b.payload), kTestMagic, kTestVersion);
  CHECK(b.hdr.valid == 1);
  CHECK(FrameHeaderOk(b.hdr, b.payload, sizeof(b.payload), kTestMagic,
                      kTestVersion));

  return 0;
}
