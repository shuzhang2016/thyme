# Golden checks on key gen_graph emit outputs (built-in templates), plus the
# catalog-driven extras.txt generalization (custom template dir) and the
# mp wiring enhancements (ports fixture template dir).
if(NOT GEN OR NOT TPL_SRC)
  message(FATAL_ERROR "GEN and TPL_SRC required")
endif()
set(ROOT "${CMAKE_BINARY_DIR}/golden_app")
set(ROOT2 "${CMAKE_BINARY_DIR}/golden_app_extras")
set(ROOT3 "${CMAKE_BINARY_DIR}/golden_app_ports")
set(ROOT4 "${CMAKE_BINARY_DIR}/golden_app_vizjson")
set(TPL "${CMAKE_BINARY_DIR}/golden_tpl")
set(TPL2 "${CMAKE_BINARY_DIR}/golden_tpl_ports")
set(TPL3 "${CMAKE_BINARY_DIR}/golden_tpl_vizjson")
file(REMOVE_RECURSE "${ROOT}" "${ROOT2}" "${ROOT3}" "${ROOT4}" "${TPL}" "${TPL2}" "${TPL3}")

execute_process(COMMAND "${GEN}" --out "${ROOT}" --update all
  --no-health-reporter RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph --update all failed: ${rc}")
endif()

function(expect_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "golden: ${file} missing expected content: ${needle}")
  endif()
endfunction()

function(expect_graph_identity root)
  file(READ "${root}/generated/.gen_meta.json" meta_body)
  string(REGEX MATCH "\"source_graph_hash\": \"([0-9a-f]+)\"" hash_row "${meta_body}")
  if(hash_row STREQUAL "")
    message(FATAL_ERROR "golden: ${root} has no source graph hash")
  endif()
  set(graph_hash "${CMAKE_MATCH_1}")
  if(graph_hash STREQUAL "0000000000000000")
    message(FATAL_ERROR "golden: ${root} generated a zero graph hash")
  endif()
  expect_contains("${root}/generated/graph_ir.h"
    "inline constexpr uint64_t kGraphHash = 0x${graph_hash}ULL;")
  expect_contains("${root}/generated/graph_ir.h"
    "static_assert(kGraphHash != 0, \"generated graph hash must be non-zero\");")
  expect_contains("${root}/src/mp/mp_supervisor_adapter.cpp" "cfg.graph_hash = kGraphHash;")
  expect_contains("${root}/src/mp/mp_roles.cpp" "0, /*record_ring=*/true, kGraphHash")
endfunction()

expect_graph_identity("${ROOT}")
expect_contains("${ROOT}/generated/wiring.h" "thyme::App app_;")
expect_contains("${ROOT}/generated/wiring.h" "std::array<thyme::HealthSlot")
expect_contains("${ROOT}/generated/wiring.cpp" "thyme::App::InstallStopSignals();")
expect_contains("${ROOT}/generated/mp_unknown.h" "inline thyme::CyclicThread* MakeUnknownThread(")

# L8: the default scaffold graph (single tick, no edges) emits no
# record/replay/record-check artifacts. Assert their absence here; the record
# shapes are checked against the ports fixture (which has edges) below.
foreach(_recf record_registry.h record_table.h record_tap_helpers.h
             graph_record_check.h replay_inject.h replay_stages.h)
  if(EXISTS "${ROOT}/generated/${_recf}")
    message(FATAL_ERROR "default scaffold must not emit generated/${_recf}")
  endif()
endforeach()

# extras.txt pipe format: name | cpp_type | read_types | write_types
file(COPY "${TPL_SRC}/" DESTINATION "${TPL}")
file(WRITE "${TPL}/extras.txt" "mybuf | MyCustomBuffer | tick |\n")
execute_process(COMMAND "${GEN}" --out "${ROOT2}" --update all
  --template-dir "${TPL}" --no-health-reporter RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph with --template-dir failed: ${rc}")
endif()
expect_graph_identity("${ROOT2}")
expect_contains("${ROOT2}/generated/wiring.h" "MyCustomBuffer mybuf;")
expect_contains("${ROOT2}/generated/mp_unknown.h" "RoleLocals& l")
expect_contains("${ROOT2}/generated/integration.md" "MyCustomBuffer")

# mp wiring enhancements: run gen_graph against the ports fixture (one known
# consumer, one unknown producer, a trailing health_reporter) and check the
# new generated shape.
file(COPY "${TPL2_SRC}/" DESTINATION "${TPL2}")
execute_process(COMMAND "${GEN}" --out "${ROOT3}" --update all
  --template-dir "${TPL2}" --in "${TPL2}/graph.yaml" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (ports fixture) failed: ${rc}")
endif()
expect_graph_identity("${ROOT3}")
expect_contains("${ROOT3}/generated/graph_ir.h" "kGraphNodesCount = 3")
expect_contains("${ROOT3}/generated/graph_check.h" "must be the last node")
expect_contains("${ROOT3}/generated/mp_roles.h" "inline thyme::CyclicThread* MakeKnownThread(")
expect_contains("${ROOT3}/generated/mp_roles.h" "kMpWorkerCount = 2")
expect_contains("${ROOT3}/generated/mp_roles.h" "return new ConsumerThread(")
expect_contains("${ROOT3}/generated/mp_roles.h"
  "return new HealthReporterThread(health_slots, kMpWorkerCount, kMpWorkerHz, &health_slots[kGraphHealthN - 1]);")
expect_contains("${ROOT3}/generated/mp_ports.h" "struct MpContext")
expect_contains("${ROOT3}/generated/mp_ports.h" "struct ConsumerThreadPorts")
expect_contains("${ROOT3}/generated/mp_ports.h" "thyme::LocalIndexQueue<FooPool, kRingCap> foo_queue")
expect_contains("${ROOT3}/generated/mp_ports.h" "kRingCap> foo_queue")
expect_contains("${ROOT3}/generated/wiring.cpp"
  "consumer(ConsumerThreadPorts{thyme::LocalIndexQueue<FooPool, kRingCap>(&foo_pool, &foo_ring)}, MpContext{&health[1]})")
expect_contains("${ROOT3}/generated/wiring.cpp"
  "prod(ProducerPorts{thyme::LocalIndexQueue<FooPool, kRingCap>(&foo_pool, &foo_ring)}, MpContext{&health[0]})")
expect_contains("${ROOT3}/generated/mp_roles.h"
  "return new ConsumerThread(ConsumerThreadPorts{thyme::LocalIndexQueue<FooPool, kRingCap>(&l.foo_pool, &l.foo_ring)}, MpContext{&health_slots[1]});")
expect_contains("${ROOT}/generated/wiring.cpp" "tick(TickPorts{}, MpContext{&health[0]})")

# Record shapes live in the ports fixture now (it has an edge, so the
# record/replay artifacts are still emitted).
expect_contains("${ROOT3}/generated/record_table.h" "inline thyme::ThymeRecordTable GetThymeRecordTable()")
expect_contains("${ROOT3}/generated/record_table.h" "thyme::ThymeStructDesc* out")
expect_contains("${ROOT3}/generated/record_registry.h" "thyme::RecordMirrorRingPage")
expect_contains("${ROOT3}/generated/record_registry.h" "kRecordMirrorRingSpecs[]")
expect_contains("${ROOT3}/generated/record_table.h" "ThymeRecordMirrorRingRequiredBytes()")
expect_contains("${ROOT3}/src/support/replay_runner.cpp"
  "reader.SeekToMonoNs(opt.from_mono_ns)")
expect_contains("${ROOT3}/src/support/replay_runner.cpp"
  "thyme::replay::SmallStructInjector injector;")
expect_contains("${ROOT3}/src/support/replay_runner.cpp"
  "thyme::replay::ResolveTopologyMapping(")
expect_contains("${ROOT3}/src/support/replay_runner.cpp"
  "return ReplayInjectOne(*static_cast<Pipeline*>(ctx), target_struct_id,")
expect_contains("${ROOT}/CMakeLists.txt" "thyme::replay")
expect_contains("${ROOT3}/CMakeLists.txt" "thyme::replay")

# Record/replay cross-cutting in the generated base: unknown-type bases guard
# Loop with the replay pause check and sink the mirror page + tap into the
# base; hand nodes override OnLoop with algorithm only.
expect_contains("${ROOT3}/generated/prod_generated.h" "void Loop() override final")
expect_contains("${ROOT3}/generated/prod_generated.h" "ReplayProducerShouldPause(name())")
expect_contains("${ROOT3}/generated/prod_generated.h" "RecordTapMirror(record_page_")
expect_contains("${ROOT3}/generated/prod_generated.h" "virtual bool OnTick(const TickIn& in, TickOut* out) = 0;")
expect_contains("${ROOT}/generated/tick_generated.h" "void Loop() override final")
expect_contains("${ROOT}/generated/tick_generated.h" "virtual void OnLoop() = 0;")

# GEN-04: viz_ser mode is declared by the template (contracts_json.txt), not
# probed from the app tree. No marker -> generic base64 envelope fallback...
expect_contains("${ROOT3}/generated/viz_ser.h" "payload_b64")
expect_contains("${ROOT3}/generated/viz_ser.h" "thyme/contracts/frame_header.h")
# ...marker present -> per-slot dispatch through the contracts JSON header.
file(COPY "${TPL2_SRC}/" DESTINATION "${TPL3}")
file(WRITE "${TPL3}/contracts_json.txt" "dummy/contracts/contracts_json.h\n")
execute_process(COMMAND "${GEN}" --out "${ROOT4}" --update generated
  --template-dir "${TPL3}" --in "${TPL3}/graph.yaml" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (viz contracts_json fixture) failed: ${rc}")
endif()
expect_contains("${ROOT4}/generated/viz_ser.h"
  "#include \"dummy/contracts/contracts_json.h\"")
expect_contains("${ROOT4}/generated/viz_ser.h" "thyme_contracts_json::ToJson(v, *out);")
expect_contains("${ROOT4}/generated/viz_ser.h" "thyme_contracts_json::kSchema_Foo")
