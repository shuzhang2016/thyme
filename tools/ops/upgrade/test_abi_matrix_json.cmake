# OPS-06: `thyme_probe abi --matrix` must equal the checked-in golden and the
# same binary must produce it byte-for-byte on every run. Deployment scripts
# consume this JSON, so drift is a deployment bug, not just a test failure.
#
# Invoked by ctest with -DPROBE=<thyme_probe> -DGOLDEN=<golden json>.

if(NOT DEFINED PROBE OR NOT DEFINED GOLDEN)
  message(FATAL_ERROR "usage: cmake -DPROBE=<probe> -DGOLDEN=<json> -P this")
endif()

execute_process(
  COMMAND "${PROBE}" abi --matrix
  OUTPUT_VARIABLE first
  ERROR_VARIABLE err
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "thyme_probe abi --matrix failed (rc=${rc}): ${err}")
endif()

execute_process(
  COMMAND "${PROBE}" abi --matrix
  OUTPUT_VARIABLE second
  RESULT_VARIABLE rc2)
if(NOT rc2 EQUAL 0)
  message(FATAL_ERROR "second thyme_probe abi --matrix failed (rc=${rc2})")
endif()
if(NOT first STREQUAL second)
  message(FATAL_ERROR "ABI matrix JSON is not stable across runs")
endif()

# The --abi-matrix alias must print exactly the same document.
execute_process(
  COMMAND "${PROBE}" --abi-matrix
  OUTPUT_VARIABLE alias_out
  RESULT_VARIABLE rc3)
if(NOT rc3 EQUAL 0)
  message(FATAL_ERROR "thyme_probe --abi-matrix failed (rc=${rc3})")
endif()
if(NOT alias_out STREQUAL first)
  message(FATAL_ERROR "--abi-matrix and 'abi --matrix' disagree")
endif()

file(READ "${GOLDEN}" golden)
string(STRIP "${first}" actual)
string(STRIP "${golden}" expected)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR
    "ABI matrix JSON drifted from the golden.\n"
    "If the header constants changed on purpose, update\n"
    "${GOLDEN} with:\n  ${PROBE} abi --matrix > ${GOLDEN}")
endif()

# Sanity: the matrix is a JSON object with the expected entries, and the
# phase-1 schema marker is present (scripts rely on it).
string(FIND "${actual}" "{\"schema\":1,\"entries\":[" at_start)
if(NOT at_start EQUAL 0)
  message(FATAL_ERROR "ABI matrix JSON does not start with the schema marker")
endif()
string(FIND "${actual}" "\"constant\":\"kPinnedSlotStoreVersion\"" pinned_slot)
string(FIND "${actual}" "\"constant\":\"kPinnedSlotLeaseVersion\"" lease)
string(FIND "${actual}" "\"constant\":\"kLatestSlotLayoutVersion\"" slot)
if(pinned_slot LESS 0 OR lease LESS 0 OR slot LESS 0)
  message(FATAL_ERROR "ABI matrix is missing a core version row")
endif()
