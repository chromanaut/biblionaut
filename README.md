# Biblionaut

EPUB reader firmware for the Xteink X4 (ESP32-C3, 800x480 e-ink, SD card),
written so that everything except the hardware layer also builds and runs on
a desktop.

## What it is for

A reader of books, written from scratch for a small device: a processor
with a few hundred kilobytes of RAM and no floating point, an e-ink panel
that shows four grays, seven buttons and an SD card. It is to read EPUB 2
and EPUB 3 books whose text flows, and to read them well:

- Type set as a printed book's is: the lines of a paragraph chosen
  together, words broken by their language's own patterns, and text drawn
  in the panel's grays.
- A page turned as quickly as the panel allows, and a chapter as quickly
  as a page.
- A place in a book that stays exact when the face, its size or the
  margins change.
- What everything costs in time and memory measured and reported, on the
  device as on the desktop.

It reads EPUB and nothing else, and it has no wireless.

## How it is made

- **Everything but the hardware layer builds and runs on a desktop**,
  where nearly all of the testing is done. A desktop application stands in
  for the device, with a simulated panel and scripted buttons ([The panel
  and the buttons](docs/panel-and-buttons.md)).
- **Memory comes from whoever calls.** The portable parts take nothing
  from the general heap, and a piece that needs more than it was given
  says so ([Architecture](docs/architecture.md)).
- **A book is read through small buffers**, a piece at a time, and no file
  of it is ever whole in memory.
- **One loop, in slices.** Long work is done about 20 ms a turn, and each
  piece states the most that one call of it may do, so that the buttons
  are answered throughout.
- **Nothing a book holds is trusted.** Whatever bytes it is given, a
  reader ends in a status. Each reader has a fuzz target
  ([Fuzzing](docs/fuzzing.md)), and the tools that run the readers on the
  desktop have end-to-end tests that set them beside readings by code that
  shares none of theirs, on files the tests make and on the real books in
  `corpus/` ([Real books](docs/real-books.md)).
- **A firmware can be built to check one thing** on a device, and reports
  it in the terms the desktop's tools use, so that a device and a desktop
  can be set side by side ([Building](docs/building.md#firmwares-that-check-one-thing),
  [Diagnostics](docs/diagnostics.md)).

## What there is so far

No book is shown yet. There are the readers of a book's own files (ZIP,
XML, its package, its table of contents, its style sheets and what their
rules come to); the importing of a book into one file on the card, its
chapters as blocks of text with how each is set; the pieces that set text
(hyphenation, the lines of a paragraph, the lines of a page, the fonts);
and the pieces that draw (SVG drawings, PNG and JPEG pictures, scaled and
dithered to the panel's grays, and what was drawn kept on the card). The
X4's panel, buttons and card are driven. The application shows a test
pattern; it can be built to show in its place the drawings and pictures on
a card, or the card's text set in the faces of a fonts image, which a
device has in a partition of its flash.

## Layout

| Directory | Contents |
|---|---|
| `core/` | Shared foundation: arenas, a heap within a fixed block of memory, the timing trace, counters and gauges, the framed diagnostic stream, checksums, the inflater, the heap-failure hook, and the interfaces the portable parts use to reach the outside world (clock, byte source, byte sink, display, buttons, power) |
| `hw/` | Hardware abstraction layer. `hw/include` is the interface. `hw/desktop` implements it for the host; on a device, `hw/esp32c3` holds what the chip provides and `hw/x4` what is specific to the X4 |
| `epub/` | Book parsing: zip, XML, OPF / TOC, CSS, the importer and the intermediate format |
| `draw/` | Fonts and drawing: glyphs, metrics, the display-list format, the band renderer, images |
| `layout/` | Book layout: line breaking, hyphenation, pagination, positions. Emits display lists |
| `reader/` | The reader application. The only part that knows about all the others |
| `app/` | Entry points: `app/desktop` for the host, and `app/firmware`, the ESP-IDF project that builds the one firmware image for all supported devices |
| `tools/` | Host programs that run the firmware's code on one file (`bn-zip`, `bn-xml`, `bn-epub`, `bn-css`, `bn-svg` and others), and developer scripts ([Tools](docs/tools.md)) |
| `third_party/` | How the code that is not ours is fetched and built: for each, a CMake file and its settings. The code itself is downloaded when a build is configured and is not in this repository (`THIRD_PARTY.md` lists it) |
| `tests/` | Host tests, and the fuzz targets (`tests/fuzz`) |
| `corpus/` | Real books for the tests to read: 28 EPUB files, in the public domain or openly licensed, which are under their own terms and not this project's licence (`corpus/README.md`) |
| `docs/` | How each piece works, how to run its tool or its check, and what they print |

## Getting started

```
cmake --preset host
cmake --build --preset host
ctest --preset host
./build/host/app/desktop/biblionaut-desktop
```

[Building](docs/building.md) has what the host build needs and downloads,
its presets, and the firmware's build. [Checks](docs/checks.md) has what CI
runs, and how to run it here.

## Documentation

Working on it:

- [Building](docs/building.md): the host build and its presets, the
  firmware, and the firmwares that check one thing.
- [Checks](docs/checks.md): what CI runs, and how to run it here.
- [Architecture](docs/architecture.md): the parts and what each may use,
  the rules the portable parts keep, and the loop.
- [Diagnostics](docs/diagnostics.md): the stream a device sends,
  `bn-trace`, crash reports and the watchdog.
- [Tools](docs/tools.md): the host programs and scripts, and where each is
  described.
- [Fuzzing](docs/fuzzing.md): the fuzz targets, what each holds its reader
  to, and `tools/fuzz.py`.
- [Real books](docs/real-books.md): the books in `corpus/`, and
  `tools/check-books.py`.

The device:

- [The panel and the buttons](docs/panel-and-buttons.md): the panel's
  planes, windows and grays, the buttons, the desktop's simulation and
  window, and sleep.
- [Storage](docs/storage.md): files on the card, and the storage check.

Reading a book:

- [Decompression](docs/inflater.md): the inflater, and the checksum.
- [ZIP files](docs/zip.md)
- [XML](docs/xml.md)
- [What a book says of itself](docs/package.md): the package.
- [The table of contents](docs/toc.md)
- [Style sheets](docs/css.md): the reader of CSS.
- [Kinds of face](docs/css-families.md): what a `font-family` comes to.
- [A book's rules, and what they come to](docs/css-cascade.md): the table
  of a book's rules, and the cascade.
- [What a document asks for](docs/document-sheets.md): the sheets a
  document is read with.
- [What each element comes to](docs/element-styles.md): what an element
  takes from the one it is inside, and what its element gives it.
- [A document's blocks](docs/document-blocks.md): where a block of text
  begins and ends, its kind, and its record.
- [The file a book is imported into](docs/bundle.md): the bundle, as a
  container that loses no chapter when the power goes, and as made from
  a book.
- [A chapter's bytes](docs/segment.md): what a chapter is in a bundle,
  with what writes one and what reads one.
- [Importing a chapter](docs/chapter-import.md): a document made into a
  chapter of the bundle, and what becomes of its text.
- [Reading the books on a card](docs/book-check.md): the book check.

Setting text:

- [Where a word may be broken](docs/hyphenation.md): the hyphenator.
- [Breaking paragraphs into lines](docs/line-breaker.md): the line breaker.
- [Setting a paragraph's text in lines](docs/paragraph-setter.md): the
  paragraph setter, and `bn-lines`.
- [Choosing the lines of a page](docs/page-chooser.md): the page chooser.
- [A chapter in pages, on a device](docs/chapter-pager.md): the pager, which
  reads a chapter's blocks, has them set and chooses pages among their
  lines.
- [A chapter in pages](docs/pages.md): `bn-pages`, which reads a chapter of a
  bundle and sets it in pages on the desktop.
- [Timing layout on a device](docs/layout-check.md): the layout check.
- [Fonts](docs/fonts.md): the fonts image, its presets and its reader, and
  the partition for it.
- [Trying faces on the panel](docs/text-viewer.md): the text viewer.

Drawing:

- [Drawings](docs/svg.md): SVG, from path data to a whole file.
- [Keeping a drawing](docs/drawing-spool.md): a drawing read once, and
  drawn from what was kept a band at a time.
- [Pictures](docs/pictures.md): PNG, JPEG, scaling and dithering.
- [Keeping what is shown](docs/picture-cache.md): the cache of pictures on
  the card.
- [Drawings and pictures on the panel](docs/drawing-viewer.md): the
  drawing viewer.

## Licence

MIT; see `LICENSE`. Third-party components are listed in `THIRD_PARTY.md`.
The books in `corpus/` are not part of that: each is under the terms it
came with, which `corpus/README.md` gives.
