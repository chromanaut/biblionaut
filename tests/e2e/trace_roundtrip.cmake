# End-to-end check of the diagnostic stream, run by CTest:
#
#   1. run the desktop application with its serial line captured to a file;
#   2. decode the capture with bn-trace;
#   3. check what came out, including the pictures the simulated panel shows
#      and what a scripted button press led to;
#   4. do it again with the application holding a crash report from an
#      earlier run (an invented one), and check that the report arrives.
#
# Needs -DAPP=<biblionaut-desktop> -DTOOL=<bn-trace> -DWORK_DIR=<scratch directory>.
set(capture "${WORK_DIR}/trace_roundtrip.bin")
set(timeline "${WORK_DIR}/trace_roundtrip.json")
set(panel "${WORK_DIR}/trace_roundtrip_panel")
file(REMOVE "${capture}" "${timeline}" "${panel}-001.png" "${panel}-002.png")

# The simulated panel works twenty times faster than a real one, so its
# first refresh is over well before the Down button is pressed at 0.4 s.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "BN_SERIAL_OUT=${capture}" "BN_DISPLAY_OUT=${panel}" "BN_DISPLAY_SPEED=20"
          "BN_INPUT=400:down" "BN_STORAGE=${WORK_DIR}" "${APP}" --run-ms 1300
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

foreach(expected
    "=== boot 1: "
    "reader/app_start"
    "reader/storage_start status=0"
    "reader/heartbeat uptime_s=1"
    "hw/panel_write bytes=8000"
    "reader/panel_sweep picture=1"
    "hw/refresh_full power_off_after=0"
    "reader/panel_sweep picture=2"
    "reader/press_to_refresh button=5"
    "hw/refresh_fast power_off_after=0")
  string(FIND "${tool_output}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "bn-trace did not print '${expected}'. It printed:\n${tool_output}")
  endif()
endforeach()
if(tool_errors MATCHES "warning")
  message(FATAL_ERROR "bn-trace warned about the capture:\n${tool_errors}")
endif()

# One picture for each refresh: the first picture of the test pattern, and
# the one the button press moved on to. Each is a PNG of the panel's size,
# one bit per pixel. (Signature, then the header chunk: width 800, height
# 480, depth 1.)
foreach(number 001 002)
  if(NOT EXISTS "${panel}-${number}.png")
    message(FATAL_ERROR "the simulated panel did not save its picture as ${panel}-${number}.png")
  endif()
  file(READ "${panel}-${number}.png" picture_start LIMIT 25 HEX)
  if(NOT picture_start STREQUAL "89504e470d0a1a0a0000000d4948445200000320000001e001")
    message(FATAL_ERROR "panel picture ${number} does not start as an 800 x 480 1-bit PNG should: ${picture_start}")
  endif()
endforeach()
file(SHA256 "${panel}-001.png" first_picture)
file(SHA256 "${panel}-002.png" second_picture)
if(first_picture STREQUAL second_picture)
  message(FATAL_ERROR "the button press did not change what the panel shows")
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

# A run that starts with a crash report to pass on.
set(crash_capture "${WORK_DIR}/trace_roundtrip_crash.bin")
file(REMOVE "${crash_capture}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "BN_SERIAL_OUT=${crash_capture}" "BN_FAKE_CRASH=1" "${APP}" --run-ms 300
  RESULT_VARIABLE app_result)
if(NOT app_result EQUAL 0)
  message(FATAL_ERROR "the desktop application failed with a crash report to send: ${app_result}")
endif()
execute_process(
  COMMAND "${TOOL}" "${crash_capture}"
  RESULT_VARIABLE tool_result
  OUTPUT_VARIABLE tool_output
  ERROR_VARIABLE tool_errors)
if(NOT tool_result EQUAL 0)
  message(FATAL_ERROR "bn-trace failed on the capture with a crash report: ${tool_result}\n${tool_errors}")
endif()
foreach(expected
    "=== crash report: the run before this one ended in a crash ==="
    "reason     Load access fault"
    "mepc    0x4200a1b2"
    "sp+0x004  0x4200c3d4"
    "reader/heartbeat uptime_s=12"
    "=== end of crash report")
  string(FIND "${tool_output}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "bn-trace did not print '${expected}'. It printed:\n${tool_output}")
  endif()
endforeach()
if(tool_errors MATCHES "warning")
  message(FATAL_ERROR "bn-trace warned about the capture with a crash report:\n${tool_errors}")
endif()
message(STATUS "crash report round trip: ok")
