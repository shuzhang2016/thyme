# OPS-06: ABI constants matrix (header-derived, deterministic JSON) and the
# pre-flight page checker. The checker builds synthetic old/new SHM pages in
# its own namespace and unlinks them, so it is fast and deterministic.
add_executable(test_abi_matrix tests/test_abi_matrix.cpp)
target_link_libraries(test_abi_matrix PRIVATE thyme_probe_core)
add_executable(test_abi_check tests/test_abi_check.cpp)
target_link_libraries(test_abi_check PRIVATE thyme_probe_core Threads::Threads)
add_test(NAME test_abi_matrix COMMAND test_abi_matrix)
set_tests_properties(test_abi_matrix PROPERTIES LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_abi_check COMMAND test_abi_check)
set_tests_properties(test_abi_check PROPERTIES LABELS "unit;mr_gate" TIMEOUT 60)
# OPS-06: the CLI-level matrix JSON must equal the checked-in golden (deploy
# scripts consume it verbatim) and be stable across runs.
add_test(NAME test_abi_matrix_json
  COMMAND ${CMAKE_COMMAND}
    -DPROBE=$<TARGET_FILE:thyme_probe>
    -DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/tools/ops/upgrade/abi_matrix.golden.json
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/ops/upgrade/test_abi_matrix_json.cmake)
set_tests_properties(test_abi_matrix_json PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
# OPS-06: the upgrade driver defaults to a dry run and must not touch the
# package, the install prefix, the unit manager or SHM without --apply.
add_test(NAME test_upgrade_script
  COMMAND ${CMAKE_COMMAND}
    -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tools/ops/upgrade/thyme_upgrade.sh
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/test_upgrade_script
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/ops/upgrade/test_upgrade_dry_run.cmake)
set_tests_properties(test_upgrade_script PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_thyme_log_merge COMMAND test_thyme_log_merge)
add_test(NAME test_log_rotation COMMAND test_log_rotation)
set_tests_properties(test_log_rotation PROPERTIES TIMEOUT 60)
add_test(NAME test_replay_cli COMMAND test_replay_cli)
set_tests_properties(test_replay_cli PROPERTIES TIMEOUT 180)
add_test(NAME test_replay_seek_index COMMAND test_replay_seek_index)
set_tests_properties(test_replay_seek_index PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_replay_export COMMAND test_replay_export)
set_tests_properties(test_replay_export PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_self_desc COMMAND test_self_desc)
set_tests_properties(test_self_desc PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_canonical COMMAND test_canonical)
set_tests_properties(test_canonical PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_test(NAME test_replay_repair COMMAND test_replay_repair)
set_tests_properties(test_replay_repair PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)

# REP-11: pure record-key mapping rules. No I/O/SHM -> mr_gate.
thyme_test(test_topology_map thyme_replay)
add_test(NAME test_topology_map COMMAND test_topology_map)
set_tests_properties(test_topology_map PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
# TST-12: record A -> topology B replay through the real CLI + ring pages.
# Named /dev/shm segments (pid-unique, unlinked on exit) keep it out of the
# MR gate like the other SHM tests; deterministic and sub-second.
thyme_test(test_topology_replay thyme_replay_cli_lib thyme_recorder
  Threads::Threads)
add_test(NAME test_topology_replay COMMAND test_topology_replay)
set_tests_properties(test_topology_replay PROPERTIES
  LABELS "unit" TIMEOUT 60)

# TST-03 fuzz registrations: quick deterministic pass (mr_gate) + full pass
# (nightly, "fuzz" label only). Iteration counts are fixed, so both passes
# are reproducible; THYME_FUZZ_ITERS/THYME_FUZZ_SEED override for ad-hoc runs.
add_test(NAME test_fuzz_thyme_bin COMMAND fuzz_thyme_bin 2000)
add_test(NAME test_fuzz_trigger_protocol COMMAND fuzz_trigger_protocol 5000)
add_test(NAME test_fuzz_trigger_config COMMAND fuzz_trigger_config 5000)
set_tests_properties(test_fuzz_thyme_bin test_fuzz_trigger_protocol
  test_fuzz_trigger_config PROPERTIES LABELS "fuzz;mr_gate" TIMEOUT 60)
add_test(NAME test_fuzz_thyme_bin_full COMMAND fuzz_thyme_bin 100000)
add_test(NAME test_fuzz_trigger_protocol_full
  COMMAND fuzz_trigger_protocol 200000)
add_test(NAME test_fuzz_trigger_config_full COMMAND fuzz_trigger_config 200000)
set_tests_properties(test_fuzz_thyme_bin_full test_fuzz_trigger_protocol_full
  test_fuzz_trigger_config_full PROPERTIES LABELS "fuzz" TIMEOUT 300)
# GEN-07: structured fuzz of the gen_graph binary itself. Each iteration
# spawns gen_graph on a fixture-seeded YAML-aware mutation and asserts exit
# code ∈ {0,2}, file:line diagnostics + no partial output on rejection, and a
# deterministic --check on acceptance. Quick pass gates MRs; full pass runs
# nightly (subprocess-heavy, so the counts stay lower than the in-process
# TST-03 drivers).
add_test(NAME test_fuzz_gen_graph COMMAND fuzz_gen_graph 2000)
set_tests_properties(test_fuzz_gen_graph PROPERTIES
  LABELS "fuzz;mr_gate" TIMEOUT 180)
add_test(NAME test_fuzz_gen_graph_full COMMAND fuzz_gen_graph 10000)
set_tests_properties(test_fuzz_gen_graph_full PROPERTIES
  LABELS "fuzz" TIMEOUT 600)

# SEC-03/REC-18: real sealed package -> manifest/signature verification,
# tamper and absent cases, big-frame bulk coverage, partial manifests on
# invalidated/crash-leftover packages, plus the thyme-replay `integrity`
# command (offline). Fast and deterministic -> mr_gate.
thyme_test(test_package_integrity thyme_recorder thyme_replay_cli_lib
  Threads::Threads)
if(UNIX AND NOT APPLE)
  target_link_options(test_package_integrity PRIVATE "LINKER:--wrap=read")
endif()
add_test(NAME test_package_integrity COMMAND test_package_integrity)
set_tests_properties(test_package_integrity PROPERTIES TIMEOUT 120
  LABELS "mr_gate")

# SEC-08: real key rotation plus old/new/wrong-key manifest verification.
thyme_test(test_signing_key_rotation thyme_recorder)
add_test(NAME test_signing_key_rotation
  COMMAND test_signing_key_rotation
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ops/rotate_signing_key.sh)
set_tests_properties(test_signing_key_rotation PROPERTIES TIMEOUT 30
  LABELS "mr_gate")

# ABI-09: package schema sidecar round trip, honest "layout unavailable"
# degradation, strict/tampered/absent handling. Pure library, deterministic ->
# mr_gate.
thyme_test(test_record_schema thyme_probe_core)
add_test(NAME test_record_schema COMMAND test_record_schema)
set_tests_properties(test_record_schema PROPERTIES TIMEOUT 60
  LABELS "mr_gate")

# OPS-07: startup resource self-check (fd/memlock/disk/shm). Read-only host
# probes, single process, deterministic -> mr_gate.
thyme_test(test_resource_check)
add_test(NAME test_resource_check COMMAND test_resource_check)
set_tests_properties(test_resource_check PROPERTIES TIMEOUT 30
  LABELS "mr_gate")

# ABI-10: record-manifest identity in .tbin (v3 block), perf logs and the
# package sidecar; replay list/verify/info/play refuse a mismatched manifest,
# legacy packages stay readable, tampered sidecars are caught by the
# independent meta.tbin block, and invalidated packages keep a partial schema.
# Real in-process captures, deterministic -> mr_gate.
thyme_test(test_record_manifest thyme_recorder thyme_replay_cli_lib
  thyme_log_merge_lib Threads::Threads)
add_test(NAME test_record_manifest COMMAND test_record_manifest)
set_tests_properties(test_record_manifest PROPERTIES TIMEOUT 120
  LABELS "mr_gate")

# REC-21: per-channel crc=package|frame|off — config parsing/defaults/dump
# round trip, ABI-12 no-CRC seal semantics, and drain behavior per policy on
# real captures (incl. REC-18 manifest verification). Fast and deterministic
# (single process, tiny frames) -> mr_gate.
thyme_test(test_crc_policy thyme_recorder)
add_test(NAME test_crc_policy COMMAND test_crc_policy)
set_tests_properties(test_crc_policy PROPERTIES TIMEOUT 120
  LABELS "mr_gate")

# REC-27: opaque big-frame payload profile (schema v3, .raw v2 header,
# meta PayProf, --decode-profile refuse-unknown/keep-raw). Fast capture +
# export, deterministic -> mr_gate.
thyme_test(test_payload_profile thyme_recorder thyme_replay_cli_lib)
add_test(NAME test_payload_profile COMMAND test_payload_profile)
set_tests_properties(test_payload_profile PROPERTIES TIMEOUT 120
  LABELS "mr_gate")

# REC-14: real VFS ENOSPC on a size-capped tmpfs (user-namespace mount;
# loop-control is not usable inside the namespace). Skip (77) if unshare
# cannot map root.
find_program(THYME_UNSHARE unshare)
if(THYME_UNSHARE)
  thyme_test(test_enospc_fs thyme_recorder)
  add_test(NAME test_enospc_fs
    COMMAND ${THYME_UNSHARE} --map-root-user --mount --propagation private
            $<TARGET_FILE:test_enospc_fs>)
  set_tests_properties(test_enospc_fs PROPERTIES TIMEOUT 60
    LABELS "mr_gate" SKIP_RETURN_CODE 77)
endif()

option(THYME_ENABLE_TSAN "Build thread-sanitizer variant of concurrent tests" OFF)
if(THYME_ENABLE_TSAN)
  add_executable(test_notif_pipe_tsan tests/test_notif_pipe.cpp)
  target_link_libraries(test_notif_pipe_tsan PRIVATE thyme_runtime Threads::Threads)
  target_include_directories(test_notif_pipe_tsan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_options(test_notif_pipe_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(test_notif_pipe_tsan PRIVATE -fsanitize=thread)
  add_test(NAME test_notif_pipe_tsan COMMAND test_notif_pipe_tsan)
  add_executable(test_shm_spsc_ring_tsan tests/test_shm_spsc_ring.cpp)
  target_link_libraries(test_shm_spsc_ring_tsan PRIVATE thyme_runtime Threads::Threads)
  target_include_directories(test_shm_spsc_ring_tsan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_options(test_shm_spsc_ring_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(test_shm_spsc_ring_tsan PRIVATE -fsanitize=thread)
  add_test(NAME test_shm_spsc_ring_tsan COMMAND test_shm_spsc_ring_tsan)
  add_executable(test_shm_spmc_ring_tsan tests/test_shm_spmc_ring.cpp)
  target_link_libraries(test_shm_spmc_ring_tsan PRIVATE thyme_runtime Threads::Threads)
  target_include_directories(test_shm_spmc_ring_tsan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_options(test_shm_spmc_ring_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(test_shm_spmc_ring_tsan PRIVATE -fsanitize=thread)
  add_test(NAME test_shm_spmc_ring_tsan COMMAND test_shm_spmc_ring_tsan)
  # Compile event_thread.cpp into the test so TSan instruments the thread
  # loop itself (thyme_runtime is built without sanitizer flags).
  add_executable(test_event_thread_tsan tests/test_event_thread.cpp
    src/runtime/event_thread.cpp)
  target_link_libraries(test_event_thread_tsan PRIVATE thyme_runtime Threads::Threads)
  target_include_directories(test_event_thread_tsan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_options(test_event_thread_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(test_event_thread_tsan PRIVATE -fsanitize=thread)
  add_test(NAME test_event_thread_tsan COMMAND test_event_thread_tsan)
  add_executable(test_latest_slot_concurrent_tsan tests/test_latest_slot_concurrent.cpp)
  target_link_libraries(test_latest_slot_concurrent_tsan PRIVATE thyme_runtime Threads::Threads)
  target_compile_options(test_latest_slot_concurrent_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(test_latest_slot_concurrent_tsan PRIVATE -fsanitize=thread)
  add_test(NAME test_latest_slot_concurrent_tsan COMMAND test_latest_slot_concurrent_tsan)
  # RT-19: the payload copies must be race-free under TSan (no ignore-reads).
  add_executable(test_seqlock_payload_model_tsan tests/test_seqlock_payload_model.cpp)
  target_link_libraries(test_seqlock_payload_model_tsan PRIVATE thyme_runtime thyme_recorder Threads::Threads)
  target_compile_options(test_seqlock_payload_model_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(test_seqlock_payload_model_tsan PRIVATE -fsanitize=thread)
  add_test(NAME test_seqlock_payload_model_tsan COMMAND test_seqlock_payload_model_tsan)
  add_executable(test_latest_slot_stress_tsan tests/test_latest_slot_stress.cpp)
  target_link_libraries(test_latest_slot_stress_tsan PRIVATE thyme_runtime Threads::Threads)
  target_compile_options(test_latest_slot_stress_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(test_latest_slot_stress_tsan PRIVATE -fsanitize=thread)
  add_test(NAME test_latest_slot_stress_tsan COMMAND test_latest_slot_stress_tsan)
  add_executable(test_object_pool_concurrent_tsan tests/test_object_pool_concurrent.cpp)
  target_link_libraries(test_object_pool_concurrent_tsan PRIVATE thyme_runtime Threads::Threads)
  target_compile_options(test_object_pool_concurrent_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(test_object_pool_concurrent_tsan PRIVATE -fsanitize=thread)
  add_test(NAME test_object_pool_concurrent_tsan COMMAND test_object_pool_concurrent_tsan)
  # RT-06: the register-then-check handoff must be race-free under TSan (a
  # publish that is not ordered before the wake shows up as a data race on the
  # payload the consumer reads).
  add_executable(test_spsc_notify_order_tsan tests/test_spsc_notify_order.cpp)
  target_link_libraries(test_spsc_notify_order_tsan PRIVATE thyme_runtime Threads::Threads)
  target_compile_options(test_spsc_notify_order_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(test_spsc_notify_order_tsan PRIVATE -fsanitize=thread)
  add_test(NAME test_spsc_notify_order_tsan COMMAND test_spsc_notify_order_tsan)
  # RT-08: a reader registers its notify targets while the producer is calling
  # Notify (late attach). The registry is atomically published; the same test
  # source runs here so TSan proves the registration/fan-out pair race-free.
  add_executable(test_spmc_notify_race_tsan tests/test_spmc_notify_lifecycle.cpp)
  target_link_libraries(test_spmc_notify_race_tsan PRIVATE thyme_runtime Threads::Threads)
  target_compile_options(test_spmc_notify_race_tsan PRIVATE -fsanitize=thread -g -O1)
  target_link_options(test_spmc_notify_race_tsan PRIVATE -fsanitize=thread)
  add_test(NAME test_spmc_notify_race_tsan COMMAND test_spmc_notify_race_tsan)
endif()

# M7.2 — MR fast gate: gen_graph golden + yaml/rate/depth contracts, plus
# focused runtime unit tests (no stress/tsan/mp-supervisor heavies). The
# record/trigger main chain is gated on the fast, deterministic members;
# test_record_mirror_ring_mp joins with RUN_SERIAL (it races a writer by
# design and must not share cores with compile-heavy gen_graph jobs).
# test_event_thread/test_empty_app are RUN_SERIAL for the same reason (C2).
set_tests_properties(
  test_gen_graph_yaml test_gen_graph_yaml_string
  test_gen_graph_rate test_gen_graph_rate_warn test_gen_graph_depth_warn
  test_gen_graph_golden test_gen_graph_cyclic_golden test_gen_graph_minimal_norecord
  test_gen_graph_health_inject
  test_gen_graph_depth
  test_gen_graph_broadcast_emit
  test_gen_graph_derive_template
  test_gen_graph_cycle_reject
  test_gen_graph_feedback_marker
  test_gen_graph_record_manifest
  test_gen_graph_sched test_gen_graph_explain_sched
  test_empty_app
  test_perf_log test_cyclic_thread test_cyclic_phase test_event_thread
  test_core_no_alloc
  # OBS-08: logger free of timing/sleep; deterministic rate/rotation asserts.
  test_log
  test_shm_spmc_ring
  # RT-13/14/15/17: in-place publish paths and copy-path equivalence are
  # fast, deterministic, and gate the shared publish sequence.
  test_shm_spsc_begin_commit test_shm_spmc_begin_commit
  test_latest_slot_begin_commit test_copy_in_equivalence
  test_single_writer_guard
  # RT-03: single-threaded boundary states, no timing — fast + deterministic.
  test_latest_slot_seq_wrap
  test_replay_backpressure test_replay_mode
  test_replay_cli
  test_record_replay_record
  test_replay_header_rewrite test_replay_determinism
  test_replay_producer_gate
  test_session_lease
  test_recorder_drain
  test_recorder_split
  test_record_mirror test_record_mirror_ring test_record_mirror_client
  test_tap_in_primitives test_pinned_slot_channel
  test_thyme_bin_trigger
  test_mcap_writer test_mcap_export
  test_thyme_probe
  test_triggered_recorder test_trigger_center_fsm test_trigger_protocol
  test_trigger_security
  test_trigger_cancel test_trigger_notify
  # TRG-09: deterministic burst storm + overflow/malformed counters are fast
  # and deterministic; the paced 10k/s soak (test_trigger_storm_soak) stays
  # out of the gate under the "storm" label.
  test_trigger_storm test_trigger_overflow test_trigger_malformed
  # TRG-12: fast and deterministic (the crash section is fork+SIGKILL on a
  # tmp file, same profile as the OBS-05 crash test).
  test_trigger_audit
  # TRG-14: seal-latency plumbing (timestamps/pairing, never magnitudes) —
  # fast, deterministic, single process with tiny in-process sources.
  test_trigger_seal_latency
  test_trigger_config test_trigger_config_doc test_trigger_channel_filter
  # ARCH-13: generated schema vs parser/dump/defaults/reload + artifact check.
  test_trigger_config_schema test_config_schema_artifacts test_gen_config_schema
  # GEN-12/TST-11: manifest subset + manifest-sized arena invariants.
  test_record_manifest_subset test_record_manifest_arena
  # DOC-08: shipped example config must pass --check-config (fast CLI check).
  test_trigger_config_cli
  # ECO-02: triggerctl usage/exit-code/JSON contract against a fake socket.
  test_triggerctl_cli
  test_continuous test_continuous_pinned_slot test_continuous_mono_order
  test_idle_flush test_rate_limit test_retention test_retention_symlink
  test_trigger_capture_pipeline test_crash_injection
  test_record_mirror_ring_mp
  # PERF-14: wake-coalescing is on the EventThread hot path every node uses;
  # its regression test must gate MRs, not only the nightly run.
  test_wake_coalesce
  # RT-06: the publish/wake ordering every SpscRing publish depends on.
  test_spsc_notify_order
  test_pinned_slot_drain_tear test_pinned_slot_raw_crc
  test_post_grace_preempt test_trigger_priority_clamp test_trigger_busy
  test_idle_predicate test_quota_class_typo test_trigger_preempt_fail
  test_trigger_timestamp test_roll_after_close_failure
  # OBS-04/OBS-05: fast and deterministic (the contention section asserts
  # series presence, never magnitude; the crash section is fork+SIGKILL).
  test_proc_sampler test_trigger_fsm_events
  # SCH-08: deterministic (the live-mismatch case asserts flag == verdict on
  # the captured state, so it holds with or without CAP_SYS_NICE).
  test_sched_observability
  # SCH-03/SCH-04/SCH-06: bounded atomic handshakes and generous timing
  # bounds; the rotation guard is pure single-threaded.
  test_event_input_budget test_overrun_policy test_realtime_guard
  # OBS-03/OBS-06: same fast+deterministic profile (the crash section is
  # fork+SIGKILL on a tmp file; the merge test uses synthetic tmp inputs).
  # OBS-09: histogram math/concurrency/zero-alloc is deterministic; the
  # multi-writer section uses 4 threads x 50k increments only.
  test_perf_ring test_thyme_log_merge test_histogram
  # OPS-05: deterministic cap/generation/budget asserts plus fork+SIGKILL
  # crash safety on tmp files (same profile as the other crash sections).
  test_log_rotation
  test_pinned_slot_lease_abi test_pinned_slot_lease_pin test_pinned_slot_lease_capacity
  test_pinned_slot_lease_steal
  test_pinned_slot_lease_epoch test_pinned_slot_lease_copy
  test_lease_live_pin_not_stolen
  test_lease_observability test_snapshot_ownership test_reader_facade
  test_continuous_lease_tear
  test_trigger_prescan_boundary
  test_pinned_slot_view_lifetime test_pinned_slot_view_debug
  test_pinned_slot_lease_race_debug
  PROPERTIES LABELS "mr_gate")
set_tests_properties(test_record_mirror_ring_mp test_event_thread
                     test_empty_app PROPERTIES RUN_SERIAL TRUE)
