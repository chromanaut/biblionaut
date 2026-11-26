# tools: host programs. The scripts beside this file set up and drive the
# device build and run the checks; each program has its own subdirectory.
#
# Still to come: importer CLI, perf harness.
add_subdirectory(bundle)
add_subdirectory(css)
add_subdirectory(epub)
# bn-font brings FreeType with it, which a build that wants neither is
# spared: one for fuzzing, and the firmware's build of its own host tools.
if(BN_FONT_TOOL AND NOT BN_FUZZ)
  add_subdirectory(font)
endif()
add_subdirectory(hyphen)
add_subdirectory(jpeg)
add_subdirectory(lines)
add_subdirectory(png)
add_subdirectory(svg)
add_subdirectory(trace)
add_subdirectory(xml)
add_subdirectory(zip)
