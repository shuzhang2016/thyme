# ECO-06: validate the installed CMake package and pkg-config file.
#
#  1. install the build tree into a throw-away DESTDIR stage;
#  2. configure/build/run a consumer with find_package(thyme CONFIG) against
#     the install tree;
#  3. do the same against the build tree (export(EXPORT));
#  4. compile and run a second consumer with `pkg-config --cflags --libs thyme`
#     from the staged install.
#
# The test is run by ctest with -P; all paths/compilers come from CMakeLists.

cmake_minimum_required(VERSION 3.16)

foreach(required
    THYME_SOURCE_DIR THYME_BUILD_DIR THYME_INSTALL_PREFIX THYME_INSTALL_LIBDIR
    THYME_CXX_COMPILER THYME_WORK_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "test_package_export: missing -D${required}=...")
  endif()
endforeach()

function(run_step what)
  execute_process(COMMAND ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${what} failed (${result})\n${output}\n${error}")
  endif()
endfunction()

file(REMOVE_RECURSE "${THYME_WORK_DIR}")
file(MAKE_DIRECTORY "${THYME_WORK_DIR}")

set(stage "${THYME_WORK_DIR}/stage")
set(install_root "${stage}${THYME_INSTALL_PREFIX}")
set(cmake_dir "${install_root}/${THYME_INSTALL_LIBDIR}/cmake/thyme")
set(pc_dir "${install_root}/${THYME_INSTALL_LIBDIR}/pkgconfig")

# ---- 1. stage an install -------------------------------------------------
run_step("cmake --install"
  ${CMAKE_COMMAND} -E env "DESTDIR=${stage}"
  ${CMAKE_COMMAND} --install "${THYME_BUILD_DIR}")

foreach(installed
    "${cmake_dir}/thymeConfig.cmake"
    "${cmake_dir}/thymeTargets.cmake"
    "${pc_dir}/thyme.pc")
  if(NOT EXISTS "${installed}")
    message(FATAL_ERROR "expected installed file missing: ${installed}")
  endif()
endforeach()
message(STATUS "installed package staged at ${install_root}")

# The build tree must be consumable too, without an install step.
if(NOT EXISTS "${THYME_BUILD_DIR}/thymeConfig.cmake" OR
   NOT EXISTS "${THYME_BUILD_DIR}/thymeTargets.cmake")
  message(FATAL_ERROR "build-tree package files are missing")
endif()

# ---- 2/3. find_package consumers ----------------------------------------
function(build_consumer name config_dir)
  set(build_dir "${THYME_WORK_DIR}/${name}")
  run_step("configure ${name}"
    ${CMAKE_COMMAND}
    -S "${THYME_SOURCE_DIR}/tests/package_consumer"
    -B "${build_dir}"
    "-DCMAKE_BUILD_TYPE=Release"
    "-DCMAKE_CXX_COMPILER=${THYME_CXX_COMPILER}"
    "-Dthyme_DIR=${config_dir}")
  run_step("build ${name}"
    ${CMAKE_COMMAND} --build "${build_dir}" --config Release)
  run_step("run ${name}" "${build_dir}/thyme_consumer")
  message(STATUS "${name}: find_package(thyme CONFIG) OK")
endfunction()

build_consumer("consumer-install" "${cmake_dir}")
build_consumer("consumer-buildtree" "${THYME_BUILD_DIR}")

# ---- 4. pkg-config consumer ---------------------------------------------
find_program(PKG_CONFIG_EXECUTABLE pkg-config)
if(NOT PKG_CONFIG_EXECUTABLE)
  message(STATUS "pkg-config not found: skipping pkg-config consumer")
  return()
endif()

set(pkg_env
  ${CMAKE_COMMAND} -E env
  "PKG_CONFIG_PATH=${pc_dir}"
  "PKG_CONFIG_SYSROOT_DIR=${stage}")

execute_process(
  COMMAND ${pkg_env} ${PKG_CONFIG_EXECUTABLE} --modversion thyme
  RESULT_VARIABLE pc_rc OUTPUT_VARIABLE pc_version ERROR_VARIABLE pc_err)
if(NOT pc_rc EQUAL 0)
  message(FATAL_ERROR "pkg-config --modversion thyme failed: ${pc_err}")
endif()
string(STRIP "${pc_version}" pc_version)
message(STATUS "pkg-config reports thyme ${pc_version}")

execute_process(
  COMMAND ${pkg_env} ${PKG_CONFIG_EXECUTABLE} --cflags thyme
  RESULT_VARIABLE pc_rc OUTPUT_VARIABLE pc_cflags ERROR_VARIABLE pc_err)
if(NOT pc_rc EQUAL 0)
  message(FATAL_ERROR "pkg-config --cflags thyme failed: ${pc_err}")
endif()
execute_process(
  COMMAND ${pkg_env} ${PKG_CONFIG_EXECUTABLE} --libs thyme
  RESULT_VARIABLE pc_rc OUTPUT_VARIABLE pc_libs ERROR_VARIABLE pc_err)
if(NOT pc_rc EQUAL 0)
  message(FATAL_ERROR "pkg-config --libs thyme failed: ${pc_err}")
endif()

string(STRIP "${pc_cflags}" pc_cflags)
string(STRIP "${pc_libs}" pc_libs)
if(NOT pc_cflags MATCHES "-I")
  message(FATAL_ERROR "pkg-config --cflags has no include path: ${pc_cflags}")
endif()
if(NOT pc_libs MATCHES "-lthyme_runtime")
  message(FATAL_ERROR "pkg-config --libs has no thyme libraries: ${pc_libs}")
endif()

separate_arguments(pc_cflags_list NATIVE_COMMAND "${pc_cflags}")
separate_arguments(pc_libs_list NATIVE_COMMAND "${pc_libs}")
run_step("compile pkg-config consumer"
  ${THYME_CXX_COMPILER} -std=c++17 ${pc_cflags_list}
  "${THYME_SOURCE_DIR}/tests/package_consumer/main.cpp"
  ${pc_libs_list}
  -o "${THYME_WORK_DIR}/pkgconfig_consumer")
run_step("run pkg-config consumer" "${THYME_WORK_DIR}/pkgconfig_consumer")
message(STATUS "pkg-config --cflags --libs thyme OK")
