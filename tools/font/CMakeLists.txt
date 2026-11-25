# bn-font: makes the fonts image (draw/font.h) out of the font files a
# preset names, drawing them with FreeType, and says what an image holds.
#
# Making the image is a library so the tests can drive it without the
# program.
add_subdirectory("${BN_ROOT}/third_party/freetype" "${CMAKE_CURRENT_BINARY_DIR}/freetype")

add_library(bn_font_tool STATIC
  build.cpp
  preset.cpp)
# Sources include each other as "font/<file>.h".
target_include_directories(bn_font_tool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bn_font_tool
  PUBLIC bn_draw
  PRIVATE freetype bn_options)

add_executable(bn-font main.cpp)
target_link_libraries(bn-font PRIVATE bn_font_tool bn_options)
