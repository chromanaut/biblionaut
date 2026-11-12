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
