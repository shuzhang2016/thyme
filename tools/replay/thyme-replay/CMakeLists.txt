# REP-08 player CLI + REC-07 trigger-package replay tool. Sibling tool, not
# core: exceptions and iostreams are allowed here (core libs stay as-is).
add_library(thyme_replay_cli_lib STATIC
  replay_cli.cpp
  replay_runner.cpp
  # REP-08 status document (live snapshot + final summary).
  replay_status.cpp
  # REP-09 export/republish (MCAP via the ECO-01 McapWriter, or NDJSON).
  replay_export.cpp
  self_desc.cpp
  canonical.cpp
  # REC-03 single-.tbin valid-prefix salvage.
  replay_repair.cpp)
target_include_directories(thyme_replay_cli_lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(thyme_replay_cli_lib PUBLIC
  thyme_replay thyme_recorder thyme_runtime Threads::Threads)

add_executable(thyme-replay main.cpp)
target_link_libraries(thyme-replay PRIVATE thyme_replay_cli_lib)
