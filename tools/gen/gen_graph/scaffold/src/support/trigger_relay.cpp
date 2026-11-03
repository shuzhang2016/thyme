#include "support/trigger_relay.h"

#include "thyme/record/record_control.h"
#include "thyme/runtime/log.h"

#include <chrono>
#include <cstdio>

namespace trigger_relay {
namespace {

uint64_t SteadyNowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

void Request(MpControl* ctrl, uint32_t reason_code) {
  if (ctrl == nullptr) return;
  ctrl->trigger_reason_code.store(reason_code, std::memory_order_release);
  ctrl->trigger_req_seq.fetch_add(1, std::memory_order_release);
}

bool PollAndRelay(MpControl* ctrl, const char* recorder_socket,
                  RelayState& state, uint64_t debounce_ns) {
  if (ctrl == nullptr || recorder_socket == nullptr ||
      recorder_socket[0] == '\0') {
    return false;
  }

  const uint32_t req =
      ctrl->trigger_req_seq.load(std::memory_order_acquire);
  if (req == 0 || req == state.last_relayed_req) return false;

  const uint64_t now = SteadyNowNs();
  if (now < state.send_backoff_until_ns) return false;
  if (state.last_relayed_req != 0 && debounce_ns > 0 &&
      now - state.last_relay_steady_ns < debounce_ns) {
    // Count each suppressed request once, not once per poll.
    if (state.last_debounced_req != req) {
      ctrl->trigger_relay_debounced.fetch_add(1, std::memory_order_relaxed);
      state.last_debounced_req = req;
    }
    return false;
  }

  if (!thyme::record_control::SendClientCommand(recorder_socket, "trigger")) {
    const uint64_t backoff = debounce_ns > 0 ? debounce_ns : 100000000ULL;
    state.send_backoff_until_ns = now + backoff;
    // OBS-08: per-relay-attempt path; rate-limit the record.
    THYME_LOG_WARN_RL_FIELDS(
        "trigger_relay: send failed",
        thyme::log::Fields().Str("socket", recorder_socket));
    return false;
  }

  state.send_backoff_until_ns = 0;

  ctrl->trigger_ack_seq.store(req, std::memory_order_release);
  ctrl->trigger_relay_sent.fetch_add(1, std::memory_order_relaxed);
  state.last_relayed_req = req;
  state.last_relay_steady_ns = now;
  // OBS-08: the former unconditional stdout print; now a rate-limited info
  // record (first 3, then every 1000th) with the request fields.
  THYME_LOG_INFO_RL_FIELDS(
      "trigger_relay: forwarded",
      thyme::log::Fields()
          .U64("req", req)
          .U64("reason",
               ctrl->trigger_reason_code.load(std::memory_order_acquire))
          .Str("socket", recorder_socket));
  return true;
}

}  // namespace trigger_relay
