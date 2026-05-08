cmake_minimum_required(VERSION 3.16)
project(ScratchDS_Tests CXX)

set(CMAKE_CXX_STANDARD 14)

# ─────────────────────────────────────────────────────────────────────────────
# Option: enable host tests (OFF by default; ON in CI)
# ─────────────────────────────────────────────────────────────────────────────
option(SCRATCHDS_HOST_TESTS "Build host-side unit tests" OFF)

if(NOT SCRATCHDS_HOST_TESTS)
  message(STATUS "Host tests disabled. Pass -DSCRATCHDS_HOST_TESTS=ON to enable.")
  return()
endif()

enable_testing()

# ─────────────────────────────────────────────────────────────────────────────
# Fetch doctest (single-header test framework, no NDS dependency)
# ─────────────────────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
)
FetchContent_MakeAvailable(doctest)

# ─────────────────────────────────────────────────────────────────────────────
# Stub include directory: provides fake <nds.h> and friends so that
# source files that include them can still compile on a host Linux build.
# ─────────────────────────────────────────────────────────────────────────────
set(STUB_DIR ${CMAKE_SOURCE_DIR}/tests/stubs)

# ─────────────────────────────────────────────────────────────────────────────
# Sources under test (only the portable logic files)
# ─────────────────────────────────────────────────────────────────────────────
set(CORE_SOURCES
  source/core/project.cpp
  source/core/vm.cpp
)

# ─────────────────────────────────────────────────────────────────────────────
# Helper library compiled from core sources with NDS stubs
# ─────────────────────────────────────────────────────────────────────────────
add_library(scratchds_core_host STATIC ${CORE_SOURCES})
target_include_directories(scratchds_core_host PUBLIC
  source
  ${STUB_DIR}
)
target_compile_definitions(scratchds_core_host PUBLIC
  SCRATCHDS_HOST_BUILD   # lets source files skip hardware-only sections
  SCRATCHDS_VERSION="test"
)

# ─────────────────────────────────────────────────────────────────────────────
# Test executables
# ─────────────────────────────────────────────────────────────────────────────
function(add_scratchds_test name src)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE scratchds_core_host doctest::doctest)
  target_include_directories(${name} PRIVATE ${STUB_DIR} source)
  target_compile_definitions(${name} PRIVATE SCRATCHDS_HOST_BUILD)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_scratchds_test(test_scratch_value   tests/unit/test_scratch_value.cpp)
add_scratchds_test(test_project_parser  tests/unit/test_project_parser.cpp)
add_scratchds_test(test_vm_execution    tests/unit/test_vm_execution.cpp)
add_scratchds_test(test_opcode_map      tests/unit/test_opcode_map.cpp)
add_scratchds_test(test_clap_detector   tests/unit/test_clap_detector.cpp)
add_scratchds_test(test_combo_tracker   tests/unit/test_combo_tracker.cpp)
