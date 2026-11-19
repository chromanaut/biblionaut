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
# again on the start that follows a crash, so it cannot loop. `hang` stops
# the application's loop from coming round, for the watchdog to end.
set(BN_CRASH_TEST "" CACHE STRING
    "Crash on purpose after ten seconds: fault, abort, oom, stack or hang. Empty: never.")
set_property(CACHE BN_CRASH_TEST PROPERTY STRINGS "" fault abort oom stack hang)

# For checking, on a device, that the panel's two planes hold what the code
# driving them believes (reader/pattern_viewer.h says what is checked):
#
#   tools/idf -DBN_PLANE_CHECK=ON build flash    (back to normal: -DBN_PLANE_CHECK=OFF)
#
# Such a firmware reads both planes back whenever the panel comes to rest,
# which takes about a second each time.
option(BN_PLANE_CHECK "Read the panel's planes back and check them whenever it comes to rest" OFF)

# For checking, on a device, that the panel's controller takes windows
# narrower than whole rows, and timing them (main/window_check.h says what
# is done):
#
#   tools/idf -DBN_WINDOW_CHECK=ON build flash    (back to normal: -DBN_WINDOW_CHECK=OFF)
#
# Such a firmware writes and reads back both planes at start, for a few
# seconds, and then starts as usual.
option(BN_WINDOW_CHECK "Check at start that the panel takes windows narrower than whole rows" OFF)

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

# With BN_BOOK_CHECK, a check that takes seconds and not minutes: each book
# is opened, its package and its table of contents are read and sixteen
# places in it gone to, and no book is read through.
#
#   tools/idf -DBN_BOOK_CHECK=ON -DBN_BOOK_CHECK_BRIEF=ON build flash
option(BN_BOOK_CHECK_BRIEF "With BN_BOOK_CHECK: open every book and read none through" OFF)

# For timing, on a device, the line breaker, the hyphenator and the
# paragraph setter, and setting the text on the card in lines
# (reader/layout_check.h says what is done):
#
#   tools/idf -DBN_LAYOUT_CHECK=ON build flash    (back to normal: -DBN_LAYOUT_CHECK=OFF)
#
# Such a firmware does it at start, and again at each press of the Power
# button. It reads /layout/paragraphs.txt, and /layout/hyphenation.bnhy if
# it is there (otherwise it uses the built-in table for American English),
# and writes nothing to the card. Not together with BN_STORAGE_CHECK or
# BN_BOOK_CHECK.
option(BN_LAYOUT_CHECK "Time the layout code, and set the text on the SD card, at start and at each press of Power" OFF)

# A firmware that shows the SVG, PNG and JPEG files in /drawings on the SD
# card in place of the test pattern, a drawing drawn as a drawing in a book
# is and a picture read, scaled and dithered (reader/drawing_viewer.h says
# how):
#
#   tools/idf -DBN_DRAWING_VIEWER=ON build flash    (back to normal: -DBN_DRAWING_VIEWER=OFF)
#
# Down and Up go from one to the next. It writes one file to the card, a
# hidden one at its top (/.drawing-spool): what the drawing in hand draws.
# Not together with BN_STORAGE_CHECK, BN_BOOK_CHECK or BN_LAYOUT_CHECK.
# With BN_DRAWING_TOUR as well, it goes round them twice by itself, the
# second time from the cache of what it has shown, for timing them with
# nobody at the buttons:
#
#   tools/idf -DBN_DRAWING_VIEWER=ON -DBN_DRAWING_TOUR=ON build flash
option(BN_DRAWING_VIEWER "Show the SVG, PNG and JPEG files in /drawings on the SD card in place of the test pattern" OFF)
option(BN_DRAWING_TOUR "With BN_DRAWING_VIEWER: go round the drawings twice by itself" OFF)

# For checking, on a device, a partition table with a data partition for
# fonts in it, and what reading that partition costs
# (main/partition_check.h says what is done):
#
#   tools/idf -B app/firmware/build-partition-check -DBN_PARTITION_CHECK=ON build flash
#
# Such a build has a partition table of its own (partitions_fonts.csv),
# which sdkconfig.partition_check chooses, and keeps its settings in its
# own build directory rather than in app/firmware/sdkconfig, so the normal
# build is left as it was. Flashing it writes the new table, and fills the
# fonts partition with what the check expects to find there (12 MB, about
# a minute over USB). To go back, flash a normal build: it writes ESP-IDF's
# own partition table again.
option(BN_PARTITION_CHECK "Use a partition table with a fonts partition, and check it at start" OFF)
if(BN_PARTITION_CHECK)
  set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
  set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.partition_check")
endif()

# Build only what main needs (directly or indirectly), not every ESP-IDF
# component. Keeps WiFi, TLS and the rest out of the build entirely.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(biblionaut)

# The portable parts get ESP-IDF's code generation flags (optimisation level,
# section flags, libc selection) but none of its include directories.
idf_build_get_property(bn_idf_compile_options COMPILE_OPTIONS GENERATOR_EXPRESSION)
idf_build_get_property(bn_idf_cxx_compile_options CXX_COMPILE_OPTIONS GENERATOR_EXPRESSION)
idf_build_get_property(bn_idf_c_compile_options C_COMPILE_OPTIONS GENERATOR_EXPRESSION)
add_compile_options("${bn_idf_compile_options}")
add_cxx_compile_options("${bn_idf_cxx_compile_options}")
# For the one thing among them that is C: expat (third_party/expat).
add_c_compile_options("${bn_idf_c_compile_options}")

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

# The table of American English's hyphenation patterns is made as the build
# goes, by bn-hyphen (third_party/hyph-en-us). That is a program for the
# computer doing the building, not for the device, so it is compiled here a
# second time with that computer's own C and C++ compilers: by the host
# build's CMake files, in a build of their own that makes bn-hyphen and
# nothing else, under this one's build directory. It takes the files this
# build downloads, and downloads nothing itself.
set(bn_host_tools "${CMAKE_BINARY_DIR}/host-tools")
set(BN_HYPHEN "${bn_host_tools}/tools/hyphen/bn-hyphen")
set(BN_HYPHEN_DEPENDS bn_host_tools)
bn_add_portable_parts("${CMAKE_BINARY_DIR}/bn")

include(ExternalProject)
include(FetchContent)
# Nothing is installed from that build; this only keeps CMake from asking
# where it would go.
if(POLICY CMP0225)
  cmake_policy(SET CMP0225 NEW)
endif()
FetchContent_GetProperties(expat SOURCE_DIR bn_expat_source)
ExternalProject_Add(bn_host_tools
  SOURCE_DIR "${BN_ROOT}"
  BINARY_DIR "${bn_host_tools}"
  CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DBN_BUILD_TESTS=OFF -DBN_WERROR=OFF
             "-DFETCHCONTENT_SOURCE_DIR_EXPAT=${bn_expat_source}" "-DBN_HYPH_EN_US_DIR=${BN_HYPH_EN_US_DIR}"
  BUILD_COMMAND ${CMAKE_COMMAND} --build "${bn_host_tools}" --target bn-hyphen
  # Built each time, so that a change to bn-hyphen's sources is taken up;
  # when there is none, that is a moment's work.
  BUILD_ALWAYS ON
  BUILD_BYPRODUCTS "${BN_HYPHEN}"
  INSTALL_COMMAND "")

# The partition check's pattern for the fonts partition, made to the size
# the table gives the partition, and written to it with the rest by
# `flash`.
if(BN_PARTITION_CHECK)
  idf_build_get_property(bn_python PYTHON)
  partition_table_get_partition_info(bn_fonts_size "--partition-name fonts" "size")
  set(bn_fonts_pattern "${CMAKE_BINARY_DIR}/fonts_pattern.bin")
  add_custom_command(
    OUTPUT "${bn_fonts_pattern}"
    COMMAND ${bn_python} "${CMAKE_CURRENT_LIST_DIR}/partition_pattern.py" "${bn_fonts_size}" "${bn_fonts_pattern}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/partition_pattern.py" "${CMAKE_CURRENT_LIST_DIR}/partitions_fonts.csv"
    COMMENT "Making the pattern for the fonts partition"
    VERBATIM)
  add_custom_target(bn_fonts_pattern ALL DEPENDS "${bn_fonts_pattern}")
  esptool_py_flash_to_partition(flash fonts "${bn_fonts_pattern}")
  add_dependencies(flash bn_fonts_pattern)
endif()

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
