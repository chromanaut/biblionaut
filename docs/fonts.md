# Fonts

The firmware does not read font files. Every face it draws text in, at
every size it draws it, is drawn beforehand on the computer that builds,
and kept in one run of bytes, the fonts image, which the firmware reads
where it lies: on the device, in the flash partition called `fonts` (see
"Fonts on a device", below), mapped into the processor's address space a
face at a time; on the desktop, in a file read into memory. So far the
application uses the image
for one thing, showing a file of text to try faces with ([Trying faces on
the panel](text-viewer.md)); the rest of this is the image, the program that makes
it, the reader of it and the measure that sets text by it.

A face is one role (serif, sans, monospace, or the fallback for what the
others lack), one style (regular, italic, bold, bold italic) and one size
in pixels. It has its metrics, a table from code points to glyphs, a record
for each glyph and the glyphs' pictures. A glyph's picture is two bits a
pixel: four levels of ink, from paper to full. Its advance, how far it
moves the pen, is in quarters of a pixel and is the outline's own, not
rounded to the pixels its picture was fitted to: so the widths of a line's
characters add up to what the line is wide at any size. Glyph 0 of a face
is what is drawn for a character it has no glyph for. `draw/font.h`
describes the layout byte by byte. There is no kerning yet, and no
ligatures; bytes are kept for kerning in a face's record and a glyph's.

## Making the image

`bn-font` (`tools/font`) makes the image from font files, drawing each
glyph with FreeType, which the host build downloads (`third_party/freetype`)
and which is no part of the firmware. (A build configured with
`-DBN_FONT_TOOL=OFF` has no `bn-font` and downloads neither FreeType nor
the font its tests use; the firmware's build of its own host tools is
one.) Which files, and at which sizes, is said by a preset, a text file (the
build has some of its own: The presets, below):

```
sizes 26 30 34
mono regular  CourierPrime-Regular.ttf
mono bold     CourierPrime-Bold.ttf
```

A line names a face's role (`serif`, `sans`, `mono`, `fallback`), its style
(`regular`, `italic`, `bold`, `bold-italic`) and its font file, which is the
rest of the line and is found from the preset's folder unless it begins
with `/`. `sizes` are the sizes every face is drawn at, in pixels to the
em. `characters U+0020-U+007E U+00A0 ...` chooses the characters that are
drawn; without it they are Latin with its extensions, the phonetic
alphabet, the marks set over letters, Greek, Cyrillic, punctuation,
currency and letterlike symbols, number forms, arrows, mathematical
operators and the Latin ligatures. A character the font has no glyph for is
left out and counted. Anything from a `#` to the end of a line is passed
over.

```
$ bn-font build courier.preset --out courier.bnfn
mono     regular     26 px:  381 glyphs (2074 lacking), pictures   20833 bytes; ascent 21, descent 9, line 29, x-height 12
mono     regular     30 px:  381 glyphs (2074 lacking), pictures   27877 bytes; ascent 24, descent 11, line 34, x-height 14
mono     regular     34 px:  381 glyphs (2074 lacking), pictures   35732 bytes; ascent 27, descent 12, line 38, x-height 16
mono     bold        26 px:  381 glyphs (2074 lacking), pictures   22159 bytes; ascent 21, descent 9, line 29, x-height 12
mono     bold        30 px:  381 glyphs (2074 lacking), pictures   29667 bytes; ascent 24, descent 11, line 34, x-height 14
mono     bold        34 px:  381 glyphs (2074 lacking), pictures   37570 bytes; ascent 27, descent 12, line 38, x-height 16
faces 6, pictures 173838 bytes, image 204534 bytes
```

Glyphs are drawn with FreeType's light hinting, which moves an outline up
and down to the pixels and not across, and its 256 grays are made the four
levels. A serif or a sans that is given a regular and not all of the other
three styles is built, with a warning for each: the reader of the image
gives the nearest style a role has. The fonts a preset names are whoever
builds's own business: no font is in this repository, and a preset that
names fonts from the builder's computer need not be either. The pictures
are not compressed; what they would take if they were is for `bn-font
sizes` to say (below).

`bn-font info` says what an image holds, reading it by the code the
firmware reads it with; with `--glyphs` and some characters, what each face
has for them: the glyph's number, its advance in quarters of a pixel, its
picture's size and where its top left corner lies from the pen on the
baseline, and its levels of ink added up.

```
$ bn-font info courier.bnfn --glyphs U+0041 U+0067 U+0020 U+03B1
image 204534 bytes, faces 6
mono     regular     26 px:  381 glyphs; ascent 21, descent 9, line 29, x-height 12
  U+0041: glyph 34, advance 62, 16 by 15 at 0, 15, ink 249
  U+0067: glyph 72, advance 62, 14 by 17 at 1, 12, ink 288
  U+0020: glyph 1, advance 62, 0 by 0 at 0, 0, ink 0
  U+03B1: no glyph
mono     regular     30 px:  381 glyphs; ascent 24, descent 11, line 34, x-height 14
  U+0041: glyph 34, advance 72, 18 by 18 at 0, 18, ink 340
  U+0067: glyph 72, advance 72, 17 by 20 at 1, 14, ink 385
```

(and so on for the other four faces).

`bn-font sizes` says what an image's pictures would take if they were kept
in other ways than the image keeps them, which is as they are: for
choosing a way, and how many sizes a partition has room for, from the
fonts themselves. It changes nothing, and the firmware reads none of these
forms. For the faces of each size, and for all of them:

- **run-length, a glyph**: each glyph by itself as runs of pixels of one
  level, a byte to a run: the level in two bits and the length, 64 at the
  most, in six;
- **DEFLATE, a glyph**: each glyph by itself, compressed by zlib at its
  best;
- **the best, a glyph**: each glyph in whichever of those, or as it is, is
  smallest for it, and the last line says how many glyphs that was for
  each;
- **DEFLATE, groups** of 16, 64 and 256 glyphs in the order a face has
  them, and **a face** as one: compressed together, so that to draw one
  glyph its group has to be decompressed.

```
$ bn-font sizes courier.bnfn
image 204534 bytes: tables 30696, pictures 173838
26 px: 2 faces, 762 glyphs
  as they are                42992 bytes  100%
  run-length, a glyph        63138 bytes  147%
  DEFLATE, a glyph           39957 bytes   93%
  the best, a glyph          38914 bytes   91%
  DEFLATE, groups of 16      24473 bytes   57%
  DEFLATE, groups of 64      21986 bytes   51%
  DEFLATE, groups of 256     19218 bytes   45%
  DEFLATE, a face            17647 bytes   41%
```

(and the same for 30 px and 34 px)

```
all: 6 faces, 2286 glyphs
  as they are               173838 bytes  100%
  run-length, a glyph       219738 bytes  126%
  DEFLATE, a glyph          146463 bytes   84%
  the best, a glyph         144369 bytes   83%
  DEFLATE, groups of 16      86779 bytes   50%
  DEFLATE, groups of 64      78341 bytes   45%
  DEFLATE, groups of 256     68083 bytes   39%
  DEFLATE, a face            62114 bytes   36%
the best, a glyph: 691 as they are, 58 run-length, 1537 DEFLATE
```

Letters drawn in grays are poor in runs: an edge is a pixel or two of each
level, and a byte to a run comes to more than the picture as it is. A
glyph by itself is too little for DEFLATE to find much in, and what it
saves grows with the size; together, glyphs compress to half and less.
The compressed sizes are zlib's, and another version of zlib may give a
few bytes more or fewer.

## The presets

A fonts image can be made without bringing any fonts: the build has
presets, each a serif and a sans in four styles and a monospace face in
two, whose font files it downloads from their makers' repositories at a
commit it names, each checked against a hash (`third_party/fonts`). They
are downloaded when an image is asked for, not when a build is configured:

```
cmake --build --preset host --target fonts      # writes build/host/fonts/fonts.bnfn
```

Which typefaces is chosen when the build is configured:

| | |
|---|---|
| `-DBN_FONTS=noto` | Noto Serif and Noto Sans. This is what an image is made of unless something else is asked for |
| `-DBN_FONTS=plex` | IBM Plex Serif and IBM Plex Sans |
| `-DBN_FONTS=source` | Source Serif 4 and Source Sans 3 |
| `-DBN_FONTS=/a/preset/of/ones/own` | Whatever that preset names: its own font files, at its own sizes |
| `-DBN_FONTS_MONO=courier-prime` | Courier Prime as the monospace face. This is so unless something else is asked for |
| `-DBN_FONTS_MONO=family` | The family's own: Noto Sans Mono, IBM Plex Mono or Source Code Pro |

A preset here is three files of `tools/font/presets` read as one:
`ladder.preset`, which has the sizes; the family's faces; and a monospace
face. `bn-font build` takes any number of presets and reads them so, and
with `--fonts FOLDER` finds their files from that folder, which is how the
build points them at what it downloaded. The sizes are five, 22, 26, 30,
34 and 38 pixels to the em, each about an eighth larger than the one
before, 30 being the size text is first read in: on the X4's panel, about
7, 8.5, 10, 11.5 and 12.5 points as printed. Plex and Source have fewer of
the characters an image is made with than Noto has (about 900 and about
860 to 1,400 a face, against about 1,980), so their images have Noto Sans
besides, as the fallback face, for what they lack. As built, with the
12 MB of the `fonts` partition to fit in:

| Preset | Monospace face | Faces | Image |
|---|---|---|---|
| `noto` | Courier Prime | 50 | 9.64 MB |
| `noto` | Noto Sans Mono | 50 | 11.47 MB |
| `plex` | Courier Prime | 55 | 5.55 MB |
| `plex` | IBM Plex Mono | 55 | 6.04 MB |
| `source` | Courier Prime | 55 | 6.03 MB |
| `source` | Source Code Pro | 55 | 6.67 MB |

```
$ cmake --build --preset host-release --target fonts
...
mono     bold        34 px:  381 glyphs (2074 lacking), pictures   37570 bytes; ascent 27, descent 12, line 38, x-height 16
mono     bold        38 px:  381 glyphs (2074 lacking), pictures   47138 bytes; ascent 30, descent 13, line 43, x-height 18
faces 50, pictures 8620836 bytes, image 9639730 bytes
```

All of these typefaces are under the SIL Open Font License; what that asks
of a firmware given out with an image made of them is in `THIRD_PARTY.md`
(Fonts in a firmware). The tests download none of them: they read each
preset with either monospace face, and `e2e.FontTool` holds every font
file a preset names to being one the build has a commit and a hash for.

## Reading the image

`draw::FontImage` (`draw/font.h`) opens an image and gives its faces: by
number, by role, style and size, or the nearest style a role has at a size
(bold italic gives way to bold, then to italic, and any of them to
regular). A `draw::Face` gives a glyph for a code point, with its picture
as a span of the image's own bytes; nothing is copied, and nothing is
allocated.

The image is not trusted, though the firmware's own build made it: a
partition can hold anything.

- **Opening** looks at the header and at each face's record, 1,024 faces at
  the most, and refuses an image in which any face's tables run outside
  it. It reads only as many bytes as the image says it is long, so a
  partition larger than its image is no trouble.
- **Finding a glyph** halves the face's ranges of code points: 33 steps at
  the most (`Face::kMostSteps`), whatever the tables hold, and `LastSteps()`
  says how many. A glyph whose picture would run outside its face's
  pictures is not given.
- **RAM**: none but the objects themselves: under 300 bytes for an image,
  most of it the list of the faces it holds (below), and 0.6 KB in the
  device's hardware layer for the list of what it has mapped. **Flash**:
  2.7 KB for the reader, 1.2 KB for the measure below and 0.7 KB for the
  partition's mapping, as compiled for the device, besides the image
  itself.

An image is opened in one of two ways. Given a span of bytes, all of it is
there to be read, and every face has its glyphs from the first: so the
tools open one, and the tests. A device has not the address space to map
the whole of an image ([Fonts on a device](#fonts-on-a-device)), so there
an image is opened through a `core::ByteMap` (`core/byte_map.h`), which
gives a stretch of bytes when asked and takes it back, and only the header
and the table of faces are kept readable, 49 KB of them at the most. A
face then says its role, style, size and metrics, and is found by them, but
has no glyphs until it is held (`FontImage::Hold()`): that maps the one
stretch of the image from the first of the face's three tables to the end
of the last, which is the face and nothing else in an image `bn-font` made,
since it writes a face's tables and pictures together. Sixteen faces are
held at the most, and `ReleaseFaces()` gives them all up: whoever sets a
page holds the faces the page is set in, and gives them up for the next
page's. A face there is no room to map is not held, and has no glyphs. A
glyph says where in the image its picture is, and `FontImage::Bytes()`
gives the picture again from that while its face is held, which is how a
page keeps a glyph in four bytes. Nothing chooses a face to give up for
another when room runs out; that is for whoever comes to set a page in more
faces than there is room for.

`layout::FaceMeasure` (`layout/face_measure.h`) is the `TextMeasure` that
sets text in such faces. Each of the paragraph setter's styles is given a
face. A character is drawn with its style's face if that has a glyph for
it, with the fallback face if that has, and otherwise with its style's
face's glyph 0; a soft hyphen and a zero width space are not drawn and take
no room; a byte that is not UTF-8 is drawn as U+FFFD. A piece of a word is
as wide as its glyphs' advances added up, so what is drawn from the same
glyphs lines up with what was measured. The hyphen shown at a line's end is
the style's U+2010, or its hyphen-minus. Reading UTF-8 a character at a
time, strictly, is `core/utf8.h`, which the hyphenator shares.

The reader's tests write their images by hand from the description of the
layout (`tests/support/font_image.h`): what is found and what is not, an
image cut short at every byte, and one with every byte changed three ways,
from which nothing may be given that lies outside it. Each of those images
is opened through a map as well, one that gives every stretch as a copy of
exactly its size and frees it when it is given up, so that a byte read
outside a face's own stretch, or after the face was given up, is seen: it
has to go as the image opened whole goes, and every face, once held, to
give the same glyphs. The builder's tests,
and `e2e.FontTool`, try `bn-font` on a real font, Courier Prime, which the
build downloads for them (`third_party/courier-prime`): the test takes the
image apart itself, and the font file as far as its tables of characters
and widths, and holds the image to having a glyph for exactly the
characters the font has, each moving the pen as far as the font's width
for it comes to at that size; `bn-font info --glyphs` to saying of
every glyph what the image's bytes say; and `bn-font sizes` to the sizes
the test works out again, the runs written out a second time and the
compressing done by Python's zlib. Nothing draws a glyph a second way:
that a picture is its letter is for whoever looks at it. The reader has a
fuzz target as well (`font`, see [Fuzzing](fuzzing.md)).

## Fonts on a device

The firmware's fonts live in a partition of the flash of their own, read
where they lie through the processor's mapping of flash into its address
space. The firmware's partition table (`app/firmware/partitions.csv`) has
a 4 MB app partition and, after it, a 12 MB data partition, `fonts`.
Flashing a firmware writes the table and leaves that partition as it was:
an image is megabytes, and stays from one firmware to the next. It is
written by a target of its own, from a file `bn-font` made:

```
cmake --build --preset host-release --target fonts
tools/idf -DBN_FONTS_IMAGE=$PWD/build/host-release/fonts/fonts.bnfn fonts-flash
```

```
-- The fonts image: 9649010 bytes of the partition's 12582912.
...
Wrote 9649012 bytes (3561560 compressed) at 0x00400000 in 54.8 seconds (1407.7 kbit/s).
Hash of data verified.
```

A file that is not a fonts image, or one larger than the partition, is
refused before anything is written. The hardware layer gives the partition
to the rest as a `core::ByteMap` (`hw::SystemFonts()`): a stretch of it is
read by being mapped, and takes every page of 64 KB it touches. So far the
one firmware that reads it is the text viewer's ([Trying faces on the
panel](text-viewer.md)), which is where what it costs on an X4 is given.
The mapping is counted: `font_maps`, `font_maps_refused` and, with the most
it has been, `font_mapped_kb` go out with the other metrics.

### The partition check

A firmware built with `-DBN_PARTITION_CHECK=ON` checks the partition itself
on a device: that it is there, how much of it can be mapped, and what
reading it costs.

```
tools/idf -DBN_PARTITION_CHECK=ON build flash
./build/host-release/tools/trace/bn-trace --seconds 20 | tee partition.log
tools/idf -DBN_PARTITION_CHECK=OFF build flash      # back to the normal firmware
tools/idf -DBN_FONTS_IMAGE=... fonts-flash          # and the fonts image again
```

Flashing it fills `fonts` with 12 MB of a pattern, in place of any image, in
which every four bytes are a number worked out from where they are
(`app/firmware/partition_pattern.py`). At start, before the application,
it says how large the flash chip is, what partitions there are, how much
of `fonts` can be mapped at once, how long mapping a window takes, how
fast the whole partition is read and whether every word of it is right,
and what a read of 64 bytes costs where the cache has it and where it does
not (`app/firmware/main/partition_check.h`). On an X4:

```
partition check: flash: the chip says 16384 KB (ESP_OK); the build takes it to be 16384 KB (ESP_OK).
partition check: partition nvs: type 1, subtype 0x02, at 0x009000, 20 KB.
partition check: partition phy_init: type 1, subtype 0x01, at 0x00e000, 4 KB.
partition check: partition factory: type 0, subtype 0x00, at 0x010000, 4032 KB.
partition check: partition fonts: type 1, subtype 0x40, at 0x400000, 12288 KB.
partition check: pages of address space free for data: 123 (7872 KB).
partition check: the whole partition at once, 12288 KB: ESP_ERR_NO_MEM.
partition check: the most mapped at once: 123 pages (7872 KB).
partition check: a window of 64 KB: mapped in 16 us and unmapped in 4 us, on average of 100 (0 failed).
partition check: a window of 1024 KB: mapped in 79 us and unmapped in 7 us, on average of 100 (0 failed).
partition check: read all 12288 KB in 824 ms (14902 KB/s), through windows of 1024 KB.
partition check: every word is what it should be (3145728 words).
partition check: scattered over 4096 KB: 20000 reads of 64 bytes in 87541 us (4377 ns a read).
partition check: within 8 KB: 20000 reads of 64 bytes in 17349 us (867 ns a read).
partition check: passed.
```

The processor has 8 MB of address space for flash, in 128 pages of 64 KB,
shared by the app's code and constant data and whatever else is mapped:
beside an app of 280 KB, 123 pages were free, so a partition larger than
that is mapped a part at a time. Mapping a window of 1 MB takes five times
as long as one of 64 KB, and is still under a tenth of a millisecond. The
mapping is done by the chip's ROM, where the firmware takes its driver for
the flash from ([Device build](building.md#device-build)). Done by ESP-IDF's own driver it left 124 pages
free and took 26 µs for a window of either size, and reading through it
was as fast (822 ms for the 12 MB).
