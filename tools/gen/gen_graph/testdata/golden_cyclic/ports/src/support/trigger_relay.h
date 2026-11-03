#pragma once

#include "mp/mp_layout.h"

#include <cstdint>

namespace trigger_relay {

inline constexpr uint64_t kDefaultDebounceNs = 1000000000ULL;

// Module: request a recorder trigger (supervisor relays; never call recorder directly).
void Request(MpControl* ctrl, uint32_t reason_code = 0);

// Explicit per-relay-loop state (replaces thread_locals so independent
// PollAndRelay loops do not share hidden state).
struct RelayState {
  uint32_t last_relayed_req = 0;
  uint32_t last_debounced_req = 0;
  uint64_t last_relay_steady_ns = 0;
  uint64_t send_backoff_until_ns = 0;
};

// Supervisor: forward pending requests to recorder Unix socket (same as thyme-recorderctl).
// Returns true when a "trigger" datagram was sent. Each suppressed request is
// counted in ctrl->trigger_relay_debounced exactly once.
bool PollAndRelay(MpControl* ctrl, const char* recorder_socket,
                  RelayState& state,
                  uint64_t debounce_ns = kDefaultDebounceNs);

}  // namespace trigger_relay
