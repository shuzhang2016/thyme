if(NOT DEFINED TRIGGER_CENTER OR NOT DEFINED MINIMAL_CONFIG OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "TRIGGER_CENTER, MINIMAL_CONFIG and WORK_DIR are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(BASE "${WORK_DIR}/trigger.base.ini")
set(POLICY "${WORK_DIR}/trigger.policy.ini")
set(MANIFEST "${WORK_DIR}/record_manifest.h")
file(WRITE "${MANIFEST}" "#pragma once\n"
  "inline constexpr unsigned kRecordManifestCount = 3;\n"
  "inline constexpr const char* kRecordManifestChannels[] = {\n"
  "  \"can_chassis\",\n  \"demo\",\n  \"h264_front\"\n};\n")
file(WRITE "${BASE}" "[channel./demo]\nkind = mirror_ring\n"
  "record_channels = demo\nrate_hz = 10\navg_frame_bytes = 128\n")
file(WRITE "${POLICY}"
  "[trigger_center]\nallow_empty_sources = true\n[recorder]\n"
  "output_dir = ${WORK_DIR}/out\n[source.manual]\n"
  "quota_class = manual\n")

execute_process(
  COMMAND "${TRIGGER_CENTER}" --check-config "${MINIMAL_CONFIG}"
  RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
if(RESULT EQUAL 0 OR NOT STDERR MATCHES "record-manifest")
  message(FATAL_ERROR "missing manifest was not rejected (${RESULT}): ${STDOUT}${STDERR}")
endif()

execute_process(
  COMMAND "${TRIGGER_CENTER}" --check-config --record-manifest "${MANIFEST}"
    "${MINIMAL_CONFIG}"
  RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
if(NOT RESULT EQUAL 0 OR NOT STDOUT MATCHES "config OK")
  message(FATAL_ERROR "legacy check failed (${RESULT}): ${STDOUT}${STDERR}")
endif()

execute_process(
  COMMAND "${TRIGGER_CENTER}" --check-config --record-manifest "${MANIFEST}"
    "${BASE}" "${POLICY}"
  RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
if(NOT RESULT EQUAL 0 OR NOT STDOUT MATCHES "config OK")
  message(FATAL_ERROR "layered check failed (${RESULT}): ${STDOUT}${STDERR}")
endif()

set(SUPERSET_BASE "${WORK_DIR}/trigger.superset.ini")
file(WRITE "${SUPERSET_BASE}" "[channel./demo]\nkind = mirror_ring\n"
  "record_channels = demo,not_in_graph\nrate_hz = 10\navg_frame_bytes = 128\n")
execute_process(
  COMMAND "${TRIGGER_CENTER}" --check-config --record-manifest "${MANIFEST}"
    "${SUPERSET_BASE}" "${POLICY}"
  RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
if(RESULT EQUAL 0 OR NOT STDERR MATCHES "not_in_graph.*not recorded")
  message(FATAL_ERROR "external manifest subset was not enforced (${RESULT}): ${STDOUT}${STDERR}")
endif()

execute_process(
  COMMAND "${TRIGGER_CENTER}" --dump-config --record-manifest "${MANIFEST}"
    "${BASE}" "${POLICY}"
  RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
if(NOT RESULT EQUAL 0 OR NOT STDOUT MATCHES "\\[channel\\./demo\\]" OR
   NOT STDOUT MATCHES "record_channels=demo" OR
   NOT STDOUT MATCHES "\\[source.manual\\]")
  message(FATAL_ERROR "layered dump failed (${RESULT}): ${STDOUT}${STDERR}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env THYME_RESOURCE_CHECK=off
    "${TRIGGER_CENTER}" --check-resources --record-manifest "${MANIFEST}"
    "${BASE}" "${POLICY}" --json
  RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
if(NOT RESULT EQUAL 0 OR NOT STDOUT MATCHES "\"checked\":false")
  message(FATAL_ERROR "layered resource check failed (${RESULT}): ${STDOUT}${STDERR}")
endif()

file(WRITE "${POLICY}" "[channel./forbidden]\nkind = mirror_ring\n")
execute_process(
  COMMAND "${TRIGGER_CENTER}" --check-config --record-manifest "${MANIFEST}"
    "${BASE}" "${POLICY}"
  RESULT_VARIABLE RESULT OUTPUT_VARIABLE STDOUT ERROR_VARIABLE STDERR)
if(RESULT EQUAL 0 OR NOT STDERR MATCHES "policy line 1: \\[channel\\.\\*\\] sections are not allowed")
  message(FATAL_ERROR "policy channel rejection failed (${RESULT}): ${STDOUT}${STDERR}")
endif()
