# Fuzz targets: one for each thing that reads bytes a book's maker chose.
#
# A target is one function, LLVMFuzzerTestOneInput, which is given some
# bytes, puts them through the code it is for, and ends the program if
# that code does anything it should not: goes outside its memory or into
# undefined behaviour (which the sanitizers see), breaks one of its own
# promises, or does more in one call than it says it will (which the target
# checks for itself).
#
# In an ordinary build each target is a program that goes through the
# inputs kept for it in inputs/<name>, once, and CTest runs it as
# fuzz.<Name>: so the targets keep building, on both compilers, and a fault
# that an input once found stays found. In a build for fuzzing (BN_FUZZ)
# the same function is linked with the fuzzing engine, which makes up new
# inputs for as long as it is left to. tools/fuzz.py does that.
add_custom_target(fuzz_targets)

function(bn_add_fuzz_target name test)
  cmake_parse_arguments(arg "" "" "LIBS" ${ARGN})
  add_executable(fuzz_${name} ${name}_fuzz.cpp)
  target_include_directories(fuzz_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../support)
  target_link_libraries(fuzz_${name} PRIVATE ${arg_LIBS} bn_core_runtime bn_options ZLIB::ZLIB)
  add_dependencies(fuzz_targets fuzz_${name})
  if(BN_FUZZ)
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
  else()
    target_sources(fuzz_${name} PRIVATE replay_main.cpp)
    add_test(NAME fuzz.${test} COMMAND fuzz_${name} ${CMAKE_CURRENT_SOURCE_DIR}/inputs/${name})
    set_tests_properties(fuzz.${test} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=${bn_test_asan_options}")
  endif()
endfunction()

bn_add_fuzz_target(inflate Inflate LIBS bn_core)
bn_add_fuzz_target(zip Zip LIBS bn_epub)
bn_add_fuzz_target(xml Xml LIBS bn_epub)
bn_add_fuzz_target(package Package LIBS bn_epub)
bn_add_fuzz_target(toc Toc LIBS bn_epub)
bn_add_fuzz_target(css Css LIBS bn_epub)
bn_add_fuzz_target(css_rules CssRules LIBS bn_epub)
bn_add_fuzz_target(document_sheets DocumentSheets LIBS bn_epub)
bn_add_fuzz_target(region_heap RegionHeap LIBS bn_core)
bn_add_fuzz_target(hyphenator Hyphenator LIBS bn_layout)
bn_add_fuzz_target(paragraph Paragraph LIBS bn_layout)
bn_add_fuzz_target(svg_tags SvgTags LIBS bn_draw)
bn_add_fuzz_target(svg_sheet SvgSheet LIBS bn_draw)
bn_add_fuzz_target(svg_values SvgValues LIBS bn_draw)
bn_add_fuzz_target(svg_path SvgPath LIBS bn_draw)
bn_add_fuzz_target(svg Svg LIBS bn_draw)
bn_add_fuzz_target(png Png LIBS bn_draw)
bn_add_fuzz_target(jpeg Jpeg LIBS bn_draw)
bn_add_fuzz_target(scale Scale LIBS bn_draw)
bn_add_fuzz_target(picture_cache PictureCache LIBS bn_draw)
bn_add_fuzz_target(font Font LIBS bn_draw)
