# bn-png: reads PNG pictures, by themselves or inside an EPUB, with the
# reader the firmware uses, and says what it made of them.
add_executable(bn-png main.cpp)
# Sources include the tools' shared headers as "support/<file>.h".
target_include_directories(bn-png PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bn-png PRIVATE bn_draw bn_epub bn_core bn_options)
