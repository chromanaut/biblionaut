# Architecture

## The parts

Each part is a library with an include directory of its own
(`<part>/include/<part>/`, its sources in `<part>/src/`), so a part can see
only the parts it links, and the build holds them to these rules:

```
reader  ->  layout, draw, epub, hw
layout  ->  draw, epub
epub, layout, draw  ->  core        (never hw)
hw                  ->  core
```

Only code under `hw/` and `app/firmware` may include ESP-IDF or FreeRTOS headers.

`cmake/BnParts.cmake` defines the portable parts once, for both builds: the
host build (the root `CMakeLists.txt`, with `hw/desktop`), and the firmware
(`app/firmware`, an ESP-IDF project that adds `hw/esp32c3` and `hw/x4`
through its component `app/firmware/components/bn_hw`).

`core` holds the interfaces through which everything else reaches the
outside world: the clock, storage, the display, the buttons, power, a
source and a sink of bytes, where a crash report is kept, and how much
memory the system has left. `reader::App` is given them all in one
`Services` struct: `DefaultServices()` supplies those of the port it is
linked with, and the tests pass their own (`tests/support/`).

## The rules of the portable parts

The code is C++20 with exceptions and RTTI disabled, so anything that throws
ends in an abort. The portable parts (`core`, `epub`, `draw`, `layout`) are
held to more: they allocate from arenas, not the general heap, and call
nothing that can throw, so every failure reaches their caller as a return
value. `reader` and `hw` may use the general heap and the standard library's
allocating types where that is the simpler choice.

- **No general heap**: no `std::string`, `std::vector`, `std::function`,
  `std::shared_ptr`, iostreams or C stdio streams (`snprintf` into a
  caller's buffer is fine). `tools/check-banned.py` holds the parts to it.
- **Nothing that can throw**, which rules out more than it seems:
  `string_view::substr`, `.at()`, `optional::value()`.
  `tools/check-firmware-symbols.sh` finds any call into the support for
  exceptions in the firmware's build of these parts.
- **Memory comes from the caller**: a span to work in, an `Arena`, or a
  `RegionHeap` inside a block the caller gives (which is how expat gets its
  memory). An object that needs more than it was given says so; it does
  not go and get more.
- **Every failure is a return value**, usually a status with a `...Name()`
  function for diagnostics. Nothing a book or a card holds is trusted:
  whatever bytes it is given, a reader ends in a status.

## How a book is read

A book is pulled through a chain of small buffers, and no file of it is
ever whole in memory: storage, which has at most eight files open at once
([Storage](storage.md)); a `BlockFile`, one buffer of 4 KB over a file; the
ZIP reader, which finds the entries, and a reader of one entry, which
inflates it through a window of 32 KB ([ZIP files](zip.md),
[Decompression](inflater.md)); and the XML parser, given 1 KB a step
([XML](xml.md)), which tells whatever comes after it, such as the reader
of the package ([What a book says of itself](package.md)), what it finds.
A style sheet comes the same way as far as its entry, and is read 1 KB a
step too ([Style sheets](css.md)).

## One loop, in slices

The application is one loop calling `App::Tick()`. Work that takes long is
written to be done a piece at a time (`XmlParser::Step()`,
`Package::Step()`, `CssReader::Step()`, a check's `Tick()` working for about
20 ms and returning), so that the panel and the buttons carry on around it.
Each such piece states a bound on the work one call does, which no input
can stretch, and has a test that holds it to that (`Inflater::MostInputFor()`,
`LineBreaker::MostWorkFor()`, `CssCascade::MostWorkFor()`). Where a piece
calls others that have bounds of their own, it calls them a stated number
of times in one call. On the device the loop is watched: if no turn comes
round for five seconds, the watchdog ends the firmware in a crash report
([Diagnostics](diagnostics.md#crash-reports)).

## Naming

Namespaces are `snake_case`; classes, structs and functions are
`PascalCase`; variables, parameters and members are `camelCase`, with a
trailing underscore on private members; constants are `kPascalCase`.
clang-tidy enforces this.
