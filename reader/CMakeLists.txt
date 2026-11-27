# reader: the reader application. Activities, settings, refresh manager,
# scheduler and worker jobs, diagnostics screens. The only part that knows
# about all the others and wires them together.
add_library(bn_reader STATIC
  src/app.cpp
  src/book_check.cpp
  src/drawing_viewer.cpp
  src/gray_rows.cpp
  src/layout_check.cpp
  src/pattern_viewer.cpp
  src/storage_check.cpp
  src/text_viewer.cpp)
target_include_directories(bn_reader PUBLIC include)
target_compile_definitions(bn_reader PRIVATE BN_VERSION_STRING="${BN_VERSION_STRING}")
target_link_libraries(bn_reader
  PUBLIC bn_core bn_draw
  PRIVATE bn_layout bn_epub bn_hw bn_hyph_en_us bn_options)
