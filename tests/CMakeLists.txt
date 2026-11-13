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

# What the sanitizer runtime is told when a test runs. The heap tests ask
# for impossible sizes on purpose; without this the runtime aborts instead
# of letting malloc return null.
set(bn_test_asan_options "allocator_may_return_null=1")
# On arm64 Linux, GCC's runtime is very slow at one of its checks: looking
# for uses of a stack frame after its function has returned. The inflater's
# tests took 218 s with it and 4 s without, where x86-64 shows no such
# cost. So there that one check is off, and the x86-64 Linux run (CI's) is
# the one that makes it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$"
   AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  string(APPEND bn_test_asan_options ":detect_stack_use_after_return=0")
endif()

# One executable per part, linked against that part only, so a test cannot
# reach further than the code it tests.
function(bn_add_unit_test name)
  cmake_parse_arguments(arg "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${arg_SOURCES})
  target_include_directories(${name} PRIVATE support)
  target_link_libraries(${name} PRIVATE ${arg_LIBS} bn_core_runtime bn_options GTest::gtest_main)
  gtest_discover_tests(${name} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")
endfunction()

bn_add_unit_test(core_tests
  SOURCES
    unit/core/arena_test.cpp
    unit/core/block_file_test.cpp
    unit/core/byte_source_test.cpp
    unit/core/checksum_test.cpp
    unit/core/crash_test.cpp
    unit/core/file_replacement_test.cpp
    unit/core/frame_test.cpp
    unit/core/heap_test.cpp
    unit/core/inflate_test.cpp
    unit/core/input_sampler_test.cpp
    unit/core/metrics_test.cpp
    unit/core/storage_test.cpp
    unit/core/trace_level0_test.cpp
    unit/core/trace_level1_test.cpp
    unit/core/trace_stream_test.cpp
    unit/core/trace_test.cpp
  LIBS bn_core)

# The inflater is checked against zlib, which only the tests use.
find_package(ZLIB REQUIRED)
target_link_libraries(core_tests PRIVATE ZLIB::ZLIB)

bn_add_unit_test(draw_tests
  SOURCES
    unit/draw/test_pattern_test.cpp
  LIBS bn_draw)

# The ZIP reader's tests make their ZIP files with zlib doing the
# compressing and the checksums.
bn_add_unit_test(epub_tests
  SOURCES
    unit/epub/zip_test.cpp
  LIBS bn_epub)
target_link_libraries(epub_tests PRIVATE ZLIB::ZLIB)

# The desktop port, which the other host programs run on. Its own headers
# are reached the way its sources reach them, as "desktop/...".
bn_add_unit_test(hw_tests
  SOURCES
    unit/hw/folder_storage_test.cpp
    unit/hw/scripted_input_test.cpp
    unit/hw/sim_display_test.cpp
  LIBS bn_hw)
target_include_directories(hw_tests PRIVATE ${PROJECT_SOURCE_DIR}/hw)

bn_add_unit_test(reader_tests
  SOURCES
    unit/reader/app_test.cpp
    unit/reader/book_check_test.cpp
    unit/reader/pattern_viewer_test.cpp
    unit/reader/storage_check_test.cpp
  LIBS bn_reader)
# The pattern viewer's tests compare what reaches the panel with the picture
# from draw, which the reader links but does not pass on. The book check's
# tests are in the same position with the ZIP reader from epub, and make
# their ZIP files with zlib.
target_include_directories(reader_tests PRIVATE
  $<TARGET_PROPERTY:bn_draw,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:bn_epub,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(reader_tests PRIVATE ZLIB::ZLIB)

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

# The application, the simulated panel and the scripted buttons together,
# across the panel's drive voltages being switched off.
add_test(
  NAME e2e.PanelSwitchOff
  COMMAND ${CMAKE_COMMAND}
    -DAPP=$<TARGET_FILE:biblionaut-desktop>
    -DTOOL=$<TARGET_FILE:bn-trace>
    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/panel_switch_off.cmake)

# tools/bn-symbolize against a stand-in ELF archive and addr2line.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME e2e.Symbolize
    COMMAND ${CMAKE_COMMAND}
      -DPYTHON=${Python3_EXECUTABLE}
      -DSCRIPT=${PROJECT_SOURCE_DIR}/tools/bn-symbolize
      -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/symbolize.cmake)

  # bn-zip, and through it the ZIP reader, against ZIP files made by
  # something else: Python's zipfile module, and the zip command where
  # there is one.
  add_test(
    NAME e2e.ZipTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/zip_tool.py
      $<TARGET_FILE:bn-zip> ${CMAKE_CURRENT_BINARY_DIR}/zip_tool)
  set_tests_properties(e2e.ZipTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # The book check, run by the desktop application over a folder of books,
  # against bn-zip and zipfile on the same files.
  add_test(
    NAME e2e.BookCheck
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/book_check.py
      $<TARGET_FILE:biblionaut-desktop> $<TARGET_FILE:bn-trace> $<TARGET_FILE:bn-zip>
      ${CMAKE_CURRENT_BINARY_DIR}/book_check)
  set_tests_properties(e2e.BookCheck PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")
endif()
