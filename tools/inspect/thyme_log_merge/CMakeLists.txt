# thyme_log_merge — OBS-06 multi-process log merge (JSONL + T9P1 perf dumps).
# Sibling tool, not core: exceptions and STL containers are allowed here; the
# library half exists so tests/test_thyme_log_merge exercises the same code
# the binary runs (main.cpp stays thin).
add_library(thyme_log_merge_lib STATIC log_merge.cpp)
target_include_directories(thyme_log_merge_lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
# OBS-09: the analyzer reuses thyme::LatencyHistogram (header-only core type)
# for bucket math and percentile reporting, so offline and on-wire numbers
# come from one implementation.
target_link_libraries(thyme_log_merge_lib PUBLIC thyme_runtime)

add_executable(thyme_log_merge main.cpp)
target_link_libraries(thyme_log_merge PRIVATE thyme_log_merge_lib)
