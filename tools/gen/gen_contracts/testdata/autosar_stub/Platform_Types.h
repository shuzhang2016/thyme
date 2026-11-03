// Minimal Platform_Types.h stand-in for tools/gen/gen_contracts tests only.
//
// The real AUTOSAR header comes from the target RTE; these aliases have the
// same size/alignment as the primitives they stand for, which is all the
// generated layout asserts care about.
#pragma once

#include <cstdint>

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int8_t sint8;
typedef int16_t sint16;
typedef int32_t sint32;
typedef int64_t sint64;
typedef float float32;
typedef double float64;
typedef bool boolean;
