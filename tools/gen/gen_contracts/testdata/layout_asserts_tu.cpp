// ABI-03 compile gate for generated contract layout asserts.
//
// This TU is compiled by tools/gen/gen_contracts/test_layout_asserts.cmake against
// a freshly generated (never checked-in) header tree. The generated
// static_asserts are the real gate: if the Python layout model and the C++
// compiler's natural-alignment rules disagree, this include fails to compile.
// The asserts here are an independent smoke test of the shared fixture's
// expected numbers, so a silently wrong generator model is caught twice.
#include "lsl/contracts/thyme_types.h"

#include <cstddef>

// 1-field contract.
static_assert(sizeof(OneField_t) == 8, "OneField_t size");
static_assert(alignof(OneField_t) == 8, "OneField_t alignment");

// Padding: uint8 @0, uint64 @8, uint16 @16, uint8 @18, tail padding -> 24.
static_assert(sizeof(Padded_t) == 24, "Padded_t size");
static_assert(offsetof(Padded_t, b) == 8, "Padded_t.b offset");
static_assert(offsetof(Padded_t, c) == 16, "Padded_t.c offset");
static_assert(offsetof(Padded_t, d) == 18, "Padded_t.d offset");

// Nested-struct stride includes the element's own trailing padding.
static_assert(sizeof(Elem_t) == 8, "Elem_t size (trailing padding)");
static_assert(sizeof(Arrays_t::elems[0]) == sizeof(Elem_t), "array stride == sizeof");
static_assert(sizeof(Arrays_t) == 32, "Arrays_t size");

int main() { return 0; }
