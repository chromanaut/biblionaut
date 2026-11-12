# ESP-IDF component wrapper for the device hardware layer. The sources live
# under hw/ (esp32c3 for what the chip provides, then one directory per
# device); this is the only place they meet ESP-IDF's build system.
get_filename_component(bn_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

idf_component_register(
  SRCS
    "${bn_root}/hw/esp32c3/clock.cpp"
    "${bn_root}/hw/esp32c3/crash.cpp"
    "${bn_root}/hw/esp32c3/memory_probe.cpp"
    "${bn_root}/hw/esp32c3/serial.cpp"
    "${bn_root}/hw/esp32c3/spi_bus.cpp"
    "${bn_root}/hw/esp32c3/trace_sync.cpp"
    "${bn_root}/hw/x4/display.cpp"
    "${bn_root}/hw/x4/input.cpp"
    "${bn_root}/hw/x4/platform.cpp"
  INCLUDE_DIRS "${bn_root}/hw/include"
  # hw/ itself, for headers the device code shares, as "esp32c3/spi_bus.h".
  PRIV_INCLUDE_DIRS "${bn_root}/hw"
  PRIV_REQUIRES
    esp_adc esp_app_format esp_driver_gpio esp_driver_spi esp_driver_usb_serial_jtag esp_hw_support esp_rom
    esp_system esp_timer freertos riscv soc)

# hw -> core. bn_core is defined by the project after components are
# registered; CMake resolves the name when it generates.
target_link_libraries(${COMPONENT_LIB} PUBLIC bn_core)

# The trace level the portable parts are built with (BN_TRACE_LEVEL), so the
# device code compiles the same trace points in or out. Only that setting is
# taken from bn_options, not its warning flags: this code includes ESP-IDF
# headers, which are not written to them.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
  $<TARGET_PROPERTY:bn_options,INTERFACE_COMPILE_DEFINITIONS>)

# Crash reports: every call to ESP-IDF's panic handler goes to
# __wrap_esp_panic_handler in hw/esp32c3/crash.cpp first, which saves a
# report and then calls the real handler.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
