# epub: book parsing layer. Zip, XML, OPF / TOC, CSS cascade, the importer,
# and the intermediate format (writer and reader). Depends on core only, and
# on expat, which is fetched and built by third_party/expat and which no
# header of this part shows.
add_library(bn_epub STATIC
  src/book_bundle.cpp
  src/book_files.cpp
  src/book_path.cpp
  src/bundle.cpp
  src/chapter_import.cpp
  src/css.cpp
  src/css_families.cpp
  src/css_rules.cpp
  src/document_blocks.cpp
  src/document_sheets.cpp
  src/html_entities.cpp
  src/package.cpp
  src/package_digest.cpp
  src/segment.cpp
  src/segment_reader.cpp
  src/segment_writer.cpp
  src/sheets_digest.cpp
  src/style_stack.cpp
  src/toc.cpp
  src/toc_digest.cpp
  src/xml.cpp
  src/xml_digest.cpp
  src/zip.cpp)
target_include_directories(bn_epub PUBLIC include)
target_link_libraries(bn_epub
  PUBLIC bn_core
  PRIVATE bn_expat bn_options)
