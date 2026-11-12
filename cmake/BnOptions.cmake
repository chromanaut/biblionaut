# bn_options: language rules and warnings shared by every Biblionaut target.
# Link it PRIVATE so the flags do not leak into third-party code.
#
# C++20, no exceptions, no RTTI.
add_library(bn_options INTERFACE)
target_compile_features(bn_options INTERFACE cxx_std_20)
target_compile_options(bn_options INTERFACE
  -fno-exceptions
  -fno-rtti
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wconversion)
if(BN_WERROR)
  target_compile_options(bn_options INTERFACE -Werror)
endif()

# How much timing trace is compiled in: 0 none, 1 top-level spans only,
# 2 everything. Left empty, core/trace.h picks its default (2).
set(BN_TRACE_LEVEL "" CACHE STRING "Trace level compiled in: 0, 1 or 2 (empty: the default, 2)")
if(NOT BN_TRACE_LEVEL STREQUAL "")
  target_compile_definitions(bn_options INTERFACE BN_TRACE_LEVEL=${BN_TRACE_LEVEL})
endif()
