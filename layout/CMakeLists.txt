# layout: book layout. Paragraph source over the intermediate format, line
# breaking, hyphenation, pagination, position model, page index. Output is
# display lists; it never touches pixels.
#
# Depends on core, on draw (font metrics and display-list types only) and on
# epub (intermediate-format reader only).
add_library(bn_layout STATIC
  src/line_breaker.cpp
  src/page_chooser.cpp)
target_include_directories(bn_layout PUBLIC include)
target_link_libraries(bn_layout
  PUBLIC bn_core bn_draw bn_epub
  PRIVATE bn_options)
