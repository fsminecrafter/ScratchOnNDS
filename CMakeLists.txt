cmake_minimum_required(VERSION 3.16)
project(ScratchDS_Tests CXX)

set(CMAKE_CXX_STANDARD 14)

option(SCRATCHDS_HOST_TESTS "Build host-side unit tests" OFF)

if(NOT SCRATCHDS_HOST_TESTS)
  message(STATUS "Host tests disabled. Pass -DSCRATCHDS_HOST_TESTS=ON to enable.")
  return()
endif()

enable_testing()

# ─────────────────────────────────────────────────────────────────────────────
# Fetch dependencies
# ─────────────────────────────────────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
)
FetchContent_MakeAvailable(doctest)

FetchContent_Declare(
  jsmn
  GIT_REPOSITORY https://github.com/zserge/jsmn.git
  GIT_TAG        v1.1.0
)
FetchContent_MakeAvailable(jsmn)

# ─────────────────────────────────────────────────────────────────────────────
# Directories
# ─────────────────────────────────────────────────────────────────────────────
set(STUB_DIR ${CMAKE_SOURCE_DIR}/tests/stubs)

# ─────────────────────────────────────────────────────────────────────────────
# Core sources + host stubs for hardware singletons vm.cpp links against
# ─────────────────────────────────────────────────────────────────────────────
set(CORE_SOURCES
  source/core/project.cpp
  source/core/vm.cpp
  tests/stubs/audio_manager_stub.cpp
  tests/stubs/input_handler_stub.cpp
)

add_library(scratchds_core_host STATIC ${CORE_SOURCES})

target_include_directories(scratchds_core_host PUBLIC
  ${STUB_DIR}
  source
  ${jsmn_SOURCE_DIR}
)

target_compile_definitions(scratchds_core_host PUBLIC
  SCRATCHDS_HOST_BUILD
  SCRATCHDS_VERSION="test"
  JSMN_STATIC
)

# ─────────────────────────────────────────────────────────────────────────────
# Test helper
# ─────────────────────────────────────────────────────────────────────────────
function(add_scratchds_test name src)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE scratchds_core_host doctest::doctest)
  target_include_directories(${name} PRIVATE ${STUB_DIR} source ${jsmn_SOURCE_DIR})
  target_compile_definitions(${name} PRIVATE SCRATCHDS_HOST_BUILD JSMN_STATIC)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# ─────────────────────────────────────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────────────────────────────────────
add_scratchds_test(test_scratch_value   tests/unit/test_scratch_value.cpp)
add_scratchds_test(test_project_parser  tests/unit/test_project_parser.cpp)
add_scratchds_test(test_vm_execution    tests/unit/test_vm_execution.cpp)
add_scratchds_test(test_opcode_map      tests/unit/test_opcode_map.cpp)
add_scratchds_test(test_clap_detector   tests/unit/test_clap_detector.cpp)
add_scratchds_test(test_combo_tracker   tests/unit/test_combo_tracker.cpp)
