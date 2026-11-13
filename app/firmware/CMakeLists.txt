# Device build: the ESP-IDF project for the firmware image. One image serves
# every supported device (ESP32-C3); device-specific code lives under hw/.
#
#   idf.py -C app/firmware build
#
# ESP-IDF components live under this directory (main, components/bn_hw). The
# portable parts are the same plain CMake libraries the host build uses,
# added below, outside any component, so they see no ESP-IDF include paths.
cmake_minimum_required(VERSION 3.22)

get_filename_component(BN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
list(APPEND CMAKE_MODULE_PATH "${BN_ROOT}/cmake")

# For checking crash reports on a device: a firmware that crashes on purpose.
#
#   tools/idf -DBN_CRASH_TEST=fault build flash     (back to normal: -DBN_CRASH_TEST=)
#
# It crashes ten seconds after starting, in the way named, and does not crash
# again on the start that follows a crash, so it cannot loop.
set(BN_CRASH_TEST "" CACHE STRING "Crash on purpose after ten seconds: fault, abort, oom or stack. Empty: never.")
set_property(CACHE BN_CRASH_TEST PROPERTY STRINGS "" fault abort oom stack)

# For checking, on a device, that the panel's two planes hold what the code
# driving them believes (reader/pattern_viewer.h says what is checked):
#
#   tools/idf -DBN_PLANE_CHECK=ON build flash    (back to normal: -DBN_PLANE_CHECK=OFF)
#
# Such a firmware reads both planes back whenever the panel comes to rest,
# which takes about a second each time.
option(BN_PLANE_CHECK "Read the panel's planes back and check them whenever it comes to rest" OFF)

# For checking, on a device, that the SD card can be relied on and how fast
# it is (reader/storage_check.h says what is done):
#
#   tools/idf -DBN_STORAGE_CHECK=ON build flash    (back to normal: -DBN_STORAGE_CHECK=OFF)
#
# Such a firmware writes to the card at start, and again at each press of
# the Power button. It can be combined with BN_PLANE_CHECK.
option(BN_STORAGE_CHECK "Put the SD card through its paces at start and at each press of Power" OFF)

# For checking, on a device, that books can be read off the card and how
# fast (reader/book_check.h says what is done):
#
#   tools/idf -DBN_BOOK_CHECK=ON build flash    (back to normal: -DBN_BOOK_CHECK=OFF)
#
# Such a firmware reads every EPUB in /books from end to end at start, and
# again at each press of the Power button. It writes nothing to the card.
# Not together with BN_STORAGE_CHECK.
option(BN_BOOK_CHECK "Read every book on the SD card at start and at each press of Power" OFF)

# Build only what main needs (directly or indirectly), not every ESP-IDF
# component. Keeps WiFi, TLS and the rest out of the build entirely.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(biblionaut)

# The portable parts get ESP-IDF's code generation flags (optimisation level,
# section flags, libc selection) but none of its include directories.
idf_build_get_property(bn_idf_compile_options COMPILE_OPTIONS GENERATOR_EXPRESSION)
idf_build_get_property(bn_idf_cxx_compile_options CXX_COMPILE_OPTIONS GENERATOR_EXPRESSION)
add_compile_options("${bn_idf_compile_options}")
add_cxx_compile_options("${bn_idf_cxx_compile_options}")

# ESP-IDF's flags already include -Werror (with a few exceptions), so the
# parts do not add their own here.
set(BN_WERROR OFF)
include(BnVersion)
include(BnOptions)
include(BnParts)

# The device hardware layer is an ESP-IDF component; give it the name the
# parts link.
add_library(bn_hw INTERFACE)
target_link_libraries(bn_hw INTERFACE idf::bn_hw)

bn_add_portable_parts("${CMAKE_BINARY_DIR}/bn")

# Keep a copy of every firmware's ELF file, named by its SHA-256. A crash
# report carries the start of that hash, so tools/bn-symbolize can find the
# exact build a report came from, however many builds ago it was flashed.
idf_build_get_property(bn_elf EXECUTABLE)
idf_build_get_property(bn_python PYTHON)
add_custom_command(TARGET ${bn_elf} POST_BUILD
  COMMAND ${bn_python} "${BN_ROOT}/tools/bn-symbolize" --archive "$<TARGET_FILE:${bn_elf}>" --label
          "${BN_VERSION_STRING}"
  VERBATIM)

# The portable parts use the C and C++ runtime libraries. Declaring that, for
# linking only, puts the parts ahead of those libraries on the link line.
# Left undeclared they come last, and a library function that only they call
# is not found.
target_link_libraries(bn_core INTERFACE "$<LINK_ONLY:idf::cxx>" "$<LINK_ONLY:idf::esp_libc>")
