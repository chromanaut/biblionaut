# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Biblionaut is EPUB reader firmware for the Xteink X4 (ESP32-C3, no PSRAM, 800x480 e-ink, SD card), written from scratch in C++20 with exceptions and RTTI off. Everything except the hardware layer also builds and runs on a desktop, which is where nearly all testing happens.

The documentation is in `docs/`, a document for each piece (storage, the inflater, ZIP, XML, the package reader, the diagnostic stream, crash reports, the device checks, and the rest); `README.md` says what the project is for and lists them. A document is kept in step with the code: read a piece's document before changing the piece, and update it in the same change. A new piece gets a document of its own and a line in the README's list. A document says what its piece does, its limits and the reasons for them, how to run its tool or its check, and what they print. It does not keep the history of how a figure was arrived at, the faults its tests were tried against, or the results of runs over collections of books outside the repository.

## Commands

Host build (needs CMake 3.25+, Ninja, a C++20 compiler; expat, TeX's American English hyphenation patterns, FreeType, GoogleTest and, for the tests of `bn-font`, the font files of Courier Prime are downloaded at configure time):

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

The test programs are `core_tests`, `draw_tests`, `epub_tests`, `layout_tests`, `hw_tests`, `reader_tests` and `tools_tests`, in `build/host/tests/`. The end-to-end tests are named `e2e.*`; most are Python scripts in `tests/e2e/` and are only registered if Python 3 is found. The fuzz targets are `fuzz_*` in `build/host/tests/fuzz/`; in these builds each goes through the inputs kept for it in `tests/fuzz/inputs/`, as the tests named `fuzz.*`.

`host-release` is the optimised preset without sanitizers (`build/host-release`): use it for timing anything and for running the tools over many files. Its tests are run as well (`ctest --preset host-release`): a few things are built differently when a sanitizer is there, the heap inside a block among them, and the firmware has none.

`window` (`build/window`) is the desktop application alone with a window, made with SDL, which that build fetches (`BN_DESKTOP_WINDOW`): `./build/window/app/desktop/biblionaut-desktop --window` draws the X4 with the simulated panel and the buttons, which are clicked or pressed from the keyboard. Its one test is `e2e.Window` (`ctest --preset window`). What the desktop simulates of a device (panel size, refresh times, the turn for reading, how many levels the panel shows, where the buttons are) is described once, in `hw/desktop/device_profile.h`, and `BN_DEVICE` chooses it; `x4` is the only one so far. Opening the window puts it on the screen of whoever is at the computer; the test uses SDL's `dummy` video driver, which needs no screen, and `SDL_VIDEO_DUMMY_SAVE_FRAMES=1` with that driver saves each frame the window would show as a BMP in the current folder.

`tools/check-books.py FOLDER` runs `bn-zip`, `bn-xml`, `bn-epub`, `bn-css`, `bn-png`, `bn-jpeg` and `bn-bundle` over every EPUB under a folder and compares each with a reading of the same book by Python. CTest runs it over `corpus/` as `e2e.Corpus`; run it over any larger collection of books a change could affect, with the `host-release` tools.

The other checks. CI runs all of them (`.github/workflows/ci.yml`), and all of them are run before work is handed over:

| Check | Command |
|---|---|
| Formatting | `tools/check-format.sh` (`--fix` rewrites) |
| Constructs the portable parts may not use | `tools/check-banned.py` |
| clang-tidy on the host build | `tools/check-tidy.sh` |
| Firmware build | `tools/idf build` |
| Nothing in the portable parts can throw | `tools/check-firmware-symbols.sh` (after a firmware build) |
| clang-tidy on what the firmware compiles | `tools/check-firmware-tidy.py` (after a firmware build) |
| The tests again, optimised and without sanitizers | `cmake --build --preset host-release && ctest --preset host-release` |
| The desktop application with its window (CI: macOS only) | `cmake --preset window && cmake --build --preset window && ctest --preset window` |
| CI's Linux jobs: GCC 14, with sanitizers and without | `tools/linux-check.sh` (needs Docker running; arguments go to `ctest`) |

The heavy checks take turns. `tools/linux-check.sh`, `tools/check-tidy.sh`, `tools/check-firmware-tidy.py` and `tools/fuzz.py` each take every processor for minutes, so each waits until no other of them is running on the machine, whichever checkout it was started from (`tools/queued`; `tools/queued --list` says what is running and what is waiting, and `--watch` keeps saying it until it is stopped). A wait can be far longer than the check itself, so start them in the background; and do not go round the queue by typing the `cmake` or `docker` command a script would have run. Anything else as heavy can be given a turn in the same queue: `tools/queued COMMAND`. A turn is the whole machine unless the command says how many processors it keeps to, `tools/queued --cores 2 COMMAND`, and then others that say so run beside it: say it for whatever uses a processor or two (one test program, a trial that compiles one file and runs one test, a script that mostly waits), ask for no fewer than the command uses, and give each check a turn of its own, not one turn to a script of several. A run on a device, flashing and capturing as one command, can take turns in the same way, in a queue of the device's own: `tools/queued --name x4 sh -c '...'`.

CI does not build a firmware with one of the device checks switched on (below). Build and test that here when a change touches it.

Nor does CI fuzz: it only takes the fuzz targets through their kept inputs. `tools/fuzz.py` builds the targets with LLVM's fuzzing engine and runs each for a minute (`--time SECONDS` for longer; name targets to run only those), in two builds: with AddressSanitizer, and without it, where the heap inside a block is laid out as the firmware has it. Run it for a few minutes after a change to anything that reads a book's bytes (`core/inflate`, `core/region_heap`, the readers in `epub/` and the table of rules, `layout/hyphenator` and `layout/paragraph_setter`, and the readers of SVG, PNG and JPEG, the scaler and the picture cache in `draw/`) or to the reader of the fonts image (`draw/font`), and say what it found. What it builds up and finds stays in `.fuzz/`, which git ignores.

The format check and clang-tidy need the LLVM major version in `tools/llvm-version`. The Linux check catches what a macOS build does not, such as a header that libc++ pulls in by itself and libstdc++ does not, or code that only GCC parses differently. If Docker is not running, say that the check was not run.

Firmware: `tools/setup-idf.sh` once installs the pinned ESP-IDF into `.esp/`; `tools/idf` runs `idf.py` on `app/firmware` with it and passes its arguments through, so build options go first: `tools/idf -DBN_BOOK_CHECK=ON build`. The firmware build also compiles `bn-hyphen` with the computer's own compilers (in `app/firmware/build/host-tools`), to make the hyphenation table it compiles in (`third_party/hyph-en-us`). Flashing replaces what is on the device, bootloader included: do not flash unless asked.

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

`core` holds the interfaces everything else reaches the outside world through (`Clock`, `Storage`, `Display`, `Input`, `Power`, `ByteSource`, `ByteSink`, `ByteMap`, `CrashStore`, `MemoryProbe`). `reader::App` is given them as a `Services` struct: `DefaultServices()` supplies the linked port's, and tests pass the fakes in `tests/support/`.

### The rules of the portable parts

`core`, `epub`, `draw` and `layout` are written for a device with a few hundred KB of RAM:

- No general heap: no `std::string`, `std::vector`, `std::function`, `std::shared_ptr`, no iostreams, no C stdio streams (`snprintf` into a caller's buffer is fine). `tools/check-banned.py` enforces this.
- Nothing that can throw, which with exceptions off means abort. That rules out more than it seems: `string_view::substr`, `.at()`, `optional::value()`. `tools/check-firmware-symbols.sh` finds calls into exception support in the firmware's build of these parts.
- Memory comes from the caller: a span to work in (`Package`, `ZipEntryReader`'s window), an `Arena`, or a `RegionHeap` inside a caller's block (how expat gets its memory). An object that needs more than it was given reports that; it does not go and get more.
- Every failure is a return value, usually a status enum with a `...Name()` function for diagnostics. Input from a book or a card is never trusted: any bytes at all must end in a status.

`reader`, `hw`, tools and tests may use the heap and the allocating standard types where that is simpler. `new (std::nothrow)` works as it says (see `core/heap.h`).

### How a book is read

Reading is pulled through a chain of small buffers, never a whole file in memory:

`core::Storage` (at most eight files open) → `core::BlockFile` (one 4 KB buffer over a file) → `epub::ZipArchive` (finds entries) and `epub::ZipEntryReader` (a `ByteSource` over one entry, inflating through `core::Inflater` with a 32 KB window) → `epub::XmlParser` (expat, fed 1 KB a `Step()`, telling an `XmlHandler`) → `epub::Package` and whatever comes after it.

A style sheet comes through the same chain as far as the entry: `epub::CssReader` reads it from a `ByteSource`, 1 KB a `Step()`, and tells a `CssHandler` of its rules and of what it passed over. A `font-family` is told with the kind of face its names come to (`CssFamilyClass`: serif, sans, monospace, script or other, by CSS's own words and a table of names that are known), and that is all that is kept of it. `epub::CssRules` is such a handler: it keeps the rules of all a book's sheets in one block. `epub::CssCascade` goes through a document beside its parser and says, for each element as it opens, what those rules set for it; it keeps nothing of the document but a few bytes for each open element. `epub::DocumentSheets` finds the sheets a document asks for: it reads the document's head, from a document its caller hands over open or one it opens by its path through `epub::BookFiles` (a book's files by their paths; `ZipBookFiles` over the ZIP reader), reads each sheet the head links into the table once for the whole book, with the sheets it imports, and the document's own `<style>` rules for that document only, and gives the list `CssCascade::Begin()` takes; what it leaves out it counts by kind. `epub::SheetsDigest` boils what a book's documents were given down to a checksum, as the digests of a package and of a table of contents do. `epub::StyleStack` goes through a document beside the cascade and says what each element comes to: what it takes from the one it is inside, what it is given by being the element it is, and what the book's values come to as amounts (a size in thousandths of the reader's, a length as so much of that size, of a pixel and of the page's width), each with whether the book or the element said it; it keeps 28 bytes for each open element. `epub::DocumentBlocks` goes through a document with one, told of each element and of where text or a picture comes, and says what a bundle keeps of the document: where a block of text begins and ends (a block is made by what is in it, not by an element), its kind, and its record, with the largest of the margins that meet above it and the margins and padding of every block it is inside to its sides; it keeps 76 bytes for each open element, and nothing of the text. Nothing yet writes a chapter's blocks down.

A book is opened twice, with one `BlockFile` for the ZIP directory and one for the entry being read; sharing one costs a re-read at every switch between them. `ZipArchive::Find` looks an entry up by its name from where its last search ended: names asked in the directory's own order cost next to nothing each, and a name asked out of that order, or one that is not there, costs up to a read of the whole directory. So anything that wants many entries goes through the directory once with `Next` (as `Package::FindFiles` does), and a document of the spine is opened from the place the package keeps for it (`SpineItem::entry`), not found by its name again.

A book is to be imported into one file on the card, a bundle (`epub/bundle.h`, `docs/bundle.md`): a header page and tables written once, two slots of chapter records written in turn, and the chapters' segments appended, so that the power going loses at most the chapter in hand. `BundleMaker` makes the container and `Bundle` opens it, reads its records and commits a chapter; `BookBundler` (`epub/book_bundle.h`) fills a new one from a book whose package has been read, and `bn-bundle` runs that on the desktop. Nothing imports a chapter yet, and a chapter's own bytes have no format yet. Anything that changes it has to keep two rules: nothing committed shares a 512-byte sector with anything written later, and a step reads and writes a page at the most.

### One loop, in slices

The application is a single loop calling `App::Tick()`. Work that takes long is written to be done a piece at a time (`XmlParser::Step()`, `Package::Step()`, `TocReader::Step()`, `CssReader::Step()`, the checks' `Tick()` working about 20 ms and returning), so the panel and the buttons carry on around it. A piece has a stated bound on its work that no input can stretch, and a test that holds it to that (`Inflater::MostInputFor()`, `Package::kEntriesPerStep`, `LineBreaker::MostWorkFor()`, `CssCascade::MostWorkFor()`, `StyleStack::MostWorkFor()`, `DocumentBlocks::MostWorkFor()`). Where a piece calls others that have bounds of their own, it does so a stated number of times in one call, as `ParagraphSetter` gives the breaker one item at the most. On the device the loop is watched: if no turn comes round for five seconds, the watchdog ends the firmware in a crash report.

### Diagnostics

- **Trace spans** (`core/trace.h`): each part lists its span ids once in `<part>/trace_ids.h`. An id's index is part of the trace format: add at the end, never renumber. `tools/trace/names.cpp` builds the tool's name table from those lists.
- **Counters and gauges** (`core/metrics.h`) are globals that register themselves and go out in the metrics lines.
- All of it leaves the device as one framed **diagnostic stream** on the serial port; `bn-trace` (`tools/trace`) decodes a capture. A line of log text carries at most 192 bytes.

### Device checks and their desktop twins

A firmware can be built to check one thing on real hardware: `-DBN_BOOK_CHECK=ON` (with `-DBN_BOOK_CHECK_BRIEF=ON` it opens each book and reads none through), `-DBN_STORAGE_CHECK=ON`, `-DBN_LAYOUT_CHECK=ON` (these three are `reader::DeviceCheck`s, no two can be combined, and none is in a normal firmware) or `-DBN_PLANE_CHECK=ON`. `-DBN_PARTITION_CHECK=ON` checks the fonts partition at start, and flashing it fills that partition with a pattern in place of any fonts image, which has to be flashed again afterwards; it has no desktop twin. Nor has `-DBN_WINDOW_CHECK=ON`, which checks at start that the panel takes windows narrower than whole rows (`Display::WriteWindow()`), and times them, nor `-DBN_GRAY_CHECK=ON`, which shows at start a picture in the panel's four levels through the gray pass (`Display::StartGrayRefresh()`), in stages held long enough for someone to look at the panel, and times it. The desktop application runs the same checks when the environment variable of the same name is set, with `BN_STORAGE` naming a folder that stands for the card and `BN_SERIAL_OUT` a file to capture the stream in. A firmware built with `-DBN_DRAWING_VIEWER=ON`, or the desktop with `BN_DRAWINGS` set, shows the SVG, PNG and JPEG files in the card's `drawings` folder in place of the test pattern (`reader/drawing_viewer.h`, a `reader::Screen`), and takes none of the checks with it; with `-DBN_DRAWING_TOUR=ON` (`BN_DRAWING_TOUR`) as well it goes round them twice by itself, the second time from the cache of what it has shown, so a device run can be timed with nobody at the buttons. A firmware built with `-DBN_TEXT_VIEWER=ON`, or the desktop with `BN_TEXT` naming a fonts image, shows the text of the card's `layout/paragraphs.txt` set in the image's faces a page at a time (`reader/text_viewer.h`, also a `reader::Screen`); with `-DBN_TEXT_TOUR=ON` (`BN_TEXT_TOUR`) as well it goes through two pages in each face at each size by itself.

Each layer of the book pipeline also has a host tool that runs the same code on one file: `bn-zip`, `bn-xml`, `bn-epub`, `bn-css`. The book check reports counts, reads of storage and checksums in the same terms those tools print, so that what a device found can be set beside what the desktop finds for the same book. The layout check does the same with `bn-lines --figures`, over a file of paragraphs that `tools/layout-text.py` makes from books. When a check gains a figure, the matching tool should print it too.

### Fonts

The firmware reads no font files. `bn-font` (`tools/font`) draws the faces a preset names, at the sizes it gives, with FreeType on the computer that builds, and writes the fonts image (`bn-font sizes` says what an image's pictures would take compressed in each of several ways, and changes nothing); a face is kerned as its font is, from the font's GPOS table, which `tools/font/kerning.h` reads since FreeType reads only a part of it, and an image keeps a face's kerning as two classes in each glyph's record and a table of signed bytes (`Face::Kern()`), which the measure adds to a glyph's advance before it rounds the step to a whole pixel, so that two glyphs are the same distance apart wherever they come; a face has the ligatures its font makes of its own accord, from the font's GSUB table, which `tools/font/ligatures.h` reads, and an image keeps them as a list of the two or three glyphs each is made of and the glyph that stands for them (`Face::Ligature()`), drawn for the purpose where no character has it, and nothing sets text with them yet; `draw/font.h` reads an image where it lies and describes its layout: opened as a span of bytes, or through a `core::ByteMap` (`core/byte_map.h`), as a device has to, since it cannot map the whole of an image at once, and then a face has glyphs only while it is held (`FontImage::Hold()`, `ReleaseFaces()`); `layout/face_measure.h` is the `TextMeasure` that sets text in an image's faces; `reader/text_viewer.h` shows text set in them, in the panel's grays, and keeps a list of a page's glyphs, not a picture of the page. `cmake --build --preset host --target fonts` makes an image of a preset (`tools/font/presets`: Noto unless `-DBN_FONTS=plex` or `source` or a preset file's path is given, with Courier Prime as the monospace face unless `-DBN_FONTS_MONO=family`), downloading its font files then and not before (`third_party/fonts`); the sizes are in `presets/ladder.preset`. On a device the image is in the `fonts` partition of the firmware's partition table (`app/firmware/partitions.csv`), which `hw::SystemFonts()` gives as a `ByteMap`; flashing a firmware leaves it as it was, and `tools/idf -DBN_FONTS_IMAGE=PATH fonts-flash` writes it. FreeType is only ever linked into `bn-font`. No font is in the repository: the presets' fonts are fetched, each pinned by commit and hash, and what their licence asks of a firmware given out with them is in `THIRD_PARTY.md`; the tests of `bn-font` use Courier Prime, which the build fetches for them (`third_party/courier-prime`), and for kerning and ligatures two of the Noto files, fetched when a build with tests is built; and a preset naming fonts from someone's own computer stays out of it, as those fonts do.

## Conventions

- **Naming** (clang-tidy enforces it): namespaces `snake_case`; types and functions `PascalCase`; variables, parameters and members `camelCase`, with a trailing `_` on private members; constants `kPascalCase`.
- **Comments and the documents** say what the code does and why, in plain sentences. They describe the repository as it is: no references to plans, phases or decisions recorded anywhere else.
- **Tests** are named as sentences about behaviour (`TEST(Package, FindsTheCoverTheWayTheBookNamesIt)`). End-to-end tests compare a tool with something that shares none of its code (Python's `zipfile`, its expat, ElementTree) on books the test makes itself, and `e2e.Corpus` does the same on the real books in `corpus/`.
- **Everything that reads bytes a book's maker chose has a fuzz target** in `tests/fuzz/`, and a new reader comes with its own: the target, its seed inputs (`tests/fuzz/make_seeds.py` makes them, out of text written for the purpose), and a row in `tools/fuzz.py`. A target checks what the reader's header promises, not only that it does not crash, and holds each call to its stated bound. An input that finds a fault is cut down (`tools/fuzz.py --shrink`) and, once the fault is mended, kept as `tests/fuzz/inputs/<target>/found-<what it found>`.
- **End-to-end tests of the desktop application must not count turns.** A CI machine gives the application far fewer turns a second than a developer's does. Script them against the clock, and give a run longer if it has not finished.
- **Hidden files on the card.** Anything that lists a folder of the card passes over a name that begins with a dot, file or folder: a Mac writes a `._` file beside each file it copies to a card, and nobody can be asked to clear them each time.
- **Tests run side by side.** CI runs them one to a core, and a local run can do the same (`ctest --preset host -j 8`: under a minute on a ten-core Mac, against four one at a time). A test writes only files no other test writes: in a directory, or under names, of its own.
- **Output shown in the documents is real output** of the tools, regenerated when it changes, not written by hand. Only a name that would be a real book's is replaced with a made-up one. A run of `tools/fuzz.py` is not shown there: its figures are different every run, and every change to a reader would rewrite them.
- **Third-party code** is not copied into the repository. A dependency is fetched when a build is configured, pinned by hash (see `third_party/expat/`), and gets a row in `THIRD_PARTY.md`, with its licence checked against its own licence file, before it lands. Where a method or a set of values came from somewhere else, a comment and `THIRD_PARTY.md` say so.
- **The only real books in the repository are those in `corpus/`**, kept byte for byte as they were fetched, each listed in `corpus/README.md` with its authors, where it came from and its terms, and in `corpus/SHA256SUMS`. A book is added there only with both, and only on one of two grounds: it is in the public domain and everyone who wrote, translated or illustrated it died more than seventy years ago (the books from Project Gutenberg and Standard Ebooks); or the file itself states a licence that lets unchanged copies be passed on, such as CC BY-SA (the IDPF samples). Read the terms in the file, not only where it was downloaded from, and look at the fonts inside: a file that carries a font which may not be passed on stays out. No book from anyone's own library goes into the repository, and no captures from a device: a capture names the books on its owner's card.
- clang-tidy treats its findings as errors. Two that come up often: write a size as a literal (`32768`, not `32 * 1024`), and search for a character as a character (`find('.')`).
