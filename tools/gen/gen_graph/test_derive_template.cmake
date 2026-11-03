# Golden + behavior tests for gen_graph --derive-template (Group 1 derivator).
# Locks the byte-exact derived catalog tables / fragments and the no-force /
# force / single-process / diverging-signature behaviors, plus a round-trip
# dogfood that regenerates against the derived overlay.
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(SRC "${THYME_ABS}/tools/gen/gen_graph/testdata/derive_tpl")
set(EXP "${SRC}/expected")
set(OUT "${CMAKE_BINARY_DIR}/derive_tpl_out")
file(REMOVE_RECURSE "${OUT}")

# --- golden: derive and byte-compare each expected file ---
execute_process(COMMAND "${GEN}" --in "${SRC}/graph.yaml"
  --template-out "${OUT}" --derive-template RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph --derive-template (fixture) failed: ${rc}")
endif()

foreach(f pools.txt contracts.txt known_types.txt channel_defaults.txt
         shm_kinds.txt graph_types.h.inc mp_includes.inc)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
    "${EXP}/${f}" "${OUT}/${f}" RESULT_VARIABLE cmp)
  if(NOT cmp EQUAL 0)
    message(FATAL_ERROR "derive-template golden drift in ${f}")
  endif()
endforeach()

# banner.txt only with --project-name; scheduling_rings.inc never (PLAN-3).
if(EXISTS "${OUT}/banner.txt")
  message(FATAL_ERROR "derive-template emitted banner.txt without --project-name")
endif()
if(EXISTS "${OUT}/scheduling_rings.inc")
  message(FATAL_ERROR "derive-template emitted scheduling_rings.inc (PLAN-3: never)")
endif()
if(EXISTS "${OUT}/contracts_include.txt")
  message(FATAL_ERROR "derive-template emitted contracts_include.txt (phase 02 owns)")
endif()

# exec=event type must be absent from known_types.txt.
file(READ "${OUT}/known_types.txt" kn)
if(kn MATCHES "relay")
  message(FATAL_ERROR "derive-template: exec=event type leaked into known_types.txt")
endif()
if(NOT kn MATCHES "health_reporter\\|HealthReporterThread\\|reporter\\|HEALTH\\|noworker")
  message(FATAL_ERROR "derive-template: static health_reporter row missing")
endif()

# --- behavior: no --force keeps existing files; --force overwrites ---
set(OUT2 "${CMAKE_BINARY_DIR}/derive_tpl_keep")
file(REMOVE_RECURSE "${OUT2}")
file(MAKE_DIRECTORY "${OUT2}")
file(WRITE "${OUT2}/pools.txt" "sentinel\n")
execute_process(COMMAND "${GEN}" --in "${SRC}/graph.yaml"
  --template-out "${OUT2}" --derive-template RESULT_VARIABLE rc
  OUTPUT_VARIABLE out_text)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph --derive-template (keep) failed: ${rc}")
endif()
file(READ "${OUT2}/pools.txt" kept_body)
if(NOT kept_body STREQUAL "sentinel\n")
  message(FATAL_ERROR "derive-template overwrote existing pools.txt without --force")
endif()
if(NOT out_text MATCHES "kept .*pools\\.txt")
  message(FATAL_ERROR "derive-template did not report 'kept pools.txt'")
endif()

execute_process(COMMAND "${GEN}" --in "${SRC}/graph.yaml"
  --template-out "${OUT2}" --derive-template --force RESULT_VARIABLE rc
  OUTPUT_VARIABLE out_force)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph --derive-template (force) failed: ${rc}")
endif()
file(READ "${OUT2}/pools.txt" forced_body)
if(forced_body STREQUAL "sentinel\n")
  message(FATAL_ERROR "derive-template --force did not overwrite pools.txt")
endif()
if(NOT out_force MATCHES "wrote .*pools\\.txt")
  message(FATAL_ERROR "derive-template did not report 'wrote pools.txt'")
endif()

# --- behavior: --project-name emits banner.txt ---
set(OUT3 "${CMAKE_BINARY_DIR}/derive_tpl_banner")
file(REMOVE_RECURSE "${OUT3}")
execute_process(COMMAND "${GEN}" --in "${SRC}/graph.yaml"
  --template-out "${OUT3}" --derive-template --project-name "my_stack"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph --derive-template (banner) failed: ${rc}")
endif()
file(READ "${OUT3}/banner.txt" banner_body)
if(NOT banner_body STREQUAL "my_stack\n")
  message(FATAL_ERROR "derive-template banner.txt content mismatch")
endif()
file(READ "${OUT3}/graph_types.h.inc" gtb)
if(NOT gtb MATCHES "Code generated for the my_stack graph\\. DO NOT EDIT\\.")
  message(FATAL_ERROR "derive-template banner not reflected in graph_types.h.inc")
endif()

# --- behavior: single-process graph emits no shm_kinds.txt ---
set(SP_YAML "${CMAKE_BINARY_DIR}/derive_sp_graph.yaml")
file(WRITE "${SP_YAML}"
  "version: 1\npool_n: 4\nring_cap: 8\n"
  "nodes:\n"
  "  - id: a\n    type: src\n    hz: 10\n    process: main\n"
  "  - id: b\n    type: dst\n    hz: 10\n    process: main\n"
  "edges:\n"
  "  - from: a\n    to: b\n    channel: ch\n    cpp_type: Foo\n    pool: FooPool\n    transport: spsc\n"
  "processes:\n"
  "  - id: main\n    role: main\n"
  "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 2\n")
set(OUT4 "${CMAKE_BINARY_DIR}/derive_tpl_sp")
file(REMOVE_RECURSE "${OUT4}")
execute_process(COMMAND "${GEN}" --in "${SP_YAML}"
  --template-out "${OUT4}" --derive-template RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph --derive-template (single-process) failed: ${rc}")
endif()
if(EXISTS "${OUT4}/shm_kinds.txt")
  message(FATAL_ERROR "derive-template emitted shm_kinds.txt for a single-process graph")
endif()

# --- behavior: two cyclic nodes of one type with diverging edges -> exit 2 ---
set(DIV_YAML "${CMAKE_BINARY_DIR}/derive_div_graph.yaml")
file(WRITE "${DIV_YAML}"
  "version: 1\npool_n: 4\nring_cap: 8\n"
  "nodes:\n"
  "  - id: w1\n    type: worker\n    hz: 10\n    process: main\n"
  "  - id: w2\n    type: worker\n    hz: 10\n    process: main\n"
  "  - id: s1\n    type: sink\n    hz: 10\n    process: main\n"
  "  - id: s2\n    type: sink2\n    hz: 10\n    process: main\n"
  "edges:\n"
  "  - from: w1\n    to: s1\n    channel: c1\n    cpp_type: Foo\n    pool: FooPool\n    transport: spsc\n"
  "  - from: w2\n    to: s2\n    channel: c2\n    cpp_type: Bar\n    pool: BarPool\n    transport: spsc\n"
  "processes:\n"
  "  - id: main\n    role: main\n"
  "supervisor:\n  shm_prefix: \"/c9_%pid\"\n  health_n: 4\n")
set(OUT5 "${CMAKE_BINARY_DIR}/derive_tpl_div")
file(REMOVE_RECURSE "${OUT5}")
execute_process(COMMAND "${GEN}" --in "${DIV_YAML}"
  --template-out "${OUT5}" --derive-template RESULT_VARIABLE rc
  ERROR_VARIABLE div_err)
if(rc EQUAL 0)
  message(FATAL_ERROR "derive-template did not reject diverging pooled-edge signatures")
endif()
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "derive-template diverging signature exited ${rc}, expected 2")
endif()

# --- behavior: --derive-template with --update is rejected (exit 2) ---
execute_process(COMMAND "${GEN}" --in "${SRC}/graph.yaml"
  --template-out "${OUT}" --derive-template --update generated
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "--derive-template + --update did not exit 2")
endif()

# --- round-trip dogfood: derive -> --update generated against the overlay ---
# health_reporter_generated.h.inc ships in the base scaffold (phase 01), so
# the derived overlay needs no health_reporter .inc of its own.
set(DOG_TPL "${CMAKE_BINARY_DIR}/derive_tpl_dog")
set(DOG_APP "${CMAKE_BINARY_DIR}/derive_tpl_dog_app")
file(REMOVE_RECURSE "${DOG_TPL}" "${DOG_APP}")
execute_process(COMMAND "${GEN}" --in "${SRC}/graph.yaml"
  --template-out "${DOG_TPL}" --derive-template RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "dogfood derive failed: ${rc}")
endif()
# The derived template is paired with a gen_contracts output in a real
# project; the dogfood overlay only emits headers, so declare the include path
# that supplies the ABI-11 LatestSlot payload identity.
file(WRITE "${DOG_TPL}/contracts_layout.txt"
  "dummy/contracts/contract_layout.h\n")
execute_process(COMMAND "${GEN}" --in "${SRC}/graph.yaml" --out "${DOG_APP}"
  --template-dir "${DOG_TPL}" --update generated RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "dogfood --update generated failed: ${rc}")
endif()
file(READ "${DOG_APP}/generated/graph_types.h" gt)
if(NOT gt MATCHES "using FooPool = thyme::ObjectPool<thyme::Frame<Foo>, kPoolN>;")
  message(FATAL_ERROR "dogfood: FooPool alias missing from graph_types.h")
endif()
if(NOT gt MATCHES "using BazPool = thyme::ObjectPool<thyme::Frame<Baz>, kPoolN>;")
  message(FATAL_ERROR "dogfood: BazPool alias missing from graph_types.h")
endif()

message(STATUS "derive-template golden + behavior OK")
