cmake_minimum_required(VERSION 3.20)
project(thyme_ports_app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT TARGET thyme::runtime)
  set(THYME_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thyme" CACHE PATH "thyme source")
  add_subdirectory("${THYME_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/thyme")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/generated/nodes.cmake)
foreach(id IN LISTS APP_GEN_NODES)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/nodes/${id}.cpp)
    message(FATAL_ERROR "missing hand node src/nodes/${id}.cpp")
  endif()
endforeach()
set(_node_srcs "")
foreach(id IN LISTS APP_GEN_NODES)
  list(APPEND _node_srcs src/nodes/${id}.cpp)
endforeach()
add_library(ports_app_lib STATIC ${_node_srcs} generated/wiring.cpp
  src/support/replay_runner.cpp)
target_include_directories(ports_app_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ports_app_lib PUBLIC thyme::runtime thyme::app
  thyme::recorder thyme::replay)
add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE ports_app_lib)
