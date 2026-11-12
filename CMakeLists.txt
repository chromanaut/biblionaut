# Host build: the portable parts, the desktop hardware port, the desktop app
# and the tests. The device build is a separate ESP-IDF project in app/firmware that
# pulls in the same part definitions.
cmake_minimum_required(VERSION 3.25)
project(biblionaut VERSION 0.1.0 LANGUAGES CXX)

option(BN_BUILD_TESTS "Build the host test suite" ON)
option(BN_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(BN_WERROR "Treat warnings as errors" ON)
option(BN_CLANG_TIDY "Run clang-tidy on Biblionaut's own sources as they compile" OFF)

set(CMAKE_CXX_EXTENSIONS OFF)  # -std=c++20, not gnu++20

set(BN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${BN_ROOT}/cmake")

if(BN_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
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
add_subdirectory(hw)
add_subdirectory(app/desktop)
add_subdirectory(tools)

if(BN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
