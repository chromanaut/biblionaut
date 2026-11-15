# bn-hyphen: makes the table of a language's hyphenation patterns that the
# firmware reads, and breaks words with one by the firmware's own code.
#
# Making the table is a library so the tests can drive it without the
# program.
add_library(bn_hyphen_tool STATIC
  pack.cpp)
# Sources include each other as "hyphen/<file>.h".
target_include_directories(bn_hyphen_tool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bn_hyphen_tool
  PUBLIC bn_layout
  PRIVATE bn_options)

add_executable(bn-hyphen main.cpp)
target_link_libraries(bn-hyphen PRIVATE bn_hyphen_tool bn_options)
