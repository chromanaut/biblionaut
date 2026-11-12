# Firmware entry point: app_main().
idf_component_register(
  SRCS "app_main.cpp"
  PRIV_REQUIRES bn_hw esp_rom freertos)

# bn_reader, bn_core and bn_core_runtime are defined by the project after
# components are registered; CMake resolves the names when it generates.
target_link_libraries(${COMPONENT_LIB} PRIVATE bn_reader bn_core)

# The operator new / delete replacements go into this component as objects,
# and app_main() references them, so they are always part of the image.
target_sources(${COMPONENT_LIB} PRIVATE $<TARGET_OBJECTS:bn_core_runtime>)
