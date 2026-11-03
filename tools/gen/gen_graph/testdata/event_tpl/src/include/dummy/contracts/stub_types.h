#pragma once
#include "thyme/contracts/frame.h"

#include <cstddef>
#include <cstdint>

// Minimal POD contract for the event fixture. `Reading` is a bare payload
// struct; the transport layer wraps it in thyme::Frame<Reading> (frame.h).
struct Reading {
  uint32_t value;
};
