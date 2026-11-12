# epub: book parsing layer. Zip, XML, OPF / TOC, CSS cascade, the importer,
# and the intermediate format (writer and reader). Depends on core only.
#
# No sources yet. When the first one lands this becomes a STATIC
# library with its own include/ directory, like core.
add_library(bn_epub INTERFACE)
target_link_libraries(bn_epub INTERFACE bn_core)
