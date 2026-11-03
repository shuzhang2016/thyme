#pragma once
#include "thyme/contracts/frame.h"

#include <cstddef>
#include <cstdint>

// Minimal POD contract for the broadcast fixture. `Tick` is a bare payload
// struct; the transport layer wraps it in thyme::Frame<Tick> (frame.h).
struct Tick {
  uint32_t value;
};
