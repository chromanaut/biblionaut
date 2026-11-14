# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Biblionaut is EPUB reader firmware for the Xteink X4 (ESP32-C3, no PSRAM, 800x480 e-ink, SD card), written from scratch in C++20 with exceptions and RTTI off. Everything except the hardware layer also builds and runs on a desktop, which is where nearly all testing happens.

`README.md` is the documentation and is kept in step with the code: it has a section for each piece (storage, the inflater, ZIP, XML, the package reader, the diagnostic stream, crash reports, the device checks). Read the section before changing a piece, and update it in the same change.

## Commands

Host build (needs CMake 3.25+, Ninja, a C++20 compiler; expat and GoogleTest are downloaded at configure time):

```
cmake --preset host                # once, and after changes to CMake files
cmake --build --preset host        # debug, ASan + UBSan, warnings are errors
ctest --preset host                # every unit and end-to-end test
```

One test: run the test program directly, or name it to CTest.

```
./build/host/tests/epub_tests --gtest_filter='Package.*'
ctest --preset host -R e2e.BookCheck
```

The test programs are `core_tests`, `draw_tests`, `epub_tests`, `hw_tests`, `reader_tests` and `tools_tests`, in `build/host/tests/`. The end-to-end tests are named `e2e.*`; most are Python scripts in `tests/e2e/` and are only registered if Python 3 is found.

`host-release` is the optimised preset without sanitizers (`build/host-release`): use it for timing anything and for running the tools over many files. Its tests are run as well (`ctest --preset host-release`): a few things are built differently when a sanitizer is there, the heap inside a block among them, and the firmware has none.

`tools/check-books.py FOLDER` runs `bn-zip`, `bn-xml` and `bn-epub` over every EPUB under a folder and compares each with a reading of the same book by Python. CTest runs it over `corpus/` as `e2e.Corpus`; run it over any larger collection of books a change could affect, with the `host-release` tools.

The other checks. CI runs all of them (`.github/workflows/ci.yml`), and all of them are run before work is handed over:

| Check | Command |
|---|---|
| Formatting | `tools/check-format.sh` (`--fix` rewrites) |
| Constructs the portable parts may not use | `tools/check-banned.py` |
| clang-tidy on the host build | `cmake --preset tidy && cmake --build --preset tidy` |
| Firmware build | `tools/idf build` |
| Nothing in the portable parts can throw | `tools/check-firmware-symbols.sh` (after a firmware build) |
| clang-tidy on what the firmware compiles | `tools/check-firmware-tidy.py` (after a firmware build) |
| The tests again, optimised and without sanitizers | `cmake --build --preset host-release && ctest --preset host-release` |
| CI's Linux job: GCC 14, with sanitizers and then without | `tools/linux-check.sh` (needs Docker running; arguments go to `ctest`) |

CI does not build a firmware with one of the device checks switched on (below). Build and test that here when a change touches it.

The format check and clang-tidy need the LLVM major version in `tools/llvm-version`. The Linux check catches what a macOS build does not, such as a header that libc++ pulls in by itself and libstdc++ does not, or code that only GCC parses differently. If Docker is not running, say that the check was not run.

Firmware: `tools/setup-idf.sh` once installs the pinned ESP-IDF into `.esp/`; `tools/idf` runs `idf.py` on `app/firmware` with it and passes its arguments through, so build options go first: `tools/idf -DBN_BOOK_CHECK=ON build`. Flashing replaces what is on the device, bootloader included: do not flash unless asked.

The firmware is compiled at `-O2` (`app/firmware/sdkconfig.defaults`), and a timing from a device is only worth recording from such a build: ESP-IDF's default is `-Og`, at which the inflater runs a third slower. ESP-IDF writes a build's settings to `app/firmware/sdkconfig` the first time and keeps them, so after a change to `sdkconfig.defaults` that file has to be deleted before the change is taken up (`grep OPTIMIZATION app/firmware/sdkconfig` says what a build is using).

## Architecture

### Parts and what they may use

```
reader  ->  layout, draw, epub, hw
layout  ->  draw, epub
epub, layout, draw  ->  core        (never hw)
hw                  ->  core
```

Each part is a library with its own include directory (`<part>/include/<part>/`, sources in `<part>/src/`), so a part can only include what it links. `cmake/BnParts.cmake` defines the portable parts once for both builds: the host build (root `CMakeLists.txt`, with `hw/desktop`) and the firmware (`app/firmware`, an ESP-IDF project that adds `hw/esp32c3` and `hw/x4` through the component `app/firmware/components/bn_hw`). Only `hw/` and `app/firmware` may include ESP-IDF or FreeRTOS headers.

`core` holds the interfaces everything else reaches the outside world through (`Clock`, `Storage`, `Display`, `Input`, `ByteSource`, `ByteSink`, `CrashStore`, `MemoryProbe`). `reader::App` is given them as a `Services` struct: `DefaultServices()` supplies the linked port's, and tests pass the fakes in `tests/support/`.

### The rules of the portable parts

`core`, `epub`, `draw` and `layout` are written for a device with a few hundred KB of RAM:

- No general heap: no `std::string`, `std::vector`, `std::function`, `std::shared_ptr`, no iostreams, no C stdio streams (`snprintf` into a caller's buffer is fine). `tools/check-banned.py` enforces this.
- Nothing that can throw, which with exceptions off means abort. That rules out more than it seems: `string_view::substr`, `.at()`, `optional::value()`. `tools/check-firmware-symbols.sh` finds calls into exception support in the firmware's build of these parts.
- Memory comes from the caller: a span to work in (`Package`, `ZipEntryReader`'s window), an `Arena`, or a `RegionHeap` inside a caller's block (how expat gets its memory). An object that needs more than it was given reports that; it does not go and get more.
- Every failure is a return value, usually a status enum with a `...Name()` function for diagnostics. Input from a book or a card is never trusted: any bytes at all must end in a status.

`reader`, `hw`, tools and tests may use the heap and the allocating standard types where that is simpler. `new (std::nothrow)` works as it says (see `core/heap.h`).

### How a book is read

Reading is pulled through a chain of small buffers, never a whole file in memory:

`core::Storage` (at most four files open) → `core::BlockFile` (one 4 KB buffer over a file) → `epub::ZipArchive` (finds entries) and `epub::ZipEntryReader` (a `ByteSource` over one entry, inflating through `core::Inflater` with a 32 KB window) → `epub::XmlParser` (expat, fed 1 KB a `Step()`, telling an `XmlHandler`) → `epub::Package` and whatever comes after it.

A book is opened twice, with one `BlockFile` for the ZIP directory and one for the entry being read; sharing one costs a re-read at every switch between them. Looking entries up one by one with `ZipArchive::Find` reads the directory again each time, so anything that wants many entries goes through the directory once with `Next` (as `Package::FindFiles` does).

### One loop, in slices

The application is a single loop calling `App::Tick()`. Work that takes long is written to be done a piece at a time (`XmlParser::Step()`, the checks' `Tick()` working about 20 ms and returning), so the panel and the buttons carry on around it.

### Diagnostics

- **Trace spans** (`core/trace.h`): each part lists its span ids once in `<part>/trace_ids.h`. An id's index is part of the trace format: add at the end, never renumber. `tools/trace/names.cpp` builds the tool's name table from those lists.
- **Counters and gauges** (`core/metrics.h`) are globals that register themselves and go out in the metrics lines.
- All of it leaves the device as one framed **diagnostic stream** on the serial port; `bn-trace` (`tools/trace`) decodes a capture. A line of log text carries at most 192 bytes.

### Device checks and their desktop twins

A firmware can be built to check one thing on real hardware: `-DBN_BOOK_CHECK=ON`, `-DBN_STORAGE_CHECK=ON` (these two are `reader::DeviceCheck`s, cannot be combined, and are in no normal firmware) or `-DBN_PLANE_CHECK=ON`. The desktop application runs the same checks when the environment variable of the same name is set, with `BN_STORAGE` naming a folder that stands for the card and `BN_SERIAL_OUT` a file to capture the stream in.

Each layer of the book pipeline also has a host tool that runs the same code on one file: `bn-zip`, `bn-xml`, `bn-epub`. The book check reports counts, reads of storage and checksums in the same terms those tools print, so that what a device found can be set beside what the desktop finds for the same book. When a check gains a figure, the matching tool should print it too.

## Conventions

- **Naming** (clang-tidy enforces it): namespaces `snake_case`; types and functions `PascalCase`; variables, parameters and members `camelCase`, with a trailing `_` on private members; constants `kPascalCase`.
- **Comments and the README** say what the code does and why, in plain sentences. They describe the repository as it is: no references to plans, phases or decisions recorded anywhere else.
- **Tests** are named as sentences about behaviour (`TEST(Package, FindsTheCoverTheWayTheBookNamesIt)`). End-to-end tests compare a tool with something that shares none of its code (Python's `zipfile`, its expat, ElementTree) on books the test makes itself, and `e2e.Corpus` does the same on the real books in `corpus/`.
- **End-to-end tests of the desktop application must not count turns.** A CI machine gives the application far fewer turns a second than a developer's does. Script them against the clock, and give a run longer if it has not finished.
- **Output shown in the README is real output** of the tools, regenerated when it changes, not written by hand. Only a name that would be a real book's is replaced with a made-up one.
- **Third-party code** is not copied into the repository. A dependency is fetched when a build is configured, pinned by hash (see `third_party/expat/`), and gets a row in `THIRD_PARTY.md`, with its licence checked against its own licence file, before it lands. Where a method or a set of values came from somewhere else, a comment and `THIRD_PARTY.md` say so.
- **The only real books in the repository are those in `corpus/`**, kept byte for byte as they were fetched, each listed in `corpus/README.md` with its authors, where it came from and its terms, and in `corpus/SHA256SUMS`. A book is added there only with both, and only on one of two grounds: it is in the public domain and everyone who wrote, translated or illustrated it died more than seventy years ago (the books from Project Gutenberg and Standard Ebooks); or the file itself states a licence that lets unchanged copies be passed on, such as CC BY-SA (the IDPF samples). Read the terms in the file, not only where it was downloaded from, and look at the fonts inside: a file that carries a font which may not be passed on stays out. No book from anyone's own library goes into the repository, and no captures from a device: a capture names the books on its owner's card.
- clang-tidy treats its findings as errors. Two that come up often: write a size as a literal (`32768`, not `32 * 1024`), and search for a character as a character (`find('.')`).
