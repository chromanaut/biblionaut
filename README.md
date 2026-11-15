# Biblionaut

EPUB reader firmware for the Xteink X4 (ESP32-C3, 800x480 e-ink, SD card),
written so that everything except the hardware layer also builds and runs on
a desktop.

## Layout

| Directory | Contents |
|---|---|
| `core/` | Shared foundation: arenas, a heap within a fixed block of memory, the timing trace, counters and gauges, the framed diagnostic stream, checksums, the inflater, the heap-failure hook, and the interfaces the portable parts use to reach the outside world (clock, byte source, byte sink, display, buttons) |
| `hw/` | Hardware abstraction layer. `hw/include` is the interface. `hw/desktop` implements it for the host; on a device, `hw/esp32c3` holds what the chip provides and `hw/x4` what is specific to the X4 |
| `epub/` | Book parsing: zip, XML, OPF / TOC, CSS, the importer and the intermediate format |
| `draw/` | Fonts and drawing: glyphs, metrics, the display-list format, the band renderer, images |
| `layout/` | Book layout: line breaking, hyphenation, pagination, positions. Emits display lists |
| `reader/` | The reader application. The only part that knows about all the others |
| `app/` | Entry points: `app/desktop` for the host, and `app/firmware`, the ESP-IDF project that builds the one firmware image for all supported devices |
| `tools/` | Host programs (`tools/trace`: the `bn-trace` stream decoder; `tools/zip`: `bn-zip`, which reads a ZIP file as the firmware does; `tools/xml`: `bn-xml`, which does the same for XML; `tools/epub`: `bn-epub`, which says what a book says of itself; `tools/svg`: `bn-svg`, which reads the outlines of a drawing as the firmware does) and developer scripts (`bn-symbolize` for crash reports, `check-books.py` to run those tools over a folder of real books) |
| `third_party/` | How the code that is not ours is fetched and built: for each, a CMake file and its settings. The code itself is downloaded when a build is configured and is not in this repository (`THIRD_PARTY.md` lists it) |
| `tests/` | Host tests |
| `corpus/` | Real books for the tests to read: 28 EPUB files, in the public domain or openly licensed, which are under their own terms and not this project's licence (`corpus/README.md`) |

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

Needs CMake 3.25 or later, Ninja, a C++20 compiler and a C compiler. Two
things are downloaded when CMake configures, each checked against a hash:
expat, the XML parser, which is C and is why a C compiler is needed, and
GoogleTest. The tests also link zlib, to check the inflater against: macOS
has it, and on Linux it is the `zlib1g-dev` package. Seven of the end-to-end
tests are run only if Python 3 is found, and one of them makes more of its
test files if there is a `zip` command.

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
task 0  reader/press_to_refresh button=5  from the button going down to the refresh being started
interrupts  hw/refresh_fast               the panel's own time
task 0  reader/panel_sweep picture=3      the picture after, rendered and sent ahead of the next press
```

The picture a press asks for is usually in the panel already, written
there after the last refresh, so the press has only to start the refresh.

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

### The panel's two planes

The panel's controller keeps two images, and a fast refresh works from
both: the new image, and the old one it is to change from. On the X4 the
two change places by themselves after every fast refresh, and go back to
their first places when the panel's drive voltages are switched off.
`core/display.h` sets out the rules, which were found by reading the
planes back out of a device, and what they ask of whatever drives the
panel. The pattern viewer follows them: it writes the next picture ahead
of time, leaves the old image alone while the voltages are on, and writes
both again once they have been switched off. The simulated panel on the
desktop behaves the same way, so a mistake here shows in the tests.

A firmware built with `-DBN_PLANE_CHECK=ON` checks this on a device (on
the desktop, set `BN_PLANE_CHECK`). Whenever the panel comes to rest,
after a refresh and again after the voltages are switched off, both planes
are read back and the trace says what was found:

```
reader/new_plane_holds picture=3
reader/old_plane_holds picture=2
```

1 to 4 is that picture, 8 a blank plane and 9 anything else, in which case
a `new_plane_bands` or `old_plane_bands` line follows with a digit for each
band of 80 rows. The new-image plane should hold the picture after the one
on the panel, and the old-image plane the one on the panel. The metrics
count the readings and how many found otherwise:

```
plane_checks=14 planes_unexpected=0
```

Reading back takes about a second, a band at a time; a press during it is
acted on at once and the reading starts again when the panel is next at
rest. The first time anything is read, the controller's status byte and
display options are recorded too (`hw/panel_status`, `hw/panel_options`).

```
tools/idf -DBN_PLANE_CHECK=ON build flash
./build/host/tools/trace/bn-trace | tee planes.log
tools/idf -DBN_PLANE_CHECK=OFF build flash     # back to the normal firmware
```

## Storage

Files are reached through three things in `core`:

- `core/storage.h` is the bare interface a hardware port implements: open,
  read and write at an offset, sync, the handful of calls that work on
  names, and listing a directory. Every call says how it ended
  (`StorageStatus`), and every port accepts the same paths, which are the
  ones an SD card can hold.
- `core/block_file.h` is what code reads and writes files with. It keeps a
  position and a buffer of its own, asks storage for whole blocks at
  multiples of the block size, and counts what it asks for (`sd_reads`,
  `sd_read_bytes`, `sd_seeks`, `sd_writes`, `sd_write_bytes`, `sd_syncs`).
  It is also a `ByteSource`, so a parser can read a file without knowing it
  is one.
- `core/file_replacement.h` writes a file so that it is never found half
  written, whenever the power goes. An SD card cannot put one file in the
  place of another in one step, so it takes several, and
  `RecoverReplacement()` finishes or undoes whatever was cut short.

On a device the card is on the wires it shares with the panel, driven by
ESP-IDF's SD driver and FatFS, which is called directly
(`hw/esp32c3/sd_storage.cpp`). Only FAT file systems are understood, which
is what cards of up to 32 GB are sold with; a bigger card comes with exFAT
and has to be formatted as FAT32 first.

On the desktop the card is a folder (`hw/desktop/folder_storage.h`), the
one named by the `BN_STORAGE` environment variable; without it there is no
card. It is made to behave as a card does where that is cheap: the same
paths are refused, only four files can be open at once (a directory being
listed counts as one, and only two can be), an open file cannot be removed
or renamed, and a rename will not replace a file.

The application gets storage ready at start and says whether it is
(`storage_ok` in the metrics, `reader/storage_start` in the trace). It
does nothing else with it yet. On a device the trace also says what was
found, and why not if nothing usable was:

```
hw/sd_card megabytes=30436          the card's size
hw/sd_file_system kind=3            1: FAT12, 2: FAT16, 3: FAT32
hw/sd_start status=0  312.5 ms      0: ready, 1: no card, 8: a card that cannot be used
hw/sd_card_error code=263           the card did not answer
hw/sd_mount_error code=13           no file system FatFS knows: exFAT, most likely
```

A firmware built with `-DBN_STORAGE_CHECK=ON` puts the card through its
paces (on the desktop, set `BN_STORAGE_CHECK`): at start, and again at each
press of the Power button, which also looks for a card again if none was
found before. It writes a 256 KB file in 4 KB blocks, reads it back
checking every byte, reads short pieces from all over it, replaces a small
file twice, and removes what it made (`reader/storage_check.h`). It does a
little each turn, so the panel and the buttons carry on around it. Each
read, write and sync of the card is in the trace with the time it took
(`hw/sd_read`, `hw/sd_write`, `hw/sd_sync`), and the outcome is there and
in the metrics:

```
reader/storage_check passed=1
reader/storage_check_failed step_and_status=307     step 3 (writing), status 7 (no space)
storage_check=1[1..1]
```

```
tools/idf -DBN_STORAGE_CHECK=ON build flash
./build/host/tools/trace/bn-trace | tee card.log
tools/idf -DBN_STORAGE_CHECK=OFF build flash     # back to the normal firmware
```

It can be combined with `-DBN_PLANE_CHECK=ON`, to see whether using the
card disturbs what the panel holds.

## Decompression

`core/inflate.h` undoes DEFLATE (RFC 1951), the compression inside a ZIP
file and so inside an EPUB. `Inflater` is a `ByteSource` that reads another:
it draws compressed bytes from its source as it needs them and gives the
plain bytes out in pieces of whatever size is asked for, so a file is never
whole in memory in either form. It is our own code, the same on the desktop
and the device.

- **Memory**: the caller provides the 32 KB window the format needs. The
  object itself is 2.6 KB on the device. It allocates nothing.
- **Flash**: 4.2 KB, as compiled for the device.
- **Speed**: not yet tuned. On an X4, over 34 books, it gave out about
  1,530 KB a second with the ZIP checksum included (which is about a
  fifth of that time), taking in about 760 KB a second, where the SD
  card delivered about 960.
- **Bad input**: whatever bytes it is given, it either produces the data
  they stand for or stops, saying why (`kBadData`, `kTruncated`,
  `kSourceFailed`). It does not check that the data is the right data: that
  is for the checksum a ZIP file carries.
- **One call's work**: a stream cannot make one `Read()` long. DEFLATE
  lets a stream be written so that every byte of output costs a block of
  its own, with its own codes to read and build, or so that block after
  block holds nothing at all; a megabyte of either would keep a call that
  waited to fill its destination busy for as long as the megabyte lasted.
  So a call starts 17 blocks at the most and then comes back with what it
  has, which may be fewer bytes than it was asked for, and a stream with
  more than 16 blocks in a row that produce nothing is refused as bad
  data (a compressor writes such a block where it was told to flush:
  three together at the most). A call asked for `n` bytes therefore takes
  no more than `2n + 5,632` bytes from its source, whatever the stream
  (`Inflater::MostInputFor`).
- **Reading ahead**: it may take up to 260 bytes from its source that come
  after the end of the stream, so the source should end where the stream
  does.

Its tests build streams by hand for each rule of the format and each way of
breaking it, and compare it with zlib on streams zlib compresses: 800 of
them across kinds of data, sizes, compression levels and strategies, and
again with the input and output cut into pieces of many sizes. They also
turn over every bit of a stream, one at a time, and cut it short at every
length, to see that the inflater always stops. Every call in every one of
those tests is held to what one call may take from its source; and there
are streams made for that alone: a megabyte of blocks with nothing in
them, and blocks of one byte each whose codes are written the longest way
the format has, which bring a call to within a tenth of the bound.

## ZIP files

An EPUB is a ZIP file, and `epub/zip.h` reads one, going by the directory
at the end of the file:

- `ZipArchive` finds that directory and then finds entries in it, by name
  (`Find`) or one after another (`Next`). It is 24 bytes: it keeps no list
  of the entries, however many there are, and reads the directory from the
  file when asked. Each search starts where the last one ended, so asking
  for entries in the file's own order reads the directory once, not once
  for each.
- `ZipEntry` is what the directory says of one entry: 24 bytes that can be
  kept, in memory or in a file, to read the entry later without finding it
  again.
- `ZipEntryReader` gives an entry's bytes as a `ByteSource`, read from the
  file and inflated as they are asked for. It gives out no more than the
  directory says the entry holds, and at the end checks that the data
  stopped there and has the checksum the directory gives. It is 2.7 KB on
  the device and needs the inflater's 32 KB window.
- A name is the bytes the file has for it, and two names are the same if
  their bytes are. A ZIP file has a flag to say that its names are UTF-8,
  which nothing here looks at: a book's names are UTF-8 by the EPUB
  standard, and some books' ZIP files leave the flag off all the same (one
  Japanese book of some 500 tried does). A reader that goes by the flag
  cannot find such a book's chapters.

All three read through a `BlockFile`. Finding entries and reading them can
share one, but each step from an entry back to the directory then reads a
block again: reading every entry of a 3,000-entry file and looking each up
by name took 6,189 reads of storage that way and 242 with a `BlockFile`
each for the directory and the entries.

The file is not trusted: whatever it holds, every call ends with an answer.
What cannot be read is reported as such (`kUnsupported`): ZIP64, a file in
several parts, an encrypted entry, and compression other than DEFLATE.

`bn-zip` runs the same code on the desktop:

```
./build/host/tools/zip/bn-zip list book.epub
./build/host/tools/zip/bn-zip check book.epub
./build/host/tools/zip/bn-zip cat book.epub META-INF/container.xml
```

`list` prints a line for each entry. `check` reads every entry to its end,
looks each one up by name, and says how many reads of the file that took.
`cat` writes one entry out.

The unit tests make their ZIP files with a writer of their own, so that
they can get any one field wrong. Because a writer and a reader by the same
hand could share a mistake, an end-to-end test (`tests/e2e/zip_tool.py`)
also compares `bn-zip`, entry by entry and byte by byte, with Python's
`zipfile` on files made by `zipfile` and, where there is one, by the `zip`
command.

### Reading the books on a card

A firmware built with `-DBN_BOOK_CHECK=ON` reads every book on the SD card
from end to end: at start, and again at each press of the Power button. (On
the desktop, set `BN_BOOK_CHECK`, with `BN_STORAGE` naming the folder that
stands for the card.) It is how to find out whether a device reads books
correctly and how fast. It writes nothing to the card.

It takes for a book every file whose name ends in `.epub` in `/books` and
in each folder in `/books`, one level down and no further. If there is no
`/books`, or no book there, it looks in the top directory and its folders
instead (`reader/book_check.h`). With each book it does two things. First
it opens the book as a reader would: finds the ZIP directory and reads the
book's package, which says what the book is called and where its documents
are (see "What a book says of itself" below); and then, with the book
still open, reads its table of contents ("The table of contents" below). Then it closes the book, opens it afresh, reads every entry to its
end, which checks the entry's size and checksum, and looks each entry up
by name.
Every entry with an XML file's name (one ending in `.xhtml`, `.html`,
`.htm`, `.xml`, `.opf` or `.ncx`) is also parsed as it is read. It works
for about 20 ms a turn, so the panel and the buttons carry on around it,
and a run takes longer by the clock than the times it reports. All of it
is done in steps, each one call into what is being checked: a read from
an entry, a piece of a document parsed, a piece of a package or a table
of contents read. A turn runs over its 20 ms by however long its last
step takes, so the last line of a run says which step took longest, and
how long: on a device, that is whether those calls are as short as an
application that has a panel and buttons to see to needs them to be.

What it finds goes out as log text: a line when a book is begun, three for
its package, one for its table of contents, three when it ends, and seven
at the end for all of them.

```
book check: the checksum alone, 262144 bytes in 1783 us (143578 KB/s).
book 1: 1832425 bytes, /books/core/The First Book.epub
book 1: package: title "The First Book", author "A. Writer", language "en".
book 1: package: 374 files, 368 documents, cover jpeg, nav, ncx; memory 16288; checksum 7b162ac4.
book 1: package: 2 ms: card 0 ms for 24 reads of 90555 bytes.
book 1: contents: nav, 385 entries, 1 level; checksum 2eeff666; 1 ms: card 0 ms for 2 reads of 8192 bytes.
book 1: ok. 377 entries, 4353104 bytes out of 1769871. Directory found in 1 read, 0 ms.
book 1: card 1 ms for 468 reads of 1909179 bytes (1594895 KB/s); inflating 56 ms: 1661195 bytes in (28518 KB/s), 4244428 out (72865 KB/s); all 101 ms.
book 1: XML: 372 documents, 4240089 bytes, 93155 events, checksum a7e0cc8f; parsing 42 ms (97069 KB/s); most memory 11632.
...
book check: passed. 34 books, 5152 entries, 63899132 bytes out of 39526769.
book check: card 27 ms for 10091 reads of 41148789 bytes (1482380 KB/s); inflating 612 ms: 24130522 bytes in (38452 KB/s), 48502885 out (77290 KB/s); all 1053 ms.
book check: XML: 2072 documents, 31585733 bytes, 1059050 events; parsing 323 ms (95224 KB/s); 32821373 bytes of events checksummed.
book check: XML memory: the most a document used 18952 bytes; documents over 8 KB: 2072, over 12 KB: 24, over 16 KB: 5.
book check: packages: 33 read; 30 ms: card 17 ms for 258 reads of 901750 bytes; the longest 3 ms; the most memory 22448. Not read: 1.
book check: contents: 33 read (25 nav), 2666 entries; 9 ms: card 0 ms for 49 reads of 195690 bytes; the longest 1 ms.
book check: the longest step: 665 us, listing a directory.
```

(Those figures are a desktop's, and the book's names are made up.) There
are four times. `card` is the time
spent inside storage. `inflating` is the time spent reading out the
compressed entries with storage's share taken off, which is the processor
inflating and checksumming, beside what went into the inflater and what
came out. `parsing` is the time the XML parser took over what it was given,
with the reading and inflating of it taken off; the check's own counting
and checksumming of what the parser found is in that time, and the last
line but one says how many bytes that was. `all` is everything spent on the
book. A KB is 1024 bytes. The checksum is timed by itself first, over
256 KB held in memory, so that its share of `inflating` and of `parsing`
can be worked out. A book or an entry that cannot be read is reported with
`FAILED` and the reason, and the check goes on to the next. A file that
turns out not to be XML is counted (`Not XML: 2.`), and so is one that
wanted more than the 32 KB the parser is given (`Refused for memory: 1.`),
but neither makes the book a failure: the file is read out and checked all
the same.

The package's three lines stand apart from the rest. The first has what
the book calls itself, each name cut short with `...` if it is long. The
second has how many files its manifest lists and how many documents its
spine, whether it has a cover and each kind of table of contents, how much
of the 24 KB the package reader is given it used, and a checksum of
everything the package was found to hold (`epub/package_digest.h`);
then, if there are any, how many documents are not in the book or not in
the manifest, and whether the package file was cut short. The third has
what it cost to open the book and read that: the time, the card's share of
it, and the reads, finding the ZIP directory included. None of that is in
the book's other figures. A book whose package cannot be read has one line
that says why (`package: NOT READ: no container.`), and that does not make
the book a failure either.

The line for the table of contents has which kind was read (the nav, if
the book has one, or the NCX), how many entries it has and how many levels
they go to, a checksum of the entries (`epub/toc_digest.h`), and what
reading it cost, which is counted by itself as the package's is. It is
read a piece at a time, as the entries are. After that come how many
entries point into no document, if any do, and whether the file was cut
short. A book with no table of contents says `contents: none.`, and one
with a file that could not be read as one says why (`contents: NOT READ:
not a table of contents.`); neither makes the book a failure.

`bn-zip check` reads a file the same way, read for read. So the same book
on the desktop should come to the same entries, the same bytes and the same
reads (`sd_reads` and `sd_read_bytes`, in the last line `bn-zip check`
prints) as it does on the device. `bn-xml book` parses the same documents,
and its last line has the count and the checksum of everything found in
them, which should be the book's on the device too. `bn-epub info` opens
the book and reads its package the same way, and prints the checksum, the
memory and the reads that the package's lines should have. `bn-epub toc`
goes on to read the table of contents, and prints its kind, its entries
and their checksum; the reads it reports are the package's and the
table's together.

The outcome is also in the metrics (`book_check=1` if every book was sound,
`book_check_books`, `inflate_kb_per_s`, `checksum_kb_per_s`,
`xml_kb_per_s`, and `xml_memory` for the most the parser has used) and in
the trace (`reader/book_check`, and `reader/book_check_book` for each
book), where every read of the card is too (`hw/sd_read`), and the reading
of each package with its four parts (`epub/package_read`, and
`epub/package_container`, `package_file`, `package_resolve` and
`package_find_files`).

```
tools/idf -DBN_BOOK_CHECK=ON build flash
./build/host/tools/trace/bn-trace | tee books.log
tools/idf -DBN_BOOK_CHECK=OFF build flash     # back to the normal firmware
```

It cannot be combined with `-DBN_STORAGE_CHECK=ON`: the two would get in
each other's way on the card. A firmware built with neither has the code
of neither in it.

## XML

`epub/xml.h` reads XML: the files an EPUB describes itself with, and its
chapters. The parsing is done by [expat](https://libexpat.github.io);
`XmlParser` is what the rest of the code sees of it.

- A document is read from a `ByteSource`, 1 KB at each `Step()`, and never
  held whole, so a chapter can be parsed as it comes out of the ZIP file
  and the inflater. What is found is told to an `XmlHandler` as it is
  found: an element beginning, with its attributes; an element ending;
  text; an entity that was not expanded.
- **Memory**: the parser is given a block of memory when it is made, and
  everything expat needs comes out of that, through a small heap of our
  own that lives inside the block (`core/region_heap.h`). Nothing comes
  from the general heap. That heap checks every piece it is handed back
  by going along its own records to it, so a mistake of the library's
  with a pointer (one given back twice, or kept from an earlier
  document, or pointing into the middle of something) is turned away
  and counted, and cannot make the heap give out memory outside the
  block. A check goes past 17 pieces on average, and the parser asks
  for one about once in every 12 to 25 things it reports, which on the
  desktop is under 1% of its time. A document that needs more than the
  block is
  refused (`kNoMemory`), which in practice means one with a single tag
  or comment several kilobytes long, since a whole one has to be held at
  once, or with elements nested hundreds deep. Text can be any length. On
  an X4, of the 2,072 documents of 34 books chosen to differ from one
  another, all but 8 used less than 12 KB. The most was 17.1 KB, for a
  document that begins with a comment of 5.5 KB.
- **Speed**: on an X4, over the same documents, it took in about 960 KB a
  second with every event counted and checksummed. Book by book that was
  from 640, for one thick with markup, to 1,890, for Chinese prose with
  few tags, and 940 at the median.
- **Bad input**: the document is not trusted. Whatever bytes it holds,
  parsing ends with an answer, and one that is not XML is refused with
  expat's error number and the line it is on. What was found before the
  mistake has been told by then.
- **Entities are not expanded.** The five every XML document has (`&amp;`
  and the rest) and numbered characters (`&#160;`) come out as text. An
  entity of HTML's, such as `&nbsp;`, is told to the handler by name if
  the document names a DTD, as XHTML with a `PUBLIC` doctype does; in a
  document that names none it is a mistake by XML's rules, and parsing
  stops there. An entity a document declares for itself comes out as the
  reference was written. So there is nothing a document can make expand
  without end.
- **Names** are given as written, prefix and all (`dc:title`). Namespaces
  are not worked out, which also means a book that uses a prefix it never
  declared can still be read.
- **Encodings**: UTF-8, UTF-16, ISO-8859-1 and US-ASCII; everything is
  given out as UTF-8. Any other is refused (`kUnsupportedEncoding`).

expat is not in this repository: CMake downloads the release named in
`third_party/expat/CMakeLists.txt`, checks its hash, and compiles it with
the settings in `third_party/expat/expat_config.h`, the same for the
desktop and the device. The settings leave out what an EPUB does not need:
entity expansion, parameter entities and external DTDs, and namespace
processing. It is 71 KB of flash once something in the firmware uses it.

`bn-xml` runs the same code on the desktop:

```
./build/host/tools/xml/bn-xml events chapter.xhtml
./build/host/tools/xml/bn-xml book book.epub
./build/host/tools/xml/bn-xml book book.epub --memory 16384
```

`events` prints what the parser finds in one file, an event a line. `book`
parses every XML file in an EPUB as it is read out of the ZIP file, and
prints for each how it ended, a checksum of its events, and the most
memory the parser used; and at the end the count and the checksum of all
of them run together (`epub/xml_digest.h`), which is what the firmware's
book check reports for a book. `--memory` holds the parser to that many bytes, as
a device would.

The unit tests say what the parser should make of documents written for
them, and damage one in every way they can think of. An end-to-end test
(`tests/e2e/xml_tool.py`) compares `bn-xml` with the expat that Python
carries, which is another build and another version driven by other code:
every event of every document has to agree.

## What a book says of itself

`epub/package.h` reads the two files an EPUB describes itself with:
`META-INF/container.xml`, which says where the package file is, and the
package file (the OPF), which lists every file of the book and the order
its documents are read in. It is read a piece at a time, `Package::Begin`
and then `Package::Step` until there is no more (`Package::Read` is both,
for a tool or a test), and gives back:

- the title, the author and the language: the first of each that has
  anything in it, with its spaces tidied;
- the spine, which is the documents in reading order, each with the ZIP
  entry it is in, its kind, and whether the book counts it as part of the
  run of the text (`linear="no"` marks one that is not);
- the entry of the cover picture, of the EPUB 3 table of contents (the
  nav document) and of the EPUB 2 one (the NCX), where the book has them.
  The tables of contents are found here and read by `TocReader` (below).

How it does that:

- **The manifest is kept as numbers, not as text.** A book lists its files
  twice over: the package names each by an id and a path, and the ZIP file
  has an entry for each path. For every file the package lists, 16 bytes
  are kept: a number of 48 bits worked out from its id, another from its
  path (`NameHash`, in `epub/book_path.h`), its kind, and a few notes. A
  document of the spine is 28 bytes. So a book of 800 files and 400
  documents is read in 24 KB, whatever the length of its names.
- **The ZIP directory is gone through once.** When the package has been
  read, every entry of the ZIP file is taken in the file's own order, its
  name turned into the same kind of number, and that looked for among
  the files that are wanted: the documents of the spine, the cover and the
  tables of contents. It stops when it has them all. Looking each file up
  by name instead would read the directory again for every one, since a
  ZIP file's order is seldom the package's.
- **A piece at a time.** How long the whole reading takes is the book's
  to decide, and a book can be made to make it long: a package file of
  megabytes, a ZIP file of tens of thousands of entries, one listed file
  that is not there. How long a step takes is not the book's to decide. A
  step looks at 16 entries of the ZIP directory, or as many as lie in
  4 KB of it; or parses 1 KB of a file; or makes about 16,000
  comparisons among the files of the manifest. With files read 4 KB at a
  time, that is five reads of storage at the most, and usually one or
  none. `container.xml` and the package file are found by going through
  the directory in such steps too, the second from where the first was,
  and not by the ZIP reader's `Find`, which goes on until it has an
  answer. Until the last step the book's documents, cover and tables of
  contents are not to be had: the spine is empty.
- **Paths** in the package are worked out the way a link is
  (`ResolveBookPath`): from the folder the package file is in, with `..`
  and `.` followed (never to above the top of the book), `%20` and the
  like turned back into bytes, and anything after a `#` dropped. A
  reference to somewhere else, such as an address on the internet, is not
  a file of the book and is left out.
- **Memory**: the caller gives a block, and the records are made in it,
  files from one end and documents from the other. Nothing comes from the
  general heap. A book that needs more than the block is refused
  (`kNoMemory`). The parser's memory is the caller's as well
  (`XmlParser`).
- **Speed**: on an X4, opening a book and reading its package took 26 to
  301 ms for each of 33 books, 43 ms at the median; the largest lists 745
  files, and used 22.4 KB of the 24 KB it was given. Nearly half of the
  time is the card, at about 5 ms a read, and most of the rest is parsing
  the package file. Matching the spine to the manifest is a few
  milliseconds at the most. A thirty-fourth book, which lists 2,384
  files, was refused for want of memory after 0.5 s. None of that is one
  long call: over those 34 books the longest single step of the whole
  book check was 24.6 ms, and it was opening a file, which is the card's
  file system finding a name in its folder. Read in one go, as it used to
  be, the same packages took 8% less time in all and up to 463 ms in one
  call.
- **Two names with one number.** Nothing checks a name against the text
  it came from, so two different names in one book that came to the same
  number would be taken for each other. With 48 bits, that is one book in
  some 500 million for a book of a thousand files. `ReadZipEntryName`
  reads an entry's name back out of the ZIP file, for whoever needs to be
  sure.
- **What is put up with**, because real books do it: metadata written
  with capitals or with any prefix (`dc:Title`); an id in the spine that
  the manifest has not got (left out, and counted); a file the package
  lists that is not in the ZIP file (kept, and marked as not there); a
  cover named by EPUB 2's `<meta name="cover">` or by EPUB 3's
  `properties="cover-image"`, the second being believed first; a
  container that names its package with `%20` in the path; a package
  that stops being XML after its spine (`CutShort()`).
- **Bad input**: neither file is trusted. Whatever bytes they hold,
  reading ends with an answer: `kNoContainer`, `kNoPackage`,
  `kNotPackage` for one with no documents in its spine, `kUnreadable` for
  a ZIP file damaged where it had to be read, or `kNoMemory`. After any
  of those the spine is empty. Its tests read every book in steps and
  hold each step to five reads of storage: the books made to be awkward
  (five thousand entries, a megabyte of package file, a directory whose
  records lie 40 KB apart) and the damaged ones alike.

`bn-epub` runs the same code on the desktop:

```
./build/host/tools/epub/bn-epub info book.epub
./build/host/tools/epub/bn-epub info book.epub --memory 4096
```

For a small book with some things wrong with it, it prints:

```
title: Untidy
author: -
language: -
package: deep/er/package.opf
manifest: 5
cover: -
nav: -
ncx: deep/er/toc.ncx
spine: 4
+ xhtml deep/er/cover.xhtml
+ xhtml ?
+ xhtml deep/er/one.xhtml
- xhtml deep/er/cover.xhtml
unknown: 2
checksum: cd21187e
memory: 248
cost: packages_read=1 packages_failed=0 xml_documents=2 xml_failures=0 zip_finds=0 zip_records=16 sd_reads=2 sd_read_bytes=2504 sd_writes=0 sd_write_bytes=0 sd_syncs=0 sd_seeks=0 xml_memory=9536[8752..9536]
```

A `-` for the author, the cover or any of the first lines means the book has
none. Each line of the spine is `+` for a document in the run of the text or
`-` for one set aside, then its kind, then the name of its entry in the ZIP
file, or `?` where the ZIP file has no such file. `unknown` (printed only
when there are any) counts the documents the spine named that the manifest
has not got. `checksum` is of everything above it (`epub/package_digest.h`),
with each file given by where its entry is and not by its name: it is what a
device's book check reports for the same book, so the two can be compared.
`memory` is how many bytes of the block were used, and `cost` is the
counters after the reading: here 16 records of the directory and two reads
of the file. (`zip_finds` counts searches by the ZIP reader's `Find`, which
reading a package does not use: it looks for its two files itself, a piece
at a time.)
`--memory` holds it to a block of that many bytes, as a device would; it
exits with 1, and says why, for a file it cannot read as a book.

The unit tests say what should be made of books written for them, in
either version of EPUB and with their files in any order in the ZIP file,
and of every bit of a package file turned over and every length it can be
cut to. An end-to-end test (`tests/e2e/epub_tool.py`) makes books and
compares `bn-epub` with a reading of them by Python's own XML parser and
ZIP reader, written separately for the test: title, author, cover,
tables of contents and every document of the spine have to agree.

## The table of contents

`epub/toc.h` reads a book's table of contents: its parts by name, each with
the place in the book where the part begins. The package reader finds the
file; `TocReader` reads it.

- **Two kinds.** EPUB 3 has a page of XHTML with a list of links in it (the
  "nav"), and EPUB 2 an XML file made for the purpose (the "NCX"). Many
  books have both, and the two are often not the same: of 138 real books
  with both, 53 had differences between them. A book with both is read from
  its nav unless the NCX is asked for.
- **Entry by entry.** Each entry is given to a `TocHandler` as it is found,
  an entry before the entries under it: what it is called, how far in it
  is, the document it points into, and the place within that document. None
  of it is kept, so a table of contents of any length costs the same
  memory: the reader, which is about 1 KB, and what the XML parser needs.
- **What an entry is.** In an NCX: each `navPoint` in the `navMap`, by the
  text of its label and the `src` of its content, nested as the points
  are. In a nav: within the `nav` element whose `epub:type` is `toc`, each
  link in a list, and each `span` that stands where a link would as a
  heading with no link, nested as the lists are. Page lists, landmarks and
  whatever else the two files hold are passed over.
- **Where an entry points.** Its link is worked out as any link in a book
  is, from the folder the table of contents is in, and looked up in the
  package (`Package::SpineIndexOf`): the answer is the document's place in
  the spine, with what the link has after its `#` as the place within the
  document. An entry that points at nothing, or at something that is no
  document of the spine, has no document.
- **Labels** have every run of spaces and line ends made one space, and the
  text of whatever is inside a link (italics, a line break) is part of
  the label. One longer than 256 bytes is cut at a character and marked.
- **In pieces.** `Begin()` and then `Step()`, which reads 1 KB of the file
  each time, so that a long table of contents can be read a little a turn;
  or `Read()` for all of it at once.
- **Speed**: on an X4, reading the table of contents took 4 to 159 ms for
  each of 33 books, 18 ms at the median, for tables of 1 to 385 entries:
  about 0.5 ms an entry over all of them. Under a fifth of it is the
  card, which gave most tables in one read or two; the rest, inflating
  and parsing the file and placing each entry, went at about 450 KB of
  the file a second. Counted from opening the book, with its package read
  first, the contents were in hand after 36 to 433 ms, 64 ms at the
  median.
- **Bad input**: the file is not trusted. Whatever bytes it holds, reading
  ends with an answer: `kNoToc` for a book with none, `kNotToc` for a file
  with no table of contents in it, `kNoMemory`, `kUnreadable`. A file that
  stops being XML part way gives the entries that came before the mistake,
  and `CutShort()` says that it did.

`bn-epub` prints one:

```
./build/host/tools/epub/bn-epub toc book.epub
./build/host/tools/epub/bn-epub toc book.epub --ncx
```

For a small book with an NCX, in two parts:

```
toc: ncx
0	0	-	Part 1
1	0	s0	Chapter 0: The Tale & Its Telling
1	1	-	Chapter 1: The Tale & Its Telling
1	2	-	Chapter 2: The Tale & Its Telling
1	3	s3	Chapter 3: The Tale & Its Telling
1	4	-	Chapter 4: The Tale & Its Telling
1	5	-	Chapter 5: The Tale & Its Telling
1	6	s6	Chapter 6: The Tale & Its Telling
1	7	-	Chapter 7: The Tale & Its Telling
1	8	-	Chapter 8: The Tale & Its Telling
1	9	s9	Chapter 9: The Tale & Its Telling
0	10	-	Part 2
1	10	-	Chapter 10: The Tale & Its Telling
1	11	-	Chapter 11: The Tale & Its Telling
entries: 14
checksum: 5d9afae8
cost: packages_read=1 packages_failed=0 xml_documents=3 xml_failures=0 zip_finds=0 zip_records=21 sd_reads=2 sd_read_bytes=7338 sd_writes=0 sd_write_bytes=0 sd_syncs=0 sd_seeks=0 xml_memory=10144[8752..10144]
```

The first line says which kind was read. Then a line an entry, with tabs
between: how far in it is, from 0; the document it points into, by its
place in the spine from 0, or `-` for none; the place within the document,
or `-` for its start; and what it is called. Then how many there were, a
checksum of them (`epub/toc_digest.h`), which is what a device's book
check reports for the same book, and the counters after reading the
package and the table. `--nav` and `--ncx`
ask for that kind; it exits with 1, and says why, if there is none to read.

The unit tests say what should be made of tables of contents written for
them, of both kinds, and of every bit of one turned over and every length
it can be cut to. The end-to-end test (`tests/e2e/epub_tool.py`) makes
books whose two tables differ, and compares `bn-epub toc` with a reading of
each by Python's own XML parser, written separately for the test: every
entry has to agree.

## Real books

The tests make most of the books they read, so that a test can say exactly
what should be found in one. A book made for a test has only what its
writer thought of. `corpus/` holds 28 that nobody here made: 16 from
Project Gutenberg, 7 from Standard Ebooks, and 5 of the International
Digital Publishing Forum's EPUB 3 samples. Between them: eleven languages
in six kinds of writing (Latin, Greek, Cyrillic, Chinese, Arabic and
Hebrew, the last two read from right to left); EPUB 2 and EPUB 3; tables
of contents of both kinds; verse, plays and pictures; a book with 1,647
notes and one with 10,281 formulas in MathML; books with their own
fonts; documents out of the reading order; and a fixed layout whose
pages are SVG files. `corpus/README.md` lists them, with where each came
from and the terms it is passed on under, which are not this project's
licence. They are kept byte for byte as they were fetched.

`tools/check-books.py` runs `bn-zip`, `bn-xml` and `bn-epub` over every
EPUB under a folder, and compares what each says of a book with a reading
of the same book by Python:

```
tools/check-books.py corpus
tools/check-books.py --tools build/host ~/books
```

```
28 books: 1228 entries in their ZIP files, 945 XML documents with 1899986 events, 840 documents in their spines, 79 readings of tables of contents with 3914 entries.
28 files listed in SHA256SUMS: all there and unchanged.
The tools and Python agree about every book.
```

- **The ZIP file**: every entry `bn-zip list` gives is the entry Python's
  `zipfile` finds; `bn-zip check` inflates every entry and finds its
  checksum right; and a few entries are read out and compared byte for
  byte.
- **The XML**: every document `bn-xml book` parses has the events, in
  number and by checksum, that the expat in Python finds in it, and one
  that either takes for XML the other must too.
- **The package**: every line of `bn-epub info` is the line a reading of
  the package file by ElementTree comes to, the checksum among them.
- **The table of contents**: every entry of `bn-epub toc`, for the table
  the book is read by and for each kind it has, is the entry a reading of
  that file by ElementTree comes to, and so is their checksum.

Where they differ it prints a line for each difference, starting
`DIFFERENT`, and ends with 1. It takes the tools from `build/host-release`
if they are built there, or else from `build/host`; `--tools` names another
build, and `--zip`, `--xml` and `--epub` one tool each.

A folder with a file named `SHA256SUMS` in it is checked against that file
as well: every file listed has to be there with that SHA-256, and every
EPUB in the folder has to be listed. That is how a book in `corpus/` that
has been changed, or added without its line, is found.

CTest runs it over `corpus/` as `e2e.Corpus`. It is as useful over any
folder of books on the computer at hand: a few hundred take some seconds
with the `host-release` tools. The check itself was tried against 17
faults made on purpose, in the books and in what the tools print, and
reported each one.

Its readings by Python have to be as careful as the tools are. The first
book it met whose names are UTF-8 without the ZIP file saying so was read
rightly by the tools and wrongly by Python's `zipfile`, which takes such
names for another encoding; the readings now take a name's bytes as UTF-8
whatever the flag says, and `e2e.EpubTool` makes a book of that kind.

## Breaking paragraphs into lines

`layout/line_breaker.h` chooses where the lines of a paragraph end. It is
the first piece of `layout`, and it knows nothing of text, fonts or pixels.
A paragraph is given to it as a run of items, each with a width in whatever
unit the caller measures in: boxes, which are never broken; spaces, where a
line may end; breaks, which are places inside a word where a line may end
at a price, showing a hyphen if it does; and forced breaks. It gives back
the lines, each as a range of those items.

- **Lines are chosen together.** Filling each line as full as it will go
  leaves some lines tight and others with a great gap. `LineBreaker` makes
  the sum of the squares of the space left on each line as small as it can
  be, so a word is taken down early where that evens out the lines after
  it. The last line of a paragraph is allowed to be short.
- **The first lines can have widths of their own**, up to eight of them,
  and the rest share one. That is how a first line is indented, and how
  room is left beside a large first letter that stands several lines high.
- **Memory**: the caller provides an array of nodes, 32 bytes each, one for
  each place a line could end that is still in play. It allocates nothing.
  256 nodes (8 KB) hold a paragraph of about 250 such places whole.
- **Flash**: 3.5 KB, as compiled for the device.
- **A paragraph longer than its memory.** How even the lines can be made
  depends on the whole paragraph: one more word at the end can shift a
  little of every line before it. So nothing is given out before the end
  unless something makes it. When the nodes run out, the lines of the
  earlier half of them are settled as they would be if the paragraph ended
  there, and the rest is worked out again from that point. A paragraph of
  any length is broken in the same memory, and the lines come out no
  further behind the items than the nodes allow. A caller with room for
  only so many items can ask for lines sooner (`Settle()`).
- **What that costs.** A paragraph that fits its nodes is broken the
  cheapest way there is. For one that does not, the lines settled early
  cannot always be the ones the whole paragraph would have chosen. Over 30
  made-up paragraphs of 1,000 to 4,000 words with 256 nodes they cost 2%
  more than the cheapest breaking on average, and 10% at worst.
- **What will not fit.** A stretch of boxes with nowhere to break that is
  wider than its line goes on a line of its own, which says that it is too
  wide. Nothing else is ever wider than its line.
- **Not done**: spaces are not squeezed to get more on a line, two
  hyphenated lines in a row cost no more than two apart, and a last line of
  one short word is not avoided.

Its tests break paragraphs written out by hand for each rule, and compare
it with two other ways of finding the cheapest breaking, written for the
tests. One tries every way there is, on 600 made-up paragraphs short enough
for that. The other works forwards through the whole paragraph, keeping
everything, on paragraphs of up to 60,000 words. With the fewest nodes it
accepts, and with a caller that asks for lines early, the lines still have
to fit and to hold every item once.

## Outlines of drawings

A book can hold a drawing as SVG, and the first step of drawing one is
done: `draw/` reads the outline of a shape, which SVG writes as path data
(the `d` of a `<path>`), and turns it into straight lines in pixels.
Nothing fills an outline yet.

The device's processor has no floating point, so all of it is done in whole
numbers (`draw/fixed.h`). A position counts 4096ths of a unit of the
drawing or of a pixel, a factor of scale 1,048,576ths, a sine 2^30ths, and
an angle a part of a turn. Sines, cosines and the angle of a point are
found by shifts and additions. Nothing can overflow: a number too large for
its kind is held at the largest that fits, and whoever asked is told.

- `SvgPathReader` (`draw/svg_path.h`) reads path data and tells a
  `PathSink` what it draws. The data is given in pieces of any size, cut
  anywhere, so a path never has to be whole in memory: one in a book is
  over 3 MB. What the sink hears is simpler than what was written. Every
  distance has become a place, a command followed by several sets of
  numbers has become several commands, and the short forms of a curve have
  been given the control point they leave out.
- `ParseSvgTransform` (`draw/svg_transform.h`) reads a `transform`: a list
  of `matrix`, `translate`, `scale`, `rotate`, `skewX` and `skewY`, as one
  matrix.
- `PathFlattener` (`draw/flatten.h`) is a `PathSink` that puts each point
  through a matrix and cuts each curve into lines, as many as it takes for
  none to stand further from the curve than a tolerance, which is a
  quarter of a pixel unless it is told otherwise. It gives each line to a
  `LineSink` as it is made and keeps none.
- **Arcs** are drawn as SVG defines them, whatever the matrix does to
  them: radii too short to reach from one end to the other are made just
  long enough, and with no radius an arc is a line. One so shallow that it
  stays within the tolerance of a straight line is drawn as that line,
  which is what makes a radius of any size harmless: two public books
  have arcs whose radius is seventeen digits long.
- **Memory**: the reader is 168 bytes and the flattener 88, on the device,
  and cutting a curve takes about half a kilobyte of stack. Nothing is
  allocated, and nothing grows with the path.
- **Flash**: 17 KB, as compiled for the device.
- **Speed**: not yet measured on a device.
- **Bad input**: the data is not trusted. Whatever bytes it holds, reading
  ends with a status. Where path data goes wrong, everything before the
  command with the mistake in it has been drawn and nothing after it is,
  which is what SVG asks for; `ErrorOffset()` says where.
- **What a call can cost**: the data cannot make a call long. The reader
  looks at each byte once, and its `Step()` comes back once it has told the
  sink as many commands as it was asked for, or one more. One command is
  one line, or a curve of 1,024 lines at the most, or an arc of 32,768,
  whatever its numbers. So whoever reads a path a piece at a time decides
  how much one piece may draw.
- **Limits**: a position is held to a 4096th and to 524,287 either side of
  nothing, and a factor of scale to 2,047.
- **How exact**: a line is within the tolerance of its curve. A matrix's
  factors are exact to a part in a million, so the far edge of a drawing
  32,000 units wide lands within a fiftieth of a pixel. Where a path is
  written as distances from one point to the next, they are added up far
  more finely than a position is held, so thousands of small steps still
  end where they should.

`bn-svg` runs the same code on the desktop. For path data in a file, it
prints each command with its numbers as places, and under each the lines it
became, in pixels:

```
./build/host/tools/svg/bn-svg path outline.d --transform 'scale(2)'
```

```
M 10.0000 20.0000
 m 20.0000 40.0000
L 40.0000 20.0000
 l 80.0000 40.0000
A 15.0000 15.0000 0.0000 0 1 10.0000 20.0000
 l 79.4180 45.8796
 l 77.7161 51.4802
 l 74.9592 56.6436
 l 71.2129 61.2129
 l 66.6436 64.9592
 l 61.4800 67.7161
 l 55.8792 69.4177
 l 49.9990 69.9995
 l 44.1191 69.4175
 l 38.5188 67.7156
 l 33.3552 64.9587
 l 28.7861 61.2124
 l 25.0396 56.6431
 l 22.2830 51.4797
 l 20.5811 45.8794
 l 20.0000 40.0000
Z
 z
4 commands, 17 lines, 27 bytes
```

That is `M10 20h30a15 15 0 0 1-30 0z`, a line and half a circle. `--tolerance`
sets how far a line may stand from its curve, `--pieces` gives the reader
the data a few bytes at a time, and `--count` prints only the last line.

The unit tests set the whole-number working beside floating point: sines
and angles against the C library's, and the lines drawn for curves and for
arcs of every shape against the curve itself, worked out from the formula
that defines it. An end-to-end test (`tests/e2e/svg_tool.py`) sets `bn-svg`
beside a second reading of path data that shares none of its code, written
in Python with regular expressions: over paths made up at random in every
way SVG lets one be written, the commands have to be the same and the lines
within the tolerance of the true curves.

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

The firmware is compiled for speed (`-O2`), with ESP-IDF's assertions left
on: `app/firmware/sdkconfig.defaults` sets both. Every figure in this
document that was measured on a device is from such a build, and the
level matters. On an X4 reading the same 34 books, a firmware at ESP-IDF's
own default (`-Og`, for debugging) inflated at two thirds of the speed
and parsed XML an eighth slower; one compiled for size (`-Os`) was hardly
faster than that, and took three and a half times as long to draw a test
pattern. The `-O2` firmware is 22 KB larger than the first and 74 KB
larger than the second, of the megabyte it has.

ESP-IDF keeps the settings a build was first made with in
`app/firmware/sdkconfig`, which it writes itself. After a change to
`sdkconfig.defaults`, delete that file and build again, or the change is
not taken up.

Flashing replaces whatever firmware is on the device, including its
bootloader and partition table.

## Crash reports

When the firmware stops unexpectedly (a processor fault, `abort()`, a failed
assertion, running out of memory, a stack overflow, the watchdog), ESP-IDF
prints what it knows on the console and restarts the device. Before that happens the
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

The watchdog is what turns a hang into a crash. The application is one
loop, and each turn of it reports to ESP-IDF's task watchdog. If no turn
does for five seconds, because something a turn called has not come back,
the firmware stops as it does for any other fatal error, and the report
says `watchdog: a watched task did not report in time`, with the
registers and the stack of whatever was running: the call that did not
come back. (The last four registers of such a report, `mstatus` to
`mtval`, are zero: they are of the machine's state at a fault, and
nothing saves them when the watchdog breaks in.) (Left as ESP-IDF sets it up, the watchdog watches only the idle
task, and prints a warning on the console every five seconds while the
device stays hung.) The settings are in `app/firmware/sdkconfig.defaults`.

To see a report without waiting for a bug, build a firmware that crashes on
purpose ten seconds after it starts:

```
tools/idf -DBN_CRASH_TEST=fault build flash     # or abort, oom, stack, hang
./build/host/tools/trace/bn-trace | tools/bn-symbolize
tools/idf -DBN_CRASH_TEST= build flash          # back to the normal firmware
```

`hang` does not crash by itself: its loop stops coming round, and the
watchdog ends it five seconds later. None of them crashes again on the
restart that follows a crash. On the desktop,
`BN_FAKE_CRASH=1` makes the application start with an invented report, which
exercises everything after the crash itself.

## Checks

CI runs all of these on every push to `main` and every pull request
(`.github/workflows/ci.yml`). To run them locally:

| Check | Command |
|---|---|
| Unit tests, with AddressSanitizer and UndefinedBehaviorSanitizer | `cmake --build --preset host && ctest --preset host` |
| The same tests in the optimised build, without sanitizers | `cmake --build --preset host-release && ctest --preset host-release` |
| Both as CI's Linux job runs them: GCC 14 on Ubuntu, in Docker | `tools/linux-check.sh` (arguments go to `ctest`) |
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

On an arm64 computer, such as a Mac with Apple silicon, the Linux run leaves
out one of AddressSanitizer's checks: for use of a function's stack after
it has returned. GCC's runtime for arm64 Linux is slow enough at it to turn
seconds of tests into minutes (`tests/CMakeLists.txt` has the figures). CI's
Linux job runs on x86-64, where it is not slow, and makes that check.

## Licence

MIT; see `LICENSE`. Third-party components are listed in `THIRD_PARTY.md`.
The books in `corpus/` are not part of that: each is under the terms it
came with, which `corpus/README.md` gives.
