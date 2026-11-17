# The portable parts, in dependency order. Used by the host build and by the
# device build (app/firmware), so both compile the parts from one definition.
#
# Dependency rules. Each part can only include headers
# from the parts it links, because every part has its own include directory:
#
#   reader  ->  layout, draw, epub, hw
#   layout  ->  draw, epub
#   epub, layout, draw  ->  core   (never hw)
#   hw      ->  core
#
# hw is not added here: the host build adds the desktop port (hw/), the device
# build adds device support as an ESP-IDF component
# (app/firmware/components/bn_hw).
#
# Before them comes what they use that is not ours, fetched and built by
# the files under third_party/: expat, for epub; and the table of American
# English's hyphenation patterns, for reader, which bn-hyphen makes as the
# build goes. Whoever calls this sets BN_HYPHEN to the bn-hyphen to run
# (third_party/hyph-en-us/CMakeLists.txt).
function(bn_add_portable_parts binary_root)
  add_subdirectory("${BN_ROOT}/third_party/expat" "${binary_root}/third_party/expat")
  add_subdirectory("${BN_ROOT}/third_party/hyph-en-us" "${binary_root}/third_party/hyph-en-us")
  foreach(part core epub draw layout reader)
    add_subdirectory("${BN_ROOT}/${part}" "${binary_root}/${part}")
  endforeach()
endfunction()
