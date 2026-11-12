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
function(bn_add_portable_parts binary_root)
  foreach(part core epub draw layout reader)
    add_subdirectory("${BN_ROOT}/${part}" "${binary_root}/${part}")
  endforeach()
endfunction()
