# The file a book is imported into

A book is to be imported once, into a file of its own on the card, and
read from that file afterwards: a *bundle*. `epub/bundle.h` is the bundle
as a container: what is written once when it is made, the record it keeps
of each chapter, and how a chapter is added so that the power going at
any moment loses no chapter added before. What a chapter's own bytes are
is not here yet; to the container they are a run of bytes, a *segment*,
of which a record says where it is. Nothing makes a bundle from a book
yet, and nothing reads one to show it.

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
  tables in their order, then writes both slots with every chapter not
  imported, then the header page, and syncs. Until that last page is
  written the file does not begin as a bundle does, and opening it says
  so: a bundle whose making was cut short is made again.
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
