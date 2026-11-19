# bn-jpeg: reads JPEG pictures, by themselves or inside an EPUB, with the
# reader the firmware uses, and says what it made of them.
add_executable(bn-jpeg main.cpp)
# Sources include the tools' shared headers as "support/<file>.h".
target_include_directories(bn-jpeg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bn-jpeg PRIVATE bn_draw bn_epub bn_core bn_options)
