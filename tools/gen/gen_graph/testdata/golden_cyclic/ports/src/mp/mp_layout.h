#pragma once
#include "thyme/app/mp_control.h"
#include "thyme/runtime/health_slot.h"
#include "generated/graph_ir.h"
#include <cstddef>

// The shared control page is thyme::app::MpControl (created by
// thyme::app::MpSupervisor, attached by thyme::app::MpRoleSession); this alias
// keeps the support-side API (record_trigger/trigger_relay) unqualified.
using MpControl = thyme::app::MpControl;

inline constexpr std::size_t kMpHealthN = static_cast<std::size_t>(kGraphHealthN);
