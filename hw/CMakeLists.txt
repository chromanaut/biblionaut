# hw: hardware abstraction layer. Depends on core only.
#
# hw/include is the interface every port implements. This file builds the
# desktop port for the host build. Device support (hw/esp32c3 and hw/x4)
# needs ESP-IDF, so it is built as an ESP-IDF component by
# app/firmware/components/bn_hw.
add_library(bn_hw STATIC
  desktop/crash.cpp
  desktop/display.cpp
  desktop/folder_storage.cpp
  desktop/input.cpp
  desktop/memory_probe.cpp
  desktop/platform.cpp
  desktop/scripted_input.cpp
  desktop/serial.cpp
  desktop/sim_display.cpp
  desktop/trace_sync.cpp)
# include/ is what the other parts see. A port's own headers are included
# from this directory, as "desktop/sim_display.h".
target_include_directories(bn_hw
  PUBLIC include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bn_hw
  PUBLIC bn_core
  PRIVATE bn_options)
