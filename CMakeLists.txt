# Host build: the portable parts, the desktop hardware port, the desktop app
# and the tests. The device build is a separate ESP-IDF project in app/firmware that
# pulls in the same part definitions.
cmake_minimum_required(VERSION 3.25)
# C is for expat, which CMake fetches and compiles (third_party/expat).
# Everything of Biblionaut's own is C++.
project(biblionaut VERSION 0.1.0 LANGUAGES C CXX)

option(BN_BUILD_TESTS "Build the host test suite" ON)
option(BN_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(BN_WERROR "Treat warnings as errors" ON)
option(BN_CLANG_TIDY "Run clang-tidy on Biblionaut's own sources as they compile" OFF)
option(BN_FUZZ "Build the fuzz targets with a fuzzing engine, to look for new inputs (tools/fuzz.py)" OFF)
option(BN_DESKTOP_WINDOW "Give the desktop application a window (--window), made with SDL, which is fetched and built" OFF)

set(CMAKE_CXX_EXTENSIONS OFF)  # -std=c++20, not gnu++20

set(BN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${BN_ROOT}/cmake")

if(BN_SANITIZE)
  # Two adjustments to the undefined-behaviour sanitizer:
  #
  # -fno-sanitize=vptr: that check verifies an object's dynamic type through
  # its RTTI, and this code is built without RTTI. GoogleTest is built with
  # it, so it would check objects of our test classes, find no type
  # information, and report every one as having an invalid vptr.
  #
  # -fno-sanitize-recover: by default a finding is only printed and the
  # program carries on, so a test with undefined behaviour still passes.
  # This makes a finding end the program, which fails the test.
  set(bn_sanitize_flags -fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined)
  add_compile_options(${bn_sanitize_flags} -fno-omit-frame-pointer)
  add_link_options(${bn_sanitize_flags})
endif()

if(BN_FUZZ)
  # The fuzz targets (tests/fuzz) are ordinary programs in every other
  # build, where each goes through the inputs kept beside it and stops.
  # Here they are linked with LLVM's fuzzing engine, libFuzzer, which makes
  # up new inputs, and everything is compiled so that the engine can see
  # which code an input reached. Apple's compiler does not come with the
  # engine: this build wants the LLVM that the format and tidy checks use,
  # and tools/fuzz.py finds it.
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
      "BN_FUZZ needs LLVM's clang, which has the fuzzing engine; this is ${CMAKE_CXX_COMPILER_ID}. "
      "tools/fuzz.py configures the build with the right one.")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link)
  if(NOT BN_SANITIZE)
    # Without AddressSanitizer, so that the heap inside a block is laid out
    # as the firmware has it (core/region_heap.h). The undefined-behaviour
    # sanitizer changes nothing of that kind and stays, set as it is above.
    set(bn_fuzz_ubsan_flags -fsanitize=undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined)
    add_compile_options(${bn_fuzz_ubsan_flags} -fno-omit-frame-pointer)
    add_link_options(${bn_fuzz_ubsan_flags})
  endif()
endif()

if(BN_CLANG_TIDY)
  # The major version is pinned (tools/llvm-version) because each release adds
  # and changes checks. Configuration comes from .clang-tidy.
  file(STRINGS "${BN_ROOT}/tools/llvm-version" BN_LLVM_VERSION LIMIT_COUNT 1)
  find_program(BN_CLANG_TIDY_EXE
    NAMES "clang-tidy-${BN_LLVM_VERSION}" clang-tidy
    HINTS "/opt/homebrew/opt/llvm@${BN_LLVM_VERSION}/bin" /opt/homebrew/opt/llvm/bin
          "/usr/local/opt/llvm@${BN_LLVM_VERSION}/bin"
    REQUIRED)
  execute_process(COMMAND "${BN_CLANG_TIDY_EXE}" --version OUTPUT_VARIABLE _bn_tidy_version)
  if(NOT _bn_tidy_version MATCHES "version ${BN_LLVM_VERSION}\\.")
    message(FATAL_ERROR "clang-tidy ${BN_LLVM_VERSION} is required (tools/llvm-version); found:\n${_bn_tidy_version}")
  endif()
  set(CMAKE_CXX_CLANG_TIDY "${BN_CLANG_TIDY_EXE}")
endif()

include(BnVersion)
include(BnOptions)
include(BnParts)

bn_add_portable_parts("${CMAKE_BINARY_DIR}/parts")
if(BN_DESKTOP_WINDOW)
  add_subdirectory(third_party/sdl)
endif()
add_subdirectory(hw)
add_subdirectory(app/desktop)
add_subdirectory(tools)

if(BN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
