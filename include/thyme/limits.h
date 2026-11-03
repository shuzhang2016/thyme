#pragma once

#include "thyme/contracts/abi.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/runtime/app.h"
#include "thyme/runtime/record_mirror.h"
#include "thyme/runtime/shared_health.h"

namespace thyme {
// The literals below are the ABI contract; every module-level constant must
// alias the canonical kAbi* values from contracts/abi.h.
static_assert(kThymeBinMaxPayloadSize == kAbiMaxPayloadBytes,
              "kThymeBinMaxPayloadSize must alias kAbiMaxPayloadBytes");
static_assert(kRecordMirrorMaxPayload == kAbiMaxPayloadBytes,
              "kRecordMirrorMaxPayload must alias kAbiMaxPayloadBytes");
static_assert(kAbiMaxPayloadBytes == 524288u,
              "payload ceiling is part of the .thyme.bin / mirror ABI");
static_assert(kAbiMaxTransportBytes == 65535u,
              "Frame transport ceiling is the uint16_t hdr.size wire image");
static_assert(kRecordMirrorMaxArenaBytes == 2097152u,
              "record-mirror arena budget is part of the SHM layout ABI");
static_assert(kRecordMirrorMaxArenaBytes <=
                  kRecordMirrorMaxSlots * kRecordMirrorMaxPayload,
              "arena budget must not exceed the fixed per-slot ceiling sum");
static_assert(App::kMaxThreads == kAbiMaxThreads,
              "App::kMaxThreads must alias kAbiMaxThreads");
static_assert(SharedHealthPage::kMaxWorkers == kAbiMaxThreads,
              "kMaxWorkers must alias kAbiMaxThreads");
#if !defined(THYME_ABI_PROFILE_CUSTOM)
static_assert(kAbiMaxThreads == 64,
              "worker / thread ceiling is part of the health-page ABI");
#endif
// ABI-13: this is the per-*process* ceiling of thyme-managed workers, not the
// board-wide thread count (a board runs many processes, each with its own
// health page). 1024 slots cost 64 KB in the health page for a process that
// actually configures that many; the default stays 64.
static_assert(kAbiMaxThreads >= 1 && kAbiMaxThreads <= 1024,
              "ABI-13: per-process worker ceiling must stay within 1..1024");
static_assert(kThymeBinMaxStructCount == kAbiMaxStructCount,
              "kThymeBinMaxStructCount must alias kAbiMaxStructCount");
static_assert(kRecordMirrorMaxSlots == kAbiMaxStructCount,
              "kRecordMirrorMaxSlots must alias kAbiMaxStructCount");
#if !defined(THYME_ABI_PROFILE_CUSTOM)
static_assert(kAbiMaxStructCount == 64,
              "struct / slot ceiling is part of the .thyme.bin / mirror ABI");
#endif
// ABI-13: contract kinds per recorded package / per mirror page — not the
// number of distinct C++ types in the product. A multi-process recording is
// the union of every recorded process's registry, so 256 can be tight (e.g.
// 10 processes × 30 contracts). 1024 costs 192 B per kind in the mirror ring
// page (128 B descriptor + 64 B ring descriptor) and 128 B per kind in the
// .tbin registry; the 2 MiB payload arena is independent of this number.
static_assert(kAbiMaxStructCount >= 1 && kAbiMaxStructCount <= 1024,
              "ABI-13: struct/slot ceiling must stay within 1..1024");
static_assert(EventThread::kMaxWaitInputs == kAbiMaxEventWaitInputs,
              "EventThread::kMaxWaitInputs must alias kAbiMaxEventWaitInputs");
static_assert(CyclicThread::kMaxWaitInputs == kAbiMaxCyclicWaitInputs,
              "CyclicThread::kMaxWaitInputs must alias kAbiMaxCyclicWaitInputs");
static_assert(kAbiMaxEventWaitInputs == 32,
              "event wait-input ceiling is shared with gen_graph");
static_assert(kAbiMaxCyclicWaitInputs == 8,
              "cyclic wait-input ceiling is shared with gen_graph");
// ABI-13: SPMC readers are NOT a profile knob. ShmSpmcRing puts payload at
// kShmSpmcPayloadOffset = 320 (five 64 B cells) and the eight drop slots fill
// the fifth cell exactly, so raising this needs that offset to move (its own
// page-layout change). Assert the single supported value instead of a range:
// a build that overrides it fails here with this message rather than at a
// confusing offsetof() assert inside the template.
static_assert(kAbiMaxSpmcReaders == 8,
              "ABI-13: SPMC reader ceiling is fixed at 8 by kShmSpmcPayloadOffset; "
              "raising it requires moving the payload offset (page-layout change)");

}  // namespace thyme
