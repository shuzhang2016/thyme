# PERF-02/SCH-02/SCH-05/SCH-11/SCH-12: RM priority generation, admission and
# --explain-sched. Fast, deterministic, no nested app build -> mr_gate.
if(NOT GEN OR NOT THYME OR NOT CXX)
  message(FATAL_ERROR "GEN, THYME and CXX required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(ROOT "${CMAKE_BINARY_DIR}/explain_sched")
file(REMOVE_RECURSE "${ROOT}")
file(MAKE_DIRECTORY "${ROOT}")

function(expect_contains haystack needle label)
  string(FIND "${haystack}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "${label}: missing '${needle}' in:\n${haystack}")
  endif()
endfunction()

# --- fixture: RM + explicit override + costs ------------------------------
set(GOOD "${ROOT}/good.yaml")
file(WRITE "${GOOD}" [==[version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: fast
    type: tick
    hz: 100
    process: main
    cpu_set: [0]
    wcet_ns: 2000000
  - id: slow
    type: tick
    hz: 10
    process: main
    cpu_set: [0]
    wcet_ns: 5000000
    priority: 40
edges: []
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 2
]==])

execute_process(COMMAND "${GEN}" --in "${GOOD}" --explain-sched
  RESULT_VARIABLE rc OUTPUT_VARIABLE json ERROR_VARIABLE human)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "--explain-sched failed: ${rc}\n${json}${human}")
endif()
expect_contains("${json}" "\"schema\": \"thyme.gen_graph.sched.v1\"" "json schema")
expect_contains("${json}" "\"topological_order\"" "json topo")
expect_contains("${json}" "\"priority_source\": \"rate_monotonic\"" "RM source")
expect_contains("${json}" "\"priority_source\": \"explicit\"" "explicit source")
expect_contains("${json}" "\"priority\": 89" "RM priority 89")
expect_contains("${json}" "\"priority\": 40" "explicit priority 40")
expect_contains("${json}" "\"status\": \"analyzed\"" "core analyzed")
expect_contains("${json}" "\"utilization\": 0.250000" "core utilization")
expect_contains("${json}" "\"admission_status\": \"ok\"" "admission ok")
expect_contains("${human}" "topological order:" "human topo")
expect_contains("${human}" "core 0: sum(C_i/T_i)=0.250000 status=analyzed"
  "human core")
expect_contains("${human}" "admission: ok" "human admission")

# Byte-stable JSON (and deterministic human output).
execute_process(COMMAND "${GEN}" --in "${GOOD}" --explain-sched
  RESULT_VARIABLE rc2 OUTPUT_VARIABLE json2 ERROR_VARIABLE human2)
if(NOT rc2 EQUAL 0 OR NOT "${json}" STREQUAL "${json2}" OR
   NOT "${human}" STREQUAL "${human2}")
  message(FATAL_ERROR "--explain-sched is not byte-stable\n--- json 1 ---\n${json}\n--- json 2 ---\n${json2}")
endif()

# --- generated priority table + placement ---------------------------------
set(OUT "${ROOT}/good_out")
execute_process(COMMAND "${GEN}" --in "${GOOD}" --out "${OUT}" --update generated
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "codegen failed: ${rc}\n${out}${err}")
endif()
foreach(f generated/sched_plan.h generated/scheduling_apply.inc)
  if(NOT EXISTS "${OUT}/${f}")
    message(FATAL_ERROR "codegen did not emit ${f}")
  endif()
endforeach()
file(READ "${OUT}/generated/sched_plan.h" plan_h)
file(READ "${OUT}/generated/scheduling_apply.inc" apply_inc)
expect_contains("${plan_h}" "kGraphSchedPlan" "sched table")
expect_contains("${plan_h}" "SchedPrioritySource::RateMonotonic" "RM row")
expect_contains("${plan_h}" "SchedPrioritySource::Explicit" "explicit row")
expect_contains("${plan_h}" "sizeof(SchedPlanRow) == 48u" "layout assert")
expect_contains("${plan_h}" "system-reserved band" "reserved assert")
expect_contains("${apply_inc}" "p.priority = 89;" "emitted RM priority")
expect_contains("${apply_inc}" "p.priority = 40;" "emitted explicit priority")
# The generated table must compile under the strict warning gate.
file(WRITE "${OUT}/sched_tu.cpp" [==[#include "generated/sched_plan.h"
int main() { return static_cast<int>(kGraphSchedPlanCount); }
]==])
execute_process(
  COMMAND "${CXX}" -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
          -Werror -I "${OUT}" -c "${OUT}/sched_tu.cpp" -o "${OUT}/sched_tu.o"
  RESULT_VARIABLE cxx_rc OUTPUT_VARIABLE cxx_out ERROR_VARIABLE cxx_err)
if(NOT cxx_rc EQUAL 0)
  message(FATAL_ERROR "generated sched_plan.h does not compile:\n${cxx_out}${cxx_err}")
endif()

# --- unknown costs are reported, never assumed to be zero -----------------
set(UNKNOWN "${ROOT}/unknown.yaml")
file(WRITE "${UNKNOWN}" [==[version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: fast
    type: tick
    hz: 100
    process: main
    cpu_set: [0]
edges: []
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 1
]==])
execute_process(COMMAND "${GEN}" --in "${UNKNOWN}" --explain-sched
  RESULT_VARIABLE rc OUTPUT_VARIABLE json ERROR_VARIABLE human)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "unknown-cost explain failed: ${rc}\n${json}${human}")
endif()
expect_contains("${json}" "\"admission_status\": \"not_analyzed\"" "unknown admission")
expect_contains("${json}" "\"status\": \"not_analyzed\"" "unknown core")
expect_contains("${json}" "\"wcet_known\": false" "unknown wcet")
expect_contains("${human}" "admission: not_analyzed" "unknown human")

# --- overload: error by default, explicit warn mode continues -------------
set(OVERLOAD "${ROOT}/overload.yaml")
file(WRITE "${OVERLOAD}" [==[version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: fast
    type: tick
    hz: 100
    process: main
    cpu_set: [0]
    wcet_ns: 20000000
edges: []
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 1
]==])
execute_process(COMMAND "${GEN}" --in "${OVERLOAD}" --explain-sched
  RESULT_VARIABLE rc OUTPUT_VARIABLE json ERROR_VARIABLE human)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "overload explain exit ${rc}, expected 2\n${json}${human}")
endif()
expect_contains("${json}" "\"admission_status\": \"overloaded\"" "overload status")
expect_contains("${human}" "SCHED_ERROR admission core 0" "overload error")
execute_process(COMMAND "${GEN}" --in "${OVERLOAD}" --explain-sched
    --sched-admission=warn
  RESULT_VARIABLE rc OUTPUT_VARIABLE json ERROR_VARIABLE human)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "warn-mode explain exit ${rc}, expected 0\n${json}${human}")
endif()
expect_contains("${human}" "SCHED_WARN admission core 0" "overload warning")
# Codegen must refuse before writing a tree in error mode.
set(BAD_OUT "${ROOT}/overload_out")
execute_process(COMMAND "${GEN}" --in "${OVERLOAD}" --out "${BAD_OUT}"
    --update generated
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "overload codegen exit ${rc}, expected 2\n${out}${err}")
endif()
if(EXISTS "${BAD_OUT}/generated")
  message(FATAL_ERROR "overload codegen left generated/ behind")
endif()

# --- explicit priority policy: range and reserved band --------------------
set(RESERVED "${ROOT}/reserved.yaml")
file(WRITE "${RESERVED}" [==[version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: fast
    type: tick
    hz: 100
    process: main
    priority: 95
edges: []
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 1
]==])
execute_process(COMMAND "${GEN}" --in "${RESERVED}" --explain-sched
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "reserved priority exit ${rc}, expected 2\n${out}${err}")
endif()
expect_contains("${err}" "system-reserved band 90..99" "reserved diagnostic")

set(OUT_OF_RANGE "${ROOT}/out_of_range.yaml")
file(WRITE "${OUT_OF_RANGE}" [==[version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: fast
    type: tick
    hz: 100
    process: main
    priority: 100
edges: []
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 1
]==])
execute_process(COMMAND "${GEN}" --in "${OUT_OF_RANGE}" --explain-sched
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "out-of-range priority exit ${rc}, expected 2\n${out}${err}")
endif()
expect_contains("${err}" "outside the RT priority range 1..99" "range diagnostic")

# --- event node: strict rejects, best-effort documents the default --------
set(EVENT "${ROOT}/event.yaml")
file(WRITE "${EVENT}" [==[version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: ev
    type: tick
    hz: 100
    process: main
    exec: event
edges: []
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 1
]==])
execute_process(COMMAND "${GEN}" --in "${EVENT}" --explain-sched
    --sched-priority=strict
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "strict event exit ${rc}, expected 2\n${out}${err}")
endif()
expect_contains("${err}" "no period for rate-monotonic priority" "strict event")
execute_process(COMMAND "${GEN}" --in "${EVENT}" --explain-sched
  RESULT_VARIABLE rc OUTPUT_VARIABLE json ERROR_VARIABLE human)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "best-effort event exit ${rc}, expected 0\n${json}${human}")
endif()
expect_contains("${json}" "\"priority_source\": \"default\"" "event default")
expect_contains("${json}" "\"priority\": 10" "event default priority")
expect_contains("${human}" "keeping default priority 10" "event warning")

# --- feedback edges and loops are explicitly annotated --------------------
execute_process(COMMAND "${GEN}" --in "${THYME_ABS}/tools/gen/gen_graph/testdata/cycle/feedback.yaml"
    --explain-sched
  RESULT_VARIABLE rc OUTPUT_VARIABLE json ERROR_VARIABLE human)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "feedback explain exit ${rc}\n${json}${human}")
endif()
expect_contains("${json}" "\"feedback_edges\"" "feedback edges")
expect_contains("${json}" "\"loop_nodes\"" "feedback loop nodes")
expect_contains("${json}" "\"in_feedback_loop\": true" "feedback loop marker")
expect_contains("${human}" "loop: node_a -> node_b -> node_c" "human loop")

# --- an unmarked cycle is still rejected before any report ----------------
execute_process(COMMAND "${GEN}" --in "${THYME_ABS}/tools/gen/gen_graph/testdata/cycle/cycle.yaml"
    --explain-sched
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "cycle explain exit ${rc}, expected 2\n${out}${err}")
endif()
expect_contains("${err}" "directed cycle rejected" "cycle rejection")

# --- SCH-12: combined WCET/capacity golden report --------------------------
set(CAPACITY_DIR "${THYME_ABS}/tools/gen/gen_graph/testdata/explain_sched")
execute_process(COMMAND "${GEN}" --in "${CAPACITY_DIR}/capacity.yaml"
    --explain-sched
  RESULT_VARIABLE rc OUTPUT_VARIABLE json ERROR_VARIABLE human)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "capacity explain exit ${rc}\n${json}${human}")
endif()
file(READ "${CAPACITY_DIR}/capacity.expected.json" expected_json)
file(READ "${CAPACITY_DIR}/capacity.expected.txt" expected_human)
if(NOT "${json}" STREQUAL "${expected_json}")
  message(FATAL_ERROR "capacity JSON golden differs; regenerate capacity.expected.json\n${json}")
endif()
if(NOT "${human}" STREQUAL "${expected_human}")
  message(FATAL_ERROR "capacity human golden differs; regenerate capacity.expected.txt\n${human}")
endif()
expect_contains("${json}" "\"admission_status\": \"ok\"" "combined WCET admission")
expect_contains("${json}" "\"kind\": \"lossless\"" "lossless capacity")
expect_contains("${json}" "\"kind\": \"pin_window\"" "BF-07 capacity")
expect_contains("${json}" "\"required_slots\": 6" "capacity formula")

# Capacity declarations survive canonical JSON and produce the identical plan.
execute_process(COMMAND "${GEN}" --in "${CAPACITY_DIR}/capacity.yaml" --emit-json
  RESULT_VARIABLE rc OUTPUT_VARIABLE graph_json ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "capacity --emit-json failed: ${err}")
endif()
file(WRITE "${ROOT}/capacity_graph.json" "${graph_json}")
execute_process(COMMAND "${GEN}" --from-json "${ROOT}/capacity_graph.json"
    --explain-sched
  RESULT_VARIABLE rc OUTPUT_VARIABLE json_roundtrip ERROR_VARIABLE human_roundtrip)
if(NOT rc EQUAL 0 OR NOT "${json_roundtrip}" STREQUAL "${expected_json}" OR
   NOT "${human_roundtrip}" STREQUAL "${expected_human}")
  message(FATAL_ERROR "capacity JSON round-trip changed the report\n${json_roundtrip}${human_roundtrip}")
endif()

# Only the explicit capacity-specific downgrade permits an insufficient ring.
execute_process(COMMAND "${GEN}" --in
    "${THYME_ABS}/tools/gen/gen_graph/testdata/negative/lossless_insufficient.yaml"
    --explain-sched --sched-capacity=best_effort
  RESULT_VARIABLE rc OUTPUT_VARIABLE json ERROR_VARIABLE human)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "best_effort capacity exit ${rc}\n${json}${human}")
endif()
expect_contains("${json}" "\"capacity_status\": \"best_effort\"" "best_effort summary")
expect_contains("${json}" "\"best_effort\": true" "best_effort row")
expect_contains("${human}" "unverified loss may occur" "best_effort warning")
set(BEST_EFFORT_OUT "${ROOT}/best_effort_out")
execute_process(COMMAND "${GEN}" --in
    "${THYME_ABS}/tools/gen/gen_graph/testdata/negative/lossless_insufficient.yaml"
    --out "${BEST_EFFORT_OUT}" --update generated --sched-capacity=best_effort
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT EXISTS "${BEST_EFFORT_OUT}/generated/sched_plan.h")
  message(FATAL_ERROR "best_effort codegen did not continue: ${rc}\n${out}${err}")
endif()
expect_contains("${err}" "best_effort: unverified loss may occur" "codegen downgrade warning")

# --- SCH-13: static phase application (default off) -----------------------
function(expect_absent haystack needle label)
  string(FIND "${haystack}" "${needle}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "${label}: unexpectedly found '${needle}' in:\n${haystack}")
  endif()
endfunction()

file(READ "${OUT}/generated/wiring.cpp" wiring_off)
file(READ "${OUT}/generated/scheduling_apply.inc" apply_off)
expect_absent("${wiring_off}" "phase_mode" "default wiring has no phase")
expect_absent("${apply_off}" "phase_mode" "default apply has no phase")

set(PHASE_CHAIN "${ROOT}/phase_chain.yaml")
file(WRITE "${PHASE_CHAIN}" [==[version: 1
pool_n: 4
ring_cap: 8
phase_mode: best_effort
nodes:
  - id: src
    type: t
    hz: 100
    process: main
    cpu_set: [0]
    wcet_ns: 1000000
  - id: dst
    type: t
    hz: 50
    process: main
    cpu_set: [0]
    wcet_ns: 2000000
edges:
  - from: src
    to: dst
    channel: ch
    cpp_type: Foo
    pool: FooPool
    transport: spsc
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 2
]==])
set(PHASE_OUT "${ROOT}/phase_out")
execute_process(COMMAND "${GEN}" --in "${PHASE_CHAIN}" --out "${PHASE_OUT}"
    --update generated --no-health-reporter
    --template-dir "${THYME_ABS}/tools/gen/gen_graph/testdata/ports_tpl"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "SCH-13 best_effort codegen failed: ${rc}\n${out}${err}")
endif()
file(READ "${PHASE_OUT}/generated/wiring.cpp" phase_wiring)
file(READ "${PHASE_OUT}/generated/scheduling_apply.inc" phase_apply)
set(PHASE_EMITTED "${phase_wiring}${phase_apply}")
expect_contains("${PHASE_EMITTED}"
  "p.phase_mode = thyme::ThreadPlacement::PhaseMode::BestEffort;"
  "emitted best_effort phase_mode")
expect_contains("${PHASE_EMITTED}" "p.phase_ns = 1000000ULL;" "emitted dst phase")

set(PHASE_REQUIRE "${ROOT}/phase_require.yaml")
file(WRITE "${PHASE_REQUIRE}" [==[version: 1
pool_n: 4
ring_cap: 8
phase_mode: require
nodes:
  - id: src
    type: tick
    hz: 100
    process: main
  - id: dst
    type: tick
    hz: 50
    process: main
edges:
  - from: src
    to: dst
    channel: ch
    cpp_type: Tick
    pool: P
    transport: spsc
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 2
]==])
execute_process(COMMAND "${GEN}" --in "${PHASE_REQUIRE}" --explain-sched
  RESULT_VARIABLE rc OUTPUT_VARIABLE json ERROR_VARIABLE human)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "SCH-13 require incomplete should exit 2: ${rc}\n${json}${human}")
endif()
expect_contains("${human}" "SCH-13" "require names SCH-13")
expect_contains("${human}" "dst" "require names incomplete node")

message(STATUS "PERF-02/SCH-02/SCH-05/SCH-11/SCH-12/SCH-13 explain-sched OK")
