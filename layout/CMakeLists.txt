# layout: book layout. Paragraph source over the intermediate format, line
# breaking, hyphenation, pagination, position model, page index. Output is
# display lists; it never touches pixels.
#
# Depends on core, on draw (font metrics and display-list types only) and on
# epub (intermediate-format reader only).
add_library(bn_layout STATIC
  src/chapter_pager.cpp
  src/face_measure.cpp
  src/hyphenator.cpp
  src/line_breaker.cpp
  src/line_digest.cpp
  src/made_up_face.cpp
  src/page_chooser.cpp
  src/paragraph_setter.cpp)
target_include_directories(bn_layout PUBLIC include)
target_link_libraries(bn_layout
  PUBLIC bn_core bn_draw bn_epub
  PRIVATE bn_options)
