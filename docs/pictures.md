# Pictures

`PngReader` (`draw/png.h`) reads a PNG picture. It is the first of the
pieces that will turn a book's pictures into what a panel shows: a reader
of each format gives rows of 8-bit gray, the scaler (below) makes them the
size the picture is shown at, and the ditherer (`draw/dither.h`, "Fewer grays", below) brings
them down to the grays a panel has. `JpegReader` (`draw/jpeg.h`, below) is
the second. The drawing viewer shows the PNG and JPEG files on its card
this way ([Drawings and pictures on the panel](drawing-viewer.md)). GIF is not read yet. The pieces hand rows on to one another through one interface,
`RowSink` (`draw/row_sink.h`).

A picture is read from a `ByteSource`, such as an entry of the ZIP file, a
piece at a time, and never held whole: first its header, then its rows,
top first, each given to a `RowSink` as soon as it is whole. All it keeps
of a picture is the row being read and the one above it, which PNG's
filters refer back to.

- **What it reads**: every kind of picture PNG has. Gray at 1, 2, 4, 8 and
  16 bits a sample; colour (RGB), gray with alpha and colour with alpha
  (RGBA) at 8 and 16; colours from a palette at 1, 2, 4 and 8.
  Transparency by an alpha sample, by a palette's alphas, or by one gray
  or colour that stands for none (`tRNS`).
- **What it gives**: a byte a pixel, from black (0) to white (255), as the
  picture looks on white paper. A colour is the gray it looks, by the same
  rule as in a drawing (`draw/gray.h`: 0.299 of its red, 0.587 of its green
  and 0.114 of its blue), and a pixel that is not wholly solid is that much
  of its gray laid over white. A sample of 16 bits is taken by its high
  byte, and one of fewer than 8 is stretched so that its largest value is
  white. A palette index past the end of the palette is black.
- **What it leaves out**: interlaced pictures, whose rows are not whole
  until the last of seven passes over the picture: the header is read, and
  the rows refused (`kInterlaced`). Anything about colour itself (`gAMA`,
  `sRGB`, `iCCP`, `cHRM`), since what comes out is gray for paper; the
  colour a picture asks to be put behind it (`bKGD`), since it is laid on
  the page; text; and every other chunk a reader may pass over. An
  animated picture is read as its first picture, which is what its `IDAT`
  chunks hold. In the books looked at, 2,823 PNG files in all, none is
  interlaced and none is animated.
- **Bad input**: whatever bytes it is given, it ends in a status:
  `kNotPng`, `kBadHeader`, `kBadChecksum`, `kBadChunk`, `kUnknownChunk`,
  `kNoPalette`, `kBadData`, `kTruncated`, `kNoRoom` or `kSourceFailed`. Every
  chunk's checksum is checked. A wrong one ends the reading if the picture
  needs the chunk (`IHDR`, `PLTE`, `IDAT`), and the chunk is passed over,
  and counted, if not. A chunk's checksum comes after its bytes, so the rows
  made from a chunk of data have been given by the time its checksum is
  found wrong. The checksum zlib puts at the end of the data is not
  checked: the inflater has read it by then, as it reads ahead, and its
  bytes have all been through a chunk's checksum.
- **Memory**: the inflater's 32 KB window and room for two rows, both the
  caller's. A row needs the longer of its bytes, with the filter byte in
  front of them, and its gray, which is a byte a pixel
  (`PngReader::RowBytesFor()`). The rows of the widest picture in the
  library need 10,202 bytes; in the public books 29,426; 16 KB holds all
  but 73 of the public books' 2,435. The object itself is 3,048 bytes on
  the device, most of it the inflater. Reading only the header needs no
  window and no rows, and reads 33 bytes of the file.
- **One call's work**: `Step()` reads PNG's signature, or the start of a
  chunk, or up to 1 KB of one; or it asks the inflater once for up to
  1 KB of a row, and if that finishes the row, undoes its filter, turns
  it to gray and gives it out. What the inflater takes is bounded by its
  own rule. Between the bytes it takes lie the ends and starts of chunks:
  a picture's data may be cut into chunks of any length, down to a byte,
  and two empty ones in a row are passed over (no encoder writes one, and
  none of the books' pictures has one; three in a row are refused as bad
  data). So the most a call takes, whatever the file, is 284,196 bytes, for
  data a byte to a chunk with two empty chunks after each
  (`PngReader::MostInputPerStep()`). Encoders write chunks of 8 KB or more
  but the last, and a call then takes about what the inflater does.

`bn-png` (`tools/png`) runs the reader on a file, or on every PNG in an
EPUB:

```
./build/host/tools/png/bn-png read picture.png --out picture.pgm
./build/host/tools/png/bn-png read picture.png --width 480 --out small.pgm
./build/host/tools/png/bn-png header picture.png
./build/host/tools/png/bn-png book book.epub
```

`read` prints how the reading ended, the CRC-32 of the rows of gray it
gave, what the header says (width, height, kind, bits a sample), how many
rows it gave, the memory its rows need, how many chunks it passed over, and
how many of those had a wrong checksum; with `--out`, it writes the rows as
a PGM file to be looked at. `--rows` gives it less memory for rows, and
`--pieces` the file in smaller pieces. `--width` and `--height` put the
rows through the scaler, as the firmware will to show the picture at
another size; with one of them, the other keeps the picture's shape.
`header` reads no more than the header, as a book's importer would. For
the logo of a book from Standard Ebooks:

```
ok 0dd98f59 342 218 gray+alpha 8 218 1370 0 0
ok 342 218 gray+alpha 8 0 33
```

and with a kilobyte for its rows:

```
no-room 00000000 342 218 gray+alpha 8 0 1370 0 0
```

and made the panel's width, when the checksum and the rows are those of the
new picture:

```
ok 63611f11 342 218 gray+alpha 8 306 1370 0 0 scaled 480 306
```

`book` prints the same line for every PNG in the book, with its name, and
adds them up. For *Pride and Prejudice*, in `corpus/`:

```
ok 0dd98f59 342 218 gray+alpha 8 218 1370 0 0 epub/images/logo-2x.png
ok 3cffcd3b 220 140 gray+alpha 8 140 882 0 0 epub/images/logo.png
ok 4e9b3d89 2800 880 gray+alpha 8 880 11202 0 0 epub/images/titlepage-2x.png
ok c2934a08 1400 440 gray+alpha 8 440 5602 0 0 epub/images/titlepage.png
png_files=4 png_failed=0 png_bytes=97941 png_pixels=3185356 png_most_room=11202
```

The unit tests make pictures with zlib, of every kind and depth with every
filter, and set what the reader gives beside the gray each pixel should
come to, worked out a pixel at a time from its samples
(`tests/support/png_builder.h`); then spoil them in each way the reader
looks for, and hold every step to its bound, among them a picture whose
data is a byte to a chunk. An end-to-end test (`tests/e2e/png_tool.py`)
compares `bn-png` with a second reading of the same files in Python, with
Python's zlib: 400 pictures made up at random, every line the same, and
600 spoiled. `tools/check-books.py` sets the two side by side on every
PNG of a folder of books ([Real books](real-books.md)). The reader has a
fuzz target ([Fuzzing](fuzzing.md)).

## JPEG

`JpegReader` (`draw/jpeg.h`) reads a JPEG picture in the same way: from a
`ByteSource`, a piece at a time, its header first and then its rows, top
first, each given to a `RowSink`. What it keeps of a picture is one row of
its blocks: the 8 or 16 rows of pixels that are made at once.

- **What it reads**: baseline and extended pictures coded with Huffman
  codes, 8 bits a sample, gray (one component) or colour as YCbCr (three),
  sampled in any way the standard allows, in one scan or several; restart
  markers; and progressive pictures by their first scan (below).
- **What it gives**: a byte a pixel, from black (0) to white (255). A
  JPEG's gray is its first component, Y, which is 0.299 of red, 0.587 of
  green and 0.114 of blue, the rule `draw/gray.h` has for every picture. So
  only Y is turned back from frequencies into pixels: the colour is decoded
  only as far as is needed to find where Y's next block begins.
- **Smaller as it is read**: at a half, a quarter or an eighth of its size
  each way (`JpegReduction`), for less work and less memory. Each pixel is
  then the average of those it stands for in the block at its whole size,
  worked out from the block's frequencies in one transform; at an eighth
  that is the block's average alone. The transform is a product of the
  block with a table of cosines, down and then across, and each row of the
  table reads the same from either end but for its sign; so a pixel and
  the one opposite it in its row or column are made from the same
  products, which halves the multiplying and changes no number. On the
  X4 the 11 JPEGs of the card's set are read 4 to 13% sooner for it, the
  small ones read whole the most: 12.1 s for them all where multiplying
  the whole table out took 12.7 s.
  `ReductionFor()` gives the most a
  picture can be made smaller as it is read and still be as large as it is
  shown, and the scaler does the rest. Shown on the X4's page, 480 by 800,
  the rows of every JPEG in the library and the public books fit in 16 KB
  (15,360 bytes at the most); at their whole size the widest need 71,424.
- **Progressive pictures**: a progressive picture is coded in scans over
  the whole of it, each adding to every block, and none of its pixels is
  whole until the last; keeping every block's coefficients between scans
  would take about two bytes a pixel. Its first scan holds each block's
  average, which is the picture at an eighth of its size. So a progressive
  picture is read at an eighth, whatever is asked, from that scan alone,
  and made larger by the scaler: soft. The low bit of an average that a
  later scan would add is taken as nothing. libjpeg's `jpegtran` rewrites
  a progressive file as baseline without changing a coefficient. Of the
  608 progressive JPEGs in the library and the public books, every one has
  such a first scan; only 27 are covers.
- **What it leaves out**: arithmetic coding, lossless and hierarchical
  JPEG, samples of 12 or 16 bits, four components (CMYK), colour coded as
  RGB, a picture whose Y is sampled less than its colour, and one whose
  height comes after its first scan. Each has its header read and is
  refused (`kUnsupported`). The orientation an Exif block gives is not
  looked at. Of the 10,008 JPEGs in the library and the public books, the
  two that are CMYK are the only ones refused, and every Exif orientation
  is upright.
- **Bad input**: whatever bytes it is given, it ends in a status:
  `kNotJpeg`, `kBadMarker`, `kBadHeader`, `kBadTable`, `kBadScan`,
  `kUnsupported`, `kBadData`, `kTruncated`, `kNoRoom` or `kSourceFailed`.
  Bytes between segments that are not a marker are passed over and
  counted, as other readers do. Inside a scan, a code that no table has, a
  coefficient past a block's end, or a restart marker other than the one
  due is bad data; coded data that runs into the end of the file, or into
  the picture's end marker, is cut short. The rows of the blocks finished
  before either have been given.
- **Memory**: room for one row of blocks, the caller's
  (`JpegReader::RowBytesFor()`), and the object itself, which holds eight
  tables of Huffman codes, four of quantization and a block's numbers:
  9,052 bytes on the device. Reading only the header needs no room;
  the source is read 512 bytes at a time, and a header is in the first 512
  bytes unless segments of metadata come before it.
- **One call's work**: `Step()` reads JPEG's first marker; or passes over
  up to 1 KB between segments, finding a marker; or reads one table, a
  frame or scan header, or up to 1 KB of a segment that is passed over; or
  passes over up to 1 KB of a scan that is not read; or decodes up to eight
  of a scan's units of blocks and turns Y's blocks among them into pixels;
  or gives one row. So the most a call takes, whatever the file, is 34,096
  bytes: eight units of ten blocks, each of whose coefficients has the
  longest code and the most bits there can be, every byte of it 0xFF and
  so followed by a 0, and what is read ahead
  (`JpegReader::MostInputPerStep()`).

`bn-jpeg` (`tools/jpeg`) runs the reader on a file, or on every JPEG in an
EPUB:

```
./build/host/tools/jpeg/bn-jpeg read picture.jpg --out picture.pgm
./build/host/tools/jpeg/bn-jpeg read picture.jpg --width 480 --out small.pgm
./build/host/tools/jpeg/bn-jpeg read picture.jpg --reduce 8
./build/host/tools/jpeg/bn-jpeg header picture.jpg
./build/host/tools/jpeg/bn-jpeg book book.epub
```

`read` prints how the reading ended, the CRC-32 of the rows of gray it
gave, what the header says (width, height, kind), how many times smaller
it was read, how many rows it gave, the memory its rows need, and how many
segments and scans it passed over and bytes between segments that were not
a marker; with `--out`, it writes the rows as a PGM file to be looked at.
`--reduce` reads it smaller; `--width` and `--height` put the rows through
the scaler, read at the reduction the firmware would take for that size.
`--rows` and `--pieces` are as `bn-png`'s. For the cover of *Pride and
Prejudice* from Standard Ebooks, in `corpus/`:

```
ok 34521b35 1400 2100 4:2:0 1 2100 22528 1 0 0
ok 1400 2100 4:2:0 512
```

and made the panel's width, read at a half on the way:

```
ok 1194a0f2 1400 2100 4:2:0 2 720 5632 1 0 0 scaled 480 720
```

and the same cover made progressive by `jpegtran`, read at an eighth from
its first scan:

```
ok 0a856cdf 1400 2100 progressive-4:2:0 8 720 352 1 0 0 scaled 480 720
```

`book` prints the same line for every JPEG in the book, with its name, and
adds them up. For *Moby Dick* from the IDPF's samples, in `corpus/`:

```
ok 65840f70 646 902 4:4:4 1 902 5184 5 0 0 OPS/images/9780316000000.jpg
ok 64de40f2 840 1380 gray 1 1380 6720 4 0 0 OPS/images/Moby-Dick_FE_title_page.jpg
jpeg_files=2 jpeg_failed=0 jpeg_bytes=453855 jpeg_pixels=1741892 jpeg_most_room=6720
```

The unit tests make pictures from the coefficients of their blocks, of
every kind the reader reads, and set what it gives beside what those
coefficients come to in real numbers: T.81's inverse transform a pixel at
a time, and at a half and a quarter the average of the whole block's
pixels (`tests/support/jpeg_builder.h`); within a gray, and at an eighth
exactly. One test holds every pixel, at the whole size, a half and a
quarter, to the number that multiplying the block by the whole of a table
comes to, in whole numbers rounded as the reader rounds them, with tables
the test makes from the cosines for itself. Then pictures of each kind it
refuses, and pictures spoiled in
each way it looks for, every step held to its bound. An end-to-end test
(`tests/e2e/jpeg_tool.py`) compares `bn-jpeg` with a second reading of the
same files in Python, of pictures made up at random by an encoder of its
own: 160 of every kind the reader reads, each the same line for line and
byte for byte at an eighth and within a gray at the other sizes; 12 of the
kinds it refuses; and 300 spoiled. `tools/check-books.py` sets the two
side by side, at an eighth and byte for byte, on every JPEG of a folder
of books ([Real books](real-books.md)). The reader has a fuzz target
([Fuzzing](fuzzing.md)).

## Scaling

`Scaler` (`draw/scale.h`) makes a picture larger or smaller a row at a time.
It is a `RowSink`, so a reader of pictures gives it its rows as it reads
them, and it gives the rows of the picture at the new size to another
`RowSink`. It keeps nothing of the picture but what the rows still to come
need: two rows of the new width at the most.

- **Smaller**: each new pixel is the average of the old pixels it covers,
  each counted by how much of it is covered. No old pixel is left out, so a
  line one pixel wide in a large picture is still there in a small one,
  lighter for being spread.
- **Larger**: each new pixel is a blend of the two old pixels nearest its
  middle, by how near each is (bilinear). Past the middle of the first or
  last old pixel, that pixel is used as it is.
- **Each direction by itself**, so a picture can be made narrower and
  taller at once; the same size is the picture as it is.
- **The grays as they are stored** are averaged, not the light they stand
  for: a picture's grays are already spaced so that their steps look about
  even, and averaging them as they are keeps the thin dark lines of
  diagrams and formulas darker.
- **Whole numbers**, rounded three times, each to the nearest with a half
  up: a blend's weight to 4096ths of the way, a row made the new width to
  256ths of a gray, and each new pixel to a gray. Every new pixel is within
  a gray of the same working-out in real numbers. The sums stay inside 32
  bits for sides of up to 65,535.
- **No division for each pixel**: each number that every pixel of a row
  is divided by (the old width, the units of a new row, twice the new size
  for a blend's weight) is made once into a multiplier and two shifts
  (`draw/divide.h`), by Torbjörn Granlund and Peter L. Montgomery's method,
  which gives the quotient division gives for every 32-bit number. The old
  row is walked once, by a loop for each way of scaling, with nothing
  decided again for each pixel. On the X4 that made scaling twice as fast,
  every pixel coming out as before.
- **Memory**: the caller's, nine bytes for each pixel of the new width
  (4,320 for the panel's 480).
- **One call's work**: `Row()` goes over the old row once and the new width
  once, and gives nothing out; `Step()` makes one new row and gives it out.
  One old row can be needed by several new ones when a picture is made
  taller, so all the new rows an old row has made ready are given out
  before the next old row is taken; one given before then stops the
  scaling (`kOutOfTurn`). After each step of the reader, the caller calls
  `Step()` until it gives nothing.

Its unit tests set it beside a working-out of the same picture in real
numbers, at 300 sizes made up at random and at the sizes a book's pictures
meet, and hold it within a gray of it; and beside its three roundings
worked out from exact fractions, at 400 more, where every pixel has to be
the same. A picture of one gray stays that gray at any size, the same size
is the picture byte for byte, and the largest sides keep their sums inside
32 bits. The end-to-end test scales 82 of its pictures with `bn-png` and
holds every pixel within a gray of a working-out in Python. The scaler has
a fuzz target ([Fuzzing](fuzzing.md)). The divisions' multipliers are held to
division itself: for every divisor up to 70,000 at the numbers where a
quotient changes, for every number up to 2^22 and a step through the rest
by eight divisors, for every power of two and the numbers either side of
it, and for a million divisions at random.

## Fewer grays

`Ditherer` (`draw/dither.h`) turns rows of 256 grays into as many levels as
a panel shows, from two up, in place. Each pixel becomes the nearest level
to its gray and what was carried to it, and what is left over is carried on
as Floyd and Steinberg did it: seven sixteenths to the next in the row, and
three, five and one to the three below. So a patch comes out, on the whole,
as light as it was, and a gray between two levels as a mixture of them.
Rows are given from the top, as strips make them: what is carried down from
the last row of one strip goes into the first of the next. The memory it
carries in is the caller's, two rows of 16-bit numbers. The gray of each
level is looked up, in a table made when it begins. A pixel that is white
or black with nothing carried to it is passed over, being a level already
and leaving nothing to carry on: most of a page of text or of a drawing.
On the X4, as the viewer times it, with a row's packing into bits after
it: 0.91 µs a pixel over 20 photographs and engravings, where working
each gray out by a division took 1.11; and 0.14 to 0.19 s for a page of
a drawing, where passing over nothing took 0.31.
The unit tests hold it to patches as light as their grays, a ramp as light
as it was column by column, levels already levels left alone, only levels
coming out, and pixel for pixel to the same sums done the plain way.
