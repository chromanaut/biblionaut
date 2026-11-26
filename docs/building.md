# Building

## Host build

Needs CMake 3.25 or later, Ninja, a C++20 compiler and a C compiler. Five
things are downloaded when CMake configures, each checked against a hash:
expat, the XML parser, which is C and is why a C compiler is needed;
TeX's hyphenation patterns for American English, which the build makes
into a table ([Where a word may be broken](hyphenation.md)); FreeType, which draws the
glyphs for `bn-font` ([Fonts](fonts.md)); GoogleTest; and, for the tests of
`bn-font`, the font files of Courier Prime. (Two more font files are
downloaded when a build with tests is built, for the tests of kerning; and
the fonts of a fonts image only when one is built: see [The
presets](fonts.md#the-presets).) The tests also link zlib, to check the inflater against and to
make their ZIP files with, and so does `bn-font`, to say what a fonts image
would take compressed: macOS has it, and on Linux it is the `zlib1g-dev`
package. Most of the end-to-end
tests are run only if Python 3 is found, and one of them makes more of its
test files if there is a `zip` command. A build with the desktop
application's window downloads a third thing, SDL (see [A window on the
desktop](panel-and-buttons.md#a-window-on-the-desktop)).

```
cmake --preset host
cmake --build --preset host
ctest --preset host
./build/host/app/desktop/biblionaut-desktop
```

The `host` preset is a debug build with AddressSanitizer and
UndefinedBehaviorSanitizer. `host-release` is optimised, without
sanitizers, and has the same tests (`cmake --preset host-release`, and so
on): it is the nearer of the two to how the firmware is compiled.

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

The build also needs the computer's own C and C++ compilers, the ones the
host build uses. The table of American English's hyphenation patterns is
made as the firmware is built, by `bn-hyphen`, which is a program for the
computer and not for the device: the firmware build compiles it with the
host build's CMake files, in a build of its own under
`app/firmware/build/host-tools` that makes nothing else. It takes the
patterns the firmware build downloaded, and downloads nothing itself.

The firmware is compiled for speed (`-O2`), with ESP-IDF's assertions left
on: `app/firmware/sdkconfig.defaults` sets both. Every figure in these
documents that was measured on a device is from such a build, and the
level matters. On an X4 reading the same 34 books, a firmware at ESP-IDF's
own default (`-Og`, for debugging) inflated at two thirds of the speed
and parsed XML an eighth slower; one compiled for size (`-Os`) was hardly
faster than that, and took three and a half times as long to draw a test
pattern. The `-O2` firmware is 22 KB larger than the first and 74 KB
larger than the second, of the megabyte it has.

Three more settings there leave some of ESP-IDF's own code in flash that
it would otherwise copy into RAM, and on this chip RAM that holds code is
RAM the heap does not have. The driver for the chip's flash is taken from
the chip's ROM; the ring buffer's functions for interrupt handlers and the
C library's locks stay in flash. ESP-IDF keeps such code in RAM so that it
can run while the chip's flash is being written, which this firmware never
does. On an X4 they leave the heap 13 KB more (298 KB free with the test
pattern showing, against 285 KB), and the largest block that can be taken
16 KB larger. Reading the same 34 books took as long with them as without
(108.5 s and 108.6 s), and so did the card's own check and writing to the
panel; getting a picture out of the cache and ready to show took 2% longer
(207 ms against 204 ms). A fourth setting, which moves the heap's own
functions, would leave 17 KB more again; but ESP-IDF then moves the SPI
driver's interrupt handler out of RAM with them, and the card's driver
uses that for every block of data. With it the card read 6% slower and
the 34 books took 4% longer, so it is left as it was.

ESP-IDF keeps the settings a build was first made with in
`app/firmware/sdkconfig`, which it writes itself. After a change to
`sdkconfig.defaults`, delete that file and build again, or the change is
not taken up.

Flashing replaces whatever firmware is on the device, including its
bootloader and partition table. The table (`app/firmware/partitions.csv`)
gives the firmware 4 MB and the 12 MB after it to the fonts image, which
flashing a firmware does not write: `tools/idf
-DBN_FONTS_IMAGE=/a/fonts/image fonts-flash` does ([Fonts on a
device](fonts.md#fonts-on-a-device)).

### Firmwares that check one thing

A firmware can be built to check one thing on a device, or to do one thing
in place of the test pattern, chosen by an option when it is built.

| Option | What the firmware does | Described in |
|---|---|---|
| `-DBN_BOOK_CHECK=ON`, and `-DBN_BOOK_CHECK_BRIEF=ON` as well for a brief one | Reads every book on the card, and says what it found and what that cost | [Reading the books on a card](book-check.md) |
| `-DBN_STORAGE_CHECK=ON` | Puts the card through its paces | [Storage](storage.md#the-storage-check) |
| `-DBN_LAYOUT_CHECK=ON` | Times the layout code, and sets the text of real books | [Timing layout on a device](layout-check.md) |
| `-DBN_PLANE_CHECK=ON` | Reads the panel's two planes back whenever it comes to rest | [The panel's two planes](panel-and-buttons.md#the-panels-two-planes) |
| `-DBN_WINDOW_CHECK=ON` | Writes windows of a plane, reads them back and times them | [Windows of a plane](panel-and-buttons.md#windows-of-a-plane) |
| `-DBN_GRAY_CHECK=ON` | Shows a picture in the panel's four levels, to be looked at | [Grays](panel-and-buttons.md#grays) |
| `-DBN_PARTITION_CHECK=ON` | Fills the partition for fonts with a pattern, and checks it | [The partition check](fonts.md#the-partition-check) |
| `-DBN_DRAWING_VIEWER=ON`, and `-DBN_DRAWING_TOUR=ON` to go round by itself | Shows the drawings and pictures on the card | [Drawings and pictures on the panel](drawing-viewer.md) |
| `-DBN_TEXT_VIEWER=ON`, and `-DBN_TEXT_TOUR=ON` to go through it by itself | Shows the text on the card in the faces of the fonts partition's image | [Trying faces on the panel](text-viewer.md) |
| `-DBN_CRASH_TEST=fault`, `abort`, `oom`, `stack` or `hang` | Crashes on purpose, to show a crash report | [Diagnostics](diagnostics.md#crash-reports) |
