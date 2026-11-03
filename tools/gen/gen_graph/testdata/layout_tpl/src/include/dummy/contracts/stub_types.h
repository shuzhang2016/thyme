#pragma once
#include "thyme/contracts/frame.h"

#include <cstddef>
#include <cstdint>

// Minimal POD contract for the GEN-08 layout fixture. `Reading` is a bare
// payload struct; the transport layer wraps it in thyme::Frame<Reading>.
// contract_sizes.txt pins its schema size at 4 bytes.
struct Reading {
  uint32_t value;
};
