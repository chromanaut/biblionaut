# bn-trace: decodes the diagnostic stream a device (or a desktop run) sends,
# prints it, and writes a timeline for ui.perfetto.dev.
#
# The decoding is a library so the tests can drive it without the program.
add_library(bn_trace_tool STATIC
  decoder.cpp
  input.cpp
  names.cpp
  output.cpp)
# Sources include each other as "trace/<file>.h".
target_include_directories(bn_trace_tool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# bn_reader, bn_epub and bn_hw for their lists of trace ids; each part that
# gains ids is added here.
target_link_libraries(bn_trace_tool
  PUBLIC bn_core bn_reader bn_epub bn_hw
  PRIVATE bn_options)

add_executable(bn-trace main.cpp)
target_link_libraries(bn-trace PRIVATE bn_trace_tool bn_options)
