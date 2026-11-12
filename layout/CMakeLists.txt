# layout: book layout. Paragraph source over the intermediate format, line
# breaking, hyphenation, pagination, position model, page index. Output is
# display lists; it never touches pixels.
#
# Depends on core, on draw (font metrics and display-list types only) and on
# epub (intermediate-format reader only).
#
# No sources yet. When the first one lands this becomes a STATIC
# library with its own include/ directory, like core.
add_library(bn_layout INTERFACE)
target_link_libraries(bn_layout INTERFACE bn_core bn_draw bn_epub)
