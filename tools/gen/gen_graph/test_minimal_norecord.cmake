# Proof fixture for conditional codegen (L8): a single tick node with no
# edges. The generated/ dir must contain only the always-emitted artifacts —
# wiring, *_generated, graph_ir, graph_check, nodes.cmake, graph_types,
# integration.md, shm_slots, and the scheduling files — and must LACK every
# record/replay/record-check artifact (they are only emitted when the graph
# has >= 1 recorded channel). The zero-entry record manifest is the one
# always-emitted record artifact (GEN-12).
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${THYME_ABS}/tools/gen/gen_graph/testdata/minimal_norecord")
set(ROOT "${CMAKE_BINARY_DIR}/minimal_norecord")
file(REMOVE_RECURSE "${ROOT}")

execute_process(COMMAND "${GEN}" --out "${ROOT}" --update generated
  --template-dir "${TPL}" --in "${TPL}/graph.yaml" --no-health-reporter
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (minimal_norecord) failed: ${rc}")
endif()

function(expect_absent file)
  if(EXISTS "${file}")
    message(FATAL_ERROR "minimal_norecord: unexpected generated file ${file}")
  endif()
endfunction()

expect_absent("${ROOT}/generated/record_registry.h")
expect_absent("${ROOT}/generated/record_table.h")
expect_absent("${ROOT}/generated/record_tap_helpers.h")
expect_absent("${ROOT}/generated/graph_record_check.h")
expect_absent("${ROOT}/generated/replay_inject.h")
expect_absent("${ROOT}/generated/replay_stages.h")

# GEN-12: the manifest itself is always emitted (count 0) so --check catches
# recorded-set drift even when nothing is recordable.
if(NOT EXISTS "${ROOT}/generated/record_manifest.h")
  message(FATAL_ERROR "minimal_norecord: record_manifest.h must be emitted")
endif()
file(READ "${ROOT}/generated/record_manifest.h" manifest_body)
string(FIND "${manifest_body}" "kRecordManifestCount = 0;" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "minimal_norecord: record_manifest.h must be empty")
endif()

# Inverse guard: the always-emitted baseline must be present, so the absence
# check above cannot pass against a gen_graph that emitted nothing at all.
if(NOT EXISTS "${ROOT}/generated/wiring.h" OR
   NOT EXISTS "${ROOT}/generated/wiring.cpp" OR
   NOT EXISTS "${ROOT}/generated/graph_ir.h" OR
   NOT EXISTS "${ROOT}/generated/graph_check.h" OR
   NOT EXISTS "${ROOT}/generated/nodes.cmake" OR
   NOT EXISTS "${ROOT}/generated/graph_types.h" OR
   NOT EXISTS "${ROOT}/generated/integration.md" OR
   NOT EXISTS "${ROOT}/generated/shm_slots.h" OR
   NOT EXISTS "${ROOT}/generated/tick_generated.h")
  message(FATAL_ERROR "minimal_norecord: expected baseline generated files missing")
endif()
