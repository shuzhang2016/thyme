# GEN-08: generated code emits layout/size static_asserts.
#
# 1. Generate the layout fixture (latest SHM slot, queue ring, broadcast ring,
#    depth override, schema sizes) and grep one assertion per class.
# 2. Compile the generated check headers standalone under the strict warning
#    gate, so every emitted static_assert is evaluated by the real compiler.
# 3. A schema-known contract above the 64 KB Frame limit routes to PinnedSlot;
#    a max_frame_bytes smaller than that fixed contract is still rejected.
#
# Fast and deterministic (no nested app build) -> mr_gate.
if(NOT GEN OR NOT THYME OR NOT CXX)
  message(FATAL_ERROR "GEN, THYME and CXX required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${THYME_ABS}/tools/gen/gen_graph/testdata/layout_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/gen_graph_layout_asserts")
file(REMOVE_RECURSE "${ROOT}")

execute_process(COMMAND "${GEN}" --out "${ROOT}" --update generated
  --template-dir "${TPL}" --in "${TPL}/graph.yaml" --no-health-reporter
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (layout fixture) failed: ${rc}")
endif()

function(expect_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "layout fixture: ${file} missing: ${needle}")
  endif()
endfunction()

# Slot table: model-derived region size + index/value drift + transport ceiling.
expect_contains("${ROOT}/generated/shm_slots.h" "region size must come from LatestSlotLayoutModel")
expect_contains("${ROOT}/generated/shm_slots.h" "must index kGraphShmSlotSizes")
expect_contains("${ROOT}/generated/shm_slots.h" "latest page needs a non-zero generated contract layout identity")
expect_contains("${ROOT}/generated/shm_slots.h" "64 KB Frame transport limit")
# Per-channel transport + schema size + power-of-two depths/caps.
expect_contains("${ROOT}/generated/graph_check.h" "framed wire bytes exceed the 64 KB transport limit")
expect_contains("${ROOT}/generated/graph_check.h" "static_assert(sizeof(Reading) == 4u")
expect_contains("${ROOT}/generated/graph_check.h" "ring_cap must be a power of two")
expect_contains("${ROOT}/generated/graph_check.h" "Cap must be a power of two")
expect_contains("${ROOT}/generated/graph_check.h" "depth must be a power of two")
expect_contains("${ROOT}/generated/graph_check.h" "SPSC page must be 5 header cells")
# Broadcast region geometry.
expect_contains("${ROOT}/generated/graph_ir.h" "cap must be a power of two")
expect_contains("${ROOT}/generated/graph_ir.h" "SPMC page must be 5 header cells")
# Record path ceilings.
expect_contains("${ROOT}/generated/record_registry.h" "exceeds kAbiMaxStructCount")
expect_contains("${ROOT}/generated/record_registry.h" "512 KB record payload ceiling")

# Compile the generated headers standalone: a static_assert drift in any of
# these classes fails this step with the compiler's message.
file(WRITE "${ROOT}/layout_tu.cpp" [==[#include "generated/shm_slots.h"
#include "generated/graph_check.h"
#include "generated/record_registry.h"
#include "generated/mp_role_locals.h"
int main() { return 0; }
]==])
execute_process(
  COMMAND "${CXX}" -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
          -Werror -I "${ROOT}" -I "${TPL}/src/include" -I "${THYME_ABS}/include"
          -I "${THYME_ABS}/tools/gen/gen_graph/scaffold/src"
          -c "${ROOT}/layout_tu.cpp" -o "${ROOT}/layout_tu.o"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "generated layout asserts do not compile:\n${out}${err}")
endif()

# ARCH-19: 65500 payload bytes exceed the Frame limit and route to PinnedSlot.
set(BIG_TPL "${ROOT}/big_tpl")
set(BIG_OUT "${ROOT}/big_out")
file(COPY "${TPL}/" DESTINATION "${BIG_TPL}")
file(WRITE "${BIG_TPL}/contract_sizes.txt" "Reading|65500\n")
file(READ "${TPL}/graph.yaml" big_graph)
string(REPLACE "cpp_type: Reading" "cpp_type: Reading\n    record: false" big_graph "${big_graph}")
file(WRITE "${BIG_TPL}/graph.yaml" "${big_graph}")
execute_process(COMMAND "${GEN}" --out "${BIG_OUT}" --update generated
  --template-dir "${BIG_TPL}" --in "${BIG_TPL}/graph.yaml" --no-health-reporter
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "oversized PinnedSlot route failed: ${rc}\n${out}${err}")
endif()
expect_contains("${BIG_OUT}/generated/channel_capabilities.json"
  "\"transport_kind\": \"pinned_slot\"")
expect_contains("${BIG_OUT}/generated/channel_capabilities.json"
  "\"max_wire_bytes\": 65500")

# A declared wire budget below the fixed contract remains a hard error.
set(BAD_TPL "${ROOT}/bad_tpl")
set(BAD_OUT "${ROOT}/bad_out")
file(COPY "${BIG_TPL}/" DESTINATION "${BAD_TPL}")
string(REPLACE "record: false" "record: false\n    max_frame_bytes: 64000" bad_graph "${big_graph}")
file(WRITE "${BAD_TPL}/graph.yaml" "${bad_graph}")
execute_process(COMMAND "${GEN}" --out "${BAD_OUT}" --update generated
  --template-dir "${BAD_TPL}" --in "${BAD_TPL}/graph.yaml" --no-health-reporter
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "undersized PinnedSlot budget: exit ${rc}, expected 2\n${out}${err}")
endif()
string(FIND "${err}" "max_frame_bytes is smaller than the fixed contract size" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "undersized PinnedSlot budget diagnostic missing:\n${err}")
endif()
if(EXISTS "${BAD_OUT}/generated")
  message(FATAL_ERROR "undersized PinnedSlot budget left generated/ behind")
endif()
execute_process(COMMAND "${GEN}" --out "${BAD_OUT}.rerun" --update generated
  --template-dir "${BAD_TPL}" --in "${BAD_TPL}/graph.yaml" --no-health-reporter
  RESULT_VARIABLE rc2 OUTPUT_VARIABLE out2 ERROR_VARIABLE err2)
if(NOT rc2 EQUAL 2 OR NOT "${err}" STREQUAL "${err2}")
  message(FATAL_ERROR "PinnedSlot rejection is not deterministic:\n${err}\n${err2}")
endif()

message(STATUS "GEN-08 generated layout asserts OK")
