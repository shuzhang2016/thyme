#pragma once

// header_policy.h — REP-12: header rewrite policy for replay injection
// (roadmap §3.17: "replay header rewrite: before injection set hdr.mono_ns to
// the replay instant, session_id to the new session, reorder sequence by
// policy, and recompute the CRC").
//
// Why each field is rewritten
// ---------------------------
// A replayed frame is a NEW publication into a LIVE channel, not a copy of the
// recorded slot image. Copying recorded header fields verbatim would corrupt
// the target's own bookkeeping:
//
//   * mono_ns — the replay-scheduled time (the event's position on the merged
//     timeline, which under wall-clock pacing is the instant the frame goes
//     live). The target slot headers already get it (PinnedSlotWriter::Commit /
//     TryWriteRing take it as a parameter); the policy makes the same rewrite
//     uniform for a FrameHeader embedded in the payload. Recording-time
//     timestamps inside a live system would mislead latency measurements and
//     time-window harvesters.
//   * session_id — the current replay session's id, never the recorded one.
//     The target rings/channels carry their own session/boot identity at the
//     page/channel level (RecordMirrorRingPage::Header::session_id is set by
//     the target owner's Init; PinnedSlotStore has no per-slot session field)
//     and the injector never touches those — the target's identity wins by
//     construction. What the injector CAN carry over is a FrameHeader
//     embedded in the payload (the app-level Frame<P> wire format): its
//     session_id is rewritten to the replay session id so consumers can
//     distinguish replayed frames from both the original capture and the
//     target's live producers.
//   * sequence — default: renumbered per target channel, monotonically
//     increasing from 0 over the injection order. Mixing recorded sequences
//     into a live system breaks consumers that track gaps (a recorded window
//     starts mid-stream, so its first sequence is arbitrary). Opt-in
//     (renumber_sequence = false) preserves the recorded producer sequence,
//     e.g. for record -> replay -> record byte-equality analysis. The
//     channel-assigned publish order (PinnedSlot slot seq, ring publish_index)
//     is always the target's own and is never imported from the package.
//   * CRC — after the rewrite the recorded CRC no longer covers the bytes.
//     PinnedSlotWriter::Commit always recomputes the slot CRC over the payload
//     actually published (or stores 0 when the channel has CRC disabled);
//     for an embedded FrameHeader the injector re-seals per ABI-12: a frame
//     recorded with kFrameFlagCrcDisabled keeps the flag set and crc32 = 0,
//     any other frame gets a fresh Crc32HeaderPayload over the rewritten
//     header + payload.
//
// Embedded-frame detection: a payload counts as framed only when it starts
// with a self-consistent FrameHeader (size field == payload length,
// valid == 1 — ABI-05 data-valid, magic != 0, and the ABI-12 CRC contract
// holds — full CRC verification for normal frames, crc32 == 0 for
// crc-disabled ones). Anything else is injected byte-for-byte. Detection of a
// CRC-enabled frame costs one extra CRC pass over the payload; frames larger
// than 64 KB cannot be Frame wire images (uint16 size) and are rejected before
// any CRC work.
//
// The pre-injection integrity check is unchanged: PinnedSlotInjector verifies
// the recorded FrameIndexRecord::payload_crc against the RECORDED bytes
// before any rewrite.
//
// Hot path: no allocation. The big-frame rewrite happens in the target slot
// after the payload copy; the small-struct rewrite uses a per-injector
// scratch buffer allocated in Open() (a Frame wire image is at most 64 KB,
// so one fixed-size scratch covers every frame).

#include "thyme/contracts/frame_header.h"

#include <cstdint>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace thyme {
namespace replay {

// Injection-time header policy. The default construction is the safe rewrite;
// setting rewrite_embedded_frame = false and renumber_sequence = false
// reproduces the pre-REP-12 byte-for-byte injection.
struct ReplayHeaderPolicy {
  // Detect an embedded FrameHeader and rewrite its mono_ns/session_id/
  // sequence, then re-seal per ABI-12. Master switch for the payload rewrite;
  // target slot headers are always written from the scheduled values.
  bool rewrite_embedded_frame = true;
  // Embedded hdr.mono_ns := the replay-scheduled time (the event's timeline
  // mono_ns). Only meaningful with rewrite_embedded_frame.
  bool rewrite_mono_ns = true;
  // Embedded hdr.session_id := session_id below. Only meaningful with
  // rewrite_embedded_frame.
  bool rewrite_session = true;
  // Renumber sequences per target channel from 0 in injection order. Applies
  // to the small-struct ring's source_sequence and to the embedded
  // FrameHeader's sequence. false = pass the recorded producer sequence
  // through and keep the recorded embedded sequence.
  bool renumber_sequence = true;
  // Replay session id stamped into rewritten embedded headers. 0 = generate
  // one at Open (the player generates a single id shared by both injectors).
  uint64_t session_id = 0;
};

// Non-crypto session id: realtime clock mixed with the pid (splitmix64
// finalizer). Only needs to distinguish one replay session from another.
inline uint64_t GenerateReplaySessionId() {
  struct timespec ts {};
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t x = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
  x ^= static_cast<uint64_t>(getpid()) << 32;
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  x = x ^ (x >> 31);
  return x != 0 ? x : 1;
}

// True when payload[0..len) starts with a self-consistent FrameHeader (see
// the header comment for the exact contract). Never throws, never allocates.
inline bool DetectEmbeddedFrame(const uint8_t* payload, uint32_t len) {
  if (payload == nullptr || len < sizeof(FrameHeader) || len > 0xFFFFu) {
    return false;
  }
  FrameHeader h{};
  std::memcpy(&h, payload, sizeof(h));
  if (h.magic == 0) {
    return false;
  }
  // FrameHeaderOk enforces the size/span match, ABI-12 (crc-disabled frames
  // must carry crc32 == 0) and the ABI-05 data-valid boundary (valid == 1).
  // Echoing the header's own magic/version validates self-consistency without
  // knowing the product's magic.
  return FrameHeaderOk(h, payload + sizeof(FrameHeader),
                       len - sizeof(FrameHeader), h.magic, h.version);
}

// Rewrite the FrameHeader at dst[0..36) in place and re-seal per ABI-12.
// Callers must have established DetectEmbeddedFrame(dst, len) on the same
// bytes. magic/version/valid/flags (incl. the CRC-disable bit) are preserved.
inline void RewriteEmbeddedFrame(uint8_t* dst, uint32_t len,
                                 const ReplayHeaderPolicy& policy,
                                 uint64_t mono_ns, uint32_t sequence) {
  FrameHeader h{};
  std::memcpy(&h, dst, sizeof(h));
  if (policy.rewrite_mono_ns) {
    h.mono_ns = mono_ns;
  }
  if (policy.rewrite_session) {
    h.session_id = policy.session_id;
  }
  if (policy.renumber_sequence) {
    h.sequence = sequence;
  }
  if (FrameCrcDisabled(h)) {
    // ABI-12: no-CRC frames keep the flag set and crc32 == 0.
    h.crc32 = 0;
  } else {
    h.crc32 = 0;
    h.crc32 = Crc32HeaderPayload(h, dst + sizeof(FrameHeader),
                                 len - sizeof(FrameHeader));
  }
  std::memcpy(dst, &h, sizeof(h));
}

}  // namespace replay
}  // namespace thyme
