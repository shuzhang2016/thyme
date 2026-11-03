#pragma once

#include "mp/mp_layout.h"

namespace record_trigger {

// Called from RunRole so mp worker threads can request triggers via supervisor relay.
void SetControl(MpControl* ctrl);

// Non-blocking; no-op if SetControl was not called (single-process mode).
void Request(uint32_t reason_code = 0);

MpControl* Control();

}  // namespace record_trigger
