# bn-bundle: makes the bundle of a book with the code the firmware uses,
# says what a bundle holds, and prints a chapter of one as text or writes
# one from that text.
add_executable(bn-bundle chapter.cpp import.cpp main.cpp)
# Sources include the tools' shared headers as "support/<file>.h".
target_include_directories(bn-bundle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bn-bundle PRIVATE bn_epub bn_core bn_options)
