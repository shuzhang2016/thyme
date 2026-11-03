add_test(NAME test_scaffold COMMAND test_scaffold)
add_test(NAME test_shm_page_header COMMAND test_shm_page_header)
set_tests_properties(test_shm_page_header PROPERTIES LABELS "unit;mr_gate")
add_test(NAME test_frame_header COMMAND test_frame_header)
# ABI-05: FrameHeader field semantics (pure, fast, deterministic).
set_tests_properties(test_frame_header PROPERTIES LABELS "unit;mr_gate")
add_test(NAME test_frame COMMAND test_frame)
set_tests_properties(test_frame PROPERTIES LABELS "unit;mr_gate")
add_test(NAME test_crc32_hw COMMAND test_crc32_hw)
add_test(NAME test_spsc_ring COMMAND test_spsc_ring)
add_test(NAME test_spsc_notify_order COMMAND test_spsc_notify_order)
add_test(NAME test_perf_log COMMAND test_perf_log)
add_test(NAME test_perf_ring COMMAND test_perf_ring)
add_test(NAME test_histogram COMMAND test_histogram)
add_test(NAME test_proc_sampler COMMAND test_proc_sampler)
add_test(NAME test_proc_ext COMMAND test_proc_ext)
set_tests_properties(test_proc_ext PROPERTIES LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_host_sampler COMMAND test_host_sampler)
set_tests_properties(test_host_sampler PROPERTIES LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_notif_pipe COMMAND test_notif_pipe)
add_test(NAME test_queue_push_fail_credit COMMAND test_queue_push_fail_credit)
add_test(NAME test_shm_spsc_ring COMMAND test_shm_spsc_ring)
add_test(NAME test_shm_spmc_ring COMMAND test_shm_spmc_ring)
add_test(NAME test_shm_spsc_begin_commit COMMAND test_shm_spsc_begin_commit)
add_test(NAME test_shm_spmc_begin_commit COMMAND test_shm_spmc_begin_commit)
add_test(NAME test_latest_slot_begin_commit COMMAND test_latest_slot_begin_commit)
add_test(NAME test_single_writer_guard COMMAND test_single_writer_guard)
set_tests_properties(test_single_writer_guard PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_latest_slot_seq_wrap COMMAND test_latest_slot_seq_wrap)
add_test(NAME test_retry_accounting COMMAND test_retry_accounting)
set_tests_properties(test_retry_accounting PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_retry_accounting_shm COMMAND test_retry_accounting_shm)
set_tests_properties(test_retry_accounting_shm PROPERTIES
  LABELS "unit" TIMEOUT 60)
add_test(NAME test_spmc_notify_lifecycle COMMAND test_spmc_notify_lifecycle)
set_tests_properties(test_spmc_notify_lifecycle PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_cacheline_audit COMMAND test_cacheline_audit)
set_tests_properties(test_cacheline_audit PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_copy_in_equivalence COMMAND test_copy_in_equivalence)
add_test(NAME test_event_wake_word COMMAND test_event_wake_word)
add_test(NAME test_wake_coalesce COMMAND test_wake_coalesce)
add_test(NAME test_event_thread COMMAND test_event_thread)
add_test(NAME test_sched_observability COMMAND test_sched_observability)
set_tests_properties(test_sched_observability PROPERTIES TIMEOUT 60)
add_test(NAME test_event_input_budget COMMAND test_event_input_budget)
set_tests_properties(test_event_input_budget PROPERTIES TIMEOUT 30)
add_test(NAME test_overrun_policy COMMAND test_overrun_policy)
set_tests_properties(test_overrun_policy PROPERTIES TIMEOUT 60)
add_test(NAME test_realtime_guard COMMAND test_realtime_guard)
set_tests_properties(test_realtime_guard PROPERTIES TIMEOUT 30)
add_test(NAME test_object_pool COMMAND test_object_pool)
add_test(NAME test_object_pool_gen_wrap COMMAND test_object_pool_gen_wrap)
set_tests_properties(test_object_pool_gen_wrap PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_object_pool_freelist COMMAND test_object_pool_freelist)
set_tests_properties(test_object_pool_freelist PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME bench_object_pool COMMAND bench_object_pool)
# RUN_SERIAL: timing evidence must not share the machine with other tests.
set_tests_properties(bench_object_pool PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
add_test(NAME test_health_slot COMMAND test_health_slot)
# RT-07/ABI-02: pure compile-time layout/endianness gate (fast, deterministic).
add_test(NAME test_abi_layout COMMAND test_abi_layout)
set_tests_properties(test_abi_layout PROPERTIES LABELS "unit;mr_gate")
# ABI-04: cross-version compatibility matrix (fast, deterministic).
add_test(NAME test_abi_cross_version COMMAND test_abi_cross_version)
set_tests_properties(test_abi_cross_version PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_cyclic_thread COMMAND test_cyclic_thread)
add_test(NAME test_cyclic_phase COMMAND test_cyclic_phase)
set_tests_properties(test_cyclic_phase PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 15)
add_test(NAME test_cyclic_eintr COMMAND test_cyclic_eintr)
set_tests_properties(test_cyclic_eintr PROPERTIES TIMEOUT 15)
add_test(NAME test_latest_slot COMMAND test_latest_slot)
# ARCH-19: zero-copy pinned latest store (large payload, one writer, N readers).
add_test(NAME test_pinned_latest_store COMMAND test_pinned_latest_store)
set_tests_properties(test_pinned_latest_store PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_pinned_spmc_ring COMMAND test_pinned_spmc_ring)
set_tests_properties(test_pinned_spmc_ring PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_channel_facade COMMAND test_channel_facade)
set_tests_properties(test_channel_facade PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_local_index_queue COMMAND test_local_index_queue)
set_tests_properties(test_local_index_queue PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_local_hub COMMAND test_local_hub)
set_tests_properties(test_local_hub PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME bench_channel_facade
  COMMAND bench_channel_facade --json bench_channel_facade.json)
set_tests_properties(bench_channel_facade PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
add_test(NAME test_write_section COMMAND test_write_section)
set_tests_properties(test_write_section PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
# ARCH-11: v3 described LatestSlot page — model geometry, combined PageSpec
# identity, and rejection of wrong layout/page_bytes/feature/version and
# legacy v2 pages. Fast and deterministic -> mr_gate.
add_test(NAME test_latest_slot_page COMMAND test_latest_slot_page)
set_tests_properties(test_latest_slot_page PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
# ARCH-11 pipeline: supervisor + forked role + probe over one v3 page.
add_test(NAME test_latest_slot_pipeline COMMAND test_latest_slot_pipeline)
set_tests_properties(test_latest_slot_pipeline PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_seqlock_payload_model COMMAND test_seqlock_payload_model)
set_tests_properties(test_seqlock_payload_model PROPERTIES TIMEOUT 60)
add_test(NAME bench_atomic_payload COMMAND bench_atomic_payload)
set_tests_properties(bench_atomic_payload PROPERTIES TIMEOUT 180)
# PERF-07: correctness-gated only; timing evidence runs without competing tests.
add_test(NAME bench_copy_budget COMMAND bench_copy_budget --json bench_copy_budget.json)
set_tests_properties(bench_copy_budget PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
# PERF-15: short correctness-gated A/B; intentionally not an mr_gate timing test.
add_test(NAME bench_pinned_slot_copy
  COMMAND bench_pinned_slot_copy --json bench_pinned_slot_copy.json)
set_tests_properties(bench_pinned_slot_copy PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
# CI-05: writes <build>/bench_core.json (ctest runs in the build dir) so a
# local `gen_acceptance.py --build-dir build` sees a fresh bench run.
add_test(NAME bench_core COMMAND bench_core --json bench_core.json)
# RUN_SERIAL: timing evidence must not share the machine with other tests.
set_tests_properties(bench_core PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
# TRG-14: same RUN_SERIAL rule as bench_core -- latency evidence.
add_test(NAME bench_trigger_seal
  COMMAND bench_trigger_seal --json bench_trigger_seal.json)
set_tests_properties(bench_trigger_seal PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_test(NAME test_latest_slot_concurrent COMMAND test_latest_slot_concurrent)
add_test(NAME test_latest_slot_stress COMMAND test_latest_slot_stress)
set_tests_properties(test_latest_slot_stress PROPERTIES TIMEOUT 30)
add_test(NAME test_shared_health_stress COMMAND test_shared_health_stress)
set_tests_properties(test_shared_health_stress PROPERTIES TIMEOUT 30)
add_test(NAME test_object_pool_concurrent COMMAND test_object_pool_concurrent)
add_test(NAME test_shm COMMAND test_shm)
add_test(NAME test_shm_region_construct COMMAND test_shm_region_construct)
add_test(NAME test_shm_lifecycle COMMAND test_shm_lifecycle)
set_tests_properties(test_shm_lifecycle PROPERTIES TIMEOUT 60)
add_test(NAME test_app COMMAND test_app)
add_test(NAME test_log COMMAND test_log)
set_tests_properties(test_log PROPERTIES TIMEOUT 30)
add_test(NAME test_core_no_alloc COMMAND test_core_no_alloc)
set_tests_properties(test_core_no_alloc PROPERTIES TIMEOUT 30)
add_test(NAME test_shared_health COMMAND test_shared_health)
add_test(NAME test_replay_mode COMMAND test_replay_mode)
add_test(NAME test_replay_backpressure COMMAND test_replay_backpressure)
add_test(NAME test_replay_reader COMMAND test_replay_reader)
set_tests_properties(test_replay_reader PROPERTIES TIMEOUT 60)
add_test(NAME test_replay_session COMMAND test_replay_session)
set_tests_properties(test_replay_session PROPERTIES TIMEOUT 60)
add_test(NAME test_pinned_slot_injector COMMAND test_pinned_slot_injector)
set_tests_properties(test_pinned_slot_injector PROPERTIES TIMEOUT 120)
add_test(NAME test_replay_sync COMMAND test_replay_sync)
# REP-05: records a real package at 4 ms / 8 ms channel rates and replays it
# into live targets, so its waits are wall-clock bound. It failed once in a
# `ctest -j8` full run while passing standalone; serial slot, assertions kept.
set_tests_properties(test_replay_sync PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
add_test(NAME test_replay_playlist COMMAND test_replay_playlist)
set_tests_properties(test_replay_playlist PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_record_replay_record COMMAND test_record_replay_record)
set_tests_properties(test_record_replay_record PROPERTIES TIMEOUT 120)
add_test(NAME test_replay_header_rewrite COMMAND test_replay_header_rewrite)
set_tests_properties(test_replay_header_rewrite PROPERTIES TIMEOUT 120)
add_test(NAME test_replay_determinism COMMAND test_replay_determinism)
set_tests_properties(test_replay_determinism PROPERTIES TIMEOUT 120)
add_test(NAME test_replay_producer_gate COMMAND test_replay_producer_gate)
set_tests_properties(test_replay_producer_gate PROPERTIES TIMEOUT 30)
add_test(NAME test_session_lease COMMAND test_session_lease)
set_tests_properties(test_session_lease PROPERTIES TIMEOUT 30)
add_test(NAME test_recorder_drain COMMAND test_recorder_drain)
set_tests_properties(test_recorder_drain PROPERTIES TIMEOUT 30)
add_test(NAME test_recorder_split COMMAND test_recorder_split)
set_tests_properties(test_recorder_split PROPERTIES TIMEOUT 30)
add_test(NAME test_mp_supervisor COMMAND test_mp_supervisor)
set_tests_properties(test_mp_supervisor PROPERTIES TIMEOUT 60)
add_test(NAME test_mp_supervisor_restart COMMAND test_mp_supervisor_restart)
set_tests_properties(test_mp_supervisor_restart PROPERTIES TIMEOUT 60)
add_test(NAME test_status_server COMMAND test_status_server)
set_tests_properties(test_status_server PROPERTIES TIMEOUT 60)
add_test(NAME test_systemd_notify COMMAND test_systemd_notify)
set_tests_properties(test_systemd_notify PROPERTIES TIMEOUT 30)
add_test(NAME test_mp_session_identity COMMAND test_mp_session_identity)
add_test(NAME test_record_mirror COMMAND test_record_mirror)
add_test(NAME test_record_control COMMAND test_record_control)
add_test(NAME test_record_large_payload COMMAND test_record_large_payload)
add_test(NAME test_record_mirror_client COMMAND test_record_mirror_client)
add_test(NAME test_record_mirror_ring COMMAND test_record_mirror_ring)
add_test(NAME test_tap_in_primitives COMMAND test_tap_in_primitives)
add_test(NAME test_tap_cross_process COMMAND test_tap_cross_process)
add_test(NAME test_record_mirror_ring_mp COMMAND test_record_mirror_ring_mp)
add_test(NAME test_trigger_capture_pipeline COMMAND test_trigger_capture_pipeline)
add_test(NAME test_crash_injection COMMAND test_crash_injection)
set_tests_properties(test_crash_injection PROPERTIES TIMEOUT 60)
add_test(NAME test_triggered_recorder COMMAND test_triggered_recorder)
set_tests_properties(test_triggered_recorder PROPERTIES TIMEOUT 90)
add_test(NAME test_thyme_bin_trigger COMMAND test_thyme_bin_trigger)
add_test(NAME test_pinned_slot_drain_tear COMMAND test_pinned_slot_drain_tear)
add_test(NAME test_pinned_slot_raw_crc COMMAND test_pinned_slot_raw_crc)
add_test(NAME test_pinned_slot_lease_abi COMMAND test_pinned_slot_lease_abi)
add_test(NAME test_pinned_slot_lease_pin COMMAND test_pinned_slot_lease_pin)
add_test(NAME test_pinned_slot_lease_capacity COMMAND test_pinned_slot_lease_capacity)
add_test(NAME test_pinned_slot_lease_steal COMMAND test_pinned_slot_lease_steal)
add_test(NAME test_pinned_slot_lease_epoch COMMAND test_pinned_slot_lease_epoch)
add_test(NAME test_pinned_slot_lease_copy COMMAND test_pinned_slot_lease_copy)
add_test(NAME test_lease_live_pin_not_stolen COMMAND test_lease_live_pin_not_stolen)
add_test(NAME test_lease_observability COMMAND test_lease_observability)
add_test(NAME test_snapshot_ownership COMMAND test_snapshot_ownership)
add_test(NAME test_reader_facade COMMAND test_reader_facade)
add_test(NAME test_pinned_slot_view_lifetime COMMAND test_pinned_slot_view_lifetime)
add_test(NAME test_pinned_slot_view_debug COMMAND test_pinned_slot_view_debug)
add_test(NAME test_pinned_slot_lease_race_debug COMMAND test_pinned_slot_lease_race_debug)
add_test(NAME test_post_grace_preempt COMMAND test_post_grace_preempt)
add_test(NAME test_roll_after_close_failure COMMAND test_roll_after_close_failure)
add_test(NAME test_trigger_priority_clamp COMMAND test_trigger_priority_clamp)
add_test(NAME test_trigger_busy COMMAND test_trigger_busy)
add_test(NAME test_quota_class_typo COMMAND test_quota_class_typo)
add_test(NAME test_trigger_preempt_fail COMMAND test_trigger_preempt_fail)
add_test(NAME test_trigger_timestamp COMMAND test_trigger_timestamp)
add_test(NAME test_idle_predicate COMMAND test_idle_predicate)
add_test(NAME test_perf_dump_on_crash COMMAND test_perf_dump_on_crash)
set_tests_properties(test_pinned_slot_drain_tear PROPERTIES TIMEOUT 60)
add_test(NAME test_trigger_center_fsm COMMAND test_trigger_center_fsm)
add_test(NAME test_trigger_fsm_events COMMAND test_trigger_fsm_events)
add_test(NAME test_trigger_config COMMAND test_trigger_config)
add_test(NAME test_record_manifest_subset COMMAND test_record_manifest_subset)
add_test(NAME test_record_manifest_arena COMMAND test_record_manifest_arena)
add_test(NAME test_trigger_config_doc COMMAND test_trigger_config_doc)
add_test(NAME test_trigger_config_schema COMMAND test_trigger_config_schema)
set_tests_properties(test_trigger_config_schema PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
# ARCH-13: the schema is checked in; its generated header/doc blocks must be
# byte-identical to a fresh generation, and the generator's own invariants
# (unique keys, status/reload consistency) have a unit test.
add_test(NAME test_config_schema_artifacts
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/config_schema/gen_config_schema.py
    --check --repo-root ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(test_config_schema_artifacts PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_gen_config_schema
  COMMAND python3
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/config_schema/test_gen_config_schema.py)
set_tests_properties(test_gen_config_schema PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_trigger_channel_filter COMMAND test_trigger_channel_filter)
set_tests_properties(test_trigger_channel_filter PROPERTIES TIMEOUT 60)
add_test(NAME test_trigger_protocol COMMAND test_trigger_protocol)
add_test(NAME test_trigger_security COMMAND test_trigger_security)
add_test(NAME test_trigger_cancel COMMAND test_trigger_cancel)
add_test(NAME test_trigger_notify COMMAND test_trigger_notify)
set_tests_properties(test_trigger_notify PROPERTIES TIMEOUT 60)
# TRG-09.
add_test(NAME test_trigger_storm COMMAND test_trigger_storm)
set_tests_properties(test_trigger_storm PROPERTIES TIMEOUT 60)
add_test(NAME test_trigger_storm_soak COMMAND test_trigger_storm soak)
set_tests_properties(test_trigger_storm_soak PROPERTIES LABELS "storm"
  TIMEOUT 120)
add_test(NAME test_trigger_overflow COMMAND test_trigger_overflow)
add_test(NAME test_trigger_malformed COMMAND test_trigger_malformed)
add_test(NAME test_trigger_audit COMMAND test_trigger_audit)
set_tests_properties(test_trigger_audit PROPERTIES TIMEOUT 60)
add_test(NAME test_trigger_seal_latency COMMAND test_trigger_seal_latency)
set_tests_properties(test_trigger_seal_latency PROPERTIES TIMEOUT 60)
add_test(NAME test_trigger_e2e COMMAND test_trigger_e2e)
set_tests_properties(test_trigger_e2e PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
# Forked producer + real socket + 10 s post window. A full `ctest -j12` run
# segfaulted once while 3/3 standalone runs passed, so run it alone instead of
# assuming the crash is a product defect; if it returns, capture a core
# (core_pattern is apport here) before touching the assertions.
add_test(NAME test_idle_flush COMMAND test_idle_flush)
add_test(NAME test_rate_limit COMMAND test_rate_limit)
add_test(NAME test_retention COMMAND test_retention)
add_test(NAME test_package_lifecycle COMMAND test_package_lifecycle)
set_tests_properties(test_package_lifecycle PROPERTIES LABELS "mr_gate" TIMEOUT 60)
add_test(NAME test_retention_symlink COMMAND test_retention_symlink)
add_test(NAME test_continuous COMMAND test_continuous)
add_test(NAME test_continuous_mono_order COMMAND test_continuous_mono_order)
add_test(NAME test_continuous_pinned_slot COMMAND test_continuous_pinned_slot)
set_tests_properties(test_continuous_pinned_slot PROPERTIES TIMEOUT 60)
add_test(NAME test_continuous_lease_tear COMMAND test_continuous_lease_tear)
set_tests_properties(test_continuous_lease_tear PROPERTIES TIMEOUT 60)
add_test(NAME test_trigger_prescan_boundary COMMAND test_trigger_prescan_boundary)
# The boundary sample uses the trigger's explicit t0, so package-I/O scheduling
# delays cannot move its stamp beyond the window being tested.
set_tests_properties(test_trigger_prescan_boundary PROPERTIES TIMEOUT 60)
add_test(NAME test_pinned_slot_channel COMMAND test_pinned_slot_channel)
add_test(NAME test_thyme_bin COMMAND test_thyme_bin)
add_test(NAME test_thyme_bin_tcp COMMAND test_thyme_bin_tcp)
add_test(NAME test_thyme_bin_gap COMMAND test_thyme_bin_gap)
add_test(NAME test_thyme_bin_session COMMAND test_thyme_bin_session)
add_test(NAME test_thyme_bin_corrupt COMMAND test_thyme_bin_corrupt)
add_test(NAME test_thyme_bin_index COMMAND test_thyme_bin_index)
add_test(NAME test_thyme_bin_fanout COMMAND test_thyme_bin_fanout)
add_test(NAME test_thyme_bin_fdsink COMMAND test_thyme_bin_fdsink)
add_test(NAME test_mcap_writer COMMAND test_mcap_writer)
add_test(NAME test_mcap_export COMMAND test_mcap_export)
add_test(NAME test_mcap_verify COMMAND test_mcap_verify)
set_tests_properties(test_mcap_verify PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
if(Python3_Interpreter_FOUND)
  # ECO-03: stdlib-only Python reader against C++-written fixtures.
  add_test(NAME test_thyme_py COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/inspect/thyme_py/tests/run_tests.py
    $<TARGET_FILE:thyme_py_fixture>)
  set_tests_properties(test_thyme_py PROPERTIES
    LABELS "unit;mr_gate" TIMEOUT 60)
  # ECO-12: optional test-side reference dependencies. Missing mcap/PyYAML is a
  # visible CTest skip (77), never a silent pass and never a runtime dependency.
  add_test(NAME test_mcap_official_python
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_eco12_mcap.py
      --fixture $<TARGET_FILE:eco12_mcap_fixture>
      --replay $<TARGET_FILE:thyme-replay>)
  add_test(NAME test_gen_graph_yaml_reference
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph/test_yaml_reference.py
      --gen $<TARGET_FILE:gen_graph>
      --source-dir ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gen_graph)
  # REP-15: the hand-rolled FileDescriptorSet encoder must satisfy the official
  # protobuf parser (MCAP stores the bytes opaquely, so the mcap reader above
  # cannot prove they are valid). Optional test-side dependency -> skip 77.
  add_test(NAME test_rep15_protobuf
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rep15_protobuf.py
      --fixture $<TARGET_FILE:descriptor_fixture>)
  set_tests_properties(test_mcap_official_python test_gen_graph_yaml_reference
    test_rep15_protobuf
    PROPERTIES LABELS "unit;mr_gate" TIMEOUT 60 SKIP_RETURN_CODE 77)
  # SEC-07: real record_control server under malformed/oversized input.
  add_test(NAME test_pentest_record_control
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/pentest_record_control.py
      --server $<TARGET_FILE:pentest_server>)
  set_tests_properties(test_pentest_record_control PROPERTIES
    LABELS "unit;mr_gate" TIMEOUT 60)
endif()
# ECO-06: install the project into a throw-away DESTDIR stage, consume it with
# find_package(thyme CONFIG) from the install tree and from the build tree, and
# compile a consumer with pkg-config --cflags --libs thyme.
add_test(NAME test_package_export COMMAND ${CMAKE_COMMAND}
  "-DTHYME_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
  "-DTHYME_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
  "-DTHYME_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}"
  "-DTHYME_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
  "-DTHYME_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
  "-DTHYME_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-export-test"
  -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_package_export.cmake)
set_tests_properties(test_package_export PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 180)
add_test(NAME test_thyme_probe COMMAND test_thyme_probe)
set_tests_properties(test_thyme_probe PROPERTIES TIMEOUT 60)
add_test(NAME test_abi_profile COMMAND test_abi_profile)
set_tests_properties(test_abi_profile PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
