# End-to-end check of the desktop application with its window, run by
# CTest in a build configured with BN_DESKTOP_WINDOW:
#
#   1. run the application without a window, the simulated panel saving its
#      pictures, with four scripted presses: Down, Down, Confirm, Back;
#   2. run it again with the window open, on SDL's stand-in for a screen
#      (its "dummy" video driver, which needs none), with the same presses;
#   3. check that the second run opened its window and put the same
#      pictures on the panel as the first.
#
# The keys of a real keyboard cannot be pressed from here. What becomes of
# them is tested in tests/unit/hw/key_input_test.cpp; this shows that the
# window, with the buttons it hands the application, changes nothing else.
#
# Everything here hangs on the clock, not on how often the application gets
# a turn. The panel works twenty times faster than a real one, so the
# longest refresh, the full one Back asks for, takes under 0.2 s, and the
# presses are half a second apart. The voltages are switched off two seconds
# after the last refresh, after the runs are over.
#
# Needs -DAPP=<biblionaut-desktop> -DWORK_DIR=<scratch directory>.
set(presses "BN_INPUT=400:down,900:down,1400:confirm,1900:back")
set(pictures 001 002 003 004 005)

foreach(run headless windowed)
  set(panel "${WORK_DIR}/window_${run}")
  foreach(number ${pictures} 006)
    file(REMOVE "${panel}-${number}.png")
  endforeach()
endforeach()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "BN_DISPLAY_OUT=${WORK_DIR}/window_headless" "BN_DISPLAY_SPEED=20" "${presses}"
          "${APP}" --run-ms 2600
  RESULT_VARIABLE headless_result)
if(NOT headless_result EQUAL 0)
  message(FATAL_ERROR "the desktop application failed without a window: ${headless_result}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "BN_DISPLAY_OUT=${WORK_DIR}/window_windowed" "BN_DISPLAY_SPEED=20" "${presses}"
          "SDL_VIDEO_DRIVER=dummy" "${APP}" --window --run-ms 2600
  RESULT_VARIABLE windowed_result
  OUTPUT_VARIABLE windowed_output
  ERROR_VARIABLE windowed_errors)
if(NOT windowed_result EQUAL 0)
  message(FATAL_ERROR "the desktop application failed with a window: ${windowed_result}\n${windowed_errors}")
endif()
string(FIND "${windowed_output}" "Click the buttons" found)
if(found EQUAL -1)
  message(FATAL_ERROR "the desktop application did not say how to press the buttons, so did not open its window. "
                      "It printed:\n${windowed_output}${windowed_errors}")
endif()

# Five pictures each: the first, and one for each press.
foreach(run headless windowed)
  foreach(number ${pictures})
    if(NOT EXISTS "${WORK_DIR}/window_${run}-${number}.png")
      message(FATAL_ERROR "the ${run} run did not save its picture as ${WORK_DIR}/window_${run}-${number}.png")
    endif()
  endforeach()
  if(EXISTS "${WORK_DIR}/window_${run}-006.png")
    message(FATAL_ERROR "the ${run} run refreshed the panel more often than it was asked to")
  endif()
endforeach()
foreach(number ${pictures})
  file(SHA256 "${WORK_DIR}/window_headless-${number}.png" headless_picture)
  file(SHA256 "${WORK_DIR}/window_windowed-${number}.png" windowed_picture)
  if(NOT headless_picture STREQUAL windowed_picture)
    message(FATAL_ERROR "with the window open, picture ${number} differs from the one without it: "
                        "${WORK_DIR}/window_windowed-${number}.png and ${WORK_DIR}/window_headless-${number}.png")
  endif()
endforeach()
