# bn-svg: reads the parts of an SVG drawing with the code the firmware draws
# one with, and prints what it made of them.
add_executable(bn-svg main.cpp)
target_link_libraries(bn-svg PRIVATE bn_draw bn_core bn_options)
