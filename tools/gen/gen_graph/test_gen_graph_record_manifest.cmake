# GEN-12 / TST-11 — record manifest emission.
#
# A mixed graph records alpha + charlie and sets bravo to record: false. The
# emitted manifest/registry/taps/viz/replay/gates must contain only recorded
# channels while the in-process wiring for bravo stays intact. The manifest is
# ordered by channel name, so reordering graph.yaml edges must not renumber
# ids; a channel that sorts later must not shift existing ids.
if(NOT GEN OR NOT TPL_SRC OR NOT THYME OR NOT CXX OR NOT TRIGGER_CENTER)
  message(FATAL_ERROR "GEN, TPL_SRC, THYME, CXX and TRIGGER_CENTER required")
endif()
get_filename_component(TPL_SRC "${TPL_SRC}" ABSOLUTE)
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(ROOT "${CMAKE_BINARY_DIR}/gen_graph_record_manifest")
file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}")

# viz per-slot dispatch needs the contracts_json marker (GEN-04).
set(TPL "${ROOT}/tpl")
file(COPY "${TPL_SRC}/" DESTINATION "${TPL}")
file(WRITE "${TPL}/contracts_json.txt" "dummy/contracts/contracts_json.h\n")
file(WRITE "${TPL}/contract_sizes.txt" "Foo|4\nBar|8\nBaz|12\nQux|16\n")

function(write_graph path order)
  if("${order}" STREQUAL "none")
    file(WRITE "${path}" "version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: producer
    hz: 10
    process: main
  - id: cons
    type: consumer
    hz: 10
    process: main
  - id: health_reporter
    type: health_reporter
    hz: 10
    process: main
edges:
  - from: prod
    to: cons
    channel: alpha
    cpp_type: Foo
    pool: FooPool
    transport: spsc
    record: false
  - from: prod
    to: cons
    channel: bravo
    cpp_type: Bar
    pool: FooPool
    transport: spsc
    record: false
  - from: prod
    to: cons
    channel: charlie
    cpp_type: Baz
    pool: FooPool
    transport: spsc
    record: false
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: \"/c9_%pid\"
  health_n: 3
")
    return()
  endif()
  set(edges "")
  if("${order}" STREQUAL "normal")
    set(edges
"  - from: prod
    to: cons
    channel: alpha
    cpp_type: Foo
    pool: FooPool
    transport: spsc
  - from: prod
    to: cons
    channel: bravo
    cpp_type: Bar
    pool: FooPool
    transport: spsc
    record: false
  - from: prod
    to: cons
    channel: charlie
    cpp_type: Baz
    pool: FooPool
    transport: spsc")
  elseif("${order}" STREQUAL "reordered")
    set(edges
"  - from: prod
    to: cons
    channel: charlie
    cpp_type: Baz
    pool: FooPool
    transport: spsc
  - from: prod
    to: cons
    channel: alpha
    cpp_type: Foo
    pool: FooPool
    transport: spsc
  - from: prod
    to: cons
    channel: bravo
    cpp_type: Bar
    pool: FooPool
    transport: spsc
    record: false")
  elseif("${order}" STREQUAL "all")
    set(edges
"  - from: prod
    to: cons
    channel: alpha
    cpp_type: Foo
    pool: FooPool
    transport: spsc
  - from: prod
    to: cons
    channel: bravo
    cpp_type: Bar
    pool: FooPool
    transport: spsc
  - from: prod
    to: cons
    channel: charlie
    cpp_type: Baz
    pool: FooPool
    transport: spsc")
  elseif("${order}" STREQUAL "compile")
    # One cpp_type (Foo) so the generated headers compile against the ports
    # fixture's stub contract.
    set(edges
"  - from: prod
    to: cons
    channel: alpha
    cpp_type: Foo
    pool: FooPool
    transport: spsc
  - from: prod
    to: cons
    channel: bravo
    cpp_type: Foo
    pool: FooPool
    transport: spsc
    record: false
  - from: prod
    to: cons
    channel: charlie
    cpp_type: Foo
    pool: FooPool
    transport: spsc")
  elseif("${order}" STREQUAL "later")
    set(edges
"  - from: prod
    to: cons
    channel: alpha
    cpp_type: Foo
    pool: FooPool
    transport: spsc
  - from: prod
    to: cons
    channel: bravo
    cpp_type: Bar
    pool: FooPool
    transport: spsc
    record: false
  - from: prod
    to: cons
    channel: charlie
    cpp_type: Baz
    pool: FooPool
    transport: spsc
  - from: prod
    to: cons
    channel: zulu
    cpp_type: Qux
    pool: FooPool
    transport: spsc")
  endif()
  file(WRITE "${path}" "version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: producer
    hz: 10
    process: main
  - id: cons
    type: consumer
    hz: 10
    process: main
  - id: health_reporter
    type: health_reporter
    hz: 10
    process: main
edges:
${edges}
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: \"/c9_%pid\"
  health_n: 3
")
endfunction()

function(generate name graph)
  execute_process(COMMAND "${GEN}" --out "${ROOT}/${name}" --update generated
    --template-dir "${TPL}" --in "${graph}" --no-health-reporter
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gen_graph (${name}) failed: ${rc}\n${out}${err}")
  endif()
endfunction()

function(expect_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "${file} missing expected content: ${needle}")
  endif()
endfunction()

function(expect_missing file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "${file} must not contain: ${needle}\n${body}")
  endif()
endfunction()

write_graph("${ROOT}/normal.yaml" normal)
write_graph("${ROOT}/reordered.yaml" reordered)
write_graph("${ROOT}/all.yaml" all)
write_graph("${ROOT}/later.yaml" later)
generate(mixed "${ROOT}/normal.yaml")
generate(reordered "${ROOT}/reordered.yaml")
generate(all "${ROOT}/all.yaml")
generate(later "${ROOT}/later.yaml")

set(GEN_DIR "${ROOT}/mixed/generated")
set(MANIFEST "${GEN_DIR}/record_manifest.h")
set(REGISTRY "${GEN_DIR}/record_registry.h")

# --- Manifest content: exactly the recorded channels, ordered by name. ---
expect_contains("${MANIFEST}" "kRecordManifestCount = 2;")
expect_contains("${MANIFEST}" "{0, \"alpha\", \"Foo\", \"prod\"},")
expect_contains("${MANIFEST}" "{1, \"charlie\", \"Baz\", \"prod\"},")
expect_missing("${MANIFEST}" "bravo")
expect_contains("${MANIFEST}" "kRecordManifestChannels[] = {")
expect_contains("${REGISTRY}" "kAlpha = 0,")
expect_contains("${REGISTRY}" "kCharlie = 1,")
expect_missing("${REGISTRY}" "Bravo")
expect_missing("${REGISTRY}" "Bar")
expect_contains("${REGISTRY}" "kRecordRegistryCount = 2;")
expect_contains("${REGISTRY}" "kRecordRegistryCount == kRecordManifestCount")
expect_contains("${REGISTRY}" "kRecordMirrorRingSpecs[]")
expect_contains("${REGISTRY}" "{sizeof(thyme::Frame<Foo>), 1}")
expect_contains("${REGISTRY}" "{sizeof(thyme::Frame<Baz>), 1}")

# --- OPS-13: one aggregate mirror source, with derived non-zero metadata. ---
set(TRIGGER_BASE "${GEN_DIR}/trigger.base.ini")
expect_contains("${TRIGGER_BASE}" "[channel./c9_%pid_rec]")
expect_contains("${TRIGGER_BASE}" "record_channels = alpha,charlie")
expect_contains("${TRIGGER_BASE}" "rate_hz = 20")
expect_contains("${TRIGGER_BASE}" "avg_frame_bytes = 48")
file(READ "${TRIGGER_BASE}" trigger_base_body)
string(REGEX MATCHALL "\\[channel\\." trigger_channel_sections "${trigger_base_body}")
list(LENGTH trigger_channel_sections trigger_channel_count)
if(NOT trigger_channel_count EQUAL 1)
  message(FATAL_ERROR "manifest rows were duplicated into ${trigger_channel_count} SHM sources")
endif()
file(WRITE "${ROOT}/continuous.policy.ini"
  "[recorder]\noutput_dir = ${ROOT}/out\n"
  "[continuous]\nauto_arm = true\nwait_idle = true\ndemo_idle = true\n")
execute_process(COMMAND "${TRIGGER_CENTER}" --check-config --shm-pid 1
  --record-manifest "${MANIFEST}" "${TRIGGER_BASE}" "${ROOT}/continuous.policy.ini"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT err MATCHES "sustained 960 B/s")
  message(FATAL_ERROR "generated base+policy did not load end-to-end: ${rc}\n${out}${err}")
endif()

# Missing contract sizes stay unknown: no zero-valued metadata and no continuous plan.
set(TPL_UNKNOWN "${ROOT}/tpl_unknown")
file(COPY "${TPL}/" DESTINATION "${TPL_UNKNOWN}")
file(REMOVE "${TPL_UNKNOWN}/contract_sizes.txt")
execute_process(COMMAND "${GEN}" --out "${ROOT}/unknown" --update generated
  --template-dir "${TPL_UNKNOWN}" --in "${ROOT}/normal.yaml" --no-health-reporter
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (unknown metadata) failed: ${rc}\n${out}${err}")
endif()
expect_missing("${ROOT}/unknown/generated/trigger.base.ini" "rate_hz =")
expect_missing("${ROOT}/unknown/generated/trigger.base.ini" "avg_frame_bytes =")
execute_process(COMMAND "${TRIGGER_CENTER}" --check-config --shm-pid 1
  --record-manifest "${ROOT}/unknown/generated/record_manifest.h"
  "${ROOT}/unknown/generated/trigger.base.ini" "${ROOT}/continuous.policy.ini"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "metadata is incomplete")
  message(FATAL_ERROR "unknown continuous metadata was not rejected: ${rc}\n${out}${err}")
endif()

# A feedback edge has no resolved steady-state rate and is not assigned zero.
file(READ "${ROOT}/normal.yaml" feedback_yaml)
string(REPLACE "channel: alpha\n    cpp_type:"
  "channel: alpha\n    feedback: true\n    delay: 1\n    cpp_type:" feedback_yaml "${feedback_yaml}")
file(WRITE "${ROOT}/feedback.yaml" "${feedback_yaml}")
generate(feedback "${ROOT}/feedback.yaml")
expect_missing("${ROOT}/feedback/generated/trigger.base.ini" "rate_hz =")
expect_missing("${ROOT}/feedback/generated/trigger.base.ini" "avg_frame_bytes =")

# --- ABI-09/ABI-10: the generated record table carries the manifest identity --
# The template has no contracts_layout.txt here, so the recorder gets the hash
# and version with an explicit "layout unavailable" contract lookup.
set(TABLE "${GEN_DIR}/record_table.h")
expect_contains("${TABLE}" "ThymeRecordMirrorRingRequiredBytes()")
expect_contains("${TABLE}" "inline const thyme::schema::RecordManifest kThymeRecordManifest = {")
expect_contains("${TABLE}" "kRecordManifestHash,")
expect_contains("${TABLE}" "thyme::schema::kManifestVersion,")
expect_contains("${TABLE}" "no contracts_layout.txt: field layout unavailable")
expect_contains("${TABLE}" "&kThymeRecordManifest,")

# With a contracts_layout.txt marker the table wires the gen_contracts field
# layout lookup (explicit template config, no filesystem probing).
set(TPL_LAYOUT "${ROOT}/tpl_layout")
file(COPY "${TPL}/" DESTINATION "${TPL_LAYOUT}")
file(WRITE "${TPL_LAYOUT}/contracts_layout.txt" "dummy/contracts/contract_layout.h\n")
execute_process(COMMAND "${GEN}" --out "${ROOT}/layout" --update generated
  --template-dir "${TPL_LAYOUT}" --in "${ROOT}/normal.yaml" --no-health-reporter
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (layout marker) failed: ${rc}\n${out}${err}")
endif()
expect_contains("${ROOT}/layout/generated/record_table.h"
  "#include \"dummy/contracts/contract_layout.h\"")
expect_contains("${ROOT}/layout/generated/record_table.h"
  "&thyme_contract_layout::FindContractLayout}")

# --- No taps / viz / replay / gates for the unrecorded channel. ---
expect_contains("${GEN_DIR}/record_tap_helpers.h" "RecordTapAlpha")
expect_contains("${GEN_DIR}/record_tap_helpers.h" "RecordTapCharlie")
expect_missing("${GEN_DIR}/record_tap_helpers.h" "Bravo")

file(READ "${GEN_DIR}/viz_ser.h" viz_body)
foreach(needle "case 0: return thyme_contracts_json::kSchema_Foo"
               "case 1: return thyme_contracts_json::kSchema_Baz")
  string(FIND "${viz_body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "viz_ser.h missing ${needle}")
  endif()
endforeach()
expect_missing("${GEN_DIR}/viz_ser.h" "kSchema_Bar")
expect_missing("${GEN_DIR}/viz_ser.h" "case 2:")
string(REGEX MATCHALL "case [0-9]+:" viz_cases "${viz_body}")
list(LENGTH viz_cases viz_case_count)
# Two slots x (VizSchemaSlot + VizEncodeSlot).
if(NOT viz_case_count EQUAL 4)
  message(FATAL_ERROR "viz_ser.h has ${viz_case_count} slot cases, expected 4")
endif()

expect_contains("${GEN_DIR}/replay_inject.h" "case 0:")
expect_contains("${GEN_DIR}/replay_inject.h" "case 1:")
expect_missing("${GEN_DIR}/replay_inject.h" "case 2:")

expect_contains("${GEN_DIR}/graph_ir.h" "kGraphRecordGateCount = 1;")
expect_contains("${GEN_DIR}/graph_ir.h" "{\"main\", 0}")
expect_contains("${GEN_DIR}/integration.md" "| record topics | 2")
expect_contains("${GEN_DIR}/integration.md" "--topics alpha,charlie")
expect_missing("${GEN_DIR}/integration.md" "  - bravo")

# Optional record gates keep every recorded channel but require no frame.
write_graph("${ROOT}/optional.yaml" normal)
file(READ "${ROOT}/optional.yaml" optional_yaml)
string(REPLACE "record: false" "record: true" optional_yaml "${optional_yaml}")
string(REPLACE "transport: spsc" "transport: spsc\n    record_gate: false" optional_yaml "${optional_yaml}")
file(WRITE "${ROOT}/optional.yaml" "${optional_yaml}")
generate(optional "${ROOT}/optional.yaml")
expect_contains("${ROOT}/optional/generated/graph_ir.h" "kGraphRecordGateCount = 0;")
expect_contains("${ROOT}/optional/generated/record_manifest.h" "kRecordManifestCount = 3;")

# --- In-process wiring for the unrecorded edge stays intact. ---
expect_contains("${GEN_DIR}/mp_ports.h" "bravo_")
expect_contains("${GEN_DIR}/prod_generated.h" "bravo_")

# --- ID stability: YAML edge order does not affect ids. ---
file(READ "${MANIFEST}" mixed_manifest)
file(READ "${ROOT}/reordered/generated/record_manifest.h" reordered_manifest)
if(NOT mixed_manifest STREQUAL reordered_manifest)
  message(FATAL_ERROR "record_manifest.h changed when graph edges were reordered")
endif()

# A channel that sorts after the existing ones must not renumber them.
expect_contains("${ROOT}/later/generated/record_manifest.h" "{0, \"alpha\", \"Foo\", \"prod\"},")
expect_contains("${ROOT}/later/generated/record_manifest.h" "{1, \"charlie\", \"Baz\", \"prod\"},")
expect_contains("${ROOT}/later/generated/record_manifest.h" "{2, \"zulu\", \"Qux\", \"prod\"},")

# --- Wholesale record:false shrinks the registry (and therefore the arena). ---
expect_contains("${ROOT}/all/generated/record_registry.h" "kRecordRegistryCount = 3;")
if(NOT EXISTS "${ROOT}/mixed/generated/record_registry.h")
  message(FATAL_ERROR "mixed graph lost its record registry")
endif()

# All edges unrecorded -> no registry/table/taps, but a zero-entry manifest.
write_graph("${ROOT}/none.yaml" none)
generate(none "${ROOT}/none.yaml")
foreach(_f record_registry.h record_table.h record_tap_helpers.h
             graph_record_check.h replay_inject.h replay_stages.h viz_ser.h)
  if(EXISTS "${ROOT}/none/generated/${_f}")
    message(FATAL_ERROR "all-unrecorded graph emitted generated/${_f}")
  endif()
endforeach()
expect_contains("${ROOT}/none/generated/record_manifest.h" "kRecordManifestCount = 0;")
expect_missing("${ROOT}/none/generated/trigger.base.ini" "[channel.")
expect_contains("${ROOT}/none/generated/graph_ir.h" "kGraphRecordGateCount = 0;")
expect_contains("${ROOT}/none/generated/mp_ports.h" "bravo_")
expect_contains("${ROOT}/none/generated/prod_generated.h" "bravo_")
expect_missing("${ROOT}/none/generated/prod_generated.h" "void RecordTap")
expect_missing("${ROOT}/none/generated/prod_generated.h" "RecordTapMirror")
expect_missing("${ROOT}/none/generated/prod_generated.h" "record_page_")

# Generate a single-contract mixed graph and compile its node bases: the
# unrecorded edge must keep its Ports/ring members and must not leak
# RecordStructId/tap references into code that no longer has them.
write_graph("${ROOT}/compile.yaml" compile)
generate(compile "${ROOT}/compile.yaml")
file(WRITE "${ROOT}/compile_tu.cpp" "#include \"generated/prod_generated.h\"
#include \"generated/cons_generated.h\"
#include \"generated/record_registry.h\"
int main() { return 0; }
")
execute_process(
  COMMAND "${CXX}" -std=c++17 -fsyntax-only
          -I "${ROOT}/compile" -I "${TPL}/src/include"
          -I "${THYME_ABS}/include" "${ROOT}/compile_tu.cpp"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "mixed record graph does not compile:\n${out}${err}")
endif()

file(WRITE "${ROOT}/unrecorded_tu.cpp" "#include \"generated/record_registry.h\"
int main() { return static_cast<uint16_t>(RecordStructId::kBravo); }
")
execute_process(
  COMMAND "${CXX}" -std=c++17 -fsyntax-only
          -I "${ROOT}/compile" -I "${TPL}/src/include"
          -I "${THYME_ABS}/include" "${ROOT}/unrecorded_tu.cpp"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0)
  message(FATAL_ERROR "record:false RecordStructId::kBravo unexpectedly compiled")
endif()

# OPS-13: [chassis] offsets come from the contract layout, shifted to the
# recorded Frame sample. align 16 puts the payload at 48, not at field 0.
set(TPL_CHASSIS "${ROOT}/tpl_chassis")
file(COPY "${TPL}/" DESTINATION "${TPL_CHASSIS}")
file(APPEND "${TPL_CHASSIS}/contract_sizes.txt" "ChassisPod|32\n")
file(WRITE "${TPL_CHASSIS}/extras.txt" "chassis|ChassisPod|consumer|producer\n")
file(MAKE_DIRECTORY "${TPL_CHASSIS}/src/include/chassis")
file(WRITE "${TPL_CHASSIS}/src/include/chassis/contract_layout.h"
"#pragma once
#include \"thyme/record/record_schema.h\"
namespace thyme_contract_layout {
inline constexpr thyme::schema::FieldLayout kChassisPodFields[] = {
    {\"park_brk_status\", \"uint8\", 0u, 1u, 1u, 1u},
    {\"vehicle_speed\", \"float32\", 4u, 4u, 1u, 4u},
};
inline constexpr thyme::schema::ContractLayout kContracts[] = {
    {\"ChassisPod\", 32u, 2u, 16u, kChassisPodFields},
};
}
")
file(WRITE "${TPL_CHASSIS}/contracts_layout.txt" "chassis/contract_layout.h\n")
file(WRITE "${ROOT}/chassis.yaml" "version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: producer
    hz: 10
    process: main
  - id: cons
    type: consumer
    hz: 10
    process: main
edges:
  - from: prod
    to: cons
    channel: alpha
    cpp_type: Foo
    pool: FooPool
    transport: spsc
  - from: prod
    to: cons
    channel: chassis_can
    cpp_type: ChassisPod
    pool: FooPool
    transport: spsc
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: \"/c9_%pid\"
  health_n: 2
")
execute_process(COMMAND "${GEN}" --out "${ROOT}/chassis" --update generated
  --template-dir "${TPL_CHASSIS}" --in "${ROOT}/chassis.yaml" --no-health-reporter
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (chassis) failed: ${rc}\n${out}${err}")
endif()
set(CHASSIS_BASE "${ROOT}/chassis/generated/trigger.base.ini")
expect_contains("${CHASSIS_BASE}" "[chassis]")
expect_contains("${CHASSIS_BASE}" "page_shm = /c9_%pid_rec")
expect_contains("${CHASSIS_BASE}" "struct_id = 1")
expect_contains("${CHASSIS_BASE}" "epb_offset = 48")
expect_contains("${CHASSIS_BASE}" "speed_offset = 52")
expect_missing("${CHASSIS_BASE}" "epb_offset = 0")
file(WRITE "${ROOT}/chassis.policy.ini"
  "[recorder]\noutput_dir = ${ROOT}/out\n"
  "[trigger_center]\nallow_empty_sources = true\n"
  "[continuous]\nauto_arm = true\nwait_idle = true\n")
execute_process(COMMAND "${TRIGGER_CENTER}" --check-config --shm-pid 7
  --record-manifest "${ROOT}/chassis/generated/record_manifest.h"
  "${CHASSIS_BASE}" "${ROOT}/chassis.policy.ini"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "config OK")
  message(FATAL_ERROR "generated chassis base was rejected: ${rc}\n${out}${err}")
endif()

# A chassis extra whose type is not recorded does not invent offsets.
file(WRITE "${TPL_CHASSIS}/extras.txt" "chassis|NotRecorded|consumer|\n")
execute_process(COMMAND "${GEN}" --out "${ROOT}/chassis_absent" --update generated
  --template-dir "${TPL_CHASSIS}" --in "${ROOT}/chassis.yaml" --no-health-reporter
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (chassis absent) failed: ${rc}\n${out}${err}")
endif()
expect_contains("${ROOT}/chassis_absent/generated/trigger.base.ini"
  "chassis section omitted: NotRecorded is not a recorded mirror struct")
expect_missing("${ROOT}/chassis_absent/generated/trigger.base.ini" "[chassis]")

# Two recorded channels of the chassis type are ambiguous.
file(WRITE "${TPL_CHASSIS}/extras.txt" "chassis|ChassisPod|consumer|\n")
file(WRITE "${ROOT}/chassis_ambiguous.yaml" "version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: producer
    hz: 10
    process: main
  - id: cons
    type: consumer
    hz: 10
    process: main
edges:
  - from: prod
    to: cons
    channel: alpha
    cpp_type: ChassisPod
    pool: FooPool
    transport: spsc
  - from: prod
    to: cons
    channel: bravo
    cpp_type: ChassisPod
    pool: FooPool
    transport: spsc
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: \"/c9_%pid\"
  health_n: 2
")
execute_process(COMMAND "${GEN}" --out "${ROOT}/chassis_ambiguous" --update generated
  --template-dir "${TPL_CHASSIS}" --in "${ROOT}/chassis_ambiguous.yaml"
  --no-health-reporter
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "more than one channel")
  message(FATAL_ERROR "ambiguous chassis type was not rejected: ${rc}\n${out}${err}")
endif()

# OPS-13: explicit pinned_slot/h264 edges are recorder sources, not mirror structs.
file(WRITE "${ROOT}/bulk.yaml" "version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: producer
    hz: 10
    process: main
  - id: cons
    type: consumer
    hz: 10
    process: main
edges:
  - from: prod
    to: cons
    channel: alpha
    cpp_type: Foo
    pool: FooPool
    transport: spsc
  - from: prod
    to: cons
    channel: cloud
    cpp_type: Foo
    pool: FooPool
    transport: spsc
    record_kind: pinned_slot
    avg_frame_bytes: 200000
  - from: prod
    to: cons
    channel: front
    cpp_type: Foo
    pool: FooPool
    transport: spsc
    record_kind: h264
    bitrate_kbps: 4000
    record_shm: /h264_front
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: \"/c9_%pid\"
  health_n: 2
")
execute_process(COMMAND "${GEN}" --out "${ROOT}/bulk" --update generated
  --template-dir "${TPL}" --in "${ROOT}/bulk.yaml" --no-health-reporter
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (bulk) failed: ${rc}\n${out}${err}")
endif()
set(BULK_BASE "${ROOT}/bulk/generated/trigger.base.ini")
expect_contains("${BULK_BASE}" "record_channels = alpha")
expect_contains("${BULK_BASE}" "[channel./h264_front]")
expect_contains("${BULK_BASE}" "kind = h264_stream")
expect_contains("${BULK_BASE}" "name = front")
expect_contains("${BULK_BASE}" "bitrate_kbps = 4000")
expect_contains("${BULK_BASE}" "[channel./cloud]")
expect_contains("${BULK_BASE}" "kind = pinned_slot")
expect_contains("${BULK_BASE}" "avg_frame_bytes = 200000")
expect_contains("${BULK_BASE}" "rate_hz = 10")
expect_missing("${BULK_BASE}" "gop_s =")
expect_missing("${BULK_BASE}" "record_channels = alpha,cloud")
expect_contains("${ROOT}/bulk/generated/record_manifest.h" "kRecordManifestCount = 1;")
expect_contains("${ROOT}/bulk/generated/record_manifest.h" "kRecordBulkChannels[]")
expect_contains("${ROOT}/bulk/generated/record_manifest.h" "\"cloud\"")
expect_contains("${ROOT}/bulk/generated/record_manifest.h" "\"front\"")
expect_missing("${ROOT}/bulk/generated/record_registry.h" "kFront")
expect_missing("${ROOT}/bulk/generated/record_registry.h" "kCloud")
file(WRITE "${ROOT}/bulk.policy.ini"
  "[recorder]\noutput_dir = ${ROOT}/out\n"
  "[trigger_center]\nallow_empty_sources = true\n")
execute_process(COMMAND "${TRIGGER_CENTER}" --check-config --shm-pid 9
  --record-manifest "${ROOT}/bulk/generated/record_manifest.h"
  "${BULK_BASE}" "${ROOT}/bulk.policy.ini"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "config OK")
  message(FATAL_ERROR "generated bulk base was rejected: ${rc}\n${out}${err}")
endif()

file(WRITE "${ROOT}/bulk_bad.yaml" "version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: producer
    hz: 10
    process: main
  - id: cons
    type: consumer
    hz: 10
    process: main
edges:
  - from: prod
    to: cons
    channel: front
    cpp_type: Foo
    pool: FooPool
    transport: spsc
    record_kind: h264
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: \"/c9_%pid\"
  health_n: 2
")
execute_process(COMMAND "${GEN}" --out "${ROOT}/bulk_bad" --update generated
  --template-dir "${TPL}" --in "${ROOT}/bulk_bad.yaml" --no-health-reporter
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "bitrate_kbps")
  message(FATAL_ERROR "h264 without bitrate was accepted: ${rc}\n${out}${err}")
endif()

message(STATUS "gen_graph record manifest OK")
