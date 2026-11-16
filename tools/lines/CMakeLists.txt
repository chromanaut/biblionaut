# bn-lines: sets paragraphs of text in lines by the firmware's own code,
# with a made-up face to measure the text in.
add_executable(bn-lines main.cpp)
target_link_libraries(bn-lines PRIVATE bn_layout bn_options)
