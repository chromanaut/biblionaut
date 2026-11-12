# draw: fonts and drawing. Font partition mapping, glyph lookup, metrics,
# kerning and ligature tables, fallback chain, glyph decode, the display-list
# format, the band renderer, image decode / scale / dither. Knows nothing
# about books. Depends on core only.
add_library(bn_draw STATIC
  src/test_pattern.cpp)
target_include_directories(bn_draw PUBLIC include)
target_link_libraries(bn_draw
  PUBLIC bn_core
  PRIVATE bn_options)
