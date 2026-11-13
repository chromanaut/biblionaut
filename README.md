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
| `tools/` | Host programs (`tools/trace`: the `bn-trace` stream decoder; `tools/zip`: `bn-zip`, which reads a ZIP file as the firmware does; `tools/xml`: `bn-xml`, which does the same for XML; `tools/epub`: `bn-epub`, which says what a book says of itself) and developer scripts (`bn-symbolize` for crash reports) |
| `third_party/` | How the code that is not ours is fetched and built: for each, a CMake file and its settings. The code itself is downloaded when a build is configured and is not in this repository (`THIRD_PARTY.md` lists it) |
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

Needs CMake 3.25 or later, Ninja, a C++20 compiler and a C compiler. Two
things are downloaded when CMake configures, each checked against a hash:
expat, the XML parser, which is C and is why a C compiler is needed, and
GoogleTest. The tests also link zlib, to check the inflater against: macOS
has it, and on Linux it is the `zlib1g-dev` package. Five of the end-to-end
tests are run only if Python 3 is found, and one of them makes more of its
test files if there is a `zip` command.

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
- **Flash**: 3.7 KB, as compiled for the device.
- **Speed**: not yet tuned. On an X4, over 30 books, it gave out about
  970 KB a second with the ZIP checksum included (which is about a
  seventh of that time), taking in about 620 KB a second, where the SD
  card delivered about 880.
- **Bad input**: whatever bytes it is given, it either produces the data
  they stand for or stops, saying why (`kBadData`, `kTruncated`,
  `kSourceFailed`). It does not check that the data is the right data: that
  is for the checksum a ZIP file carries.
- **Reading ahead**: it may take up to 260 bytes from its source that come
  after the end of the stream, so the source should end where the stream
  does.

Its tests build streams by hand for each rule of the format and each way of
breaking it, and compare it with zlib on streams zlib compresses: 800 of
them across kinds of data, sizes, compression levels and strategies, and
again with the input and output cut into pieces of many sizes. They also
turn over every bit of a stream, one at a time, and cut it short at every
length, to see that the inflater always stops.

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
are (see "What a book says of itself" below), all in one go. Then it
closes the book, opens it afresh, reads every entry to its end, which
checks the entry's size and checksum, and looks each entry up by name.
Every entry with an XML file's name (one ending in `.xhtml`, `.html`,
`.htm`, `.xml`, `.opf` or `.ncx`) is also parsed as it is read. But for the
package, it works for about 20 ms a turn, so the panel and the buttons
carry on around it, and a run takes longer by the clock than the times it
reports.

What it finds goes out as log text: a line when a book is begun, three for
its package, three when it ends, and five at the end for all of them.

```
book check: the checksum alone, 262144 bytes in 1354 us (189069 KB/s).
book 1: 1498334 bytes, /books/A Series/1 The First Book.epub
book 1: package: title "The First Book", author "A. Writer", language "eng".
book 1: package: 119 files, 105 documents, cover jpeg, nav, ncx; memory 4844; checksum 3ee7ef89.
book 1: package: 1 ms: card 0 ms for 9 reads of 35260 bytes.
book 1: ok. 122 entries, 3022700 bytes out of 1477560. Directory found in 1 read, 0 ms.
book 1: card 1 ms for 372 reads of 1521306 bytes (1394976 KB/s); inflating 35 ms: 1477540 bytes in (40440 KB/s), 3022680 out (82730 KB/s); all 56 ms.
book 1: XML: 109 documents, 2042449 bytes, 78209 events, checksum 10acf92b; parsing 19 ms (102244 KB/s); most memory 10928.
...
book check: passed. 31 books, 4310 entries, 181650944 bytes out of 85181353.
book check: card 45 ms for 21188 reads of 86632380 bytes (1867015 KB/s); inflating 1829 ms: 75728235 bytes in (40421 KB/s), 172197826 out (91913 KB/s); all 2467 ms.
book check: XML: 2587 documents, 66166529 bytes, 1823978 events; parsing 543 ms (118946 KB/s); 66286144 bytes of events checksummed.
book check: XML memory: the most a document used 11648 bytes; documents over 8 KB: 2587, over 12 KB: 0, over 16 KB: 0.
book check: packages: 31 read; 33 ms: card 22 ms for 368 reads of 1380309 bytes; the longest 1 ms; the most memory 10628.
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

`bn-zip check` reads a file the same way, read for read. So the same book
on the desktop should come to the same entries, the same bytes and the same
reads (`sd_reads` and `sd_read_bytes`, in the last line `bn-zip check`
prints) as it does on the device. `bn-xml book` parses the same documents,
and its last line has the count and the checksum of everything found in
them, which should be the book's on the device too. `bn-epub info` opens
the book and reads its package the same way, and prints the checksum, the
memory and the reads that the package's lines should have.

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
  from the general heap. A document that needs more than the block is
  refused (`kNoMemory`), which in practice means one with a single tag
  several kilobytes long, since a whole tag has to be held at once, or
  with elements nested hundreds deep. Text can be any length. On an X4, no
  document in 30 books (1,392 documents) used more than 9.1 KB.
- **Speed**: on an X4, over the same documents, it took in 830 to 910 KB a
  second with every event counted and checksummed, in two runs of two
  builds of the firmware: from 530 to 650 for chapters thick with markup,
  and 1,060 to 1,090 for plain prose.
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
processing. It is 72 KB of flash once something in the firmware uses it.

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
its documents are read in. `Package::Read` gives back:

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
- **Speed**: on an X4, opening a book and reading its package took 24 to
  135 ms for each of 30 books, 80 ms at the median; the largest lists 199
  files. A little over half of that is the card, at 4 ms a read, and most
  of the rest is parsing the package file. Matching the names is 8 ms at
  the most. None of those books used more than 5.2 KB of the block.
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
  of those the spine is empty.

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
cost: packages_read=1 packages_failed=0 xml_documents=2 xml_failures=0 zip_finds=2 zip_records=16 sd_reads=2 sd_read_bytes=2504 sd_writes=0 sd_write_bytes=0 sd_syncs=0 sd_seeks=0 xml_memory=9536[8752..9536]
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
counters after the reading: here two lookups by name (the container and the
package file), 16 records of the directory, and two reads of the file.
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
cost: packages_read=1 packages_failed=0 xml_documents=3 xml_failures=0 zip_finds=2 zip_records=21 sd_reads=2 sd_read_bytes=7338 sd_writes=0 sd_write_bytes=0 sd_syncs=0 sd_seeks=0 xml_memory=10144[8752..10144]
```

The first line says which kind was read. Then a line an entry, with tabs
between: how far in it is, from 0; the document it points into, by its
place in the spine from 0, or `-` for none; the place within the document,
or `-` for its start; and what it is called. Then how many there were, and
the counters after reading the package and the table. `--nav` and `--ncx`
ask for that kind; it exits with 1, and says why, if there is none to read.

The unit tests say what should be made of tables of contents written for
them, of both kinds, and of every bit of one turned over and every length
it can be cut to. The end-to-end test (`tests/e2e/epub_tool.py`) makes
books whose two tables differ, and compares `bn-epub toc` with a reading of
each by Python's own XML parser, written separately for the test: every
entry has to agree.

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

On an arm64 computer, such as a Mac with Apple silicon, the Linux run leaves
out one of AddressSanitizer's checks: for use of a function's stack after
it has returned. GCC's runtime for arm64 Linux is slow enough at it to turn
seconds of tests into minutes (`tests/CMakeLists.txt` has the figures). CI's
Linux job runs on x86-64, where it is not slow, and makes that check.

## Licence

MIT; see `LICENSE`. Third-party components are listed in `THIRD_PARTY.md`.
