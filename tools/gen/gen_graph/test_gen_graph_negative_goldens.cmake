# GEN-07 — negative goldens for gen_graph inputs.
#
# Every row in testdata/negative/cases.txt names a malformed graph, the exit
# code, the yaml line the diagnostic must point at, and a diagnostic
# substring. For each fixture this harness asserts, in generation and --check
# modes:
#   * exit code is exactly the expected one (2 — never 0, never a signal),
#   * stdout+stderr contains the expected diagnostic substring,
#   * stdout+stderr contains "<absolute fixture path>:<line>:" when a line is
#     pinned (graph-level diagnostics use "-"),
#   * the run is deterministic: a second run produces byte-identical stderr,
#   * no partial output: no generated/ tree and no staging dirs are left in
#     the output directory.
# It also fails when a *.yaml fixture has no row, so the net cannot silently
# shrink. Run by ctest as test_gen_graph_negative_goldens (mr_gate).
if(NOT GEN)
  message(FATAL_ERROR "GEN required")
endif()
get_filename_component(GEN "${GEN}" ABSOLUTE)
set(CASE_DIR "${CMAKE_CURRENT_LIST_DIR}/testdata/negative")
set(CASES "${CASE_DIR}/cases.txt")
if(NOT EXISTS "${CASES}")
  message(FATAL_ERROR "missing ${CASES}")
endif()

set(ROOT "${CMAKE_BINARY_DIR}/gen_graph_negative_goldens")
file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}")

file(GLOB FIXTURES RELATIVE "${CASE_DIR}" "${CASE_DIR}/*.yaml")
list(SORT FIXTURES)

file(STRINGS "${CASES}" ROWS)
set(DECLARED "")
set(COUNT 0)
foreach(row IN LISTS ROWS)
  string(STRIP "${row}" row)
  if(row STREQUAL "" OR row MATCHES "^#")
    continue()
  endif()
  string(REPLACE "|" ";" fields "${row}")
  list(LENGTH fields field_count)
  if(NOT field_count EQUAL 4)
    message(FATAL_ERROR "malformed cases.txt row (want name|exit|line|needle): ${row}")
  endif()
  list(GET fields 0 name)
  list(GET fields 1 expected_rc)
  list(GET fields 2 expected_line)
  list(GET fields 3 needle)
  string(STRIP "${name}" name)
  string(STRIP "${expected_rc}" expected_rc)
  string(STRIP "${expected_line}" expected_line)
  string(STRIP "${needle}" needle)

  list(FIND DECLARED "${name}" already)
  if(NOT already EQUAL -1)
    message(FATAL_ERROR "duplicate case '${name}' in cases.txt")
  endif()
  list(APPEND DECLARED "${name}")

  set(yaml "${CASE_DIR}/${name}.yaml")
  if(NOT EXISTS "${yaml}")
    message(FATAL_ERROR "case '${name}' has no fixture ${yaml}")
  endif()

  set(app "${ROOT}/${name}")
  execute_process(
    COMMAND "${GEN}" --in "${yaml}" --out "${app}" --update generated
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT "${rc}" STREQUAL "${expected_rc}")
    message(FATAL_ERROR
      "${name}: exit ${rc}, expected ${expected_rc}\n${out}${err}")
  endif()
  set(combined "${out}${err}")
  string(FIND "${combined}" "${needle}" needle_pos)
  if(needle_pos EQUAL -1)
    message(FATAL_ERROR
      "${name}: diagnostic does not mention '${needle}':\n${combined}")
  endif()
  if(NOT expected_line STREQUAL "-")
    string(FIND "${combined}" "${yaml}:${expected_line}:" line_pos)
    if(line_pos EQUAL -1)
      message(FATAL_ERROR
        "${name}: diagnostic does not point at ${yaml}:${expected_line}:\n${combined}")
    endif()
  endif()

  # No partial output and no staging leftovers on the failure path.
  if(EXISTS "${app}/generated")
    message(FATAL_ERROR "${name}: rejected graph left generated/ behind")
  endif()
  file(GLOB leftovers "${app}/.generated.*" "${app}/.gen_graph_tmp*"
       "${app}/.gen_graph_tpl")
  if(leftovers)
    message(FATAL_ERROR "${name}: staging dirs left behind: ${leftovers}")
  endif()

  # Determinism: a second run must emit byte-identical diagnostics.
  execute_process(
    COMMAND "${GEN}" --in "${yaml}" --out "${app}.rerun" --update generated
    RESULT_VARIABLE rc2 OUTPUT_VARIABLE out2 ERROR_VARIABLE err2)
  if(NOT "${rc2}" STREQUAL "${expected_rc}")
    message(FATAL_ERROR "${name}: rerun exit ${rc2}, expected ${expected_rc}")
  endif()
  if(NOT "${err}" STREQUAL "${err2}")
    message(FATAL_ERROR
      "${name}: diagnostics are not deterministic:\n--- first ---\n${err}\n--- second ---\n${err2}")
  endif()

  # --check uses the same loader and must reject before creating a tree.
  execute_process(
    COMMAND "${GEN}" --check --in "${yaml}" --out "${app}.check"
    RESULT_VARIABLE check_rc OUTPUT_VARIABLE check_out ERROR_VARIABLE check_err)
  if(NOT "${check_rc}" STREQUAL "${expected_rc}")
    message(FATAL_ERROR
      "${name}: --check exit ${check_rc}, expected ${expected_rc}\n${check_out}${check_err}")
  endif()
  if(NOT "${check_err}" STREQUAL "${err}")
    message(FATAL_ERROR
      "${name}: --check diagnostic differs from generation:\n--- generation ---\n${err}\n--- check ---\n${check_err}")
  endif()
  if(EXISTS "${app}.check/generated")
    message(FATAL_ERROR "${name}: --check rejected graph left generated/ behind")
  endif()
  file(GLOB check_leftovers "${app}.check/.generated.*"
       "${app}.check/.gen_graph_tmp*" "${app}.check/.gen_graph_tpl")
  if(check_leftovers)
    message(FATAL_ERROR
      "${name}: --check staging dirs left behind: ${check_leftovers}")
  endif()

  math(EXPR COUNT "${COUNT} + 1")
endforeach()

foreach(fixture IN LISTS FIXTURES)
  string(REGEX REPLACE "\\.yaml$" "" stem "${fixture}")
  list(FIND DECLARED "${stem}" index)
  if(index EQUAL -1)
    message(FATAL_ERROR "fixture ${fixture} has no row in testdata/negative/cases.txt")
  endif()
endforeach()
if(COUNT EQUAL 0)
  message(FATAL_ERROR "cases.txt declared no cases")
endif()

message(STATUS "gen_graph negative goldens: ${COUNT} cases OK")
