cmake_minimum_required(VERSION 3.16)
project(thyme_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(thyme 0.1 CONFIG REQUIRED)
message(STATUS "thyme version: ${thyme_VERSION}")

add_executable(thyme_consumer main.cpp)
# Link every exported library so a missing/misnamed imported target fails the
# configure or link step, not just the ones this toy program happens to use.
target_link_libraries(thyme_consumer PRIVATE
  thyme::runtime
  thyme::recorder
  thyme::app
  thyme::replay
  thyme::probe)
