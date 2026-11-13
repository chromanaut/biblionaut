# epub: book parsing layer. Zip, XML, OPF / TOC, CSS cascade, the importer,
# and the intermediate format (writer and reader). Depends on core only, and
# on expat, which is fetched and built by third_party/expat and which no
# header of this part shows.
add_library(bn_epub STATIC
  src/xml.cpp
  src/xml_digest.cpp
  src/zip.cpp)
target_include_directories(bn_epub PUBLIC include)
target_link_libraries(bn_epub
  PUBLIC bn_core
  PRIVATE bn_expat bn_options)
