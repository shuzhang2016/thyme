#pragma once
// FrameHeader wire format. NOTE on size: hdr.size is uint16_t wire bytes
// (header + payload span), so transport frames are capped at 64 KB — larger
// contracts belong on PinnedSlotStore, not on a Frame edge (see frame.h).
//
// Field contract (ABI-05, docs/reference/abi-compatibility.md):
//   magic/version — identify the product + wire format; FrameHeaderOk requires
//                   both to match the reader's expected values exactly.
//   size          — wire span, header included. FrameHeaderOk requires it to
//                   equal sizeof(FrameHeader) + payload span.
//   valid         — DATA-valid, not header-structural-valid. 1 means the
//                   payload bytes under this header were fully written and are
//                   stable; 0 means "do not read the payload". A conforming
//                   writer sets it only after the payload is complete (the
//                   seal helpers do this); FrameHeaderOk rejects valid != 1 at
//                   the data boundary. Use FrameHeaderStructurallyOk when a
//                   reader only has the header and wants metadata.
//   reserved[0]   — bit0 = kFrameFlagCrcDisabled; all other bits must be 0.
//   crc32         — CRC over this header (crc32 zeroed) + payload. Independent
//                   of `valid`: a CRC-disabled frame still has valid == 1.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(__x86_64__) || defined(__i386__)
#define THYME_CRC32_X86_PCLMUL 1
#include <immintrin.h>
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#define THYME_CRC32_ARM_ACLE 1
#include <arm_acle.h>
#if defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1u << 7)
#endif
#endif
#endif

namespace thyme {

#pragma pack(push, 1)
/** \brief Wire header for every `Frame<T>`: magic, version, wire size, valid
 *  flag and CRC32.
 *  \note Trivially copyable. The producer owns/writes it; the consumer reads a
 *        sealed header only. A payload is readable only when `valid == 1` and
 *        the CRC matches; `size` spans header + payload and is capped at 64 KB
 *        by the uint16 field (larger contracts use PinnedSlotStore). */
struct FrameHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint64_t session_id;
  uint64_t mono_ns;
  uint32_t sequence;
  uint8_t valid;  // ABI-05: 1 = payload complete/stable (data-valid), 0 = not
  uint8_t reserved[3];
  uint32_t crc32;
  uint32_t reserved2;  // explicit tail: makes sizeof(FrameHeader) 40 (8-multiple)
};
#pragma pack(pop)
static_assert(sizeof(FrameHeader) == 40,
              "FrameHeader is 40 bytes (uint16 size; 8-byte multiple)");
static_assert(offsetof(FrameHeader, reserved) == 29,
              "FrameHeader reserved bits are ABI (CRC-disable flag)");
static_assert(offsetof(FrameHeader, crc32) == 32,
              "crc32 offset is the FrameHeader wire ABI");
static_assert(offsetof(FrameHeader, reserved2) == 36,
              "explicit tail keeps the wire header 40 bytes");

// ABI-12: reserved[0] bit0 marks a frame whose CRC was intentionally not
// computed. crc32 is 0 in that case, but a zero crc32 alone is NOT enough to
// skip verification (a real CRC can be 0). Old readers that do not know this
// bit will still try to verify the CRC and reject the frame; no-CRC is opt-in.
// All other reserved bits, and reserved[1..2], must be 0: FrameHeaderOk
// rejects a frame that carries an undefined reserved bit (a future flag this
// build cannot honour) instead of silently accepting it.
inline constexpr uint8_t kFrameFlagCrcDisabled = 1u;

inline bool FrameCrcDisabled(const FrameHeader& h) {
  return (h.reserved[0] & kFrameFlagCrcDisabled) != 0;
}

template <typename T>
struct FramePodHasPayload {
  template <typename U>
  static auto test(int) -> decltype(static_cast<void>(sizeof(U::payload)), std::true_type{});
  template <typename>
  static std::false_type test(...);
  static constexpr bool value = decltype(test<T>(0))::value;
};

template <typename T, bool HasPayload = FramePodHasPayload<T>::value>
struct FramePodWireBytesImpl {
  static constexpr std::size_t value = sizeof(T);
};
template <typename T>
struct FramePodWireBytesImpl<T, true> {
  static constexpr std::size_t value = sizeof(FrameHeader) + sizeof(T::payload);
};

template <typename T>
constexpr std::size_t FramePodWireBytes = FramePodWireBytesImpl<T>::value;

namespace detail {

// Bitwise reference: CRC-32 (IEEE 802.3, reflected poly 0xEDB88320), running
// CRC semantics — Crc32()/Crc32HeaderPayload() apply the init/final XOR.
inline uint32_t Crc32UpdateBitwise(uint32_t c, const uint8_t* data, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    c ^= data[i];
    for (int b = 0; b < 8; ++b) {
      c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
  }
  return c;
}

#if defined(THYME_CRC32_X86_PCLMUL)
// PCLMULQDQ carry-less-multiply folding (Gopal et al., "Fast CRC Computation
// for Generic Polynomials Using PCLMULQDQ"). Function-level target keeps the
// binary runnable on CPUs without PCLMUL/SSE4.1; call sites must gate on
// Crc32HwAccelerated(). SSE4.2's crc32 insn is CRC32C and cannot be used here.
__attribute__((target("pclmul,sse4.1")))
inline uint32_t Crc32UpdatePclmul(uint32_t c, const uint8_t* data, std::size_t n) {
  if (n < 16) {
    return Crc32UpdateBitwise(c, data, n);
  }
  // k12 = {x^128 mod P, x^192 mod P}: fold distance of one 16-byte lane.
  const __m128i k12 = _mm_set_epi32(0x00000001, 0x751997D0, 0x00000000,
                                    static_cast<int>(0xCCAA009Eu));
  // Barrett reduction constants: low qword mu' = 0xB4E5B025F7011641,
  // high qword P' = 0x1DB710640 (reflected forms, Intel paper p.16).
  const __m128i bark = _mm_set_epi32(0x00000001, static_cast<int>(0xDB710640u),
                                     static_cast<int>(0xB4E5B025u),
                                     static_cast<int>(0xF7011641u));
  __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
  x = _mm_xor_si128(x, _mm_cvtsi32_si128(static_cast<int>(c)));
  data += 16;
  n -= 16;
  while (n >= 16) {
    const __m128i t = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
    const __m128i lo = _mm_clmulepi64_si128(x, k12, 0x01);
    const __m128i hi = _mm_clmulepi64_si128(x, k12, 0x10);
    x = _mm_xor_si128(_mm_xor_si128(lo, hi), t);
    data += 16;
    n -= 16;
  }
  const __m128i t0 = _mm_clmulepi64_si128(x, bark, 0x00);
  __m128i t1 = _mm_clmulepi64_si128(t0, bark, 0x10);
  t1 = _mm_blend_epi16(t1, _mm_setzero_si128(), 0xCF);
  const __m128i t2 = _mm_xor_si128(t1, x);
  const __m128i ra = _mm_clmulepi64_si128(t2, bark, 0x01);
  const __m128i rb = _mm_clmulepi64_si128(ra, bark, 0x10);
  c = static_cast<uint32_t>(_mm_extract_epi32(rb, 2));
  return Crc32UpdateBitwise(c, data, n);
}

inline bool Crc32HwAccelerated() {
  static const bool kHas = __builtin_cpu_supports("pclmul") != 0 &&
                           __builtin_cpu_supports("sse4.1") != 0;
  return kHas;
}
#elif defined(THYME_CRC32_ARM_ACLE)
// ARMv8 CRC32 extension (ARMv8 targets). Per ACLE, __crc32b/h/w/d chain the CRC
// accumulator directly (seed 0xFFFFFFFF, feed, final ^0xFFFFFFFF) — the same
// running-CRC semantics as Crc32UpdateBitwise, so no inversion here; the
// callers (Crc32/Crc32HeaderPayload) own the init/final XOR. Little-endian
// loads match the instruction byte order.
inline uint32_t Crc32UpdateArm(uint32_t c, const uint8_t* data, std::size_t n) {
  uint32_t x = c;
  while (n >= sizeof(uint64_t)) {
    uint64_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    x = __crc32d(x, v);
    data += sizeof(v);
    n -= sizeof(v);
  }
  if (n >= sizeof(uint32_t)) {
    uint32_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    x = __crc32w(x, v);
    data += sizeof(v);
    n -= sizeof(v);
  }
  if (n >= sizeof(uint16_t)) {
    uint16_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    x = __crc32h(x, v);
    data += sizeof(v);
    n -= sizeof(v);
  }
  if (n != 0) {
    x = __crc32b(x, *data);
  }
  return x;
}

inline bool Crc32HwAccelerated() {
#if defined(__linux__)
  // Runtime probe: __ARM_FEATURE_CRC32 only proves this TU was built with
  // +crc; a target core may still lack the extension (SIGILL risk).
  static const bool kHas = (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
  return kHas;
#else
  return true;
#endif
}
#else
inline bool Crc32HwAccelerated() { return false; }
#endif

}  // namespace detail

inline uint32_t Crc32Update(uint32_t c, const uint8_t* data, std::size_t n) {
#if defined(THYME_CRC32_X86_PCLMUL)
  if (detail::Crc32HwAccelerated()) {
    return detail::Crc32UpdatePclmul(c, data, n);
  }
#elif defined(THYME_CRC32_ARM_ACLE)
  if (detail::Crc32HwAccelerated()) {
    return detail::Crc32UpdateArm(c, data, n);
  }
#endif
  return detail::Crc32UpdateBitwise(c, data, n);
}

inline uint32_t Crc32(const uint8_t* data, std::size_t n) {
  return Crc32Update(0xFFFFFFFFu, data, n) ^ 0xFFFFFFFFu;
}

inline uint32_t Crc32HeaderPayload(const FrameHeader& h_crc_zero, const uint8_t* payload,
                                   std::size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&h_crc_zero), sizeof(FrameHeader));
  c = Crc32Update(c, payload, n);
  return c ^ 0xFFFFFFFFu;
}

inline void FrameHeaderSeal(FrameHeader& h, const uint8_t* payload, std::size_t n,
                            uint32_t magic, uint16_t version) {
  h.magic = magic;
  h.version = version;
  h.size = static_cast<uint16_t>(sizeof(FrameHeader) + n);
  h.reserved[0] &= static_cast<uint8_t>(~kFrameFlagCrcDisabled);
  h.reserved2 = 0;  // explicit tail is always zero on the wire
  h.crc32 = 0;
  // ABI-05: the payload is complete at this point, so the frame becomes
  // data-valid. Set it before the CRC pass so the CRC covers valid == 1.
  h.valid = 1;
  h.crc32 = Crc32HeaderPayload(h, payload, n);
}

// ABI-12: seal without computing CRC. crc32 is 0 and the reserved bit tells
// FrameHeaderOk to skip the check. Callers that need integrity must use the
// normal FrameHeaderSeal/FrameSeal. ABI-05: valid is still 1 — data-valid is
// independent of the CRC policy.
inline void FrameHeaderSealNoCrc(FrameHeader& h, uint32_t magic,
                                 uint16_t version) {
  h.magic = magic;
  h.version = version;
  h.reserved[0] = static_cast<uint8_t>(
      (h.reserved[0] & static_cast<uint8_t>(~kFrameFlagCrcDisabled)) |
      kFrameFlagCrcDisabled);
  h.reserved2 = 0;  // explicit tail is always zero on the wire
  h.valid = 1;
  h.crc32 = 0;
}

// Header-only structural validation (ABI-05): magic/version match, only
// defined reserved bits, a boolean `valid` value, and a wire size that can at
// least hold the header. Deliberately does NOT check `valid == 1` or the CRC —
// this is the metadata-inspection path for a reader that holds only the header.
// Never consume payload bytes on the strength of this function alone; call
// FrameHeaderOk at the data boundary (see docs/reference/abi-compatibility.md rule 9).
inline bool FrameHeaderStructurallyOk(const FrameHeader& h, uint32_t magic,
                                      uint16_t version) {
  if (h.magic != magic || h.version != version) return false;
  // reserved[0] bit0 (kFrameFlagCrcDisabled) is the only defined reserved bit;
  // every other reserved bit must be 0 (docs/reference/abi-compatibility.md rule 3).
  // A frame carrying a bit this build does not implement is a future flag or
  // corruption: reject it instead of interpreting it with today's semantics.
  if ((h.reserved[0] & static_cast<uint8_t>(~kFrameFlagCrcDisabled)) != 0 ||
      h.reserved[1] != 0 || h.reserved[2] != 0 || h.reserved2 != 0) {
    return false;
  }
  // valid is a boolean; values above 1 are undefined and no conforming writer
  // emits them. valid == 0 is structurally fine (payload not complete yet).
  if (h.valid > 1) return false;
  return h.size >= sizeof(FrameHeader);
}

inline bool FrameHeaderOk(const FrameHeader& h, const uint8_t* payload, std::size_t n,
                          uint32_t magic, uint16_t version) {
  if (!FrameHeaderStructurallyOk(h, magic, version)) return false;
  // ABI-05 data boundary: valid == 1 means the producer finished writing the
  // payload. A valid == 0 frame (slot still being filled, or never sealed) is
  // not readable data even when its CRC happens to match.
  if (h.valid != 1) return false;
  // The size field must match the wire span actually crc'ed: size is inside
  // the CRC, so a tampered size would already fail — this check makes the
  // contract explicit and catches callers passing a mismatched span. size is
  // wire bytes (header + payload), n is the payload span.
  if (h.size != n + sizeof(FrameHeader)) return false;
  if (FrameCrcDisabled(h)) {
    return h.crc32 == 0;
  }
  FrameHeader tmp = h;
  const uint32_t got = tmp.crc32;
  tmp.crc32 = 0;
  return Crc32HeaderPayload(tmp, payload, n) == got;
}

}  // namespace thyme
