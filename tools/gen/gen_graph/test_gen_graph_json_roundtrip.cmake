# ECO-08: graph.yaml <-> canonical JSON round-trip.
#
# The canonical JSON is the editor-facing representation of the *effective*
# graph (delivery defaults applied; yaml line numbers dropped). The test:
#   1. yaml --emit-json -> JSON, --from-json --emit-yaml -> yaml,
#      yaml --emit-json -> JSON; the two JSON documents are byte-identical.
#   2. generating from the original yaml and from the JSON produces the same
#      generated/ tree (`--check` from either input is green on it).
#   3. `--check` output is identical whether driven by yaml or JSON.
#   4. unknown/duplicate keys and wrong value types are rejected (exit 2).
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${THYME_ABS}/tools/gen/gen_graph/testdata/ports_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/gen_graph_json_roundtrip")
file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}")

function(run_ok out_var)
  execute_process(COMMAND ${ARGN}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "command failed (${rc}): ${ARGN}\n${out}${err}")
  endif()
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

# ---- 1. canonical JSON is a fixed point --------------------------------
run_ok(json1 "${GEN}" --in "${TPL}/graph.yaml" --emit-json)
file(WRITE "${ROOT}/graph.json" "${json1}")
run_ok(yaml1 "${GEN}" --from-json "${ROOT}/graph.json" --emit-yaml)
file(WRITE "${ROOT}/graph.yaml" "${yaml1}")
run_ok(json2 "${GEN}" --in "${ROOT}/graph.yaml" --emit-json)
if(NOT json1 STREQUAL json2)
  message(FATAL_ERROR
    "yaml -> JSON -> yaml -> JSON is not a fixed point:\n${json1}\n---\n${json2}")
endif()
# The effective delivery is materialised (an editor does not have to re-derive
# it) and diagnostics-only fields never leak into the contract.
foreach(needle "\"delivery\": \"latest\"" "\"shm_prefix\": \"/c9_%pid\"")
  string(FIND "${json1}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "canonical JSON missing '${needle}':\n${json1}")
  endif()
endforeach()
foreach(needle "\"line\"" "\"source\"")
  string(FIND "${json1}" "${needle}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "canonical JSON leaked diagnostics field ${needle}:\n${json1}")
  endif()
endforeach()

# ---- 2. generation from JSON matches generation from yaml ---------------
set(APP "${ROOT}/app")
run_ok(ignored "${GEN}" --in "${TPL}/graph.yaml" --out "${APP}"
    --update generated --template-dir "${TPL}")
execute_process(COMMAND "${GEN}" --from-json "${ROOT}/graph.json" --out "${APP}"
    --check --template-dir "${TPL}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE check_json ERROR_VARIABLE check_json_err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "--check from JSON is not green on the yaml-generated tree (${rc}):\n${check_json}${check_json_err}")
endif()
execute_process(COMMAND "${GEN}" --in "${TPL}/graph.yaml" --out "${APP}"
    --check --template-dir "${TPL}"
  RESULT_VARIABLE rc_yaml OUTPUT_VARIABLE check_yaml ERROR_VARIABLE check_yaml_err)
if(NOT rc_yaml EQUAL 0)
  message(FATAL_ERROR "--check from yaml is not green (${rc_yaml}):\n${check_yaml}${check_yaml_err}")
endif()
if(NOT "${check_json}${check_json_err}" STREQUAL "${check_yaml}${check_yaml_err}")
  message(FATAL_ERROR
    "--check output differs by input form:\nJSON: ${check_json}${check_json_err}\nYAML: ${check_yaml}${check_yaml_err}")
endif()

# Generate a fresh tree *from JSON* and prove the yaml input accepts it.
set(APP_JSON "${ROOT}/app_json")
run_ok(ignored "${GEN}" --from-json "${ROOT}/graph.json" --out "${APP_JSON}"
    --update generated --template-dir "${TPL}")
execute_process(COMMAND "${GEN}" --in "${TPL}/graph.yaml" --out "${APP_JSON}"
    --check --template-dir "${TPL}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR
    "tree generated from JSON is stale for the yaml --check (${rc}):\n${cout}${cerr}")
endif()

# ---- 3. strict rejection ------------------------------------------------
function(expect_reject name needle json_text)
  file(WRITE "${ROOT}/${name}.json" "${json_text}")
  execute_process(COMMAND "${GEN}" --from-json "${ROOT}/${name}.json" --emit-yaml
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 2)
    message(FATAL_ERROR "${name}: exit ${rc}, expected 2:\n${out}${err}")
  endif()
  string(FIND "${err}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "${name}: error does not mention '${needle}':\n${err}")
  endif()
endfunction()

string(REPLACE "\"id\": \"prod\","
  "\"id\": \"prod\",\n      \"bogus\": 1," bad_unknown "${json1}")
expect_reject(unknown_node_key "unknown key 'bogus' in node 'prod'" "${bad_unknown}")

string(REPLACE "\"id\": \"prod\","
  "\"id\": \"prod\",\n      \"id\": \"dup\"," bad_dup "${json1}")
expect_reject(duplicate_key "duplicate key 'id'" "${bad_dup}")

string(REPLACE "\"hz\": 10," "\"hz\": \"10\"," bad_type "${json1}")
expect_reject(wrong_type "hz in node 'prod' must be an integer" "${bad_type}")

string(REPLACE "\"version\": 1,"
  "\"version\": 1,\n  \"unexpected\": true," bad_top "${json1}")
expect_reject(unknown_top_key "unknown key 'unexpected' in graph" "${bad_top}")

# Unknown fields are also rejected by the yaml loader, so the JSON contract is
# not a loophole around the shared dialect.
file(WRITE "${ROOT}/unknown.yaml" "version: 1\npool_n: 4\nring_cap: 8\nnodes:\n  - id: n\n    type: tick\n    hz: 10\n    bogus: 1\n")
execute_process(COMMAND "${GEN}" --in "${ROOT}/unknown.yaml" --emit-json
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "unknown yaml key: exit ${rc}, expected 2:\n${out}${err}")
endif()
string(FIND "${err}" "unknown node key bogus" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "unknown yaml key is not named:\n${err}")
endif()

message(STATUS "gen_graph canonical JSON round-trip OK")
