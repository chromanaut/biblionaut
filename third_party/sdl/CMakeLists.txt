# SDL (https://libsdl.org), for the desktop application's window
# (hw/desktop/panel_window.h). Only a build configured with
# BN_DESKTOP_WINDOW comes here; nothing of SDL is in the firmware.
#
# None of SDL is in this repository. CMake downloads the release named
# below when it configures, checks it against the hash, and builds it with
# SDL's own CMake files: the library alone, static, and of it only what a
# window needs, which is video, drawing and the events that come with them.
#
# To move to another release: change the version and the hash together.
# The hash is the SHA-256 of the .tar.gz on SDL's releases page.
include(FetchContent)
set(bn_sdl_version 3.4.18)
FetchContent_Declare(
  sdl
  URL "https://github.com/libsdl-org/SDL/releases/download/release-${bn_sdl_version}/SDL3-${bn_sdl_version}.tar.gz"
  URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
  # Those who use it see its headers as a system's, so that their own
  # warnings stop at the door.
  SYSTEM)

set(SDL_SHARED OFF)
set(SDL_STATIC ON)
set(SDL_TEST_LIBRARY OFF)
set(SDL_TESTS OFF)
set(SDL_EXAMPLES OFF)
set(SDL_INSTALL OFF)
set(SDL_VULKAN OFF)
foreach(subsystem AUDIO GPU CAMERA JOYSTICK HAPTIC HIDAPI POWER SENSOR DIALOG TRAY)
  set(SDL_${subsystem}_DEFAULT OFF)
endforeach()

# It is built as its makers build it, not as Biblionaut's own code is:
# without the sanitizers, which would end the program at whatever they found
# in SDL, and without clang-tidy, whose findings in it are its maintainers'
# business.
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
set_property(DIRECTORY PROPERTY LINK_OPTIONS "")
set(CMAKE_C_CLANG_TIDY "")
set(CMAKE_CXX_CLANG_TIDY "")
set(CMAKE_OBJC_CLANG_TIDY "")
FetchContent_MakeAvailable(sdl)
