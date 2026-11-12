# Biblionaut

EPUB reader firmware for the Xteink X4 (ESP32-C3, 800x480 e-ink, SD card),
written so that everything except the hardware layer also builds and runs on
a desktop.

## Layout

| Directory | Contents |
|---|---|
| `core/` | Shared foundation: arenas, the timing trace, counters and gauges, the heap-failure hook, and the interfaces the portable parts use to reach the outside world (clock, byte source) |
| `hw/` | Hardware abstraction layer. `hw/include` is the interface; `hw/desktop` implements it for the host and `hw/x4` for the X4 |
| `epub/` | Book parsing: zip, XML, OPF / TOC, CSS, the importer and the intermediate format |
| `draw/` | Fonts and drawing: glyphs, metrics, the display-list format, the band renderer, images |
| `layout/` | Book layout: line breaking, hyphenation, pagination, positions. Emits display lists |
| `reader/` | The reader application. The only part that knows about all the others |
| `app/` | Entry points: `app/desktop` for the host, and `app/firmware`, the ESP-IDF project that builds the one firmware image for all supported devices |
| `tools/` | Host programs and developer scripts |
| `tests/` | Host tests |
| `corpus/` | Test books |

Dependency rules, enforced by the build (each part has its own include
directory and can only see the parts it links):

```
reader  ->  layout, draw, epub, hw
layout  ->  draw, epub
epub, layout, draw  ->  core        (never hw)
hw                  ->  core
```

Only code under `hw/` and `app/firmware` may include ESP-IDF or FreeRTOS headers.

The code is C++20 with exceptions and RTTI disabled.

Naming: namespaces are `snake_case`; classes, structs and functions are
`PascalCase`; variables, parameters and members are `camelCase`, with a
trailing underscore on private members; constants are `kPascalCase`.
clang-tidy enforces this.

## Host build

Needs CMake 3.25 or later, Ninja and a C++20 compiler. GoogleTest is
downloaded when CMake configures.

```
cmake --preset host
cmake --build --preset host
ctest --preset host
./build/host/app/desktop/biblionaut-desktop
```

The `host` preset is a debug build with AddressSanitizer and
UndefinedBehaviorSanitizer. `host-release` is optimised, without sanitizers.

How much timing trace is compiled in is set with `BN_TRACE_LEVEL`: `0` for
none, `1` for top-level spans only, `2` (the default) for everything. Pass it
when configuring, for example `cmake --preset host -DBN_TRACE_LEVEL=0` or
`tools/idf -DBN_TRACE_LEVEL=1 build`.

## Device build

The firmware builds with ESP-IDF. A setup script installs the pinned version
and its compiler into `.esp/` inside this repository (a download of a few
gigabytes, ignored by git); nothing is installed elsewhere.

```
tools/setup-idf.sh          # once
tools/idf build
tools/idf flash monitor
```

`tools/idf` runs `idf.py` on `app/firmware` with that local install, and
passes its arguments through.

Flashing replaces whatever firmware is on the device, including its
bootloader and partition table.

## Checks

CI runs all of these on every push to `main` and every pull request
(`.github/workflows/ci.yml`). To run them locally:

| Check | Command |
|---|---|
| Unit tests, with AddressSanitizer and UndefinedBehaviorSanitizer | `cmake --build --preset host && ctest --preset host` |
| Formatting | `tools/check-format.sh` (`--fix` to rewrite files) |
| Constructs the portable parts may not use (`std::string`, `std::vector`, `std::function`, `std::shared_ptr`, iostreams, C stdio streams) | `tools/check-banned.py` |
| clang-tidy (rules in `.clang-tidy`) | `cmake --preset tidy && cmake --build --preset tidy` |
| Firmware builds | `tools/idf build` |

The formatting check and clang-tidy need the LLVM major version named in
`tools/llvm-version` (`brew install llvm@<version>` on macOS).

## Licence

MIT; see `LICENSE`. Third-party components are listed in `THIRD_PARTY.md`.
