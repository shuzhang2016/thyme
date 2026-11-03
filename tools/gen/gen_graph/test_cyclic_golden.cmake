# Byte-identical lock for the cyclic (default) codegen path: regenerating the
# scaffold default graph and the ports fixture graph must reproduce the
# committed reference trees under testdata/golden_cyclic/ exactly. Any codegen
# change that alters cyclic output fails this diff; event-mode additions must
# be conditional on exec: event so this stays green.
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(REF "${THYME_ABS}/tools/gen/gen_graph/testdata/golden_cyclic")
set(OUT "${CMAKE_BINARY_DIR}/golden_cyclic_check")
file(REMOVE_RECURSE "${OUT}")

execute_process(COMMAND "${GEN}" --out "${OUT}/default" --update all
  --no-health-reporter RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph --update all (default) failed: ${rc}")
endif()
execute_process(COMMAND "${GEN}" --out "${OUT}/ports" --update all
  --template-dir "${THYME_ABS}/tools/gen/gen_graph/testdata/ports_tpl"
  --in "${THYME_ABS}/tools/gen/gen_graph/testdata/ports_tpl/graph.yaml"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph --update all (ports) failed: ${rc}")
endif()

foreach(app default ports)
  execute_process(COMMAND diff -r "${REF}/${app}" "${OUT}/${app}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE dout ERROR_VARIABLE derr)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "cyclic golden drift (${app}); cyclic output must stay byte-identical:\n${dout}${derr}")
  endif()
endforeach()
