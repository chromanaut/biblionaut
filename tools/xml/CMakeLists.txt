# bn-xml: reads an XML file, or every XML file in an EPUB, with the parser
# the firmware uses, and says what it found.
add_executable(bn-xml main.cpp)
# Sources include the tools' shared headers as "support/<file>.h".
target_include_directories(bn-xml PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bn-xml PRIVATE bn_epub bn_core bn_options)
