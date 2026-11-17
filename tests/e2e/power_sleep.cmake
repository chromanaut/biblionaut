# End-to-end check of the Power button on the desktop, run by CTest:
#
#   1. run the desktop application with its serial line captured and the
#      simulated panel saving its pictures, with scripted presses: Down;
#      Power, which puts the device to sleep; Power again, which wakes it;
#      and Down once more;
#   2. check that the capture has two runs, the second begun by waking from
#      deep sleep, with the panel put to sleep between them;
#   3. check that the panel was given its pictures in that order: the
#      first, the step, the first again when the device woke, and the step
#      again.
#
# A device that sleeps starts again when it wakes, and the desktop does the
# same: the second run is a new application, and its stream begins with a
# hello of its own.
#
# Everything here hangs on the clock, not on how often the application
# gets a turn. The panel works twenty times faster than a real one, so its
# longest refresh takes under 0.2 s, and the presses are 0.6 s and more
# apart. Its voltages, left on after a refresh, are switched off for sleep
# within a fraction of a second, and only two seconds after a refresh
# otherwise, which is after the runs are over.
#
# Needs -DAPP=<biblionaut-desktop> -DTOOL=<bn-trace> -DWORK_DIR=<scratch directory>.
set(capture "${WORK_DIR}/power_sleep.bin")
set(panel "${WORK_DIR}/power_sleep_panel")
file(REMOVE "${capture}")
foreach(number 001 002 003 004 005)
  file(REMOVE "${panel}-${number}.png")
endforeach()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "BN_SERIAL_OUT=${capture}" "BN_DISPLAY_OUT=${panel}" "BN_DISPLAY_SPEED=20"
          "BN_INPUT=400:down,1000:power,2000:power,2600:down" "${APP}" --run-ms 3200
  RESULT_VARIABLE app_result
  OUTPUT_VARIABLE app_output)
if(NOT app_result EQUAL 0)
  message(FATAL_ERROR "the desktop application failed: ${app_result}")
endif()
string(FIND "${app_output}" "Asleep" found)
if(found EQUAL -1)
  message(FATAL_ERROR "the desktop application did not say the device was asleep. It printed:\n${app_output}")
endif()

execute_process(
  COMMAND "${TOOL}" "${capture}"
  RESULT_VARIABLE tool_result
  OUTPUT_VARIABLE tool_output
  ERROR_VARIABLE tool_errors)
if(NOT tool_result EQUAL 0)
  message(FATAL_ERROR "bn-trace failed: ${tool_result}\n${tool_errors}")
endif()
if(tool_errors MATCHES "warning")
  message(FATAL_ERROR "bn-trace warned about the capture:\n${tool_errors}")
endif()

# In this order: the first run, its step, the panel switched off and the
# device put to sleep, then the second run, woken, and its step.
set(at 0)
foreach(expected
    "=== boot 1: "
    "reader/press_to_refresh button=5"
    "hw/panel_power_off"
    "reader/press_to_sleep"
    "=== boot 2: "
    "started by waking from deep sleep"
    "hw/refresh_full"
    "reader/press_to_refresh button=5")
  string(SUBSTRING "${tool_output}" ${at} -1 rest)
  string(FIND "${rest}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "bn-trace did not print '${expected}' where it should have. It printed:\n${tool_output}")
  endif()
  math(EXPR at "${at} + ${found} + 1")
endforeach()
string(FIND "${tool_output}" "=== boot 3: " third)
if(NOT third EQUAL -1)
  message(FATAL_ERROR "the capture has a third run, where there should be two:\n${tool_output}")
endif()

foreach(number 001 002 003 004)
  if(NOT EXISTS "${panel}-${number}.png")
    message(FATAL_ERROR "the simulated panel did not save its picture as ${panel}-${number}.png")
  endif()
endforeach()
if(EXISTS "${panel}-005.png")
  message(FATAL_ERROR "the panel was refreshed more often than the presses asked for")
endif()
file(SHA256 "${panel}-001.png" first)
file(SHA256 "${panel}-002.png" step)
file(SHA256 "${panel}-003.png" woken)
file(SHA256 "${panel}-004.png" step_again)
if(first STREQUAL step)
  message(FATAL_ERROR "the step before sleeping did not change what the panel shows")
endif()
if(NOT woken STREQUAL first)
  message(FATAL_ERROR "on waking, the panel did not show the first picture again: ${panel}-003.png")
endif()
if(NOT step_again STREQUAL step)
  message(FATAL_ERROR "after waking, the step did not show the second picture: ${panel}-004.png")
endif()
