# Biblionaut

EPUB reader firmware for the Xteink X4 (ESP32-C3, 800x480 e-ink, SD card),
written so that everything except the hardware layer also builds and runs on
a desktop.

## Layout

| Directory | Contents |
|---|---|
| `core/` | Shared foundation: arenas, the timing trace, counters and gauges, the framed diagnostic stream, checksums, the heap-failure hook, and the interfaces the portable parts use to reach the outside world (clock, byte source, byte sink, display, buttons) |
| `hw/` | Hardware abstraction layer. `hw/include` is the interface. `hw/desktop` implements it for the host; on a device, `hw/esp32c3` holds what the chip provides and `hw/x4` what is specific to the X4 |
| `epub/` | Book parsing: zip, XML, OPF / TOC, CSS, the importer and the intermediate format |
| `draw/` | Fonts and drawing: glyphs, metrics, the display-list format, the band renderer, images |
| `layout/` | Book layout: line breaking, hyphenation, pagination, positions. Emits display lists |
| `reader/` | The reader application. The only part that knows about all the others |
| `app/` | Entry points: `app/desktop` for the host, and `app/firmware`, the ESP-IDF project that builds the one firmware image for all supported devices |
| `tools/` | Host programs (`tools/trace`: the `bn-trace` stream decoder) and developer scripts (`bn-symbolize` for crash reports) |
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

The code is C++20 with exceptions and RTTI disabled, so anything that throws
ends in an abort. The portable parts (`core`, `epub`, `draw`, `layout`) are
held to more: they allocate from arenas, not the general heap, and call
nothing that can throw, so every failure reaches their caller as a return
value. `reader` and `hw` may use the general heap and the standard library's
allocating types where that is the simpler choice.

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

## Diagnostic stream

The application sends trace records, log text and metrics over its serial
line as binary frames (`core/frame.h`): the USB port on a device. On the
desktop the same stream goes to the file named by `BN_SERIAL_OUT`:

```
BN_SERIAL_OUT=run.bin ./build/host/app/desktop/biblionaut-desktop --run-ms 5000
```

Frames are delimited by zero bytes, so they can share the line with plain
text such as boot messages. In a serial monitor the frames show up as short
bursts of unreadable bytes between the readable lines.

`bn-trace` (built with the host build) decodes the stream. It prints every
event as a line of text and can write a timeline to open at
[ui.perfetto.dev](https://ui.perfetto.dev) or `chrome://tracing`:

```
# A capture file from a desktop run
./build/host/tools/trace/bn-trace run.bin --out run.json

# The plugged-in device, live, until Ctrl-C
./build/host/tools/trace/bn-trace --out device.json --save device.bin
```

With no input named, `bn-trace` finds the USB serial device itself; name
the port (for example `/dev/cu.usbmodem101`) if more than one is plugged in.
It restarts the device first, so the capture begins with the boot messages
and the hello that names the firmware. `--no-reset` attaches without
restarting, and then shows whatever the device had buffered since something
last read it.

In the timeline each boot of the device is a process and each task a thread;
spans are slices, log lines are markers, and each counter and gauge is its
own graph. `--save` keeps the raw bytes so a capture can be decoded again
later.

Trace ids are turned back into names from the lists in each part's
`trace_ids.h`, which the tool is built from. A capture from newer firmware
than the tool shows ids it does not know as numbers.

## The panel and the buttons

The e-ink panel's controller keeps two full images in its own memory, and
the firmware keeps none: pictures are rendered a band of rows at a time and
written straight to the controller (`core/display.h`). `hw/x4` drives the
X4's panel.

The devices have seven buttons. Six of them share two analogue pins and
cannot raise an interrupt, so `hw/x4` reads them every 5 ms and turns the
readings into events: pressed, released, held (`core/input.h`). An event
carries the time the change was first seen.

Until there are pages to show, the application shows a test pattern and the
buttons step through its four pictures (`reader/pattern_viewer.h`):

| Button | What it does |
|---|---|
| Down, Right | The next picture, by a fast refresh |
| Up, Left | The previous picture, by a fast refresh |
| Confirm | The same picture again, by a half refresh |
| Back | The same picture again, by a full refresh |

The pattern is in the panel's own orientation, which on the X4 is with the
device turned a quarter-turn anticlockwise from the way a book is read.

The trace of a press runs from the button to the panel:

```
task 1  hw/button_press button=5          the button going down, first reading to settled
task 0  reader/panel_sweep picture=2      rendering the picture and sending it, band by band
task 0  reader/press_to_refresh button=5  from the button going down to the refresh being started
interrupts  hw/refresh_fast               the panel's own time
```

On the desktop a simulation stands in for the panel, working out what a
real one would show after each refresh, including the stale pixels a fast
refresh leaves if the two images are allowed to fall out of step. Buttons
are scripted. Three environment variables control them:

```
BN_DISPLAY_OUT=panel BN_INPUT=4000:down,5000:down,6000:confirm \
  ./build/host/app/desktop/biblionaut-desktop --run-ms 9000
open panel-001.png          # one picture per refresh
```

- `BN_DISPLAY_OUT=prefix` saves what the panel shows at every refresh, as
  `prefix-001.png`, `prefix-002.png` and so on.
- `BN_INPUT` presses buttons: `milliseconds:button`, with an optional third
  field for how long the button is held (`5000:power:1500`). The buttons
  are `back`, `confirm`, `left`, `right`, `up`, `down` and `power`.
- `BN_DISPLAY_SPEED=20` makes the simulated panel twenty times quicker than
  a real one, which takes 3.6 s over a full refresh.

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

## Crash reports

When the firmware stops unexpectedly (a processor fault, `abort()`, a failed
assertion, running out of memory, a stack overflow), ESP-IDF prints what it
knows on the console and restarts the device. Before that happens the
firmware saves a report of its own: the reason, the task, the registers, the
top of the stack and the last 32 trace records. The report sits in memory
that survives a restart, and every later start sends it on the diagnostic
stream, until the device loses power or crashes again. So after a crash,
plug the device in and run `bn-trace`: the report is the first thing after
the boot.

The report gives addresses. `tools/bn-symbolize` turns them into functions
and source lines:

```
./build/host/tools/trace/bn-trace | tools/bn-symbolize
./build/host/tools/trace/bn-trace capture.bin | tools/bn-symbolize
tools/bn-symbolize 0x4200614a
```

Addresses only mean something for the exact build that crashed. Every
firmware build keeps a copy of its ELF file in `.elf-archive/` (ignored by
git, the newest 40 are kept), named by its SHA-256; a report carries the
start of that hash, and `bn-symbolize` uses it to pick the right file. If a
report comes from a build that is not in the archive, its addresses are left
alone; `--elf FILE` names a file to use regardless.

To see a report without waiting for a bug, build a firmware that crashes on
purpose ten seconds after it starts:

```
tools/idf -DBN_CRASH_TEST=fault build flash     # or abort, oom, stack
./build/host/tools/trace/bn-trace | tools/bn-symbolize
tools/idf -DBN_CRASH_TEST= build flash          # back to the normal firmware
```

It does not crash again on the restart that follows a crash. On the desktop,
`BN_FAKE_CRASH=1` makes the application start with an invented report, which
exercises everything after the crash itself.

## Checks

CI runs all of these on every push to `main` and every pull request
(`.github/workflows/ci.yml`). To run them locally:

| Check | Command |
|---|---|
| Unit tests, with AddressSanitizer and UndefinedBehaviorSanitizer | `cmake --build --preset host && ctest --preset host` |
| The same tests as CI's Linux job runs them: GCC 14 on Ubuntu, in Docker | `tools/linux-check.sh` (arguments go to `ctest`) |
| Formatting | `tools/check-format.sh` (`--fix` to rewrite files) |
| Constructs the portable parts may not use (`std::string`, `std::vector`, `std::function`, `std::shared_ptr`, iostreams, C stdio streams) | `tools/check-banned.py` |
| clang-tidy (rules in `.clang-tidy`) on everything the host build compiles | `cmake --preset tidy && cmake --build --preset tidy` |
| clang-tidy on what the firmware build compiles: the device code, and the portable parts as built for the device | `tools/check-firmware-tidy.py` (after a firmware build) |
| Firmware builds | `tools/idf build` |
| The portable parts (`core`, `epub`, `draw`, `layout`) call nothing that can throw | `tools/check-firmware-symbols.sh` (after a firmware build) |

The formatting check and both clang-tidy runs need the LLVM major version
named in `tools/llvm-version` (`brew install llvm@<version>` on macOS).

Two clang-tidy runs, because no single build compiles everything. The host
run sees the portable parts, the desktop port, the tools and the tests, and
leaves the Clang Static Analyzer out for the tests (`tests/.clang-tidy` says
why). The firmware run sees the device code under `hw/` and
`app/firmware`, which nothing else checks, by translating the firmware's
GCC compile commands into ones clang can use.

The Linux run is worth doing before pushing changes to how memory is
handled. Its compiler and C library catch faults the macOS build cannot see,
such as a null pointer passed to `memset` with a length of zero, and under
the sanitizers a finding fails the test.

## Licence

MIT; see `LICENSE`. Third-party components are listed in `THIRD_PARTY.md`.
