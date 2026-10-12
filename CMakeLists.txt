cmake_minimum_required(VERSION 3.20)
project(tracedb VERSION 0.1.0 LANGUAGES CXX)

# ---- Language settings ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Default build type ----
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---- Sanitizers ----
option(TRACEDB_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
if(TRACEDB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined
                      -fno-sanitize-recover=all
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---- Warnings (applied only to our own targets) ----
set(TRACEDB_WARNINGS -Wall -Wextra -Wpedantic -Wshadow)

# ---- The library ----
add_library(tracedb
  src/status.cc
  src/util/coding.cc
  src/span_codec.cc
  src/memtable/memtable.cc
  src/db_impl.cc
)
target_include_directories(tracedb
  PUBLIC  ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_compile_options(tracedb PRIVATE ${TRACEDB_WARNINGS})

# ---- GoogleTest ----
include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

# ---- Tests ----
enable_testing()
add_executable(tracedb_tests
  tests/unit/status_test.cc
  tests/unit/coding_test.cc
  tests/unit/span_codec_test.cc
  tests/unit/memtable_test.cc
  tests/unit/db_test.cc
  tests/differential/differential_test.cc
)
target_include_directories(tracedb_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tracedb_tests PRIVATE tracedb GTest::gtest_main)
target_compile_options(tracedb_tests PRIVATE ${TRACEDB_WARNINGS})

include(GoogleTest)
gtest_discover_tests(tracedb_tests)
