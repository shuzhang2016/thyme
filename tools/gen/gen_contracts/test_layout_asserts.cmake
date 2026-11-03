# ABI-03: generated contract layout asserts.
#
# 1. Regenerate a contracts header tree from tools/gen/gen_contracts/testdata/
#    layout_sample.json (never a checked-in artifact).
# 2. Check the emitted sizeof/offsetof/alignof/array-stride asserts exist with
#    the expected computed numbers (single-field, padded, array, nested cases).
# 3. Compile a TU that includes the generated header under the project's strict
#    warning set with -Werror: the generated asserts must hold for the real
#    C++ compiler, and emitting them must be warning-free.
# 4. Tamper one size assert and one array-stride assert in a copy and require
#    the compile to fail, proving the asserts are actually enforced.
#
# Required: -DPYTHON=<python3> -DCXX=<compiler> -DTHYME=<repo root> -DWORK=<dir>
if(NOT PYTHON OR NOT CXX OR NOT THYME OR NOT WORK)
  message(FATAL_ERROR "PYTHON, CXX, THYME and WORK are required")
endif()

set(TESTDATA "${THYME}/tools/gen/gen_contracts/testdata")
set(GEN "${THYME}/tools/gen/gen_contracts/gen_contracts.py")
set(GEN_OUT "${WORK}/gen")
set(BAD_OUT "${WORK}/tampered")
file(REMOVE_RECURSE "${WORK}")

execute_process(
  COMMAND "${PYTHON}" "${GEN}" --in "${TESTDATA}/layout_sample.json"
          --out "${GEN_OUT}" --force --date 2026-01-01
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_contracts failed (${rc}):\n${cout}${cerr}")
endif()

set(HEADER "${GEN_OUT}/include/lsl/contracts/thyme_types.h")
if(NOT EXISTS "${HEADER}")
  message(FATAL_ERROR "generator did not write ${HEADER}")
endif()
file(READ "${HEADER}" body)

# Expected asserts, one per covered case.
set(expected
  "static_assert(sizeof(OneField_t) == 8,"
  "static_assert(alignof(OneField_t) == 8,"
  "static_assert(offsetof(Padded_t, b) == 8,"
  "static_assert(offsetof(Padded_t, c) == 16,"
  "static_assert(offsetof(Padded_t, d) == 18,"
  "static_assert(offsetof(Arrays_t, elems) == 8,"
  "static_assert(sizeof(Arrays_t::elems) == 16,"
  "static_assert(sizeof(Arrays_t::elems[0]) == 8,"
  "static_assert(sizeof(Arrays_t::u16x3[0]) == 2,"
  "static_assert(sizeof(Arrays_t::tail[0]) == 1,"
  "static_assert(offsetof(Nested_t, padded) == 16,"
  "static_assert(sizeof(Nested_t) == 48,")
foreach(want IN LISTS expected)
  string(FIND "${body}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "generated header is missing assert: ${want}\n${body}")
  endif()
endforeach()

# 3. The generated asserts must compile under the strict warning gate.
set(STRICT_FLAGS -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Werror)
execute_process(
  COMMAND "${CXX}" ${STRICT_FLAGS} "-I${GEN_OUT}/include"
          -c "${TESTDATA}/layout_asserts_tu.cpp" -o "${WORK}/layout_asserts_tu.o"
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR
    "generated layout asserts failed to compile (${rc}):\n${cout}${cerr}")
endif()

# 3b. ABI-09: the runtime layout table must compile against the same header
#     (it emits table <-> offsetof/sizeof/alignof static_asserts, so a model
#     drift fails here instead of producing a wrong package schema).
file(WRITE "${WORK}/contract_layout_tu.cpp" [==[#include "lsl/contracts/contract_layout.h"
int main() {
  const thyme::schema::ContractLayout* c =
      thyme_contract_layout::FindContractLayout("Padded_t");
  return c == nullptr ? 1 : static_cast<int>(c->field_count);
}
]==])
execute_process(
  COMMAND "${CXX}" ${STRICT_FLAGS} "-I${GEN_OUT}/include"
          "-I${THYME}/include"
          -c "${WORK}/contract_layout_tu.cpp" -o "${WORK}/contract_layout_tu.o"
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR
    "generated contract layout table failed to compile (${rc}):\n${cout}${cerr}")
endif()

# 4. Negative control: tampered numbers must break the build.
file(COPY "${GEN_OUT}/include" DESTINATION "${BAD_OUT}")
set(bad_header "${BAD_OUT}/include/lsl/contracts/thyme_types.h")
file(READ "${bad_header}" bad_body)
string(REPLACE "static_assert(sizeof(Padded_t) == 24,"
               "static_assert(sizeof(Padded_t) == 25," bad_body "${bad_body}")
string(REPLACE "static_assert(sizeof(Arrays_t::elems[0]) == 8,"
               "static_assert(sizeof(Arrays_t::elems[0]) == 9,"
               bad_body "${bad_body}")
file(WRITE "${bad_header}" "${bad_body}")
execute_process(
  COMMAND "${CXX}" ${STRICT_FLAGS} "-I${BAD_OUT}/include"
          -c "${TESTDATA}/layout_asserts_tu.cpp" -o "${WORK}/tampered.o"
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(rc EQUAL 0)
  message(FATAL_ERROR
    "tampered layout asserts still compiled; the asserts are not enforced")
endif()
string(FIND "${cerr}${cout}" "static assertion failed" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR
    "tampered header failed for the wrong reason:\n${cout}${cerr}")
endif()

# 5. AUTOSAR type spellings change only the field decls; the asserts must still
#    be emitted and compile against a Platform_Types.h provider.
set(AS_OUT "${WORK}/autosar")
execute_process(
  COMMAND "${PYTHON}" "${GEN}" --in "${TESTDATA}/layout_sample.json"
          --out "${AS_OUT}" --force --autosar-types --date 2026-01-01
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_contracts --autosar-types failed (${rc}):\n${cout}${cerr}")
endif()
execute_process(
  COMMAND "${CXX}" ${STRICT_FLAGS} "-I${AS_OUT}/include"
          "-I${TESTDATA}/autosar_stub"
          -c "${TESTDATA}/layout_asserts_tu.cpp" -o "${WORK}/autosar.o"
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR
    "AUTOSAR-spelled contract asserts failed to compile (${rc}):\n${cout}${cerr}")
endif()

# 6. ABI-14: a bounded array rejects n > count only in Debug. The Release
#    object must be byte-identical to the same contract without length_of.
set(ABI14_OUT "${WORK}/abi14")
set(ABI14_BASE_OUT "${WORK}/abi14_base")
execute_process(
  COMMAND "${PYTHON}" "${GEN}" --in "${TESTDATA}/length_of_sample.json"
          --out "${ABI14_OUT}" --force --with-fill-stub --date 2026-01-01
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_contracts length_of failed (${rc}):\n${cout}${cerr}")
endif()

file(READ "${TESTDATA}/length_of_sample.json" abi14_base_json)
string(REPLACE ", \"length_of\": \"n\"" "" abi14_base_json "${abi14_base_json}")
file(WRITE "${WORK}/length_of_base.json" "${abi14_base_json}")
execute_process(
  COMMAND "${PYTHON}" "${GEN}" --in "${WORK}/length_of_base.json"
          --out "${ABI14_BASE_OUT}" --force --with-fill-stub --date 2026-01-01
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_contracts length_of baseline failed (${rc}):\n${cout}${cerr}")
endif()

function(compile_length_of_tu out mode object)
  if(mode STREQUAL "RELEASE")
    set(mode_flags -DNDEBUG -O2)
  else()
    set(mode_flags -O0)
  endif()
  execute_process(
    COMMAND "${CXX}" ${STRICT_FLAGS} ${mode_flags} "-I${out}/include"
            "-I${THYME}/include" "${TESTDATA}/length_of_tu.cpp" -o "${object}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "length_of ${mode} compile failed (${rc}):\n${cout}${cerr}")
  endif()
endfunction()

compile_length_of_tu("${ABI14_OUT}" DEBUG "${WORK}/length_of_debug")
compile_length_of_tu("${ABI14_OUT}" RELEASE "${WORK}/length_of_release")
compile_length_of_tu("${ABI14_BASE_OUT}" RELEASE "${WORK}/length_of_base_release")
foreach(mode debug release)
  execute_process(
    COMMAND "${WORK}/length_of_${mode}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR
      "length_of ${mode} behavior failed (${rc}):\n${cout}${cerr}")
  endif()
endforeach()

file(SHA256 "${WORK}/length_of_release" abi14_release_hash)
file(SHA256 "${WORK}/length_of_base_release" abi14_base_release_hash)
if(NOT abi14_release_hash STREQUAL abi14_base_release_hash)
  message(FATAL_ERROR "length_of changed the Release object; expected no check or branch")
endif()
