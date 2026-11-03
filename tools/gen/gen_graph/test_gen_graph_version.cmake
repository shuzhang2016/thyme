# GEN-09 — generator version, generated-tree metadata and migration policy.
#
# 1. `--version` prints the generator semantic version and the generated
#    output format version without needing --in/--out.
# 2. Every full generation writes generated/.gen_meta.json (tool, generator
#    version, output format version, source graph hash) and stamps the version
#    into the file banners; regeneration is byte-stable.
# 3. `--check` passes on the fresh tree and fails (exit 1) on a hand-edited or
#    missing meta, on a tree produced by a different output format, and on a
#    different generator version — each with an actionable diagnostic.
# 4. A failed check never writes into generated/.
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${THYME_ABS}/tools/gen/gen_graph/testdata/ports_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/gen_graph_version")
file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}")

# ---- 1. --version needs no graph and prints both versions --------------------
execute_process(COMMAND "${GEN}" --version
  RESULT_VARIABLE rc OUTPUT_VARIABLE vout ERROR_VARIABLE verr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph --version exited ${rc}: ${vout}${verr}")
endif()
string(REGEX MATCH "gen_graph [0-9]+\\.[0-9]+\\.[0-9]+ \\(output format [0-9]+\\)"
  VERSION_LINE "${vout}")
if(VERSION_LINE STREQUAL "")
  message(FATAL_ERROR "--version did not print both versions:\n${vout}")
endif()
string(REGEX REPLACE "^gen_graph ([0-9]+\\.[0-9]+\\.[0-9]+) .*$" "\\1"
  GEN_VER "${VERSION_LINE}")
string(REGEX REPLACE ".*output format ([0-9]+).*$" "\\1"
  FORMAT_VER "${VERSION_LINE}")

# ---- helper: generate / check ------------------------------------------------
function(generate)
  execute_process(COMMAND "${GEN}" --out "${ROOT}" --update generated
      --template-dir "${TPL}" --in "${TPL}/graph.yaml"
    RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gen_graph generation failed (${rc}):\n${cout}${cerr}")
  endif()
endfunction()

function(run_check expect_rc out_var)
  execute_process(COMMAND "${GEN}" --check --out "${ROOT}"
      --template-dir "${TPL}" --in "${TPL}/graph.yaml"
    RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
  if(NOT rc EQUAL ${expect_rc})
    message(FATAL_ERROR
      "gen_graph --check exited ${rc}, expected ${expect_rc}:\n${cout}${cerr}")
  endif()
  set(${out_var} "${cout}${cerr}" PARENT_SCOPE)
endfunction()

# ---- 2. meta + banner on a fresh tree ---------------------------------------
generate()
set(META "${ROOT}/generated/.gen_meta.json")
if(NOT EXISTS "${META}")
  message(FATAL_ERROR "generation did not write generated/.gen_meta.json")
endif()
file(READ "${META}" meta_body)
foreach(needle
    "\"tool\": \"gen_graph\""
    "\"generator_version\": \"${GEN_VER}\""
    "\"format_version\": ${FORMAT_VER}"
    "\"source_graph_hash\": ")
  string(FIND "${meta_body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "generated/.gen_meta.json missing '${needle}':\n${meta_body}")
  endif()
endforeach()

function(expect_contains file needle what)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "${what}: ${file} missing '${needle}'")
  endif()
endfunction()

# The banner stamp reaches engine-emitted headers, .inc-owned headers
# (graph_types.h / noworker), and the generated integration guide.
expect_contains("${ROOT}/generated/wiring.h"
  "(output format ${FORMAT_VER})" "banner")
expect_contains("${ROOT}/generated/graph_types.h"
  "(output format ${FORMAT_VER})" "inc-owned graph_types.h")
expect_contains("${ROOT}/generated/health_reporter_generated.h"
  "(output format ${FORMAT_VER})" "noworker .inc header")
expect_contains("${ROOT}/generated/integration.md"
  "(output format ${FORMAT_VER})" "integration guide")

# Regeneration is byte-stable (the meta hash is deterministic).
file(SHA256 "${META}" meta_sha)
generate()
file(SHA256 "${META}" meta_sha2)
if(NOT meta_sha STREQUAL meta_sha2)
  message(FATAL_ERROR "generated/.gen_meta.json is not stable across regens")
endif()
run_check(0 out)
string(FIND "${out}" "up to date" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check on a fresh tree did not report up to date:\n${out}")
endif()

# Mapping-key reordering, comments, and blank lines do not change the effective
# graph or any graph_ir.h byte. A semantic edit must change kGraphHash.
file(READ "${ROOT}/generated/graph_ir.h" original_ir)
file(READ "${TPL}/graph.yaml" source_yaml)
string(REPLACE "version: 1\npool_n: 4\nring_cap: 8\n"
  "ring_cap: 8\n\n# formatting-only change\nversion: 1\npool_n: 4\n"
  reordered_yaml "${source_yaml}")
string(REPLACE
  "  - id: prod\n    type: producer\n    hz: 10\n    process: main\n"
  "  - type: producer\n    process: main\n    hz: 10\n    id: prod\n"
  reordered_yaml "${reordered_yaml}")
set(REORDERED_YAML "${CMAKE_BINARY_DIR}/gen_graph_version_reordered.yaml")
set(REORDERED_ROOT "${CMAKE_BINARY_DIR}/gen_graph_version_reordered")
file(WRITE "${REORDERED_YAML}" "${reordered_yaml}")
execute_process(COMMAND "${GEN}" --out "${REORDERED_ROOT}" --update generated
  --template-dir "${TPL}" --in "${REORDERED_YAML}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "reordered YAML generation failed: ${rc}")
endif()
file(READ "${REORDERED_ROOT}/generated/graph_ir.h" reordered_ir)
if(NOT reordered_ir STREQUAL original_ir)
  message(FATAL_ERROR "formatting-only YAML edits changed graph_ir.h")
endif()

string(REPLACE "pool_n: 4" "pool_n: 5" semantic_yaml "${source_yaml}")
set(SEMANTIC_YAML "${CMAKE_BINARY_DIR}/gen_graph_version_semantic.yaml")
set(SEMANTIC_ROOT "${CMAKE_BINARY_DIR}/gen_graph_version_semantic")
file(WRITE "${SEMANTIC_YAML}" "${semantic_yaml}")
execute_process(COMMAND "${GEN}" --out "${SEMANTIC_ROOT}" --update generated
  --template-dir "${TPL}" --in "${SEMANTIC_YAML}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "semantic YAML generation failed: ${rc}")
endif()
file(READ "${SEMANTIC_ROOT}/generated/graph_ir.h" semantic_ir)
string(REGEX MATCH "kGraphHash = 0x([0-9a-f]+)ULL" original_hash_row "${original_ir}")
string(REGEX MATCH "kGraphHash = 0x([0-9a-f]+)ULL" semantic_hash_row "${semantic_ir}")
if(original_hash_row STREQUAL "" OR semantic_hash_row STREQUAL "")
  message(FATAL_ERROR "generated graph_ir.h has no kGraphHash")
endif()
if(original_hash_row STREQUAL semantic_hash_row)
  message(FATAL_ERROR "semantic YAML edit did not change kGraphHash")
endif()

# ---- 3a. hand-edited meta (hash changed) -> exit 1 naming the file ----------
file(WRITE "${META}"
"{\n  \"tool\": \"gen_graph\",\n  \"generator_version\": \"${GEN_VER}\",\n  \"format_version\": ${FORMAT_VER},\n  \"source_graph_hash\": \"0000000000000000\"\n}\n")
run_check(1 out)
string(FIND "${out}" ".gen_meta.json: content differs" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check did not report the hand-edited meta:\n${out}")
endif()
string(FIND "${out}" "does not match the current graph" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check did not diagnose the stale graph hash:\n${out}")
endif()

# ---- 3b. missing meta -> exit 1 naming it -----------------------------------
generate()
file(REMOVE "${META}")
run_check(1 out)
string(FIND "${out}" ".gen_meta.json: missing" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check did not report the missing meta:\n${out}")
endif()

# ---- 3c. tree produced by another output format -> migration error ----------
generate()
math(EXPR OLD_FORMAT "${FORMAT_VER} - 1")
file(WRITE "${META}"
"{\n  \"tool\": \"gen_graph\",\n  \"generator_version\": \"${GEN_VER}\",\n  \"format_version\": ${OLD_FORMAT},\n  \"source_graph_hash\": \"0000000000000000\"\n}\n")
run_check(1 out)
string(FIND "${out}" "produced by output format v${OLD_FORMAT}" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR
    "--check did not diagnose the output-format mismatch:\n${out}")
endif()
string(FIND "${out}" "--update generated" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR
    "--check format error did not name the migration command:\n${out}")
endif()
# A failed check must not repair (write) the tree.
file(READ "${META}" still_bad)
string(FIND "${still_bad}" "\"format_version\": ${OLD_FORMAT}" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check wrote into generated/ (bad meta was repaired)")
endif()

# ---- 3d. generator version drift (same format) -> exit 1 --------------------
generate()
file(WRITE "${META}"
"{\n  \"tool\": \"gen_graph\",\n  \"generator_version\": \"0.0.0\",\n  \"format_version\": ${FORMAT_VER},\n  \"source_graph_hash\": \"0000000000000000\"\n}\n")
run_check(1 out)
string(FIND "${out}" "produced by gen_graph 0.0.0" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check did not diagnose the generator drift:\n${out}")
endif()

# ---- 4. regeneration removes all drift; no staging dirs left behind ---------
generate()
run_check(0 out)
string(FIND "${out}" "${GEN_VER}" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check banner does not name the generator version:\n${out}")
endif()
file(GLOB leftovers "${ROOT}/.generated.*" "${ROOT}/.gen_graph_tpl")
if(leftovers)
  message(FATAL_ERROR "version fixture left staging dirs behind: ${leftovers}")
endif()

message(STATUS "gen_graph GEN-09 version/migration fixture OK (${VERSION_LINE})")
