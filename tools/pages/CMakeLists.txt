# bn-pages: sets a chapter of a bundle in pages, with the reader of the
# format and the firmware's own setting of lines and choosing of pages, in
# the made-up face.
add_executable(bn-pages main.cpp pager.cpp)
# Sources include the tools' shared headers as "support/<file>.h".
target_include_directories(bn-pages PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bn-pages PRIVATE bn_layout bn_epub bn_options)
