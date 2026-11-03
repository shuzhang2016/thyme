enable_testing()
add_subdirectory(tools/gen/gen_graph)
add_subdirectory(tools/apps/trigger_center)
# TRG-17/OPS-13: legacy and generated-base + policy CLI contracts, including
# effective dump, resource JSON and policy channel rejection.
add_test(NAME test_trigger_config_cli
  COMMAND ${CMAKE_COMMAND}
    -DTRIGGER_CENTER=$<TARGET_FILE:trigger_center>
    -DMINIMAL_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tools/apps/trigger_center/trigger_config.minimal.ini
    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test_trigger_config_cli
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/test_trigger_config_cli.cmake)
# OPS-11: deploy-unit contract (directives, start-up gates, ordering) plus an
# optional systemd-analyze syntax pass over staged copies.
add_test(NAME test_deploy_units
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/check_deploy_units.py
    --root ${CMAKE_CURRENT_SOURCE_DIR} --self-test)
set_tests_properties(test_deploy_units PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
# OPS-11: real readiness handshake -- READY=1 only after sources attach, then
# WATCHDOG=1 and a clean STOPPING=1 on SIGTERM. Bounded (< 15 s) and uses
# pid-scoped SHM names/processes, so it can join the MR gate.
add_test(NAME test_trigger_center_ready
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_trigger_center_ready.py
    --trigger-center $<TARGET_FILE:trigger_center>
    --demo-producer $<TARGET_FILE:demo_producer>)
set_tests_properties(test_trigger_center_ready PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
# TRG-15: default process split seals a package, and the in-process path
# remains available for the first-payload comparison.
add_test(NAME test_trigger_center_split
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_trigger_center_split.py
    --trigger-center $<TARGET_FILE:trigger_center>
    --demo-producer $<TARGET_FILE:demo_producer>
    --triggerctl $<TARGET_FILE:triggerctl>)
# TRG-15: the test forks a real trigger_center parent/child pair and waits on
# datagrams between them; under `ctest -j8` the timing window is not reliable
# (it passed standalone but failed in a full parallel run), so it owns a serial
# slot instead of widening its waits.
set_tests_properties(test_trigger_center_split PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 90 RUN_SERIAL TRUE)
# ECO-02: triggerctl CLI contract (usage/exit codes, status request/response
# JSON shape, verify document) against a fake control socket.
add_test(NAME test_triggerctl_cli
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_triggerctl_cli.py
    $<TARGET_FILE:triggerctl>)
set_tests_properties(test_triggerctl_cli PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_subdirectory(tools/replay/mcap_export)
add_subdirectory(tools/inspect/thyme_probe)
# OBS-06: multi-process log merge (.fsm JSONL + T9P1 perf dumps).
add_subdirectory(tools/inspect/thyme_log_merge)
add_executable(test_thyme_log_merge tests/test_thyme_log_merge.cpp)
target_link_libraries(test_thyme_log_merge PRIVATE thyme_log_merge_lib
  thyme_runtime Threads::Threads)
# OPS-05: rotation/budget geometry for fsm/audit/perf, and merging the `.1..N`
# generations with thyme_log_merge (same library the CLI runs).
thyme_test(test_log_rotation thyme_log_merge_lib thyme_recorder)
add_subdirectory(tools/replay/thyme-replay)
# OPS-08: granular fault-matrix roles (kill -9 producer/reader/supervisor,
# bounded SHM, injected ENOSPC, simulated clock jump).
add_subdirectory(tools/ops/fault_matrix)
# OPS-08: the fault matrix forks and SIGKILLs real processes and owns
# /dev/shm objects, so it is not part of the MR gate. Bounded (< 60 s) and
# deterministic; run it nightly/manually with `ctest -L fault`.
add_test(NAME test_fault_matrix
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ops/fault_matrix/fault_matrix.py
    --helper $<TARGET_FILE:fault_role>
    --json ${CMAKE_CURRENT_BINARY_DIR}/fault_matrix.json)
set_tests_properties(test_fault_matrix PROPERTIES
  LABELS "fault" TIMEOUT 60)
# REP-08/REC-07: the player CLI's logic is a library so the in-process test
# exercises the same parsing/runner the binary uses (main.cpp stays thin).
thyme_test(test_replay_cli thyme_replay_cli_lib thyme_recorder Threads::Threads)
# REP-06: per-channel seek/index metadata + O(log n) seek on the footer index
# and on the scan-built fallback (synthetic package, no SHM/timing) -> gate.
thyme_test(test_replay_seek_index thyme_replay_cli_lib Threads::Threads)
# REP-09: MCAP/NDJSON export round-trip, schema sidecar linkage, bounded
# writer buffer and CLI errors (synthetic package, no SHM/timing) -> gate.
thyme_test(test_replay_export thyme_replay_cli_lib Threads::Threads)
# ECO-13: self-describing CBOR/MessagePack/TLV export. Codec, fail-closed
# layout and one synthetic package; no SHM and no wall-clock bound.
thyme_test(test_self_desc thyme_replay_cli_lib Threads::Threads)
# REP-15: protobuf/CDR export from a known layout. Wire bytes, fail-closed
# schema and one MCAP schema record. No Foxglove/rosbag2 process.
thyme_test(test_canonical thyme_replay_cli_lib Threads::Threads)
# REC-03: single-.tbin valid-prefix salvage, metadata preservation and atomic
# no-overwrite publication (synthetic files, no SHM/timing) -> gate.
thyme_test(test_replay_repair thyme_replay_cli_lib Threads::Threads)
add_test(NAME test_gen_graph_yaml COMMAND test_gen_graph_yaml)
add_test(NAME test_gen_graph_yaml_string COMMAND test_gen_graph_yaml_string)
add_test(NAME test_gen_graph_rate COMMAND test_gen_graph_rate)
add_test(NAME test_gen_graph_rate_warn COMMAND test_gen_graph_rate_warn)
add_test(NAME test_gen_graph_depth_warn COMMAND test_gen_graph_depth_warn)
add_test(NAME test_gen_contracts
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_contracts/test_gen_contracts.py)
set_tests_properties(test_gen_contracts PROPERTIES LABELS "unit;mr_gate")
# ABI-03: regen a contract header, verify the emitted sizeof/offsetof/alignof
# and array-stride asserts, compile them under the strict warning gate, and
# prove tampered numbers break the build. Fast + deterministic -> mr_gate.
add_test(NAME test_contracts_layout_asserts
  COMMAND ${CMAKE_COMMAND}
    -DPYTHON=python3
    -DCXX=${CMAKE_CXX_COMPILER}
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/test_contracts_layout_asserts
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_contracts/test_layout_asserts.cmake)
set_tests_properties(test_contracts_layout_asserts
  PROPERTIES LABELS "unit;mr_gate" TIMEOUT 60)
# OBS-07: Prometheus textfile bridge (status JSON / thyme_probe / --hist JSONL /
# SEC-02 audit) and the rule+dashboard metric contract. Pure Python: the
# exporter is a sidecar, not a C++ target, so this is its only build gate.
add_test(NAME test_thyme_metrics_exporter
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/inspect/thyme_metrics_exporter/test_thyme_metrics_exporter.py)
set_tests_properties(test_thyme_metrics_exporter PROPERTIES
  LABELS "unit;mr_gate")
# OBS-15: channel observability audit (latest/reject/single-slot pinned must
# be allowlisted with a reason; prints a per-edge detection matrix).
add_test(NAME test_channel_audit
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_channel_audit.py)
set_tests_properties(test_channel_audit PROPERTIES LABELS "unit;mr_gate")

# OBS-10: eval_chains baseline gate (tolerance, missing metric, host mismatch)
# with fixture JSON. The wall-clock scenarios themselves run only in the
# scheduled perf-eval-chains job; MRs gate the comparison logic.
add_test(NAME test_check_eval_chains
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_check_eval_chains.py)
set_tests_properties(test_check_eval_chains PROPERTIES
  LABELS "unit;mr_gate")
# PERF-06: per-runner bench baseline comparison (tolerance, p99/p999, missing
# metric, host mismatch, no baseline), baseline merge/write, and the scheduled
# job's YAML shape. Wall-clock benches run only in nightly-bench.
add_test(NAME test_check_bench
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_check_bench.py)
set_tests_properties(test_check_bench PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
# CI-04/TST-01: exporter path, lcov/llvm parsing, CMake source inventory and
# per-library gate accept/reject behavior. The instrumented suite is nightly.
add_test(NAME test_check_coverage
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_check_coverage.py)
set_tests_properties(test_check_coverage PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
# DOC-01: doc command/target checks and test-count claims (fixture unit tests)
# plus the live command check over this checkout. Counts need a CTest listing
# and are therefore enforced by the test-mr-gate CI job, not here (a sanitizer
# or TSan tree has a different count).
add_test(NAME test_check_doc_commands
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_check_doc_commands.py)
set_tests_properties(test_check_doc_commands PROPERTIES
  LABELS "unit;mr_gate")
add_test(NAME test_doc_commands
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/check_doc_commands.py
    --root ${CMAKE_CURRENT_SOURCE_DIR} --no-counts)
set_tests_properties(test_doc_commands PROPERTIES
  LABELS "unit;mr_gate")
add_test(NAME test_check_coding_rule_assessment
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_check_coding_rule_assessment.py)
set_tests_properties(test_check_coding_rule_assessment PROPERTIES
  LABELS "unit;mr_gate")
add_test(NAME test_coding_rule_assessment
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/check_coding_rule_assessment.py
    --root ${CMAKE_CURRENT_SOURCE_DIR} --build-dir ${CMAKE_BINARY_DIR})
set_tests_properties(test_coding_rule_assessment PROPERTIES
  LABELS "unit;mr_gate")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_test(NAME test_coding_rule_language_subset
    COMMAND python3
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_coding_rule_language_subset.py
      --cxx ${CMAKE_CXX_COMPILER})
  set_tests_properties(test_coding_rule_language_subset PROPERTIES
    LABELS "unit;mr_gate" TIMEOUT 30)
endif()
# ABI-07: the compact matrix in ARCHITECTURE.md must match the version
# constants in the owning headers (and the OPS-06 JSON matrix).
add_test(NAME test_check_abi_matrix
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_check_abi_matrix.py)
set_tests_properties(test_check_abi_matrix PROPERTIES
  LABELS "unit;mr_gate")
add_test(NAME test_doc_abi_matrix
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/check_abi_matrix.py
    --root ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(test_doc_abi_matrix PROPERTIES
  LABELS "unit;mr_gate")
# TST-05: every scoped fix(...) commit needs a regression-corpus row (or an
# explicit no-test marker). Fixture tests drive the parser/scanner against a
# throwaway git repo; the live test checks this checkout's last 250 commits.
add_test(NAME test_check_regression_corpus
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_check_regression_corpus.py)
set_tests_properties(test_check_regression_corpus PROPERTIES
  LABELS "unit;mr_gate")
add_test(NAME test_regression_corpus
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/check_regression_corpus.py
    --repo ${CMAKE_CURRENT_SOURCE_DIR}
    --corpus ${CMAKE_CURRENT_SOURCE_DIR}/docs/reference/regression-corpus.md)
set_tests_properties(test_regression_corpus PROPERTIES
  LABELS "unit;mr_gate")
add_test(NAME test_check_test_environment
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_check_test_environment.py)
set_tests_properties(test_check_test_environment PROPERTIES
  LABELS "unit;mr_gate")
add_test(NAME test_environment_preflight
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/check_test_environment.py)
set_tests_properties(test_environment_preflight PROPERTIES
  LABELS "unit;mr_gate")

# REP-15 cross-check fixture: writes the FileDescriptorSet that
# test_rep15_protobuf.py parses with the official protobuf library.
add_executable(descriptor_fixture tests/descriptor_fixture.cpp)
target_link_libraries(descriptor_fixture PRIVATE thyme_replay_cli_lib)
target_include_directories(descriptor_fixture PRIVATE
  ${CMAKE_SOURCE_DIR}/tools/replay/thyme-replay
  ${CMAKE_SOURCE_DIR}/tests)

# SEC-07: real record_control server used by pentest_record_control.py.
add_executable(pentest_server tests/pentest_server.cpp)
target_link_libraries(pentest_server PRIVATE thyme_recorder)
