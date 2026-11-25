# hw: hardware abstraction layer. Depends on core only.
#
# hw/include is the interface every port implements. This file builds the
# desktop port for the host build. Device support (hw/esp32c3 and hw/x4)
# needs ESP-IDF, so it is built as an ESP-IDF component by
# app/firmware/components/bn_hw.
add_library(bn_hw STATIC
  desktop/crash.cpp
  desktop/device_layout.cpp
  desktop/device_profile.cpp
  desktop/display.cpp
  desktop/folder_storage.cpp
  desktop/fonts.cpp
  desktop/input.cpp
  desktop/memory_probe.cpp
  desktop/panel_view.cpp
  desktop/platform.cpp
  desktop/power.cpp
  desktop/scripted_input.cpp
  desktop/serial.cpp
  desktop/sim_display.cpp
  desktop/storage.cpp
  desktop/trace_sync.cpp
  desktop/window_input.cpp)
# include/ is what the other parts see. A port's own headers are included
# from this directory, as "desktop/sim_display.h".
target_include_directories(bn_hw
  PUBLIC include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bn_hw
  PUBLIC bn_core
  PRIVATE bn_options)

# The desktop application's window, apart from the rest so that only the
# application links SDL. Without BN_DESKTOP_WINDOW there is no window, and
# opening one says so. Its user, the desktop application, reaches its
# header as "desktop/panel_window.h".
if(BN_DESKTOP_WINDOW)
  add_library(bn_hw_window STATIC desktop/panel_window_sdl.cpp)
  target_link_libraries(bn_hw_window PRIVATE SDL3::SDL3-static)
else()
  add_library(bn_hw_window STATIC desktop/panel_window_none.cpp)
endif()
target_include_directories(bn_hw_window PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bn_hw_window
  PUBLIC bn_hw
  PRIVATE bn_options)
