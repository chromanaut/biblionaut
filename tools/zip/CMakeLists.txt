# bn-zip: lists, checks and reads out the entries of a ZIP file, such as an
# EPUB, with the reader the firmware uses.
add_executable(bn-zip main.cpp)
# Sources include the tools' shared headers as "support/<file>.h".
target_include_directories(bn-zip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bn-zip PRIVATE bn_epub bn_core bn_options)
