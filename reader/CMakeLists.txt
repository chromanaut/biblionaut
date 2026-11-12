# reader: the reader application. Activities, settings, refresh manager,
# scheduler and worker jobs, diagnostics screens. The only part that knows
# about all the others and wires them together.
add_library(bn_reader STATIC
  src/app.cpp
  src/pattern_viewer.cpp
  src/storage_check.cpp)
target_include_directories(bn_reader PUBLIC include)
target_compile_definitions(bn_reader PRIVATE BN_VERSION_STRING="${BN_VERSION_STRING}")
target_link_libraries(bn_reader
  PUBLIC bn_core
  PRIVATE bn_layout bn_draw bn_epub bn_hw bn_options)
