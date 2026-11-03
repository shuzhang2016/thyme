// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/wiring.h"
#include "generated/record_registry.h"

#include "thyme/app/replay_mode.h"
#include "thyme/runtime/notif_pipe.h"

#include <cstring>

inline bool ReplayInjectOne(Pipeline& p, uint16_t struct_id,
                            const void* data, uint32_t size) {
  switch (struct_id) {
    case 0: {
      if (size != sizeof(thyme::Frame<Reading>)) return false;
      thyme::Frame<Reading> tmp{};
      std::memcpy(&tmp, data, size);
      return p.event_transports_.sink__s0_c0_pipe.PushOrWait(tmp, thyme::app::replay_mode::MakePushWaitGate());
    }
    case 1: {
      if (size != sizeof(thyme::Frame<Reading>)) return false;
      thyme::Frame<Reading> tmp{};
      std::memcpy(&tmp, data, size);
      return p.event_transports_.sink__s1_c1_pipe.PushOrWait(tmp, thyme::app::replay_mode::MakePushWaitGate());
    }
    case 2: {
      if (size != sizeof(thyme::Frame<Reading>)) return false;
      thyme::Frame<Reading> tmp{};
      std::memcpy(&tmp, data, size);
      return p.event_transports_.sink__s0_c2_pipe.PushOrWait(tmp, thyme::app::replay_mode::MakePushWaitGate());
    }
    case 3: {
      if (size != sizeof(thyme::Frame<Reading>)) return false;
      thyme::Frame<Reading> tmp{};
      std::memcpy(&tmp, data, size);
      return p.event_transports_.sink__s1_c3_pipe.PushOrWait(tmp, thyme::app::replay_mode::MakePushWaitGate());
    }
    default: return false;
  }
}
