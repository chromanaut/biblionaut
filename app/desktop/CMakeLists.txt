# Desktop entry point: the reader application on the desktop hardware port.
add_executable(biblionaut-desktop main.cpp)
target_link_libraries(biblionaut-desktop PRIVATE bn_reader bn_hw bn_core_runtime bn_options)
