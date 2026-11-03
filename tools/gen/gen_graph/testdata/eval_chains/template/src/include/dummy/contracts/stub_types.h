#pragma once
#include "thyme/contracts/frame.h"

#include <cstdint>

// Minimal POD payload for the OBS-10 chain fixture.
struct Reading {
  uint32_t value;
};
