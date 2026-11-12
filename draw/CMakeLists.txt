# draw: fonts and drawing. Font partition mapping, glyph lookup, metrics,
# kerning and ligature tables, fallback chain, glyph decode, the display-list
# format, the band renderer, image decode / scale / dither. Knows nothing
# about books. Depends on core only.
#
# No sources yet. When the first one lands this becomes a STATIC
# library with its own include/ directory, like core.
add_library(bn_draw INTERFACE)
target_link_libraries(bn_draw INTERFACE bn_core)
