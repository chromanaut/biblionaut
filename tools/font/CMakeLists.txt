# bn-font: makes the fonts image (draw/font.h) out of the font files a
# preset names, drawing them with FreeType, and says what an image holds.
#
# Making the image is a library so the tests can drive it without the
# program.
add_subdirectory("${BN_ROOT}/third_party/freetype" "${CMAKE_CURRENT_BINARY_DIR}/freetype")

# What an image's pictures would take compressed is worked out with zlib,
# the computer's own, as the tests use it.
find_package(ZLIB REQUIRED)

add_library(bn_font_tool STATIC
  build.cpp
  preset.cpp
  sizes.cpp)
# Sources include each other as "font/<file>.h".
target_include_directories(bn_font_tool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bn_font_tool
  PUBLIC bn_draw
  PRIVATE freetype ZLIB::ZLIB bn_options)

add_executable(bn-font main.cpp)
target_link_libraries(bn-font PRIVATE bn_font_tool bn_options)

# The fonts image, made when asked for: `cmake --build <build> --target fonts`
# writes <build>/fonts/fonts.bnfn. Which typefaces it is made of is chosen
# when the build is configured:
#
#   BN_FONTS        noto (the default), plex or source: a preset here, in
#                   presets/, whose font files are downloaded for it
#                   (third_party/fonts); or the path of a preset file of
#                   one's own, which names its own font files and its own
#                   sizes, and is used by itself.
#   BN_FONTS_MONO   courier-prime (the default) for Courier Prime as the
#                   monospace face, or family for the preset's own family's.
#                   Nothing to a preset of one's own.
#
# A preset here is put together from three files: the sizes
# (presets/ladder.preset), the family's faces, and a monospace face.
set(BN_FONTS "noto" CACHE STRING "The fonts image's typefaces: noto, plex, source, or the path of a preset file")
set(BN_FONTS_MONO "courier-prime" CACHE STRING "Its monospace face: courier-prime, or family for the preset's own")
add_subdirectory("${BN_ROOT}/third_party/fonts" "${CMAKE_BINARY_DIR}/third_party/fonts")

set(bn_fonts_image "${CMAKE_BINARY_DIR}/fonts/fonts.bnfn")
set(bn_presets "${CMAKE_CURRENT_SOURCE_DIR}/presets")
if(BN_FONTS MATCHES "^(noto|plex|source)$")
  if(BN_FONTS_MONO STREQUAL "courier-prime")
    set(bn_fonts_mono courier-prime)
  elseif(BN_FONTS_MONO STREQUAL "family")
    set(bn_fonts_mono ${BN_FONTS})
  else()
    message(FATAL_ERROR "BN_FONTS_MONO is courier-prime or family, not '${BN_FONTS_MONO}'.")
  endif()
  set(bn_fonts_presets "${bn_presets}/ladder.preset" "${bn_presets}/${BN_FONTS}.preset"
      "${bn_presets}/mono-${bn_fonts_mono}.preset")
  # The family's files, the monospace face's, and Noto's, which has the
  # fallback face of the others.
  set(bn_fonts_files ${BN_FONT_FILES_${BN_FONTS}} ${BN_FONT_FILES_${bn_fonts_mono}} ${BN_FONT_FILES_noto})
  list(REMOVE_DUPLICATES bn_fonts_files)
  set(bn_fonts_from --fonts "${BN_FONTS_DIR}")
elseif(EXISTS "${BN_FONTS}")
  get_filename_component(bn_fonts_presets "${BN_FONTS}" ABSOLUTE)
  set(bn_fonts_files "")
  set(bn_fonts_from "")
else()
  message(FATAL_ERROR "BN_FONTS is noto, plex, source or the path of a preset file, not '${BN_FONTS}'.")
endif()
add_custom_command(
  OUTPUT "${bn_fonts_image}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/fonts"
  COMMAND bn-font build ${bn_fonts_presets} ${bn_fonts_from} --out "${bn_fonts_image}"
  DEPENDS bn-font ${bn_fonts_presets} ${bn_fonts_files}
  COMMENT "Making the fonts image (${BN_FONTS})"
  VERBATIM)
add_custom_target(fonts DEPENDS "${bn_fonts_image}")
