# epub: book parsing layer. Zip, XML, OPF / TOC, CSS cascade, the importer,
# and the intermediate format (writer and reader). Depends on core only.
add_library(bn_epub STATIC
  src/zip.cpp)
target_include_directories(bn_epub PUBLIC include)
target_link_libraries(bn_epub
  PUBLIC bn_core
  PRIVATE bn_options)
