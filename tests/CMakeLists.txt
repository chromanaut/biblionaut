# Host tests. Unit tests now; golden display-list, golden-image, corpus and
# fuzz tests join them under this directory as the parts they cover arrive.
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  SYSTEM)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
# clang-tidy checks our code, not GoogleTest's.
set(_bn_clang_tidy "${CMAKE_CXX_CLANG_TIDY}")
set(CMAKE_CXX_CLANG_TIDY "")
FetchContent_MakeAvailable(googletest)
set(CMAKE_CXX_CLANG_TIDY "${_bn_clang_tidy}")
include(GoogleTest)

# One executable per part, linked against that part only, so a test cannot
# reach further than the code it tests.
function(bn_add_unit_test name)
  cmake_parse_arguments(arg "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${arg_SOURCES})
  target_include_directories(${name} PRIVATE support)
  target_link_libraries(${name} PRIVATE ${arg_LIBS} bn_core_runtime bn_options GTest::gtest_main)
  gtest_discover_tests(${name}
    # The heap tests ask for impossible sizes on purpose; without this the
    # sanitizer runtime aborts instead of letting malloc return null.
    PROPERTIES ENVIRONMENT "ASAN_OPTIONS=allocator_may_return_null=1")
endfunction()

bn_add_unit_test(core_tests
  SOURCES
    unit/core/arena_test.cpp
    unit/core/byte_source_test.cpp
    unit/core/checksum_test.cpp
    unit/core/frame_test.cpp
    unit/core/heap_test.cpp
    unit/core/metrics_test.cpp
    unit/core/trace_level0_test.cpp
    unit/core/trace_level1_test.cpp
    unit/core/trace_stream_test.cpp
    unit/core/trace_test.cpp
  LIBS bn_core)

bn_add_unit_test(reader_tests
  SOURCES
    unit/reader/app_test.cpp
  LIBS bn_reader)

bn_add_unit_test(tools_tests
  SOURCES
    unit/tools/trace_tool_test.cpp
  LIBS bn_trace_tool)

# The whole diagnostic path on the desktop: application -> capture file ->
# bn-trace -> timeline.
add_test(
  NAME e2e.TraceRoundTrip
  COMMAND ${CMAKE_COMMAND}
    -DAPP=$<TARGET_FILE:biblionaut-desktop>
    -DTOOL=$<TARGET_FILE:bn-trace>
    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/trace_roundtrip.cmake)
