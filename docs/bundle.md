# The file a book is imported into

A book is to be imported once, into a file of its own on the card, and
read from that file afterwards: a *bundle*. `epub/bundle.h` is the bundle
as a container: what is written once when it is made, the record it keeps
of each chapter, and how a chapter is added so that the power going at
any moment loses no chapter added before. What a chapter's own bytes are
is not here yet; to the container they are a run of bytes, a *segment*,
of which a record says where it is. `epub/book_bundle.h` makes a book's
bundle from its EPUB, with every chapter not imported ([A bundle made
from a book](#a-bundle-made-from-a-book), below). Nothing imports a
chapter yet, and nothing reads a bundle to show it.

```
0          the header page                4,096 bytes
4,096      the book's four tables         one after another: strings,
                                          spine, pictures, contents
A          slot 0                         a whole number of pages
A + S      slot 1                         the same size
A + 2S     the chapters' segments         each begins on a multiple of 512
```

Numbers are little-endian. A checksum is a CRC-32, the one a ZIP file
uses.

- **The header page and the tables** are written when the bundle is made
  and never again. The header page says which EPUB the bundle is of (its
  size, when it was last written, and the checksum of its ZIP directory),
  how many documents and pictures the book has, where the tables, the
  slots and the segments are, and each table's checksum; its last four
  bytes are the checksum of the rest of it. The tables of the spine and
  of the pictures hold a record of 24 bytes for each file: where it is in
  the EPUB, as `ZipEntry` has it, and two bytes for whoever fills the
  table. The other two tables are bytes to the container.
- **A slot** holds a record of 88 bytes for each document of the spine:
  what has become of it (not imported, imported whole, imported and cut
  short, not a document, refused), where its segment is, where in the
  segment its directory is and the directory's checksum, how many blocks
  and characters it has, its text counted by class of font family, and
  which rows of 256 characters it uses. With them go a sequence number,
  where the segments end, and a checksum of the slot.
- **Two slots, written in turn.** To add a chapter its segment is written
  where the segments end, filled out with zeros to the end of its last
  sector, and synced; then the slot that is not the newer one is written,
  a copy of the newer with that chapter's record changed and the next
  sequence number, and synced. Opening takes the slot with the higher
  number that passes its checksum. So a cut before the slot is whole
  leaves the other slot standing, which knows nothing of the new segment:
  its bytes lie past the end that slot records, and the next segment is
  written over them. At most the chapter in hand is lost.
- **Nothing committed shares a sector with anything written later.** A
  card writes whole sectors of 512 bytes, and a write that is cut short
  may leave any part of them. So the slots begin on pages and are written
  in whole pages, and every segment begins on a sector and is filled out
  to the end of its last one before it is committed.
- **Made in one go, the header page last.** `BundleMaker` is given the
  tables in their order, and then what the book is, which is only all
  known once the tables are written (how many entries the contents came
  to, for one). It writes both slots with every chapter not imported,
  then the header page, and syncs. Until that last page is written the
  file does not begin as a bundle does, and opening it says so: a bundle
  whose making was cut short is made again.
- **A piece at a time.** Opening, committing and checking a table are
  begun, and then `Step()` is called until it says there is no more. A
  step reads one page of the file at the most, writes one at the most and
  syncs once at the most, however many documents the book has: a slot is
  4 KB for a book of up to 46 documents and 180 KB for one of 2,014, and
  is read through when the bundle is opened and written through at every
  commit.
- **Memory**: one page of 4,096 bytes, the caller's, beginning at a
  multiple of four so that a card's driver takes it as it is; used only
  while a piece of work is under way. Nothing of the book is kept: a
  record asked for is read from the file.
- **What opening checks, and what it does not.** The header page and the
  slot it takes are checked against their checksums, and then against
  what a header and a slot can say: tables that follow one another,
  slots of the length their records come to, segments that lie among the
  segments, a directory inside its segment. Opening also adds up what
  the chapters come to (`Totals()`), since it reads every record anyway.
  It does not read the tables: `Check()` reads one against its checksum
  when asked.
- **Bad input**: whatever a file holds, opening it ends in a status:
  `kNotBundle` (too short, or it does not begin as one), `kOtherVersion`,
  `kDamaged` (it fails a checksum, or says of itself what cannot be),
  `kStoreFailed`. A checksum says when a bundle is to be made again; it
  is not what makes reading one safe, and the fuzz target gives the
  reader files whose checksums have been put right round whatever else
  they hold.

**The power going.** The tests' stand-in for storage loses its power
after any number of changes to the card, and can now do it part way
through a write: with some of the write's first bytes written, and with
the rest of every sector the write was to touch left as rubbish. What had
been written and not synced is then lost, or kept, or kept only where it
was written over bytes the file already had, or of that only the last
write made, as a test asks: a card is free to keep what it is given in
any order until it is told to sync. One test makes a bundle and commits
six chapters to it, and cuts the power at every change of the card there
is in that, each of those four ways, with the write in hand cut short
eight ways: some 2,000 cuts. After each, the file is either no bundle,
which only a making that had not returned leaves, or a bundle in which
every chapter is as its last commit that returned left it, or as the
commit under way would have, with all of its segment; and another chapter
can be committed to it. A second test cuts the power twice: once with a
segment on the card and its chapter not recorded, and again while another
segment is written over that one, the card keeping only its last write.
A chapter that is then recorded has its own segment, which is what the
sync between a segment and its slot is for.

**Checked** in `tests/unit/epub/bundle_test.cpp`: beside that, the bytes
of a bundle against the layout above, read with nothing of the code under
test; any one byte of the header page or of the slot in force changed;
headers and slots that pass their checksums and say what none should;
records that lie across two pages of a slot; and a step held to a page of
work for a book of 2,014 documents. The fuzz target `bundle` opens a
file, reads its records, checks its tables, commits a chapter to it and
opens it again ([Fuzzing](fuzzing.md)); its seeds are bundles written out
by `tests/fuzz/make_seeds.py`, which shares none of this code, and they
open as what they were written to be.

## A bundle made from a book

`epub/book_bundle.h` fills a new bundle from an EPUB whose package has
been read (`BookBundler`): the four tables, and what the header page says
of the EPUB. It imports no chapter.

- **The strings**: the title, the first author, the language and the
  identifier, as the package gives them ([What a book says of
  itself](package.md)), each a length in two bytes and then its bytes.
- **The spine**: a record for each document, in reading order, with
  where its file is in the EPUB, its kind, and bits for "in the run of
  the book" and "the file is in the EPUB".
- **The pictures**: a record for each of the manifest's files that is a
  picture (JPEG, PNG, GIF or SVG by its media type), in the manifest's
  order. A picture is known everywhere else by its place here, which
  `Package::PictureOf()` works out from a path. The record has where the
  file is, its kind, a bit for "the file is in the EPUB" and a bit for
  the cover.
- **The contents**: the entries of the table of contents the book is
  read by, one after another: how far in the entry is, the document it
  points into, the place within the document, and what it is called. A
  book with no table has none. One whose table cannot be read, or goes
  wrong part way, or has more than 65,535 entries, has the entries that
  could be had, and the header page says they are not the whole of the
  book's (`amiss`): the bundle is made all the same.
- **Which EPUB it is of**: the EPUB's size, when it was last written, and
  the checksum of its ZIP directory, the bytes as they are in the file
  (`ZipDirectorySum`, [ZIP files](zip.md)). The directory has every
  entry's name, size and checksum, so a file changed in any way has
  another. `Bundle::IsOf()` sets the three beside a file's.

**The pictures are in the manifest's order, and the EPUB gives its files
in its own.** Looking each picture up by name would read the EPUB's
directory through once apiece, and writing each record where it falls
would be a write of part of a sector apiece. So the directory is gone
through once for every 170 pictures, a page of their records being put
together in memory and then written: once for most books, and three
times for one of 400 pictures.

**A piece at a time**, as everything is: `Begin()`, then `Step()` until
there is no more. A step reads no more of the EPUB than a step of the
package reader or of the table of contents reader does (five blocks of
4 KB), writes two pages of the bundle at the most, and syncs once at the
most. Its memory is two pages the caller gives, one for the bundle's
maker and one for the pictures' records and the directory's bytes; the
object is 1.4 KB besides in a desktop build, most of it the reader of
the table of contents, and less on the device, where a pointer is half
the size.

`bn-bundle` runs it on the desktop:

```
./build/host/tools/bundle/bn-bundle make book.epub book.bundle
./build/host/tools/bundle/bn-bundle info book.bundle
```

`make` prints how many of each thing it wrote, and how many times it went
through the EPUB's directory for the pictures:

```
documents=3 pictures=3 pictures_present=2 contents=3 passes=1 amiss=0 bytes=16384
```

`info` prints what a bundle holds: the versions it was made under, the EPUB it is of, the
strings, a line for each document of the spine and each picture (where
the file is in the EPUB as three numbers: where its header is, how many
bytes it holds, and their checksum), each entry of the contents as
`bn-epub toc` prints one, the slot in force, how many chapters are in
each state, and whether each table is what its checksum says:

```
format: 1
text rules: 0
importer: 0
made by: bn-bundle
epub: 2562 bytes, written 1795000000, directory cca5fb72
package: 0b438ebd
amiss: -
title: A Small Book
author: A. Writer
language: en-GB
identifier: urn:uuid:5f0c9d2e-1b7a-4c3d-9e8f-000000000001
spine: 3
+ xhtml 1646 37 62e76318
+ xhtml 1731 37 edf29a23
- xhtml 1890 39 3d327cc0
pictures: 3
png 1816 510 b99bdf85
jpeg 1368 680 73d176af cover
?
contents: 3
0	0	-	One
1	0	part-2	A part of it
0	1	-	Two
slot: sequence 2, 4096 bytes, segments from 16384 to 16384
chapters: 3 not imported, 0 whole, 0 cut short, 0 not documents, 0 refused
tables: ok ok ok ok
```

**Checked.** `tests/unit/epub/book_bundle_test.cpp` makes books in
memory and reads every table of the bundle back beside them: each record
of the spine and of the pictures opens the file it is for; 400 pictures
put into the ZIP file last first come out in the manifest's order, in
three passes; a table of contents that is missing, is no table, goes
wrong part way or is too long; storage failing before every step; and
every step held to what it may read and write. `e2e.BundleTool`
(`tests/e2e/bundle_tool.py`) reads a bundle's bytes a second way, by the
places the format gives them, and sets them beside a reading of the book
by Python's `zipfile` and ElementTree; `bn-bundle info` then has to print
what that reading comes to, line for line. `tools/check-books.py` makes
the same comparison on real books ([Real books](real-books.md)). The fuzz
target `book_bundle` makes a bundle of whatever book it is given
([Fuzzing](fuzzing.md)).
