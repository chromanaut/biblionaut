# ESP-IDF component wrapper for the device hardware layer. The sources live
# under hw/ (one directory per device); this is the only place they meet
# ESP-IDF's build system.
get_filename_component(bn_root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

idf_component_register(
  SRCS "${bn_root}/hw/x4/platform.cpp"
  INCLUDE_DIRS "${bn_root}/hw/include"
  PRIV_REQUIRES esp_timer)

# hw -> core. bn_core is defined by the project after components are
# registered; CMake resolves the name when it generates.
target_link_libraries(${COMPONENT_LIB} PUBLIC bn_core)
