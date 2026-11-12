# hw: hardware abstraction layer. Depends on core only.
#
# hw/include is the interface every port implements. This file builds the
# desktop port for the host build. Device support (hw/x4) needs ESP-IDF, so
# it is built as an ESP-IDF component by app/firmware/components/bn_hw.
add_library(bn_hw STATIC
  desktop/platform.cpp)
target_include_directories(bn_hw PUBLIC include)
target_link_libraries(bn_hw
  PUBLIC bn_core
  PRIVATE bn_options)
