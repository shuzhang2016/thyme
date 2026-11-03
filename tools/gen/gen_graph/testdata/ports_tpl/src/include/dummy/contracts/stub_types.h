#pragma once
#include "thyme/contracts/frame.h"

#include <cstddef>
#include <cstdint>

// Minimal POD contract for the generated-app test fixture. `Foo` is a bare
// payload struct; the transport layer wraps it in thyme::Frame<Foo> (frame.h),
// so the wire image is FrameHeader + Foo and the record layer records the whole
// Frame<Foo> object (sizeof), never hdr.size.
struct Foo {
  uint32_t value;
};
