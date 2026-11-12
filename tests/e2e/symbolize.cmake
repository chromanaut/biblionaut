# End-to-end check of tools/bn-symbolize, run by CTest. The real thing needs
# the device toolchain and a firmware build, so this stands in for both: an
# "ELF file" that is just some bytes, and an addr2line that knows two
# addresses.
#
# Needs -DPYTHON=<python3> -DSCRIPT=<tools/bn-symbolize> -DWORK_DIR=<scratch directory>.
set(work "${WORK_DIR}/symbolize")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

# The script names files relative to the repository when they are inside it.
# `shown` is this directory as the script will print it.
file(REAL_PATH "${work}" work)
get_filename_component(repository "${SCRIPT}" DIRECTORY)
file(REAL_PATH "${repository}/.." repository)
cmake_path(IS_PREFIX repository "${work}" inside_repository)
if(inside_repository)
  file(RELATIVE_PATH shown "${repository}" "${work}")
else()
  set(shown "${work}")
endif()

set(addr2line "${work}/fake-addr2line")
file(WRITE "${addr2line}" [=[#!/bin/sh
# Called as: -pfiaC -e ELF ADDRESS...
elf="$3"
shift 3
for address in "$@"; do
  case "$address" in
    0x4200c3d4)
      echo "$address: bn::reader::App::Tick() at reader/src/app.cpp:83 [$(cat "$elf")]"
      echo " (inlined by) app_main at app/firmware/main/app_main.cpp:150"
      ;;
    0x4200a1b2) echo "$address: Crash() at crash.cpp:7 [$(cat "$elf")]" ;;
    *) echo "$address: ?? ??:0" ;;
  esac
done
]=])
file(CHMOD "${addr2line}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

# Two builds in the archive.
set(archive "${work}/archive")
foreach(build first second)
  file(WRITE "${work}/${build}.elf" "the ${build} build")
  execute_process(
    COMMAND "${PYTHON}" "${SCRIPT}" --archive "${work}/${build}.elf" --label "0.1.0+${build}" --archive-dir "${archive}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "archiving failed: ${result}\n${output}${errors}")
  endif()
  file(SHA256 "${work}/${build}.elf" hash_${build})
  if(NOT EXISTS "${archive}/0.1.0+${build}-${hash_${build}}.elf")
    message(FATAL_ERROR "the ${build} ELF file was not archived under its hash:\n${output}")
  endif()
endforeach()
string(SUBSTRING "${hash_first}" 0 9 id_first)
string(SUBSTRING "${hash_second}" 0 9 id_second)

# A crash report from the first build, then one from the second, then one
# from a build that was never archived.
file(WRITE "${work}/input.txt"
"[     1.000000] task 0  reader/heartbeat uptime_s=1
=== crash report: the run before this one ended in a crash ===
  firmware   0.1.0+first  (ELF ${id_first})
  registers  mepc    0x4200a1b2  ra      0x4200c3d4  sp      0x3fc9f000  gp      0x00000000
             sp+0x004  0x4200c3d4
             sp+0x00c  0x42001122
  firmware   0.1.0+second  (ELF ${id_second})
             sp+0x004  0x4200c3d4
  firmware   0.1.0+lost  (ELF 000000000)
             sp+0x004  0x4200c3d4
")
execute_process(
  COMMAND "${PYTHON}" "${SCRIPT}" --archive-dir "${archive}" --addr2line "${addr2line}"
  INPUT_FILE "${work}/input.txt"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "bn-symbolize failed: ${result}\n${output}${errors}")
endif()

set(expected
"[     1.000000] task 0  reader/heartbeat uptime_s=1
=== crash report: the run before this one ended in a crash ===
  firmware   0.1.0+first  (ELF ${id_first})
      [bn-symbolize] using ${shown}/archive/0.1.0+first-${hash_first}.elf
  registers  mepc    0x4200a1b2  ra      0x4200c3d4  sp      0x3fc9f000  gp      0x00000000
      0x4200a1b2: Crash() at crash.cpp:7 [the first build]
      0x4200c3d4: bn::reader::App::Tick() at reader/src/app.cpp:83 [the first build]
       (inlined by) app_main at app/firmware/main/app_main.cpp:150
             sp+0x004  0x4200c3d4
      0x4200c3d4: bn::reader::App::Tick() at reader/src/app.cpp:83 [the first build]
       (inlined by) app_main at app/firmware/main/app_main.cpp:150
             sp+0x00c  0x42001122
  firmware   0.1.0+second  (ELF ${id_second})
      [bn-symbolize] using ${shown}/archive/0.1.0+second-${hash_second}.elf
             sp+0x004  0x4200c3d4
      0x4200c3d4: bn::reader::App::Tick() at reader/src/app.cpp:83 [the second build]
       (inlined by) app_main at app/firmware/main/app_main.cpp:150
  firmware   0.1.0+lost  (ELF 000000000)
      [bn-symbolize] no ELF file for firmware 000000000 in ${shown}/archive; its addresses are left as they are
             sp+0x004  0x4200c3d4
")
if(NOT output STREQUAL expected)
  file(WRITE "${work}/output.txt" "${output}")
  file(WRITE "${work}/expected.txt" "${expected}")
  message(FATAL_ERROR "bn-symbolize did not print what was expected. Compare:\n  ${work}/output.txt\n  ${work}/expected.txt")
endif()

# --elf wins over what the text says, with a warning when they disagree.
execute_process(
  COMMAND "${PYTHON}" "${SCRIPT}" --archive-dir "${archive}" --addr2line "${addr2line}" --elf "${work}/second.elf"
  INPUT_FILE "${work}/input.txt"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "bn-symbolize --elf failed: ${result}\n${output}${errors}")
endif()
foreach(wanted
    "warning: ${shown}/second.elf is not firmware ${id_first}"
    "0x4200a1b2: Crash() at crash.cpp:7 [the second build]")
  string(FIND "${output}" "${wanted}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "bn-symbolize --elf did not print '${wanted}'. It printed:\n${output}")
  endif()
endforeach()
string(FIND "${output}" "[the first build]" found)
if(NOT found EQUAL -1)
  message(FATAL_ERROR "bn-symbolize --elf still used the archive:\n${output}")
endif()

# Addresses given as arguments.
execute_process(
  COMMAND "${PYTHON}" "${SCRIPT}" --addr2line "${addr2line}" --elf "${work}/first.elf" 0x4200a1b2 4200c3d4 0x42001122
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "bn-symbolize with addresses failed: ${result}\n${output}${errors}")
endif()
set(expected
"0x4200a1b2: Crash() at crash.cpp:7 [the first build]
0x4200c3d4: bn::reader::App::Tick() at reader/src/app.cpp:83 [the first build]
 (inlined by) app_main at app/firmware/main/app_main.cpp:150
0x42001122: not in ${shown}/first.elf
")
if(NOT output STREQUAL expected)
  message(FATAL_ERROR "bn-symbolize with addresses printed:\n${output}\nbut this was expected:\n${expected}")
endif()
message(STATUS "symbolize: ok")
