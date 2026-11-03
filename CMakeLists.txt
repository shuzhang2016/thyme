cmake_minimum_required(VERSION 3.16)
project(thyme VERSION 0.1.0 LANGUAGES CXX)

option(THYME_BUILD_TESTS "Build the thyme test suite" ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
include(GNUInstallDirs)

# TST-16 — compiler cache. The gen_graph golden tests each configure and build
# a generated project, recompiling the same thyme sources tens of times; with
# RUN_SERIAL that compilation is the dominant cost of a full `ctest`. ccache
# turns a repeat build into cache hits and is a no-op when it is absent.
# Opt out with -DTHYME_USE_CCACHE=OFF.
option(THYME_USE_CCACHE "Use ccache as the C++ compiler launcher when present" ON)
if(THYME_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(THYME_CCACHE ccache)
  if(THYME_CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${THYME_CCACHE}")
    message(STATUS "thyme: compiler launcher is ccache (${THYME_CCACHE})")
  else()
    message(STATUS "thyme: ccache not found; building without a compiler cache")
  endif()
endif()

# CI-04 / TST-01 — per-library coverage. This must be declared before the
# library and test targets: add_compile_options/add_link_options only apply to
# targets created afterwards, and the block used to sit at the bottom of this
# file where it instrumented nothing. Clang emits LLVM profiles consumed by
# llvm-profdata/llvm-cov; GCC emits .gcda consumed by lcov. Use exactly the
# toolchain that was configured — tools/ops/coverage/export_coverage.sh detects
# which of the two formats the build produced.
option(THYME_ENABLE_COVERAGE "Instrument for llvm-cov/gcov coverage reports (CI-04)" OFF)
if(THYME_ENABLE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(--coverage)
    add_link_options(--coverage)
  else()
    message(WARNING "THYME_ENABLE_COVERAGE: unsupported compiler ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

# Strict warnings gate (ISO 26262 / MISRA pre-audit hygiene). Directory-scoped:
# every target below is first-party (no third-party code is built here), so
# add_compile_options is safe. THYME_WERROR is the CI-only hard gate (roadmap
# CI-02): it appends -Werror to the same first-party flag list, so a warning
# fails the MR build. It defaults OFF — local builds keep treating warnings as
# signal, not a hard fail, because compiler versions differ across machines.
option(THYME_STRICT_WARNINGS "Enable strict compiler warnings for thyme targets" ON)
option(THYME_WERROR
       "Treat first-party warnings as errors (CI gate; OFF locally)" OFF)
if(THYME_STRICT_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow
                      -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)
  if(THYME_WERROR)
    add_compile_options(-Werror)
  endif()
endif()

# Embedded language subset for the four core libraries. Tools/tests keep
# exceptions; -fno-rtti is propagated to consumers because they may subclass
# core polymorphic types. Dynamic allocation is a coding rule
# (docs/reference/coding-rules.md) enforced by review + the allocation sentinel test
# (TST-13), not by a compiler flag.
option(THYME_CORE_EMBEDDED
       "Build thyme_runtime/recorder/app/replay with -fno-exceptions -fno-rtti" ON)
# CI-08: reproducible builds. Map the checkout path out of debug/macro
# information so two builds of the same commit from different directories
# produce identical objects. CI still pins the image/toolchain by digest;
# this only removes the local absolute-path source of nondeterminism.
option(THYME_REPRODUCIBLE "Use path-prefix maps for reproducible objects" ON)
if(THYME_REPRODUCIBLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
    "-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=."
    "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=.")
endif()

# ABI-13 — build-time capacity profile. Only the ceilings that size fixed SHM
# tables without moving a payload offset are overridable: the record-mirror
# struct table and the health-page worker slots. The wire-format ceilings
# (512 KB record payload, 2 MB mirror arena, 64 KB transport) stay fixed: they
# are coupled to .thyme.bin and to uint16_t hdr.size. The SPMC reader ceiling
# also stays fixed at 8, because ShmSpmcRing's payload offset (five 64 B cells)
# is exactly filled by the eight per-reader drop slots — raising it is a page
# layout change, not a profile knob (see include/thyme/limits.h).
#
# Every page that embeds one of these mixes it into ShmLayoutHash(), so two
# peers built with different profiles are rejected at attach instead of being
# read with the wrong table size. Changing a value here therefore requires a
# matching ABI-version bump and a rebuild of *every* process in the deployment.
#
# Defaults are the reviewed baseline; when they are left untouched the build is
# byte-for-byte the pre-ABI-13 build (no THYME_ABI_PROFILE_CUSTOM).
set(THYME_ABI_MAX_STRUCT_COUNT 64 CACHE STRING
    "ABI-13: contract kinds per recorded package / mirror page (1..1024)")
set(THYME_ABI_MAX_THREADS 64 CACHE STRING
    "ABI-13: per-process thyme worker ceiling (1..1024)")

if(NOT THYME_ABI_MAX_STRUCT_COUNT STREQUAL "64"
   OR NOT THYME_ABI_MAX_THREADS STREQUAL "64")
  add_compile_definitions(THYME_ABI_PROFILE_CUSTOM=1)
  message(STATUS
          "thyme ABI-13 profile: structs=${THYME_ABI_MAX_STRUCT_COUNT} "
          "threads=${THYME_ABI_MAX_THREADS} "
          "(CUSTOM — layout_hash differs from the default profile)")
else()
  message(STATUS "thyme ABI-13 profile: default (structs=64 threads=64)")
endif()
add_compile_definitions(
  THYME_ABI_MAX_STRUCT_COUNT=${THYME_ABI_MAX_STRUCT_COUNT}
  THYME_ABI_MAX_THREADS=${THYME_ABI_MAX_THREADS})

add_library(thyme_runtime STATIC
  src/runtime/cyclic_thread.cpp
  src/runtime/event_thread.cpp
  # OPS-05: shared size/generation rotation + process log budget.
  src/runtime/rotating_file.cpp
  # OBS-08: leveled, rate-limited service logger (fixed buffers, no alloc).
  src/runtime/log.cpp
  src/runtime/parent_death.cpp
  # OPS-07: allocation-free startup resource self-check (fd/memlock/disk/shm).
  src/runtime/resource_check.cpp
  src/runtime/shm_lifecycle.cpp
  src/runtime/shm_page.cpp
  src/runtime/shm_region.cpp
  src/runtime/producer_gate.cpp
  src/runtime/session_lease.cpp
  src/runtime/app.cpp
  src/record/thyme_bin.cpp
  src/record/thyme_bin_stream.cpp
  src/record/package_hash.cpp
  # ABI-09/ABI-10: package schema sidecar + record-manifest identity.
  src/record/record_schema.cpp
  src/record/mcap_writer.cpp
  src/record/mcap_export.cpp)
add_library(thyme::runtime ALIAS thyme_runtime)
target_include_directories(thyme_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(thyme_runtime PUBLIC Threads::Threads)
if(UNIX AND NOT APPLE)
  target_link_libraries(thyme_runtime PUBLIC rt)
endif()

add_library(thyme_recorder STATIC
  src/record/record_control.cpp
  src/record/record_mirror_client.cpp
  src/record/record_mirror_ring_client.cpp
  src/record/triggered_recorder.cpp
  src/record/triggered_recorder_lifecycle.cpp
  src/record/triggered_recorder_capture.cpp
  src/record/triggered_recorder_drain.cpp
  src/record/recorder_drain.cpp
  src/record/triggered_recorder_continuous.cpp
  src/record/triggered_recorder_package.cpp
  src/record/clock_anchor.cpp
  src/record/package_integrity.cpp
  src/record/quota_state.cpp
  src/record/trigger_config.cpp
  # ARCH-13: generated schema table + --check-config cross-check.
  src/record/trigger_config_schema.cpp
  src/record/trigger_audit.cpp
  src/record/trigger_center.cpp
  src/record/retention.cpp)
add_library(thyme::recorder ALIAS thyme_recorder)
target_link_libraries(thyme_recorder PUBLIC thyme_runtime)

add_library(thyme_app STATIC
  src/app/mp_supervisor.cpp
  src/app/mp_runtime.cpp
  src/app/mp_session_identity.cpp
  src/app/replay_mode.cpp
  src/app/status_server.cpp
  src/app/systemd_notify.cpp)
add_library(thyme::app ALIAS thyme_app)
target_include_directories(thyme_app PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(thyme_app PUBLIC thyme_runtime thyme_recorder)
# OPS-10: the status endpoint reports the build identity to monitoring.
target_compile_definitions(thyme_app PUBLIC
  THYME_VERSION_STRING="thyme ${PROJECT_VERSION}")

# REP-01/REP-02: package-level replay reader and paced ReplaySession.
# REP-04/REP-05: big-frame/small-struct injectors and the synchronized player.
# Core mechanism library: no CLI (that stays in siblings/REP-08).
add_library(thyme_replay STATIC
  src/replay/replay_reader.cpp
  src/replay/replay_session.cpp
  src/replay/pinned_slot_injector.cpp
  src/replay/small_struct_injector.cpp
  src/replay/replay_player.cpp
  src/replay/replay_playlist.cpp
  src/replay/replay_timeline.cpp
  src/replay/topology_map.cpp)
add_library(thyme::replay ALIAS thyme_replay)
target_include_directories(thyme_replay PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(thyme_replay PUBLIC thyme_runtime Threads::Threads)

# ECO-09/ECO-11: read-only SHM flow view. The core exposes no data-plane
# write API (no Push/Write/Commit, no lease acquire, no cursor advance); the
# CLI is a thin main over it.
add_library(thyme_probe_core STATIC
  src/probe/abi_check.cpp
  src/probe/abi_matrix.cpp
  src/probe/page_reader.cpp
  src/probe/latency.cpp
  # ABI-09: package schema sidecar view (decode without generated headers).
  src/probe/schema_view.cpp
  src/probe/probe_api.cpp)
add_library(thyme::probe ALIAS thyme_probe_core)
target_include_directories(thyme_probe_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(thyme_probe_core PUBLIC thyme_runtime Threads::Threads)

if(THYME_CORE_EMBEDDED AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  foreach(_thyme_core thyme_runtime thyme_recorder thyme_app thyme_replay)
    # -fno-exceptions is core-only; -fno-rtti must also apply to consumers that
    # subclass core polymorphic types, otherwise the base typeinfo is missing
    # at link time.
    target_compile_options(${_thyme_core} PRIVATE -fno-exceptions PUBLIC -fno-rtti)
    get_target_property(_thyme_core_options ${_thyme_core} COMPILE_OPTIONS)
    foreach(_thyme_flag -fno-exceptions -fno-rtti)
      list(FIND _thyme_core_options "${_thyme_flag}" _thyme_flag_index)
      if(_thyme_flag_index EQUAL -1)
        message(FATAL_ERROR
          "thyme core language subset: ${_thyme_core} lacks ${_thyme_flag}")
      endif()
    endforeach()
  endforeach()
  message(STATUS "thyme core embedded rules: ON (-fno-exceptions -fno-rtti)")
else()
  message(STATUS "thyme core embedded rules: OFF")
endif()

if(THYME_BUILD_TESTS)
include(tests/runtime/CMakeLists.txt)
include(tests/record/CMakeLists.txt)
include(tests/tools/CMakeLists.txt)
endif()
if(THYME_BUILD_TESTS)
include(tests/generator/CMakeLists.txt)
include(tests/registrations/CMakeLists.txt)
include(tests/integration/CMakeLists.txt)
endif()

include(CMakePackageConfigHelpers)
install(TARGETS thyme_runtime thyme_recorder thyme_app thyme_replay
                thyme_probe_core
  EXPORT thymeTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/thyme DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT thymeTargets
  FILE thymeTargets.cmake
  NAMESPACE thyme::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/thyme)
# Build-tree consumers (tests, sibling tool builds) use the same config file
# and target set without an install step.
export(EXPORT thymeTargets
  FILE ${CMAKE_CURRENT_BINARY_DIR}/thymeTargets.cmake
  NAMESPACE thyme::)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/thymeConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/thymeConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/thyme)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/thymeConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/thymeConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/thymeConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/thyme)

# ECO-06: pkg-config metadata. The library order is dependents first so a
# static link resolves thyme_app/recorder/replay/probe against thyme_runtime.
if(UNIX AND NOT APPLE)
  set(THYME_PC_RT "-lrt")
else()
  set(THYME_PC_RT "")
endif()
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/thyme.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/thyme.pc
  @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/thyme.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# Test isolation: tests in this directory run with THYME_SHM_TRACK=1, so every
# POSIX shm object they create is unlinked at normal exit (including a CHECK
# failure, which exits rather than aborts). Without this a failing or
# mis-cleaning test leaks /dev/shm segments and, once tmpfs fills, any test that
# faults a freshly written page dies with SIGBUS. Production never sets it.
# THYME_SHM_TEST_PREFIX gives tests that scan or share names a per-case
# namespace; pid suffixes still distinguish concurrent instances of one case.
# TEST_LAUNCHER adds its invocation pid and cleans that prefix plus any names
# owned by the command's descendants on exit or termination.
# Leak guard: run the suite's non-excluded cases one at a time and assert that
# /dev/shm returns to its prior state. RUN_SERIAL keeps the snapshot clean.
if(THYME_BUILD_TESTS)
add_test(NAME test_shm_leak_audit
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/check_shm_leaks.py
          --build-dir ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(test_shm_leak_audit PROPERTIES
  LABELS "unit" RUN_SERIAL TRUE TIMEOUT 900)

get_property(_thyme_tests DIRECTORY PROPERTY TESTS)
foreach(_thyme_test IN LISTS _thyme_tests)
  string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _thyme_shm_prefix "${_thyme_test}")
  set_tests_properties(${_thyme_test} PROPERTIES
    TEST_LAUNCHER "python3;${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_launcher.py"
    ENVIRONMENT "THYME_SHM_TRACK=1;THYME_SHM_TEST_PREFIX=thyme_${_thyme_shm_prefix}_")
endforeach()
endif()
