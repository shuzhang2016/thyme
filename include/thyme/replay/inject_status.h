#pragma once

// inject_status.h — outcome of injecting one replay event into a live target
// (REP-03/REP-04). Shared by the big-frame and small-struct injectors so the
// synchronized player (REP-05) maps one vocabulary onto its counters.

#include <cstdint>

namespace thyme {
namespace replay {

enum class InjectStatus : uint8_t {
  kOk = 0,
  // Event carries no injectable payload for this target: a GAP/trigger/
  // session marker on a mapped channel. Never counted as data loss.
  kIgnored,
  // BF-03: the target slot was pinned by a reader; the writer skipped it
  // (head advanced, no frame published). Expected under concurrent readers.
  kSkippedPinned,
  // The event payload is larger than the target slot capacity. Nothing was
  // written and nothing was truncated.
  kOversize,
  // No target is mapped for the event's (channel[, struct_id]) pair.
  kUnmapped,
  // The event itself is unusable: null payload or len == 0 for a data record.
  kBadEvent,
  // The package payload CRC disagrees with the .raw bytes. With CRC
  // verification enabled the frame is not injected.
  kCrcMismatch,
  // The target refused the write for a non-geometry reason (e.g. a torn-slot
  // race on a small-struct ring).
  kWriteFailed,
  // REP-10: the target channel's producer gate refused the write. For a
  // replay injector this means it lost gate ownership (another replay stole
  // the expired lease); for a real producer it is the expected replay block.
  kGated,
  kNotOpen,
};

const char* InjectStatusName(InjectStatus status);

}  // namespace replay
}  // namespace thyme
