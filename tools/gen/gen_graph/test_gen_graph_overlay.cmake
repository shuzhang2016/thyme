# Direct test of the --template-dir overlay semantics: the effective template
# is base(scaffold) ∪ overlay, where the overlay shadows base files with the
# same relative path and may add new files. Without --template-dir the run is
# base-only and must produce a buildable project (see also test_empty_app).
if(NOT GEN OR NOT THYME OR NOT OVL_SRC)
  message(FATAL_ERROR "GEN, THYME and OVL_SRC required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(OVL "${CMAKE_BINARY_DIR}/overlay_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/overlay_app")
set(ROOT_BASE "${CMAKE_BINARY_DIR}/overlay_base_app")
file(REMOVE_RECURSE "${OVL}" "${ROOT}" "${ROOT_BASE}")
file(COPY "${OVL_SRC}/" DESTINATION "${OVL}")

function(expect_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "overlay: ${file} missing expected content: ${needle}")
  endif()
endfunction()

# --- overlay run: base ∪ overlay ---
execute_process(COMMAND "${GEN}" --out "${ROOT}" --update all
  --template-dir "${OVL}" --no-health-reporter RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph --template-dir (overlay) failed: ${rc}")
endif()

# overlay banner.txt replaces the base one (banner flows into generated output)
expect_contains("${ROOT}/generated/graph_ir.h" "Code generated for the overlay banner.")
# overlay vocabulary (extras.txt) is read by the emit path
expect_contains("${ROOT}/generated/wiring.h" "MyCustomBuffer mybuf;")
# overlay src/main.cpp replaces the base scaffold main
expect_contains("${ROOT}/src/main.cpp" "OVERLAY_MAIN_MARKER")
# overlay-added files are scaffolded into the project
if(NOT EXISTS "${ROOT}/src/support/overlay_add.h")
  message(FATAL_ERROR "overlay: overlay-added src/support/overlay_add.h not scaffolded")
endif()

# The merged project (base ∪ overlay) must configure, build and run.
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT}" -B "${ROOT}/build"
  -DTHYME_SOURCE_DIR=${THYME_ABS} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "overlay app configure failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${ROOT}/build" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "overlay app build failed")
endif()
execute_process(COMMAND "${ROOT}/build/app" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "overlay app exited ${rc}")
endif()

# --- base-only run: scaffold without --template-dir ---
execute_process(COMMAND "${GEN}" --out "${ROOT_BASE}" --update all
  --no-health-reporter RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph base-only failed: ${rc}")
endif()
# base banner, no overlay vocabulary
expect_contains("${ROOT_BASE}/generated/graph_ir.h"
  "Code generated for the generated graph.")
file(READ "${ROOT_BASE}/generated/wiring.h" base_wiring)
string(FIND "${base_wiring}" "MyCustomBuffer" pos)
if(NOT pos EQUAL -1)
  message(FATAL_ERROR "overlay: base-only run leaked overlay vocabulary")
endif()
# base scaffold main (not the overlay one): the full-CLI main prints the
# "ad_stack v1" banner; the smoke-main loop_count() expression is gone.
expect_contains("${ROOT_BASE}/src/main.cpp" "ad_stack v1")
if(EXISTS "${ROOT_BASE}/src/support/overlay_add.h")
  message(FATAL_ERROR "overlay: base-only run scaffolded overlay-added file")
endif()
