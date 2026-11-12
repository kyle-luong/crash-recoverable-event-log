cmake_minimum_required(VERSION 3.20)
project(eventlog LANGUAGES CXX
        VERSION 0.1.0
        DESCRIPTION "Crash-safe append-only event log with explicit durability guarantees")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(EVENTLOG_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(EVENTLOG_SLOW_TESTS "Enable exhaustive/slow test cases" OFF)

add_library(eventlog_flags INTERFACE)
target_compile_options(eventlog_flags INTERFACE
  -Wall -Wextra -Wpedantic -Werror
  -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
  -Wcast-qual -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion
  -Wformat=2 -Wundef)

if(EVENTLOG_SANITIZERS)
  # -fno-sanitize-recover makes a UBSan finding a hard failure instead of a log line.
  target_compile_options(eventlog_flags INTERFACE
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
  target_link_options(eventlog_flags INTERFACE -fsanitize=address,undefined)
endif()

add_library(eventlog_core STATIC
  src/error.cpp
  src/crc32c.cpp
  src/record.cpp
  src/file_ops.cpp
  src/writer.cpp
  src/reader.cpp
  src/recover.cpp
  src/payload.cpp
  src/crash_harness.cpp)
target_include_directories(eventlog_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(eventlog_core PUBLIC eventlog_flags)

add_executable(eventlog src/main.cpp src/cli.cpp)
target_link_libraries(eventlog PRIVATE eventlog_core)

enable_testing()

set(EVENTLOG_TEST_SUITES test_scaffold test_crc32c test_codec test_writer test_batch test_reader test_recovery test_crash)
foreach(suite IN LISTS EVENTLOG_TEST_SUITES)
  add_executable(${suite} tests/${suite}.cpp)
  target_link_libraries(${suite} PRIVATE eventlog_core)
  target_include_directories(${suite} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(${suite} PRIVATE EVENTLOG_SUITE_NAME="${suite}")
  if(EVENTLOG_SLOW_TESTS)
    target_compile_definitions(${suite} PRIVATE EVENTLOG_SLOW_TESTS=1)
  endif()
  add_test(NAME ${suite} COMMAND ${suite})
  set_tests_properties(${suite} PROPERTIES TIMEOUT 300)
endforeach()

add_test(NAME cli_tests
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_tests.sh $<TARGET_FILE:eventlog>)
# The crash-test sweep is the slow part of this suite, so it gets the same opt-in the
# exhaustive recovery sweep uses.
set_tests_properties(cli_tests PROPERTIES TIMEOUT 900)
if(EVENTLOG_SLOW_TESTS)
  set_tests_properties(cli_tests PROPERTIES ENVIRONMENT "EVENTLOG_SLOW_TESTS=1")
endif()
