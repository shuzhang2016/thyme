add_test(NAME test_empty_app
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_empty_app.cmake)
# The scaffold lib now compiles the real mp role/supervisor adapters (PLAN-3),
# so the serial in-test build needs the same headroom as the other app builds.
set_tests_properties(test_empty_app PROPERTIES TIMEOUT 180)
add_test(NAME test_gen_graph_golden
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTPL_SRC=${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/scaffold
    -DTPL2_SRC=${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/testdata/ports_tpl
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_golden.cmake)
set_tests_properties(test_gen_graph_golden PROPERTIES TIMEOUT 60)
add_test(NAME test_gen_graph_cyclic_golden
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_cyclic_golden.cmake)
set_tests_properties(test_gen_graph_cyclic_golden PROPERTIES TIMEOUT 60)
add_test(NAME test_gen_graph_minimal_norecord
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_minimal_norecord.cmake)
set_tests_properties(test_gen_graph_minimal_norecord PROPERTIES TIMEOUT 60)
add_test(NAME test_gen_graph_health_inject
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_health_inject.cmake)
set_tests_properties(test_gen_graph_health_inject PROPERTIES TIMEOUT 60)
add_test(NAME test_gen_graph_depth
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_depth_emit.cmake)
set_tests_properties(test_gen_graph_depth PROPERTIES TIMEOUT 60)
add_test(NAME test_gen_graph_broadcast_emit
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_broadcast_emit.cmake)
set_tests_properties(test_gen_graph_broadcast_emit PROPERTIES TIMEOUT 60)
add_test(NAME test_gen_graph_broadcast
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_broadcast.cmake)
set_tests_properties(test_gen_graph_broadcast PROPERTIES TIMEOUT 180)
add_test(NAME test_gen_graph_pinned_slot
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_pinned_slot_app.cmake)
set_tests_properties(test_gen_graph_pinned_slot PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_test(NAME test_gen_graph_tick_transports
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_tick_transports.cmake)
set_tests_properties(test_gen_graph_tick_transports PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_test(NAME test_gen_graph_local_hub
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_local_hub_app.cmake)
set_tests_properties(test_gen_graph_local_hub PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_test(NAME test_gen_graph_event
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_event_app.cmake)
set_tests_properties(test_gen_graph_event PROPERTIES TIMEOUT 180)
add_test(NAME test_gen_graph_sharded
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_sharded_app.cmake)
set_tests_properties(test_gen_graph_sharded PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
add_test(NAME test_gen_graph_ports
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -DTPL2_SRC=${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/testdata/ports_tpl
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_ports.cmake)
set_tests_properties(test_gen_graph_ports PROPERTIES TIMEOUT 180)
add_test(NAME test_gen_graph_overlay
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -DOVL_SRC=${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/testdata/overlay_tpl
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_overlay.cmake)
# Unlike the other gen_graph build tests this one was never in the RUN_SERIAL
# list; under `ctest -j8` it competes with several compiler jobs, hit the 180 s
# timeout, and a killed test leaves its session-lease page behind.
set_tests_properties(test_gen_graph_overlay PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
add_test(NAME test_gen_graph_derive_template
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_derive_template.cmake)
set_tests_properties(test_gen_graph_derive_template PROPERTIES TIMEOUT 60)
# GEN-11: DAG validation. Both are fast binary-level checks (no nested build),
# so they join the mr_gate set without RUN_SERIAL.
add_test(NAME test_gen_graph_cycle_reject
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_cycle_reject.cmake)
set_tests_properties(test_gen_graph_cycle_reject PROPERTIES TIMEOUT 60)
add_test(NAME test_gen_graph_feedback_marker
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_feedback_marker.cmake)
set_tests_properties(test_gen_graph_feedback_marker PROPERTIES TIMEOUT 60)
# GEN-02/GEN-05: semantic validation + --check. Fast checks (no nested build),
# so they join the mr_gate set without RUN_SERIAL.
add_test(NAME test_gen_graph_validate COMMAND test_gen_graph_validate)
set_tests_properties(test_gen_graph_validate PROPERTIES LABELS "unit;mr_gate")
add_test(NAME test_channel_capabilities COMMAND test_channel_capabilities)
set_tests_properties(test_channel_capabilities PROPERTIES LABELS "unit;mr_gate")
add_test(NAME test_gen_graph_sched COMMAND test_gen_graph_sched)
set_tests_properties(test_gen_graph_sched PROPERTIES LABELS "unit;mr_gate")
add_test(NAME test_gen_graph_explain_sched
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -DCXX=${CMAKE_CXX_COMPILER}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_explain_sched.cmake)
set_tests_properties(test_gen_graph_explain_sched PROPERTIES
  LABELS "mr_gate" TIMEOUT 60)
add_test(NAME test_gen_graph_validate_cli
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_validate_cli.cmake)
set_tests_properties(test_gen_graph_validate_cli PROPERTIES
  LABELS "mr_gate" TIMEOUT 60)
add_test(NAME test_gen_graph_check
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -DTPL2_SRC=${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/testdata/ports_tpl
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_check.cmake)
set_tests_properties(test_gen_graph_check PROPERTIES
  LABELS "mr_gate" TIMEOUT 60)
# ECO-08: graph.yaml <-> canonical JSON (editor contract) round-trip; strict
# unknown-key/type rejection and byte-identical generation from either input.
add_test(NAME test_gen_graph_json_roundtrip
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_json_roundtrip.cmake)
set_tests_properties(test_gen_graph_json_roundtrip PROPERTIES
  LABELS "mr_gate" TIMEOUT 60)
# GEN-03: atomic generated/ rewrite — stale files removed, user files kept,
# old tree intact + no staging dirs after a mid-fill failure.
add_test(NAME test_gen_graph_atomic
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -DTPL2_SRC=${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/testdata/ports_tpl
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_atomic.cmake)
set_tests_properties(test_gen_graph_atomic PROPERTIES
  LABELS "mr_gate" TIMEOUT 60)
# GEN-07: curated negative goldens — every malformed graph in
# testdata/negative/ must exit 2 with the pinned file:line diagnostic, stay
# deterministic, and leave no partial output. Deterministic and fast, so it
# gates MRs.
add_test(NAME test_gen_graph_negative_goldens
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_negative_goldens.cmake)
set_tests_properties(test_gen_graph_negative_goldens PROPERTIES
  LABELS "mr_gate" TIMEOUT 120)
# GEN-08: generated layout/size static_asserts — emit classes, compile the
# generated headers under the strict warning gate, and reject a schema-known
# contract above the 64 KB Frame transport limit at generation time.
add_test(NAME test_gen_graph_layout_asserts
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -DCXX=${CMAKE_CXX_COMPILER}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_layout_asserts.cmake)
set_tests_properties(test_gen_graph_layout_asserts PROPERTIES
  LABELS "mr_gate" TIMEOUT 60)
# GEN-12/TST-11: record manifest — recorded-only registry/taps/viz/replay/
# gates, channel-name ID ordering, in-process wiring intact, all-record:false
# emits no record artifacts, empty manifest still emitted for --check.
add_test(NAME test_gen_graph_record_manifest
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTPL_SRC=${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/testdata/ports_tpl
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -DCXX=${CMAKE_CXX_COMPILER}
    -DTRIGGER_CENTER=$<TARGET_FILE:trigger_center>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_record_manifest.cmake)
set_tests_properties(test_gen_graph_record_manifest PROPERTIES
  LABELS "mr_gate" TIMEOUT 60)
# GEN-09: generator version / output format metadata. The unit test covers the
# graph hash + JSON round-trip; the fixture test covers `--version`, the
# generated/.gen_meta.json stamp, byte-stable regeneration, and `--check`
# failing on a hand-edited/missing/cross-format/cross-version meta.
add_test(NAME test_gen_graph_meta COMMAND test_gen_graph_meta)
set_tests_properties(test_gen_graph_meta PROPERTIES LABELS "unit;mr_gate")
add_test(NAME test_gen_graph_version
  COMMAND ${CMAKE_COMMAND}
    -DGEN=$<TARGET_FILE:gen_graph>
    -DTHYME=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_gen_graph_version.cmake)
set_tests_properties(test_gen_graph_version PROPERTIES
  LABELS "mr_gate" TIMEOUT 60)

# Fixture tests instantiate a nested CMake tree; two in parallel can race on
# shared build directories (observed broadcast/ports/empty_app flakes under
# -j). Serialize the fixture builders; the pure parser tests stay parallel.
set_tests_properties(
  test_empty_app
  test_gen_graph_broadcast
  test_gen_graph_event
  test_gen_graph_ports
  test_gen_graph_overlay
  PROPERTIES RUN_SERIAL TRUE)
