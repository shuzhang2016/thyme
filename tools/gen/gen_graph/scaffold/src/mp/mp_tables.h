#pragma once

// Shared generated-table -> thyme::app descriptor mapping for the
// cross-process SHM channels. Both the supervisor adapter
// (mp_supervisor_adapter.cpp) and the role side (mp_roles.cpp) derive their
// channel list and per-kind slot sizes from here, so the two sides can never
// disagree on kinds, sizes, or endpoints. Sizes and kind indexes come from the
// generated graph IR (kGraphShmSlotSizes / GraphShmKind); nothing here is
// hand-written per graph.

#include "generated/graph_ir.h"
#include "generated/shm_slots.h"
#include "thyme/app/mp_supervisor.h"
#include "thyme/app/span.h"

#include <array>
#include <cstddef>
#include <cstring>

// LatestSlot rows only (delivery=latest). Queue rows share kGraphShmChannels
// with delivery=queue and are prepared via PrepareEventShmChannels.
inline thyme::span<const thyme::app::MpShmChannel> MpShmChannels() {
  static const auto holder = [] {
    struct H {
      std::array<thyme::app::MpShmChannel,
                 sizeof(kGraphShmChannels) / sizeof(kGraphShmChannels[0])>
          a{};
      std::size_t n = 0;
    } h;
    for (const auto& ch : kGraphShmChannels) {
      if (std::strcmp(ch.delivery, "latest") != 0) continue;
      h.a[h.n++] = {ch.suffix, static_cast<int>(ch.kind), ch.from_process,
                    ch.to_process};
    }
    return h;
  }();
  return {holder.a.data(), holder.n};
}

// Per-kind described LatestSlot page descriptors indexed by GraphShmKind
// value, generated from shm_kinds.txt + shm edges (see kGraphShmKindSpecs in
// shm_slots.h). Both the supervisor and the role read this one table.
inline thyme::span<const thyme::app::MpShmKindSpec> MpSlotSpecs() {
  return {kGraphShmKindSpecs, kGraphShmKindCount};
}
