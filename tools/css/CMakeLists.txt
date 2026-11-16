# bn-css: reads a style sheet with the reader the firmware uses, and says
# what it found in it and what it passed over.
add_executable(bn-css main.cpp)
# Sources include the tools' shared headers as "support/<file>.h".
target_include_directories(bn-css PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bn-css PRIVATE bn_epub bn_core bn_options)
