# End-to-end check that the application drives the panel's two planes
# rightly across the drive voltages being switched off, run by CTest:
#
#   1. run the desktop application with the simulated panel saving its
#      pictures and three scripted presses: Down; Down again once the
#      voltages have been off for a while; then Confirm, which shows the
#      same picture again by a half refresh;
#   2. check that the step made after the switch-off left the panel showing
#      exactly what the half refresh then did. A half refresh takes no
#      notice of the old-image plane, so it is right whatever went before;
#      the step is only right if the planes were.
#
# The simulated panel swaps and restores its planes as a real one was
# measured to (core/display.h), which is what makes this a test.
#
# Everything here hangs on the clock, not on how often the application
# gets a turn: a busy machine gives it one far less often than every
# 10 ms. That is why the planes are not read back in this run. A reading
# takes a dozen turns, and puts off the switch-off by as long as it takes.
#
# Needs -DAPP=<biblionaut-desktop> -DTOOL=<bn-trace> -DWORK_DIR=<scratch directory>.
set(capture "${WORK_DIR}/panel_switch_off.bin")
set(panel "${WORK_DIR}/panel_switch_off_panel")
file(REMOVE "${capture}" "${panel}-001.png" "${panel}-002.png" "${panel}-003.png" "${panel}-004.png")

# The panel works twenty times faster than a real one. The two seconds the
# application waits before switching the voltages off are not shortened.
# They run from the end of the first step's refresh, at about 0.5 s, so
# the voltages go off at about 2.5 s: a second and more before the second
# step, and long enough after it for the planes to be written again.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "BN_SERIAL_OUT=${capture}" "BN_DISPLAY_OUT=${panel}" "BN_DISPLAY_SPEED=20"
          "BN_INPUT=400:down,3600:down,4000:confirm" "${APP}" --run-ms 4400
  RESULT_VARIABLE app_result)
if(NOT app_result EQUAL 0)
  message(FATAL_ERROR "the desktop application failed: ${app_result}")
endif()

execute_process(
  COMMAND "${TOOL}" "${capture}"
  RESULT_VARIABLE tool_result
  OUTPUT_VARIABLE tool_output
  ERROR_VARIABLE tool_errors)
if(NOT tool_result EQUAL 0)
  message(FATAL_ERROR "bn-trace failed: ${tool_result}\n${tool_errors}")
endif()

# The voltages went off between the two steps: otherwise this run shows
# nothing.
foreach(expected
    "hw/panel_power_off"
    "hw/refresh_half power_off_after=0")
  string(FIND "${tool_output}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "bn-trace did not print '${expected}'. It printed:\n${tool_output}")
  endif()
endforeach()
string(FIND "${tool_output}" "hw/panel_power_off" switched_off)
string(FIND "${tool_output}" "reader/press_to_refresh button=5" first_step)
string(FIND "${tool_output}" "reader/press_to_refresh button=5" second_step REVERSE)
if(NOT (first_step LESS switched_off AND switched_off LESS second_step))
  message(FATAL_ERROR "the voltages were not switched off between the two steps. bn-trace printed:\n${tool_output}")
endif()

# Four pictures: the first, the two steps, and the half refresh.
foreach(number 001 002 003 004)
  if(NOT EXISTS "${panel}-${number}.png")
    message(FATAL_ERROR "the simulated panel did not save its picture as ${panel}-${number}.png")
  endif()
endforeach()
file(SHA256 "${panel}-002.png" first_step_picture)
file(SHA256 "${panel}-003.png" second_step_picture)
file(SHA256 "${panel}-004.png" redrawn_picture)
if(first_step_picture STREQUAL second_step_picture)
  message(FATAL_ERROR "the step after the switch-off did not change what the panel shows")
endif()
if(NOT second_step_picture STREQUAL redrawn_picture)
  message(FATAL_ERROR "after the voltages were switched off, a step left the panel showing something other than "
                      "the picture: ${panel}-003.png differs from ${panel}-004.png, the same picture by a half refresh")
endif()
