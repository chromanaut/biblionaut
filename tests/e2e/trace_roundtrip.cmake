# End-to-end check of the diagnostic stream, run by CTest:
#
#   1. run the desktop application with its serial line captured to a file;
#   2. decode the capture with bn-trace;
#   3. check what came out.
#
# Needs -DAPP=<biblionaut-desktop> -DTOOL=<bn-trace> -DWORK_DIR=<scratch directory>.
set(capture "${WORK_DIR}/trace_roundtrip.bin")
set(timeline "${WORK_DIR}/trace_roundtrip.json")
file(REMOVE "${capture}" "${timeline}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "BN_SERIAL_OUT=${capture}" "${APP}" --run-ms 1300
  RESULT_VARIABLE app_result)
if(NOT app_result EQUAL 0)
  message(FATAL_ERROR "the desktop application failed: ${app_result}")
endif()

execute_process(
  COMMAND "${TOOL}" "${capture}" --out "${timeline}"
  RESULT_VARIABLE tool_result
  OUTPUT_VARIABLE tool_output
  ERROR_VARIABLE tool_errors)
if(NOT tool_result EQUAL 0)
  message(FATAL_ERROR "bn-trace failed: ${tool_result}\n${tool_errors}")
endif()

foreach(expected "=== boot 1: " "reader/app_start" "reader/heartbeat uptime_s=1")
  string(FIND "${tool_output}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "bn-trace did not print '${expected}'. It printed:\n${tool_output}")
  endif()
endforeach()
if(tool_errors MATCHES "warning")
  message(FATAL_ERROR "bn-trace warned about the capture:\n${tool_errors}")
endif()

# The timeline must be valid JSON with the start span in it.
file(READ "${timeline}" json)
string(JSON event_count ERROR_VARIABLE json_error LENGTH "${json}" traceEvents)
if(json_error)
  message(FATAL_ERROR "the timeline is not valid JSON: ${json_error}")
endif()
set(found_start FALSE)
math(EXPR last "${event_count} - 1")
foreach(index RANGE ${last})
  string(JSON name ERROR_VARIABLE ignored GET "${json}" traceEvents ${index} name)
  string(JSON phase ERROR_VARIABLE ignored GET "${json}" traceEvents ${index} ph)
  if(name STREQUAL "app_start" AND phase STREQUAL "X")
    set(found_start TRUE)
  endif()
endforeach()
if(NOT found_start)
  message(FATAL_ERROR "the timeline has no app_start slice")
endif()
message(STATUS "trace round trip: ${event_count} timeline events")
