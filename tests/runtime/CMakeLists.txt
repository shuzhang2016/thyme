function(thyme_test name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} PRIVATE thyme_runtime ${ARGN})
  # TST-07: shared bounded-wait/eventfd helpers (tests/test_wait.h).
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
endfunction()

thyme_test(test_scaffold)
thyme_test(test_shm_page_header)
thyme_test(test_frame_header)
thyme_test(test_frame)
thyme_test(test_crc32_hw)
thyme_test(test_spsc_ring)
# RT-06: publish/wake ordering — register-then-check under a registration race
# (no lost wakeups), payload integrity across the wake, and the no-waiter skip
# path. Bounded iterations + atomic handshakes, so it is fast and deterministic.
thyme_test(test_spsc_notify_order Threads::Threads)
thyme_test(test_perf_log)
# OBS-03: rate-sized perf rings + dump-then-reset window semantics.
thyme_test(test_perf_ring)
# OBS-09: fixed-bucket histogram math/concurrency/zero-alloc + type-6 block.
thyme_test(test_histogram)
# OBS-04: low-frequency /proc + queue-depth sampler (off the hot path).
thyme_test(test_proc_sampler)
# OBS-13: optional sources (cgroup v1/v2, PSI, host disk/net, threads) driven
# through the path seam, so the layouts are covered on any host.
thyme_test(test_proc_ext)
# OBS-14: machine-level sampler (aggregate/per-core CPU, load, memory, system
# fd table) and its periodic dump thread.
thyme_test(test_host_sampler)
thyme_test(test_notif_pipe)
thyme_test(test_queue_push_fail_credit)
# TST-04: property/invariant tests for the context primitives. The in-process
# suite runs deterministic splitmix64 schedules against a reference model plus
# fixed-count concurrency handshakes, so it is fast and deterministic -> gate.
thyme_test(test_primitive_properties)
add_test(NAME test_primitive_properties COMMAND test_primitive_properties)
set_tests_properties(test_primitive_properties PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
# TST-04 SHM half: same invariants across two mappings of a named region.
# TST-15 gives every CTest an isolated prefix and exit cleanup, so this belongs
# to the MR gate with the other SHM regression tests (FUP-24).
add_executable(test_primitive_properties_shm tests/test_primitive_properties_shm.cpp)
target_link_libraries(test_primitive_properties_shm PRIVATE thyme_runtime Threads::Threads)
add_test(NAME test_primitive_properties_shm COMMAND test_primitive_properties_shm)
set_tests_properties(test_primitive_properties_shm PROPERTIES
  LABELS "unit;mr_gate" TIMEOUT 60)
add_executable(test_shm_spsc_ring tests/test_shm_spsc_ring.cpp)
add_executable(test_shm_spmc_ring tests/test_shm_spmc_ring.cpp)
# RT-13/RT-14/RT-15/RT-17: in-place BeginWrite/CommitWrite publish paths.
add_executable(test_shm_spsc_begin_commit tests/test_shm_spsc_begin_commit.cpp)
add_executable(test_shm_spmc_begin_commit tests/test_shm_spmc_begin_commit.cpp)
add_executable(test_latest_slot_begin_commit tests/test_latest_slot_begin_commit.cpp)
add_executable(test_single_writer_guard tests/test_single_writer_guard.cpp)
# RT-03: u64 seqlock sequence — boundary/wraparound states via the test seam.
add_executable(test_latest_slot_seq_wrap tests/test_latest_slot_seq_wrap.cpp)
# RT-09: bounded seqlock retry loops expose retry/torn counters. The
# in-process half (LatestSlot + mirror pages) is fast and deterministic ->
# mr_gate; the SHM half needs named regions -> "unit" like the other /dev/shm
# tests.
add_executable(test_retry_accounting tests/test_retry_accounting.cpp)
add_executable(test_retry_accounting_shm tests/test_retry_accounting_shm.cpp)
# RT-08: the SPMC notify registry is atomically published, so a reader can
# attach while the producer is publishing; covers setup/late/concurrent
# registration plus the Notify re-entrancy guard. A TSan variant proves the
# race freedom (THYME_ENABLE_TSAN).
add_executable(test_spmc_notify_lifecycle tests/test_spmc_notify_lifecycle.cpp)
# RT-10: cacheline/false-sharing audit gate (docs/reference/cacheline-audit.md). Exact
# sizeof/alignof/offsetof pins; new padding fails the build. Pure compile-time,
# so it is fast, deterministic and gates MRs.
add_executable(test_cacheline_audit tests/test_cacheline_audit.cpp)
add_executable(test_copy_in_equivalence tests/test_copy_in_equivalence.cpp)
add_executable(test_event_wake_word tests/test_event_wake_word.cpp src/runtime/event_thread.cpp)
add_executable(test_wake_coalesce tests/test_wake_coalesce.cpp)
target_link_libraries(test_shm_spsc_ring PRIVATE thyme_runtime Threads::Threads)
target_link_libraries(test_shm_spmc_ring PRIVATE thyme_runtime Threads::Threads)
target_include_directories(test_shm_spsc_ring PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_include_directories(test_shm_spmc_ring PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_shm_spsc_begin_commit PRIVATE thyme_runtime Threads::Threads)
target_link_libraries(test_shm_spmc_begin_commit PRIVATE thyme_runtime Threads::Threads)
target_link_libraries(test_latest_slot_begin_commit PRIVATE thyme_runtime Threads::Threads)
target_link_libraries(test_single_writer_guard PRIVATE thyme_runtime Threads::Threads)
target_link_libraries(test_latest_slot_seq_wrap PRIVATE thyme_runtime Threads::Threads)
target_link_libraries(test_retry_accounting PRIVATE thyme_runtime Threads::Threads)
target_link_libraries(test_retry_accounting_shm PRIVATE thyme_runtime
  Threads::Threads)
target_link_libraries(test_spmc_notify_lifecycle PRIVATE thyme_runtime
  Threads::Threads)
target_link_libraries(test_cacheline_audit PRIVATE thyme_runtime)
target_link_libraries(test_copy_in_equivalence PRIVATE thyme_runtime Threads::Threads)
target_link_libraries(test_event_wake_word PRIVATE thyme_runtime Threads::Threads)
target_link_libraries(test_wake_coalesce PRIVATE thyme_runtime Threads::Threads)
thyme_test(test_event_thread)
# SCH-08: scheduling observability (actual sched state/mismatch, per-callback
# work, queue-depth probes, overrun reason counters).
thyme_test(test_sched_observability)
# SCH-03: per-input dispatch budget with two inputs at different rates.
thyme_test(test_event_input_budget)
# SCH-04: skip/degrade/restart policy, policy counters and session marker.
thyme_test(test_overrun_policy)
# SCH-06: RT-context hook + deferred log rotation.
thyme_test(test_realtime_guard)
thyme_test(test_object_pool)
# RT-04: generation wraparound at the u32 boundary through the test seam, ABA
# rejection paths, and the opt-in free-list acquire path (contract-equivalence
# schedule). Fast + deterministic -> mr_gate.
thyme_test(test_object_pool_gen_wrap)
# ARCH-19: process-local large-payload latest store (pin + RAII borrow).
thyme_test(test_pinned_latest_store Threads::Threads)
# ARCH-19: ordered process-local 1:N pin/cursor transport.
thyme_test(test_pinned_spmc_ring Threads::Threads)
# ARCH-19: compile-time Local/Shm channel facade.
thyme_test(test_channel_facade Threads::Threads)
# ARCH-12: WriteSection RAII abort-on-dtor across the five publishers.
thyme_test(test_write_section Threads::Threads)
thyme_test(test_object_pool_freelist)
# RT-04 evidence: scan vs free-list acquire cost at 10k slots (timing only).
add_executable(bench_object_pool tests/bench_object_pool.cpp)
target_link_libraries(bench_object_pool PRIVATE thyme_runtime)
target_compile_definitions(bench_object_pool PRIVATE
  THYME_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
thyme_test(test_health_slot)
# RT-07 / ABI-02: compile-time lock on every SHM page layout + little-endian.
thyme_test(test_abi_layout)
# ABI-04: cross-version reader/writer matrix for every versioned format
# (trigger V1/V2/V3, PinnedSlot channel v2/v3 + lease v3/v4, SPSC/SPMC ring
# pages, .thyme.bin v1/v2, ThymeStructDesc/FrameHeader versions). Rejections
# are asserted by exact status with no partial delivery. Fast + deterministic.
thyme_test(test_abi_cross_version thyme_recorder Threads::Threads)
thyme_test(test_cyclic_thread)
# SCH-13: first-wake phase alignment (default off, late start, stagger).
thyme_test(test_cyclic_phase)
thyme_test(test_cyclic_eintr)
thyme_test(test_latest_slot)
# ARCH-11: v3 described LatestSlot page (64 B super-header + layout model).
# Pure layout/spec checks plus SHM create/open/reject paths; fast and
# deterministic, so it belongs to the MR gate with the other SHM regressions.
thyme_test(test_latest_slot_page)
# ARCH-11: supervisor create + role attach + probe read of the same v3
# LatestSlot page. Real fork/exec role; probe is read-only and must not change
# page bytes.
add_executable(test_latest_slot_pipeline tests/test_latest_slot_pipeline.cpp)
target_link_libraries(test_latest_slot_pipeline PRIVATE thyme_app thyme_probe_core
  Threads::Threads)
target_include_directories(test_latest_slot_pipeline PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/tests)
# RT-19: seqlock payload reads are atomic, never torn (must stay TSan-green).
add_executable(test_seqlock_payload_model tests/test_seqlock_payload_model.cpp)
target_link_libraries(test_seqlock_payload_model PRIVATE thyme_runtime thyme_recorder Threads::Threads)
# PERF-07: payload copy cost vs memcpy. Correctness-only, never a timing gate.
add_executable(bench_atomic_payload tests/bench_atomic_payload.cpp)
target_link_libraries(bench_atomic_payload PRIVATE thyme_runtime)
# PERF-07: CRC plus real cross-process SPMC and record-tap copy paths.
add_executable(bench_copy_budget tests/bench_copy_budget.cpp)
target_link_libraries(bench_copy_budget PRIVATE thyme_runtime Threads::Threads)
target_compile_definitions(bench_copy_budget PRIVATE
  THYME_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
# PERF-15: real PinnedSlot copy-body, lease pin, and producer-pressure A/B.
add_executable(bench_pinned_slot_copy tests/bench_pinned_slot_copy.cpp)
target_link_libraries(bench_pinned_slot_copy PRIVATE thyme_runtime Threads::Threads)
target_compile_definitions(bench_pinned_slot_copy PRIVATE
  THYME_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
# CI-05: fixed core-path bench, JSON output compared per runner tag against
# tools/ci/bench_baseline.json by tools/ci/check_bench.py. Not a timing gate at
# ctest time -- the gate runs in nightly-bench on the recorded baseline.
add_executable(bench_core tests/bench_core.cpp)
target_link_libraries(bench_core PRIVATE thyme_runtime thyme_recorder Threads::Threads)
target_compile_definitions(bench_core PRIVATE
  THYME_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  THYME_TRIGGER_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tools/apps/trigger_center/trigger_config.minimal.ini")
# TRG-14: trigger->seal latency distribution (p50/p90/p99/max) over N real
# Center -> TriggeredRecorder captures. Exit code gates the measurement
# plumbing only (pairing/stamps/completeness); latency magnitudes are never
# asserted -- the SLO budgets live in docs/guides/trigger-config.md.
add_executable(bench_trigger_seal tests/bench_trigger_seal.cpp)
target_link_libraries(bench_trigger_seal PRIVATE thyme_runtime thyme_recorder Threads::Threads)
target_compile_definitions(bench_trigger_seal PRIVATE
  THYME_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
add_executable(test_latest_slot_concurrent tests/test_latest_slot_concurrent.cpp)
target_link_libraries(test_latest_slot_concurrent PRIVATE thyme_runtime Threads::Threads)
add_executable(test_latest_slot_stress tests/test_latest_slot_stress.cpp)
target_link_libraries(test_latest_slot_stress PRIVATE thyme_runtime Threads::Threads)
add_executable(test_shared_health_stress tests/test_shared_health_stress.cpp)
target_link_libraries(test_shared_health_stress PRIVATE thyme_runtime Threads::Threads)
add_executable(test_object_pool_concurrent tests/test_object_pool_concurrent.cpp)
target_link_libraries(test_object_pool_concurrent PRIVATE thyme_runtime Threads::Threads)
thyme_test(test_shm)
thyme_test(test_shm_region_construct)
thyme_test(test_shm_lifecycle)
thyme_test(test_app)
# OBS-08: leveled/rate-limited logger — filtering, fields, rotation, zero-alloc.
thyme_test(test_log)
# TST-13 allocation sentinel: interposes operator new/delete and asserts the
# App hot path (cyclic tick + record tap + event dispatch) does not allocate
# after Start(). Separate binary because the interposition is process-wide.
thyme_test(test_core_no_alloc)
thyme_test(test_shared_health)
add_executable(test_replay_mode tests/test_replay_mode.cpp)
target_link_libraries(test_replay_mode PRIVATE thyme_app Threads::Threads)
add_executable(test_replay_backpressure tests/test_replay_backpressure.cpp)
target_link_libraries(test_replay_backpressure PRIVATE thyme_app thyme_runtime Threads::Threads)
thyme_test(test_replay_reader thyme_replay thyme_recorder Threads::Threads)
thyme_test(test_replay_session thyme_replay thyme_recorder Threads::Threads)
thyme_test(test_pinned_slot_injector thyme_replay thyme_recorder Threads::Threads)
thyme_test(test_replay_sync thyme_replay thyme_recorder Threads::Threads)
# REP-13: discovered continuous slices, absolute/stitch joins/seek and one gate.
thyme_test(test_replay_playlist thyme_replay thyme_recorder Threads::Threads)
# REP-07: deterministic record -> replay -> record regression (fixed grid,
# real player + injectors + a real re-recording TriggeredRecorder).
thyme_test(test_record_replay_record thyme_replay thyme_recorder
  Threads::Threads)
# REP-12: header rewrite policy on injection (embedded FrameHeader mono/
# session/sequence + ABI-12 re-seal, opt-in byte-for-byte mode).
thyme_test(test_replay_header_rewrite thyme_replay thyme_recorder
  Threads::Threads)
# REC-13: replay determinism (as-fast x2, paced 10x + pause/resume + seek,
# pause/step boundaries, mid-stream seek == fresh --from).
thyme_test(test_replay_determinism thyme_replay thyme_recorder
  Threads::Threads)
# REP-10: replay producer gate (gated Begin, double-write detection, crash
# recovery, old-ABI refusal).
add_executable(test_replay_producer_gate tests/test_replay_producer_gate.cpp)
target_link_libraries(test_replay_producer_gate PRIVATE thyme_runtime
  Threads::Threads)
add_executable(test_session_lease tests/test_session_lease.cpp)
target_link_libraries(test_session_lease PRIVATE thyme_runtime thyme_recorder
  Threads::Threads)
add_executable(test_recorder_drain tests/test_recorder_drain.cpp)
target_link_libraries(test_recorder_drain PRIVATE thyme_recorder Threads::Threads)
add_executable(test_recorder_split tests/test_recorder_split.cpp)
target_link_libraries(test_recorder_split PRIVATE thyme_recorder Threads::Threads)
add_executable(test_mp_supervisor tests/test_mp_supervisor.cpp)
target_link_libraries(test_mp_supervisor PRIVATE thyme_app Threads::Threads)
target_include_directories(test_mp_supervisor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
# OPS-01 supervisor restart (backoff, give-up, stall detection).
thyme_test(test_mp_supervisor_restart)
target_link_libraries(test_mp_supervisor_restart PRIVATE thyme_app Threads::Threads)
# OPS-10 read-only JSON status endpoint over a UNIX stream socket.
thyme_test(test_status_server)
target_link_libraries(test_status_server PRIVATE thyme_app Threads::Threads)
# OPS-03 sd_notify shim (READY=1 / WATCHDOG=1 over an AF_UNIX datagram).
thyme_test(test_systemd_notify)
target_link_libraries(test_systemd_notify PRIVATE thyme_app Threads::Threads)
add_executable(test_mp_session_identity tests/test_mp_session_identity.cpp)
target_link_libraries(test_mp_session_identity PRIVATE thyme_app Threads::Threads)

# ABI-13: build-time capacity profile. Pins the range/identity invariants that a
# single build can prove; cross-profile rejection is covered by the ARCH-11
# attach matrix (test_abi_cross_version).
add_executable(test_abi_profile tests/test_abi_profile.cpp)
target_link_libraries(test_abi_profile PRIVATE thyme_runtime)

# GEN-14: process-local index-handoff queue (pool + (index, generation) ring).
thyme_test(test_local_index_queue Threads::Threads)

# ARCH-19: ordered process-local small-structure 1:N hub.
thyme_test(test_local_hub thyme_recorder Threads::Threads)

# ARCH-19: direct primitive vs Channel facade publish/read A/B.
add_executable(bench_channel_facade tests/bench_channel_facade.cpp)
target_link_libraries(bench_channel_facade PRIVATE thyme_runtime Threads::Threads)
target_compile_definitions(bench_channel_facade PRIVATE
  THYME_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
