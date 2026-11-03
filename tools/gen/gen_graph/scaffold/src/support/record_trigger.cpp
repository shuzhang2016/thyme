#include "support/record_trigger.h"

#include "support/trigger_relay.h"

#include <atomic>

namespace record_trigger {
namespace {

// Set by RunRole during startup while worker threads may already call
// Request/Control — must be atomic, not a bare global.
std::atomic<MpControl*> g_ctrl{nullptr};

}  // namespace

void SetControl(MpControl* ctrl) {
  g_ctrl.store(ctrl, std::memory_order_release);
}

void Request(uint32_t reason_code) {
  trigger_relay::Request(g_ctrl.load(std::memory_order_acquire), reason_code);
}

MpControl* Control() { return g_ctrl.load(std::memory_order_acquire); }

}  // namespace record_trigger
