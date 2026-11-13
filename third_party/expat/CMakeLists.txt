# expat, the XML parser (https://libexpat.github.io), as Biblionaut builds it.
#
# None of expat is in this repository. CMake downloads the release named
# below when it configures, checks it against the hash, and compiles the
# four files of the library with the settings in expat_config.h, which is
# ours. The host build and the firmware build both come through here, so
# both have the same parser.
#
# To move to another release: change the version and the hash together.
# The hash is the SHA-256 of the .tar.xz on expat's releases page.
include(FetchContent)
set(bn_expat_version 2.10.0)
string(REPLACE "." "_" bn_expat_tag "R_${bn_expat_version}")
FetchContent_Declare(
  expat
  URL "https://github.com/libexpat/libexpat/releases/download/${bn_expat_tag}/expat-${bn_expat_version}.tar.xz"
  URL_HASH SHA256=c624ba2998fc65df1163a49641ff3f9ab73138b11641d92c86bee0fb01b1bc7f
  # expat's own build is not used. Naming a directory that is not there
  # keeps FetchContent from running it.
  SOURCE_SUBDIR not-built-by-its-own-cmake)
FetchContent_MakeAvailable(expat)

add_library(bn_expat STATIC
  "${expat_SOURCE_DIR}/lib/xcs.c"
  "${expat_SOURCE_DIR}/lib/xmlparse.c"
  "${expat_SOURCE_DIR}/lib/xmlrole.c"
  "${expat_SOURCE_DIR}/lib/xmltok.c")
# expat_config.h comes from this directory. Those who use the library see
# its headers as a system's, so that their own warnings stop at the door.
target_include_directories(bn_expat PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(bn_expat SYSTEM PUBLIC "${expat_SOURCE_DIR}/lib")
# It is not our code to tidy: its warnings are its maintainers' business.
target_compile_options(bn_expat PRIVATE -w)
set_target_properties(bn_expat PROPERTIES C_CLANG_TIDY "" C_STANDARD 99 C_EXTENSIONS ON)
