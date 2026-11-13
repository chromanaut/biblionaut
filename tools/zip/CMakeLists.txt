# bn-zip: lists, checks and reads out the entries of a ZIP file, such as an
# EPUB, with the reader the firmware uses.
add_executable(bn-zip main.cpp)
target_link_libraries(bn-zip PRIVATE bn_epub bn_core bn_options)
