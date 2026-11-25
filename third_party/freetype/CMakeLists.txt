# FreeType (https://freetype.org), which draws the glyphs of a font file for
# bn-font (tools/font), the program that makes the fonts image. It is used
# on the computer that builds, and nothing of it is in the firmware: the
# image holds the glyphs it drew, not its code.
#
# None of FreeType is in this repository. CMake downloads the release named
# below when it configures, checks it against the hash, and builds it with
# FreeType's own CMake files: the library alone, static, with none of the
# other libraries it can use (zlib, bzip2, libpng, HarfBuzz, Brotli), which
# fonts of the kinds bn-font reads do not need.
#
# To move to another release: change the version and the hash together.
# The hash is the SHA-256 of the .tar.xz on FreeType's download page.
include(FetchContent)
set(bn_freetype_version 2.15.0)
FetchContent_Declare(
  freetype
  URL "https://download.savannah.gnu.org/releases/freetype/freetype-${bn_freetype_version}.tar.xz"
  URL_HASH SHA256=93cd47eb719c325107f2d3cf087d06299e944c3b48a40b55537302304dea1dd5
  # Those who use it see its headers as a system's, so that their own
  # warnings stop at the door.
  SYSTEM
  EXCLUDE_FROM_ALL)

set(FT_DISABLE_ZLIB ON)
set(FT_DISABLE_BZIP2 ON)
set(FT_DISABLE_PNG ON)
set(FT_DISABLE_HARFBUZZ ON)
set(FT_DISABLE_BROTLI ON)
set(FT_DISABLE_HVF ON)
set(SKIP_INSTALL_ALL ON)
set(BUILD_SHARED_LIBS OFF)

# It is built as its makers build it, not as Biblionaut's own code is:
# without the sanitizers, which would end the program at whatever they found
# in FreeType, without warnings, and without clang-tidy, whose findings in it
# are its maintainers' business.
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
set_property(DIRECTORY PROPERTY LINK_OPTIONS "")
set(CMAKE_C_CLANG_TIDY "")
set(CMAKE_CXX_CLANG_TIDY "")
FetchContent_MakeAvailable(freetype)
target_compile_options(freetype PRIVATE -w)
