# Host tests. Unit tests, end-to-end tests and the fuzz targets now; golden
# display-list and golden-image tests join them under this directory as the
# parts they cover arrive.

# What the sanitizer runtime is told when a test runs. The heap tests ask
# for impossible sizes on purpose; without this the runtime aborts instead
# of letting malloc return null.
set(bn_test_asan_options "allocator_may_return_null=1")
# On arm64 Linux, GCC's runtime is very slow at one of its checks: looking
# for uses of a stack frame after its function has returned. The inflater's
# tests took 218 s with it and 4 s without, where x86-64 shows no such
# cost. So there that one check is off, and the x86-64 Linux run (CI's) is
# the one that makes it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$"
   AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  string(APPEND bn_test_asan_options ":detect_stack_use_after_return=0")
endif()

# zlib, which the tests use and bn-font besides (tools/font): what the
# inflater produces is checked against it, and the tests' ZIP files are made
# with it.
find_package(ZLIB REQUIRED)
if(BN_FUZZ AND APPLE AND ZLIB_INCLUDE_DIRS MATCHES "\\.sdk/usr/include$")
  # zlib's header was found in a macOS SDK, and LLVM's clang may be using
  # another SDK than that one. Each has the header, and the compiler looks
  # in its own unaided. Named on the compile line, the other's directory
  # would put that SDK's C headers ahead of the C++ library's, which then
  # does not compile. So CMake is told the compiler searches that directory
  # already, which keeps it off the line.
  list(APPEND CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES ${ZLIB_INCLUDE_DIRS})
  list(APPEND CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES ${ZLIB_INCLUDE_DIRS})
endif()

add_subdirectory(fuzz)
if(BN_FUZZ)
  # A build for fuzzing has the fuzz targets and nothing else of the tests.
  return()
endif()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  SYSTEM)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
# clang-tidy checks our code, not GoogleTest's.
set(_bn_clang_tidy "${CMAKE_CXX_CLANG_TIDY}")
set(CMAKE_CXX_CLANG_TIDY "")
FetchContent_MakeAvailable(googletest)
set(CMAKE_CXX_CLANG_TIDY "${_bn_clang_tidy}")
include(GoogleTest)

# One executable per part, linked against that part only, so a test cannot
# reach further than the code it tests.
function(bn_add_unit_test name)
  cmake_parse_arguments(arg "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${arg_SOURCES})
  target_include_directories(${name} PRIVATE support)
  target_link_libraries(${name} PRIVATE ${arg_LIBS} bn_core_runtime bn_options GTest::gtest_main)
  gtest_discover_tests(${name} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")
endfunction()

bn_add_unit_test(core_tests
  SOURCES
    unit/core/arena_test.cpp
    unit/core/block_file_test.cpp
    unit/core/byte_map_test.cpp
    unit/core/byte_source_test.cpp
    unit/core/checksum_test.cpp
    unit/core/crash_test.cpp
    unit/core/file_replacement_test.cpp
    unit/core/fixed_text_test.cpp
    unit/core/frame_test.cpp
    unit/core/heap_test.cpp
    unit/core/inflate_test.cpp
    unit/core/input_sampler_test.cpp
    unit/core/metrics_test.cpp
    unit/core/region_heap_test.cpp
    unit/core/storage_test.cpp
    unit/core/trace_level0_test.cpp
    unit/core/trace_level1_test.cpp
    unit/core/trace_stream_test.cpp
    unit/core/trace_test.cpp
    unit/core/utf8_test.cpp
  LIBS bn_core)

# The inflater is checked against zlib.
target_link_libraries(core_tests PRIVATE ZLIB::ZLIB)

bn_add_unit_test(draw_tests
  SOURCES
    unit/draw/dither_test.cpp
    unit/draw/divide_test.cpp
    unit/draw/fill_test.cpp
    unit/draw/fixed_test.cpp
    unit/draw/flatten_test.cpp
    unit/draw/font_test.cpp
    unit/draw/jpeg_test.cpp
    unit/draw/picture_cache_test.cpp
    unit/draw/png_test.cpp
    unit/draw/scale_test.cpp
    unit/draw/spool_test.cpp
    unit/draw/stroke_test.cpp
    unit/draw/svg_number_test.cpp
    unit/draw/svg_path_test.cpp
    unit/draw/svg_render_test.cpp
    unit/draw/svg_sheet_test.cpp
    unit/draw/svg_tags_test.cpp
    unit/draw/svg_transform_test.cpp
    unit/draw/svg_values_test.cpp
    unit/draw/test_pattern_test.cpp
  LIBS bn_draw)
# The PNG reader's tests make their files with zlib doing the compressing
# and the checksums.
target_link_libraries(draw_tests PRIVATE ZLIB::ZLIB)

# The ZIP reader's tests make their ZIP files with zlib doing the
# compressing and the checksums.
bn_add_unit_test(epub_tests
  SOURCES
    unit/epub/book_bundle_test.cpp
    unit/epub/book_files_test.cpp
    unit/epub/book_path_test.cpp
    unit/epub/bundle_test.cpp
    unit/epub/chapter_import_test.cpp
    unit/epub/css_families_test.cpp
    unit/epub/css_rules_test.cpp
    unit/epub/css_test.cpp
    unit/epub/document_blocks_test.cpp
    unit/epub/document_sheets_test.cpp
    unit/epub/package_test.cpp
    unit/epub/segment_test.cpp
    unit/epub/sheets_digest_test.cpp
    unit/epub/style_stack_test.cpp
    unit/epub/toc_test.cpp
    unit/epub/xml_test.cpp
    unit/epub/zip_test.cpp
  LIBS bn_epub)
target_link_libraries(epub_tests PRIVATE ZLIB::ZLIB)

# The line breaker is given paragraphs the tests make up, and checked
# against trying every way of breaking them. The hyphenator is given tables
# the tests write themselves, and checked against laying every pattern over
# every place in a word. The page chooser is given runs of lines the tests
# make up, and checked against adding their heights up over the whole run.
# The table for American English that the build makes is checked against
# the file it was made from, and against words whose breaks TeX gives. The
# measure in the faces of a fonts image is given images the tests write.
bn_add_unit_test(layout_tests
  SOURCES
    unit/layout/american_english_test.cpp
    unit/layout/face_measure_test.cpp
    unit/layout/hyphenator_test.cpp
    unit/layout/line_breaker_test.cpp
    unit/layout/line_digest_test.cpp
    unit/layout/made_up_face_test.cpp
    unit/layout/page_chooser_test.cpp
    unit/layout/paragraph_setter_test.cpp
  LIBS bn_layout bn_hyph_en_us)
target_compile_definitions(layout_tests PRIVATE
  "BN_AMERICAN_ENGLISH_TABLE=\"$<TARGET_PROPERTY:bn_hyph_en_us,BN_TABLE_FILE>\"")

# The desktop port, which the other host programs run on. Its own headers
# are reached the way its sources reach them, as "desktop/...".
bn_add_unit_test(hw_tests
  SOURCES
    unit/hw/device_layout_test.cpp
    unit/hw/device_profile_test.cpp
    unit/hw/folder_storage_test.cpp
    unit/hw/panel_view_test.cpp
    unit/hw/scripted_input_test.cpp
    unit/hw/sim_display_test.cpp
    unit/hw/window_input_test.cpp
  LIBS bn_hw)
target_include_directories(hw_tests PRIVATE ${PROJECT_SOURCE_DIR}/hw)

bn_add_unit_test(reader_tests
  SOURCES
    unit/reader/app_test.cpp
    unit/reader/book_check_test.cpp
    unit/reader/drawing_viewer_test.cpp
    unit/reader/gray_rows_test.cpp
    unit/reader/layout_check_test.cpp
    unit/reader/pattern_viewer_test.cpp
    unit/reader/storage_check_test.cpp
    unit/reader/text_viewer_test.cpp
  LIBS bn_reader)
# The pattern viewer's tests compare what reaches the panel with the picture
# from draw, which the reader links but does not pass on. The book check's
# tests are in the same position with the ZIP reader from epub, and make
# their ZIP files with zlib; and the layout check's with the setter from
# layout, which they set the text with a second time.
target_include_directories(reader_tests PRIVATE
  $<TARGET_PROPERTY:bn_draw,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:bn_epub,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:bn_layout,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(reader_tests PRIVATE ZLIB::ZLIB)

bn_add_unit_test(tools_tests
  SOURCES
    unit/tools/hyphen_tool_test.cpp
    unit/tools/trace_tool_test.cpp
  LIBS bn_hyphen_tool bn_trace_tool)
# The font builder, in a build that has it (BN_FONT_TOOL), is tried on a
# real font: Courier Prime, which the build downloads for the tests and
# nothing else.
if(BN_FONT_TOOL)
  add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/courier-prime ${CMAKE_BINARY_DIR}/third_party/courier-prime)
  target_sources(tools_tests PRIVATE unit/tools/font_kerning_test.cpp unit/tools/font_ligatures_test.cpp
                                     unit/tools/font_tool_test.cpp)
  # What bn-font says pictures compress to is checked against zlib.
  target_link_libraries(tools_tests PRIVATE bn_font_tool ZLIB::ZLIB)
  target_compile_definitions(tools_tests PRIVATE "BN_COURIER_PRIME_DIR=\"${BN_COURIER_PRIME_DIR}\"")
  # And the repository's own presets are read, each with either monospace
  # face.
  target_compile_definitions(tools_tests PRIVATE
    "BN_FONT_PRESETS_DIR=\"${PROJECT_SOURCE_DIR}/tools/font/presets\"")
endif()

# The whole diagnostic path on the desktop: application -> capture file ->
# bn-trace -> timeline.
add_test(
  NAME e2e.TraceRoundTrip
  COMMAND ${CMAKE_COMMAND}
    -DAPP=$<TARGET_FILE:biblionaut-desktop>
    -DTOOL=$<TARGET_FILE:bn-trace>
    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/trace_roundtrip.cmake)

# The application, the simulated panel and the scripted buttons together,
# across the panel's drive voltages being switched off.
add_test(
  NAME e2e.PanelSwitchOff
  COMMAND ${CMAKE_COMMAND}
    -DAPP=$<TARGET_FILE:biblionaut-desktop>
    -DTOOL=$<TARGET_FILE:bn-trace>
    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/panel_switch_off.cmake)

# The Power button on the desktop: the device put to sleep, and woken into
# a new run.
add_test(
  NAME e2e.PowerSleep
  COMMAND ${CMAKE_COMMAND}
    -DAPP=$<TARGET_FILE:biblionaut-desktop>
    -DTOOL=$<TARGET_FILE:bn-trace>
    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/power_sleep.cmake)

# The application with its window, on SDL's stand-in for a screen, against
# the same application without one: the same scripted presses have to put
# the same pictures on the panel. Only a build with a window has this.
if(BN_DESKTOP_WINDOW)
  add_test(
    NAME e2e.Window
    COMMAND ${CMAKE_COMMAND}
      -DAPP=$<TARGET_FILE:biblionaut-desktop>
      -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/window.cmake)
  set_tests_properties(e2e.Window PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")
endif()

# tools/bn-symbolize against a stand-in ELF archive and addr2line.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME e2e.Symbolize
    COMMAND ${CMAKE_COMMAND}
      -DPYTHON=${Python3_EXECUTABLE}
      -DSCRIPT=${PROJECT_SOURCE_DIR}/tools/bn-symbolize
      -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/symbolize.cmake)

  # tools/queued, which has the heavy checks take turns: jobs that end
  # when the test lets them, to see that they go one at a time and in the
  # order they came, and what becomes of the queue when one is killed.
  add_test(
    NAME e2e.Queued
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/queued.py
      ${PROJECT_SOURCE_DIR}/tools/queued ${CMAKE_CURRENT_BINARY_DIR}/queued)

  # bn-zip, and through it the ZIP reader, against ZIP files made by
  # something else: Python's zipfile module, and the zip command where
  # there is one.
  add_test(
    NAME e2e.ZipTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/zip_tool.py
      $<TARGET_FILE:bn-zip> ${CMAKE_CURRENT_BINARY_DIR}/zip_tool)
  set_tests_properties(e2e.ZipTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-epub, and through it the package reader, against a reading of the
  # same books by Python.
  add_test(
    NAME e2e.EpubTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/epub_tool.py
      $<TARGET_FILE:bn-epub> ${CMAKE_CURRENT_BINARY_DIR}/epub_tool)
  set_tests_properties(e2e.EpubTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-xml, and through it the XML parser, against Python's own expat on
  # the same documents, event for event.
  add_test(
    NAME e2e.XmlTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/xml_tool.py
      $<TARGET_FILE:bn-xml> ${CMAKE_CURRENT_BINARY_DIR}/xml_tool)
  set_tests_properties(e2e.XmlTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-css, and through it the reader of CSS, against a second reading of
  # the same sheets written in the test, line for line.
  add_test(
    NAME e2e.CssTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/css_tool.py
      $<TARGET_FILE:bn-css> ${CMAKE_CURRENT_BINARY_DIR}/css_tool)
  set_tests_properties(e2e.CssTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-css cascade, and through it the table of rules and the cascade,
  # against a second working-out of what the rules come to for each element
  # of a document, which keeps the whole document and tries everything.
  add_test(
    NAME e2e.CssCascade
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/css_cascade_tool.py
      $<TARGET_FILE:bn-css> ${CMAKE_CURRENT_BINARY_DIR}/css_cascade_tool)
  set_tests_properties(e2e.CssCascade PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-css cascade --styles, and through it what each element of a document
  # comes to, against a second working-out that keeps the document as a
  # tree and has its own list of what HTML's elements are given.
  add_test(
    NAME e2e.Styles
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/styles_tool.py
      $<TARGET_FILE:bn-css> ${CMAKE_CURRENT_BINARY_DIR}/styles_tool)
  set_tests_properties(e2e.Styles PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-css cascade --blocks, and through it the finding of a document's
  # blocks, against a second working-out that lays the whole document out
  # as one list of places and answers every question from that.
  add_test(
    NAME e2e.Blocks
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/blocks_tool.py
      $<TARGET_FILE:bn-css> ${CMAKE_CURRENT_BINARY_DIR}/blocks_tool)
  set_tests_properties(e2e.Blocks PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-bundle, and through it the making of a book's bundle, against a
  # second reading of the bundle's bytes set beside a reading of the book by
  # zipfile and ElementTree.
  add_test(
    NAME e2e.BundleTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/bundle_tool.py
      $<TARGET_FILE:bn-bundle> ${CMAKE_CURRENT_BINARY_DIR}/bundle_tool)
  set_tests_properties(e2e.BundleTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-bundle write-chapter, chapter and find, and through them the writer
  # and the reader of a chapter's segment, each against a writer and a
  # reader of the test's own: what one writes the other reads, for chapters
  # the test makes up.
  add_test(
    NAME e2e.SegmentTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/segment_tool.py
      $<TARGET_FILE:bn-bundle> ${CMAKE_CURRENT_BINARY_DIR}/segment_tool)
  set_tests_properties(e2e.SegmentTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-bundle import, and through it the importing of a chapter, against a
  # second working-out that keeps the whole document and gathers what lies
  # in each block into one list: for a book the test makes, of documents
  # written for it and made at random.
  add_test(
    NAME e2e.ImportTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/import_tool.py
      $<TARGET_FILE:bn-bundle> $<TARGET_FILE:bn-css> ${CMAKE_CURRENT_BINARY_DIR}/import_tool)
  set_tests_properties(e2e.ImportTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-pages, and through it a chapter read by the reader of its segment
  # and set in pages by the setter and the page chooser: the lines of each
  # block against the block as the test has it, and the pages against ones
  # the test chooses by adding the lines' heights up over the whole
  # chapter.
  add_test(
    NAME e2e.PagesTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/pages_tool.py
      $<TARGET_FILE:bn-pages> $<TARGET_FILE:bn-bundle> ${CMAKE_CURRENT_BINARY_DIR}/pages_tool)
  set_tests_properties(e2e.PagesTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-css sheets and bn-css document, and through them the finding of the
  # sheets a document asks for, against a second working-out in the test
  # for books it makes: each document's list, what was left out by its
  # kind, and what the rules then come to.
  add_test(
    NAME e2e.SheetsTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/sheets_tool.py
      $<TARGET_FILE:bn-css> ${CMAKE_CURRENT_BINARY_DIR}/sheets_tool)
  set_tests_properties(e2e.SheetsTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-svg, and through it the reading and flattening of SVG path data,
  # against a second reading of the same paths, with the curves they
  # describe worked out in floating point; and the pictures it fills
  # against a second way of filling the same shapes.
  add_test(
    NAME e2e.SvgTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/svg_tool.py
      $<TARGET_FILE:bn-svg> ${CMAKE_CURRENT_BINARY_DIR}/svg_tool)
  set_tests_properties(e2e.SvgTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-svg again, drawing whole files: drawings made up at random and
  # written out as SVG, against the picture each should make, worked out
  # from the drawing and not from the file.
  add_test(
    NAME e2e.SvgDraw
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/svg_draw.py
      $<TARGET_FILE:bn-svg> ${CMAKE_CURRENT_BINARY_DIR}/svg_draw)
  set_tests_properties(e2e.SvgDraw PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-png, and through it the PNG reader, against a second reading of the
  # same pictures: the file taken apart all at once, its data decompressed
  # by Python's zlib, and each pixel turned to gray the plain way.
  add_test(
    NAME e2e.PngTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/png_tool.py
      $<TARGET_FILE:bn-png> ${CMAKE_CURRENT_BINARY_DIR}/png_tool)
  set_tests_properties(e2e.PngTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-jpeg, and through it the JPEG reader, against a second reading of the
  # same pictures: the file taken apart all at once, its codes decoded a
  # symbol at a time, and its blocks turned into pixels in whole numbers at
  # an eighth and in real numbers at the other sizes. The pictures are made
  # by an encoder of the test's own.
  add_test(
    NAME e2e.JpegTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/jpeg_tool.py
      $<TARGET_FILE:bn-jpeg> ${CMAKE_CURRENT_BINARY_DIR}/jpeg_tool)
  set_tests_properties(e2e.JpegTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-hyphen, and through it the hyphenator and the tables it reads,
  # against a second way of breaking the same words: the patterns kept as
  # they were written, and every piece of a word looked up among them.
  add_test(
    NAME e2e.HyphenTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/hyphen_tool.py
      $<TARGET_FILE:bn-hyphen> ${CMAKE_CURRENT_BINARY_DIR}/hyphen_tool)
  set_tests_properties(e2e.HyphenTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-font, and through it the font builder and the reader of the fonts
  # image, against a second reading of the image it makes of a real font,
  # and of that font's own tables of characters and widths.
  if(BN_FONT_TOOL)
    add_test(
      NAME e2e.FontTool
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/font_tool.py
        $<TARGET_FILE:bn-font> ${BN_COURIER_PRIME_DIR} ${CMAKE_CURRENT_BINARY_DIR}/font_tool
        ${PROJECT_SOURCE_DIR})
    set_tests_properties(e2e.FontTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

    # The kerning bn-font reads out of a font and puts in an image, against
    # a second reading of the tables of two real fonts that kern, which the
    # build downloads (third_party/fonts, the target bn_font_files_tests).
    add_test(
      NAME e2e.FontKerning
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/font_kerning.py
        $<TARGET_FILE:bn-font> ${BN_FONTS_DIR}/noto ${CMAKE_CURRENT_BINARY_DIR}/font_kerning)
    set_tests_properties(e2e.FontKerning PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

    # And the ligatures it reads out of a font, against a second reading of
    # the same two fonts' tables.
    add_test(
      NAME e2e.FontLigatures
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/font_ligatures.py
        $<TARGET_FILE:bn-font> ${BN_FONTS_DIR}/noto ${CMAKE_CURRENT_BINARY_DIR}/font_ligatures)
    set_tests_properties(e2e.FontLigatures PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

    # The text viewer, run by the desktop application with a fonts image
    # bn-font makes of that font, against what must be so of its pages
    # however their lines fall: the ink on the panel is the ink bn-font
    # gives for each of the text's letters.
    add_test(
      NAME e2e.TextViewer
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/text_viewer.py
        $<TARGET_FILE:biblionaut-desktop> $<TARGET_FILE:bn-font> $<TARGET_FILE:bn-trace> ${BN_COURIER_PRIME_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/text_viewer)
    set_tests_properties(e2e.TextViewer PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")
  endif()

  # bn-lines, and through it the paragraph setter, against a second reading
  # of the same paragraphs: each taken apart all at once, and the cheapest
  # way of ending its lines found by trying them.
  add_test(
    NAME e2e.LinesTool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/lines_tool.py
      $<TARGET_FILE:bn-lines> $<TARGET_FILE:bn-hyphen> ${CMAKE_CURRENT_BINARY_DIR}/lines_tool)
  set_tests_properties(e2e.LinesTool PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # The layout check, run by the desktop application over a folder standing
  # for the card, against bn-lines on the same text and table; and the
  # script that makes the text, against a book whose paragraphs are known.
  add_test(
    NAME e2e.LayoutCheck
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/layout_check.py
      $<TARGET_FILE:biblionaut-desktop> $<TARGET_FILE:bn-trace> $<TARGET_FILE:bn-lines> $<TARGET_FILE:bn-hyphen>
      ${PROJECT_SOURCE_DIR}/tools/layout-text.py $<TARGET_PROPERTY:bn_hyph_en_us,BN_TABLE_FILE>
      ${CMAKE_CURRENT_BINARY_DIR}/layout_check)
  set_tests_properties(e2e.LayoutCheck PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # The drawing viewer, run by the desktop application over a folder of
  # drawings and pictures, against bn-svg's drawing of each drawing and
  # bn-png's and bn-jpeg's reading of each picture, turned onto the panel.
  add_test(
    NAME e2e.DrawingViewer
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/drawing_viewer.py
      $<TARGET_FILE:biblionaut-desktop> $<TARGET_FILE:bn-svg> $<TARGET_FILE:bn-png> $<TARGET_FILE:bn-jpeg>
      $<TARGET_FILE:bn-trace> ${CMAKE_CURRENT_BINARY_DIR}/drawing_viewer)
  set_tests_properties(e2e.DrawingViewer PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # The book check, run by the desktop application over a folder of books,
  # against bn-zip, bn-xml, bn-epub, bn-css and zipfile on the same files.
  add_test(
    NAME e2e.BookCheck
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/book_check.py
      $<TARGET_FILE:biblionaut-desktop> $<TARGET_FILE:bn-trace> $<TARGET_FILE:bn-zip> $<TARGET_FILE:bn-xml>
      $<TARGET_FILE:bn-epub> $<TARGET_FILE:bn-css> ${CMAKE_CURRENT_BINARY_DIR}/book_check)
  set_tests_properties(e2e.BookCheck PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # bn-zip, bn-xml, bn-epub, bn-css, bn-png, bn-jpeg and bn-bundle over the real books
  # in corpus/, each against a reading of the same book by Python; and the
  # books themselves against the list of what they should be.
  add_test(
    NAME e2e.Corpus
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check-books.py
      --zip $<TARGET_FILE:bn-zip> --xml $<TARGET_FILE:bn-xml> --epub $<TARGET_FILE:bn-epub>
      --css $<TARGET_FILE:bn-css> --png $<TARGET_FILE:bn-png> --jpeg $<TARGET_FILE:bn-jpeg>
      --bundle $<TARGET_FILE:bn-bundle> ${PROJECT_SOURCE_DIR}/corpus)
  set_tests_properties(e2e.Corpus PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")

  # What the same script counts in the documents of a spine, their
  # paragraphs and their ids, which it compares with no tool: against two
  # books made by the test, counted by hand as they were written.
  add_test(
    NAME e2e.DocumentCount
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/document_count.py
      ${PROJECT_SOURCE_DIR}/tools/check-books.py $<TARGET_FILE:bn-zip> $<TARGET_FILE:bn-xml> $<TARGET_FILE:bn-epub>
      $<TARGET_FILE:bn-css> $<TARGET_FILE:bn-png> $<TARGET_FILE:bn-jpeg> $<TARGET_FILE:bn-bundle>
      ${CMAKE_CURRENT_BINARY_DIR}/document_count)
  set_tests_properties(e2e.DocumentCount PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")
endif()
