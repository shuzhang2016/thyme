thyme_test(test_record_mirror)
add_executable(test_record_control tests/test_record_control.cpp)
target_link_libraries(test_record_control PRIVATE thyme_recorder)
add_executable(test_record_large_payload tests/test_record_large_payload.cpp)
target_link_libraries(test_record_large_payload PRIVATE thyme_recorder)
add_executable(test_record_mirror_client tests/test_record_mirror_client.cpp)
target_link_libraries(test_record_mirror_client PRIVATE thyme_recorder)
add_executable(test_record_mirror_ring tests/test_record_mirror_ring.cpp)
target_link_libraries(test_record_mirror_ring PRIVATE thyme_recorder)
add_executable(test_tap_in_primitives tests/test_tap_in_primitives.cpp)
target_link_libraries(test_tap_in_primitives PRIVATE thyme_recorder Threads::Threads)
add_executable(test_tap_cross_process tests/test_tap_cross_process.cpp)
target_link_libraries(test_tap_cross_process PRIVATE thyme_recorder Threads::Threads)
add_executable(test_record_mirror_ring_mp tests/test_record_mirror_ring_mp.cpp)
target_link_libraries(test_record_mirror_ring_mp PRIVATE thyme_recorder Threads::Threads)
add_executable(test_trigger_capture_pipeline tests/test_trigger_capture_pipeline.cpp)
target_link_libraries(test_trigger_capture_pipeline PRIVATE thyme_recorder Threads::Threads)
add_executable(test_crash_injection tests/test_crash_injection.cpp)
target_link_libraries(test_crash_injection PRIVATE thyme_recorder Threads::Threads)
add_executable(test_triggered_recorder tests/test_triggered_recorder.cpp)
target_link_libraries(test_triggered_recorder PRIVATE thyme_recorder Threads::Threads)
add_executable(test_thyme_bin_trigger tests/test_thyme_bin_trigger.cpp)
target_link_libraries(test_thyme_bin_trigger PRIVATE thyme_recorder)
add_executable(test_pinned_slot_drain_tear tests/test_pinned_slot_drain_tear.cpp)
target_link_libraries(test_pinned_slot_drain_tear PRIVATE thyme_recorder Threads::Threads)
add_executable(test_pinned_slot_raw_crc tests/test_pinned_slot_raw_crc.cpp)
target_link_libraries(test_pinned_slot_raw_crc PRIVATE thyme_recorder)
add_executable(test_pinned_slot_lease_abi tests/test_pinned_slot_lease_abi.cpp)
target_link_libraries(test_pinned_slot_lease_abi PRIVATE thyme_recorder)
add_executable(test_pinned_slot_lease_pin tests/test_pinned_slot_lease_pin.cpp)
target_link_libraries(test_pinned_slot_lease_pin PRIVATE thyme_recorder)
add_executable(test_pinned_slot_lease_capacity tests/test_pinned_slot_lease_capacity.cpp)
target_link_libraries(test_pinned_slot_lease_capacity PRIVATE thyme_recorder)
add_executable(test_pinned_slot_lease_steal tests/test_pinned_slot_lease_steal.cpp)
target_link_libraries(test_pinned_slot_lease_steal PRIVATE thyme_recorder Threads::Threads)
add_executable(test_pinned_slot_lease_epoch tests/test_pinned_slot_lease_epoch.cpp)
target_link_libraries(test_pinned_slot_lease_epoch PRIVATE thyme_recorder Threads::Threads)
add_executable(test_pinned_slot_lease_copy tests/test_pinned_slot_lease_copy.cpp)
target_link_libraries(test_pinned_slot_lease_copy PRIVATE thyme_recorder Threads::Threads)
add_executable(test_lease_live_pin_not_stolen tests/test_lease_live_pin_not_stolen.cpp)
target_link_libraries(test_lease_live_pin_not_stolen PRIVATE thyme_recorder)

# BF-12/BF-13/BF-14: lease observability, snapshot ownership, reader facade.
add_executable(test_lease_observability tests/test_lease_observability.cpp)
target_link_libraries(test_lease_observability PRIVATE thyme_runtime)
add_executable(test_snapshot_ownership tests/test_snapshot_ownership.cpp)
target_link_libraries(test_snapshot_ownership PRIVATE thyme_runtime)
add_executable(test_reader_facade tests/test_reader_facade.cpp)
target_link_libraries(test_reader_facade PRIVATE thyme_runtime)

add_executable(test_pinned_slot_view_lifetime tests/test_pinned_slot_view_lifetime.cpp)
target_link_libraries(test_pinned_slot_view_lifetime PRIVATE thyme_recorder Threads::Threads)
# Debug variant: omit NDEBUG so the View destructor validation is active.
add_executable(test_pinned_slot_view_debug tests/test_pinned_slot_view_lifetime.cpp)
target_link_libraries(test_pinned_slot_view_debug PRIVATE thyme_recorder Threads::Threads)
target_compile_options(test_pinned_slot_view_debug PRIVATE -UNDEBUG)
target_compile_definitions(test_pinned_slot_view_debug PRIVATE RUN_DEBUG_ABORT)

# The AcquireLease failure-path watch is observable only in a debug (non-NDEBUG) build.
add_executable(test_pinned_slot_lease_race_debug tests/test_pinned_slot_lease_race_debug.cpp)
target_link_libraries(test_pinned_slot_lease_race_debug PRIVATE thyme_recorder Threads::Threads)
target_compile_options(test_pinned_slot_lease_race_debug PRIVATE -UNDEBUG)
add_executable(test_post_grace_preempt tests/test_post_grace_preempt.cpp)
target_link_libraries(test_post_grace_preempt PRIVATE thyme_recorder)
add_executable(test_roll_after_close_failure tests/test_roll_after_close_failure.cpp)
target_link_libraries(test_roll_after_close_failure PRIVATE thyme_recorder)
add_executable(test_trigger_priority_clamp tests/test_trigger_priority_clamp.cpp)
target_link_libraries(test_trigger_priority_clamp PRIVATE thyme_recorder)
add_executable(test_trigger_busy tests/test_trigger_busy.cpp)
target_link_libraries(test_trigger_busy PRIVATE thyme_recorder Threads::Threads)
add_executable(test_quota_class_typo tests/test_quota_class_typo.cpp)
target_link_libraries(test_quota_class_typo PRIVATE thyme_recorder)
add_executable(test_trigger_preempt_fail tests/test_trigger_preempt_fail.cpp)
target_link_libraries(test_trigger_preempt_fail PRIVATE thyme_recorder)
add_executable(test_trigger_timestamp tests/test_trigger_timestamp.cpp)
target_link_libraries(test_trigger_timestamp PRIVATE thyme_recorder)
add_executable(test_idle_predicate tests/test_idle_predicate.cpp)
target_link_libraries(test_idle_predicate PRIVATE thyme_recorder)
add_executable(test_perf_dump_on_crash tests/test_perf_dump_on_crash.cpp)
target_link_libraries(test_perf_dump_on_crash PRIVATE thyme_recorder Threads::Threads)
add_executable(test_trigger_center_fsm tests/test_trigger_center_fsm.cpp)
target_link_libraries(test_trigger_center_fsm PRIVATE thyme_recorder Threads::Threads)
# OBS-05: structured FSM transition event ring + crash-safe JSONL dump.
add_executable(test_trigger_fsm_events tests/test_trigger_fsm_events.cpp)
target_link_libraries(test_trigger_fsm_events PRIVATE thyme_recorder Threads::Threads)
add_executable(test_trigger_config tests/test_trigger_config.cpp)
target_link_libraries(test_trigger_config PRIVATE thyme_recorder)
# GEN-12/TST-11: config [channel.*] must be a subset of the generated manifest.
add_executable(test_record_manifest_subset tests/test_record_manifest_subset.cpp)
target_link_libraries(test_record_manifest_subset PRIVATE thyme_recorder)
# GEN-12/TST-11: the runtime arena is sized from the manifest count.
add_executable(test_record_manifest_arena tests/test_record_manifest_arena.cpp)
target_link_libraries(test_record_manifest_arena PRIVATE thyme_recorder)
# DOC-08: doc audit table <-> parser key-set sync + shipped-example boot check.
add_executable(test_trigger_config_doc tests/test_trigger_config_doc.cpp)
target_link_libraries(test_trigger_config_doc PRIVATE thyme_recorder)
target_compile_definitions(test_trigger_config_doc PRIVATE
  THYME_DOC_TRIGGER_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/docs/guides/trigger-config.md"
  THYME_EXAMPLE_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tools/apps/trigger_center")
# ARCH-13: generated [trigger_center] schema -> parser/dump/defaults/reload gate.
add_executable(test_trigger_config_schema tests/test_trigger_config_schema.cpp)
target_link_libraries(test_trigger_config_schema PRIVATE thyme_recorder)
add_executable(test_trigger_channel_filter tests/test_trigger_channel_filter.cpp)
target_link_libraries(test_trigger_channel_filter PRIVATE thyme_recorder Threads::Threads)
add_executable(test_trigger_protocol tests/test_trigger_protocol.cpp)
target_link_libraries(test_trigger_protocol PRIVATE thyme_recorder Threads::Threads)
add_executable(test_trigger_security tests/test_trigger_security.cpp)
target_link_libraries(test_trigger_security PRIVATE thyme_recorder Threads::Threads)
# TRG-07: Cancel(trigger_id) + async outcome notification.
add_executable(test_trigger_cancel tests/test_trigger_cancel.cpp)
target_link_libraries(test_trigger_cancel PRIVATE thyme_recorder Threads::Threads)
add_executable(test_trigger_notify tests/test_trigger_notify.cpp)
target_link_libraries(test_trigger_notify PRIVATE thyme_recorder Threads::Threads)
# TRG-09: storm/overflow/malformed coverage. The storm binary has two modes:
# deterministic bursts (mr_gate) and a paced ~10k/s socket soak ("storm"
# label only, not gated).
add_executable(test_trigger_storm tests/test_trigger_storm.cpp)
target_link_libraries(test_trigger_storm PRIVATE thyme_recorder Threads::Threads)
add_executable(test_trigger_overflow tests/test_trigger_overflow.cpp)
target_link_libraries(test_trigger_overflow PRIVATE thyme_recorder Threads::Threads)
add_executable(test_trigger_malformed tests/test_trigger_malformed.cpp)
target_link_libraries(test_trigger_malformed PRIVATE thyme_recorder Threads::Threads)
# TRG-12: structured audit records (accept/reject/preempt/quota-override),
# crash safety (fork+SIGKILL) and drop accounting.
add_executable(test_trigger_audit tests/test_trigger_audit.cpp)
target_link_libraries(test_trigger_audit PRIVATE thyme_recorder Threads::Threads)
# TRG-14: trigger->seal latency measurement plumbing — accept/seal timestamp
# presence and ordering, per-trigger-id pairing (incl. preempt), wal seal-row
# journal stamp, aborted captures leave no seal endpoint. Deterministic.
add_executable(test_trigger_seal_latency tests/test_trigger_seal_latency.cpp)
target_link_libraries(test_trigger_seal_latency PRIVATE thyme_recorder Threads::Threads)
add_executable(test_trigger_e2e tests/test_trigger_e2e.cpp)
target_link_libraries(test_trigger_e2e PRIVATE thyme_recorder Threads::Threads)
add_executable(test_idle_flush tests/test_idle_flush.cpp)
target_link_libraries(test_idle_flush PRIVATE thyme_recorder)
add_executable(test_rate_limit tests/test_rate_limit.cpp)
target_link_libraries(test_rate_limit PRIVATE thyme_recorder)
add_executable(test_retention tests/test_retention.cpp)
target_link_libraries(test_retention PRIVATE thyme_recorder Threads::Threads)
add_executable(test_package_lifecycle tests/test_package_lifecycle.cpp)
target_link_libraries(test_package_lifecycle PRIVATE thyme_recorder Threads::Threads)
add_executable(test_retention_symlink tests/test_retention_symlink.cpp)
target_link_libraries(test_retention_symlink PRIVATE thyme_recorder Threads::Threads)
add_executable(test_continuous tests/test_continuous.cpp)
target_link_libraries(test_continuous PRIVATE thyme_recorder Threads::Threads)
# REP-07 follow-up: continuous slices keep per-source mono_ns order (sort +
# watermark demotion); verified through ReplayPackage.
add_executable(test_continuous_mono_order tests/test_continuous_mono_order.cpp)
target_link_libraries(test_continuous_mono_order PRIVATE thyme_replay thyme_recorder Threads::Threads)
add_executable(test_continuous_pinned_slot tests/test_continuous_pinned_slot.cpp)
target_link_libraries(test_continuous_pinned_slot PRIVATE thyme_recorder Threads::Threads)
add_executable(test_continuous_lease_tear tests/test_continuous_lease_tear.cpp)
target_link_libraries(test_continuous_lease_tear PRIVATE thyme_recorder Threads::Threads)
add_executable(test_trigger_prescan_boundary tests/test_trigger_prescan_boundary.cpp)
target_link_libraries(test_trigger_prescan_boundary PRIVATE thyme_recorder Threads::Threads)
add_executable(test_pinned_slot_channel tests/test_pinned_slot_channel.cpp)
target_link_libraries(test_pinned_slot_channel PRIVATE thyme_runtime Threads::Threads)
thyme_test(test_thyme_bin)
thyme_test(test_thyme_bin_tcp)
thyme_test(test_thyme_bin_gap)
thyme_test(test_thyme_bin_session)
thyme_test(test_thyme_bin_corrupt)
thyme_test(test_thyme_bin_index)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(test_thyme_bin_index PRIVATE "LINKER:--wrap=fseek")
endif()
add_executable(test_thyme_bin_fanout tests/test_thyme_bin_fanout.cpp)
target_link_libraries(test_thyme_bin_fanout PRIVATE thyme_recorder)
add_executable(test_thyme_bin_fdsink tests/test_thyme_bin_fdsink.cpp)
target_link_libraries(test_thyme_bin_fdsink PRIVATE thyme_recorder)
# REC-06: streaming MCAP writer and .thyme.bin -> MCAP export.
add_executable(test_mcap_writer tests/test_mcap_writer.cpp)
target_link_libraries(test_mcap_writer PRIVATE thyme_runtime)
add_executable(test_mcap_export tests/test_mcap_export.cpp)
target_link_libraries(test_mcap_export PRIVATE thyme_runtime)
# ECO-01: structural (not just payload round-trip) MCAP validation — record
# framing, summary offsets, statistics, footer/data CRCs and unchunked shape.
add_executable(test_mcap_verify tests/test_mcap_verify.cpp)
target_link_libraries(test_mcap_verify PRIVATE thyme_runtime)
# ECO-12: test-only fixture preparation and normalized self-check dump for the
# official Python MCAP reader comparison. No third-party library is linked.
add_executable(eco12_mcap_fixture tests/eco12_mcap_fixture.cpp)
target_link_libraries(eco12_mcap_fixture PRIVATE thyme_runtime)
target_include_directories(eco12_mcap_fixture PRIVATE tests)
# ECO-03: the Python reader is tested against packages written by the real
# C++ writer; the fixture binary is only built when a Python interpreter is
# available (the test is registered only then).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_executable(thyme_py_fixture tests/thyme_py_fixture.cpp)
  target_link_libraries(thyme_py_fixture PRIVATE thyme_runtime)
endif()
# ECO-09/ECO-11: read-only probe API over LatestSlot/SPSC/SPMC/PinnedSlot.
add_executable(test_thyme_probe tests/test_thyme_probe.cpp)
target_link_libraries(test_thyme_probe PRIVATE thyme_probe_core Threads::Threads)

# TST-03 — structured fuzz drivers (deterministic splitmix64 mutator + seed
# corpus runner; no libFuzzer dependency, runs as ordinary ctest). Each
# binary keeps a THYME_FUZZ_LIBFUZZER entry point for a future Clang build.
# The quick pass (small fixed iteration count) gates MRs; the full pass runs
# in the nightly full-suite job under the "fuzz" label.
add_executable(fuzz_thyme_bin tests/fuzz_thyme_bin.cpp)
target_link_libraries(fuzz_thyme_bin PRIVATE thyme_runtime)
target_include_directories(fuzz_thyme_bin PRIVATE tests)
target_compile_definitions(fuzz_thyme_bin PRIVATE
  THYME_FUZZ_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_corpus/thyme_bin")
add_executable(fuzz_trigger_protocol tests/fuzz_trigger_protocol.cpp)
target_link_libraries(fuzz_trigger_protocol PRIVATE thyme_recorder
  Threads::Threads)
target_include_directories(fuzz_trigger_protocol PRIVATE tests)
target_compile_definitions(fuzz_trigger_protocol PRIVATE
  THYME_FUZZ_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_corpus/trigger_protocol")
add_executable(fuzz_trigger_config tests/fuzz_trigger_config.cpp)
target_link_libraries(fuzz_trigger_config PRIVATE thyme_recorder)
target_include_directories(fuzz_trigger_config PRIVATE tests)
target_compile_definitions(fuzz_trigger_config PRIVATE
  THYME_FUZZ_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_corpus/trigger_config")
