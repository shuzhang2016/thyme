#pragma once
#include "thyme/contracts/frame_header.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace thyme {

// Generic framed message: packed 40-byte header followed by a POD payload.
// Frame is deliberately NOT packed: if Payload's alignment is greater than 1
// the compiler inserts padding between hdr and payload. That padding is never
// part of the wire image — sealing/verification address the payload member
// directly (see FrameBodyPtr), and the wire size is always
// sizeof(FrameHeader) + sizeof(Payload), which FramePodWireBytes computes
// automatically because Frame<P> matches the payload-member convention.
//
// TRANSPORT SIZE LIMIT: hdr.size is uint16_t, so a Frame's wire size is at
// most 64 KB (compile-time enforced by FrameInit's static_assert). Contracts
// larger than that (e.g. perception maps) must NOT ride a graph edge as a
// Frame; they go through PinnedSlotStore (runtime parameter-sized slots) and
// are recorded from there. The 512 KB kAbiMaxPayloadBytes ceiling applies to
// the record layer (mirror ring / thyme_bin codec), not to Frame transport.
template <typename Payload>
struct Frame {
  static_assert(std::is_trivially_copyable<Payload>::value,
                "Frame payload must be trivially copyable (SHM wire image)");
  static_assert(std::is_standard_layout<Payload>::value,
                "Frame payload must be standard layout (SHM wire image)");

  FrameHeader hdr;
  Payload payload;
};

// Byte span covered by the CRC: the payload member for payload-blob types,
// otherwise the whole body after the header. Single source of truth shared by
// FrameSeal/FrameVerify so the two can never drift apart; mirrors the
// semantics of gen_contracts' FillStub.
template <typename T>
inline const uint8_t* FrameBodyPtr(const T& msg, std::size_t* out_n) {
  if constexpr (FramePodHasPayload<T>::value) {
    *out_n = sizeof(msg.payload);
    return reinterpret_cast<const uint8_t*>(&msg.payload);
  }
  *out_n = sizeof(T) - sizeof(FrameHeader);
  return reinterpret_cast<const uint8_t*>(&msg) + sizeof(FrameHeader);
}

// Zero the message and fill sequence/mono_ns/session_id/size.
// magic/version/crc32 are left to FrameSeal (they are per-product parameters).
// ABI-05: `valid` stays 0 (not data-valid) until the payload is complete —
// FrameSeal/FrameSealNoCrc set it, or FrameMarkValid for an unsealed channel.
template <typename T>
inline void FrameInit(T& msg, uint32_t seq, uint64_t mono_ns, uint64_t session) {
  static_assert(std::is_standard_layout<T>::value &&
                    std::is_trivially_copyable<T>::value,
                "FrameInit requires a POD frame type");
  // hdr.size is uint16_t: without this assert an oversized frame type would be
  // silently truncated into a wrong-but-plausible wire size.
  static_assert(FramePodWireBytes<T> <= 0xFFFFu,
                "FrameInit: wire size exceeds the uint16_t hdr.size field");
  std::memset(&msg, 0, sizeof(msg));
  msg.hdr.sequence = seq;
  msg.hdr.mono_ns = mono_ns;
  msg.hdr.session_id = session;
  msg.hdr.size = static_cast<uint16_t>(FramePodWireBytes<T>);
}

// ABI-05: mark a frame data-valid without sealing (no CRC). Use only for
// channels that do not verify FrameHeaderOk (e.g. the in-process event
// transport). The payload must already be fully written; for the normal
// CRC-carrying path use FrameSeal/FrameSealNoCrc, which set this themselves.
template <typename T>
inline void FrameMarkValid(T& msg) {
  msg.hdr.valid = 1;
}

template <typename T>
inline void FrameSeal(T& msg, uint32_t magic, uint16_t version) {
  std::size_t n = 0;
  const uint8_t* body = FrameBodyPtr(msg, &n);
  FrameHeaderSeal(msg.hdr, body, n, magic, version);
}

// ABI-12: explicit no-CRC seal. Default small-structure path stays FrameSeal.
// Use only when the channel is configured with crc=off; readers must be
// ABI-12 aware or they will reject the frame.
template <typename T>
inline void FrameSealNoCrc(T& msg, uint32_t magic, uint16_t version) {
  FrameHeaderSealNoCrc(msg.hdr, magic, version);
}

template <typename T>
inline bool FrameVerify(const T& msg, uint32_t magic, uint16_t version) {
  std::size_t n = 0;
  const uint8_t* body = FrameBodyPtr(msg, &n);
  return FrameHeaderOk(msg.hdr, body, n, magic, version);
}

}  // namespace thyme
