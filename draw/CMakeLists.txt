# draw: fonts and drawing. Font partition mapping, glyph lookup, metrics,
# kerning and ligature tables, fallback chain, glyph decode, the display-list
# format, the band renderer, image decode / scale / dither. Knows nothing
# about books. Depends on core only.
add_library(bn_draw STATIC
  src/dither.cpp
  src/fill.cpp
  src/fixed.cpp
  src/flatten.cpp
  src/font.cpp
  src/jpeg.cpp
  src/picture_cache.cpp
  src/png.cpp
  src/scale.cpp
  src/spool.cpp
  src/stroke.cpp
  src/svg_number.cpp
  src/svg_path.cpp
  src/svg_render.cpp
  src/svg_sheet.cpp
  src/svg_tags.cpp
  src/svg_transform.cpp
  src/svg_values.cpp
  src/test_pattern.cpp)
target_include_directories(bn_draw PUBLIC include)
target_link_libraries(bn_draw
  PUBLIC bn_core
  PRIVATE bn_options)
