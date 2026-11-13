# bn-epub: reads the package of an EPUB with the code the firmware uses,
# and says what the book says of itself.
add_executable(bn-epub main.cpp)
# Sources include the tools' shared headers as "support/<file>.h".
target_include_directories(bn-epub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bn-epub PRIVATE bn_epub bn_core bn_options)
