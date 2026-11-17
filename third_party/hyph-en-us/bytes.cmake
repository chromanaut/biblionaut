# Writes the bytes of a file as C++ numbers, sixteen to a line, for an
# array to be initialised with:
#
#   cmake -DIN=table.bnhy -DOUT=table_bytes.inc -P bytes.cmake
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " bytes "${hex}")
# CMake's expressions cannot count, so the sixteen are written out.
string(REPEAT "0x[0-9a-f][0-9a-f], " 16 line)
string(REGEX REPLACE "(${line})" "\\1\n" bytes "${bytes}")
string(REPLACE " \n" "\n" bytes "${bytes}")
string(STRIP "${bytes}" bytes)
get_filename_component(name "${IN}" NAME)
file(WRITE "${OUT}" "// The bytes of ${name}, written by bytes.cmake. Not to be edited.\n${bytes}\n")
