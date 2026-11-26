# A chapter's bytes

A chapter of a book, once imported, is a run of bytes in the book's
bundle ([The file a book is imported into](bundle.md)): a *segment*.
`epub/segment.h` says what those bytes are, and has what writes a segment
(`SegmentWriter`) and what reads one (`SegmentReader`). Nothing turns a
document into a chapter yet: the writer is given blocks by the tests and
by `bn-bundle`, which make them up.

## What a chapter is

A chapter is **blocks**, one after another in the document's order: a
paragraph, a heading, an item of a list, a picture standing by itself, a
rule, a cell of a table. No block is inside another, and each says of
itself all that setting it needs, so reading can begin at any block with
nothing before it read.

A block is its kind, a heading's level, a **style record**, and then
**items**:

| Item | What it is |
|---|---|
| Text | UTF-8, never ending part way through a character |
| Style | How the text after it is set, until the next style or the block's end: bits for bold, italic, underline, line-through, small capitals, raised and lowered; the same bits for which the book's own style sheets asked for; the class of the font family, and whether the book named it; which of the chapter's languages; the size, of the block's |
| Link start, link end | The text between them is a link: inside the book, a note's reference, or out of the book; with the chapter it leads to and the name of the place there. Nothing is kept of where a link out of the book leads |
| Picture | Its number among the book's pictures, whether it stands in a line of text, its width and height as the document declares them, and the text that stands for it |
| End | How many characters the block is, and the checksum of the block |

A style record has only the fields a block was given, and two masks: which
fields are there, and which of them came from the book's own style sheets
and not from what the element is, so that the book's styles can be set
aside without importing again. The fields are the alignment, the
direction, the size, the height of a line, the indent of the first line,
the margins to left and right, the space before and after, bits for
breaking (a page break before, keep with the next, keep together, white
space as written, floated, a note's text, centred), how deep in
quotations and in lists, what stands before an item of a list, where a
cell is in its table, and a declared width and height. A **length** is
three amounts that are added together: so much of the reader's text size,
so many CSS pixels, and so much of the page's width. A record with every
field is 77 bytes, and one with none is not written.

**Characters** are what a place in a chapter is counted in: the bytes of
its text, one for each picture, and one for a block that has neither. A
style or a link counts nothing.

## The segment

```
blocks ...
   an index page after every 256 blocks
   an anchor page whenever one is full
... more blocks ...
the last index page and the last anchor page, as full as they are
the directory
```

Everything before the directory is records, each begun by four bytes: a
kind, a byte the kind may use, and a length. A block's head, an item and a
page all begin so. A page lies between two blocks or between two items of
one and is no part of the block or of its checksum, so whoever goes from
block to block passes over it by its length, and needs no index to do it.

- **An index page** has 16 bytes for each of up to 256 blocks: where the
  block begins, how many characters the chapter has before it, where in
  the source document whatever began it began, its kind, and the low byte
  of its bits for breaking, which is enough to choose a page by without
  reading the block. The first 65,536 blocks are indexed, in 256 pages.
  Blocks after that are still written, and are read by going on from the
  last that is indexed.
- **An anchor page** has the document's ids in the order they were met:
  each one's name, the block it is in, and how many characters into the
  block. An id on something that makes no block is at the start of the
  block that follows, and one after the last block is at the chapter's
  end. There are 128 pages at the most, of 4,096 bytes each.
- **The directory**, at the end, says how many of each there are and
  where each page is, with the page's checksum; the chapter's blocks and
  characters and the length of its source document; the size of each
  picture the chapter shows, 512 at the most; and the tags of the
  languages its text names, 15 at the most, of 23 bytes at the most. It
  is 44 bytes for a chapter of one index page and one anchor page, and
  about 9 KB at every limit at once.

**Everything read is checked by something checked before it**, with no
pass over the segment: the chapter's record in the bundle has the
directory's checksum, the directory has each page's, and a block ends
with its own. The four bytes before a page are of no checksum: a change
to them sends the reading of the blocks astray, where a block's checksum
or a record that is none stops it.

What is left out for a limit is counted, and the count goes into the
chapter's record: a block past the 65,536th (out of the index only), an
id with a name of more than 255 bytes, one for which no anchor page is
left, one in a block that is not indexed, a picture past the 512th the
chapter shows (its size only), a language past the fifteenth.

## Writing one

```
Begin(file, bundle.DataEnd())
for each block:
  BeginBlock(), then Text(), Style(), LinkStart(), LinkEnd() and
  Picture() as the block has them, then EndBlock()
Anchor() wherever an id is met, in a block or between two
BeginEndBlocks(), then Step() until it says there is no more
SetPictureSize() for each of Pictures() whose size could be read
BeginFinish(), then Step() until it says there is no more
bundle.Commit(chapter, writer.Chapter())
```

- **It writes whole pages, in order.** A call writes two pages at the
  most, and a `Step()` one. Nothing written is part of the bundle until
  it is committed: a segment that is given up, or cut off by the power
  going, lies past where the segments end and is written over by the
  next.
- **None of its memory grows with the chapter or with a paragraph.** It is
  19,304 bytes the caller gives: the page being filled, the index page
  and the anchor page being filled, a kilobyte for the text item being
  put together, the directory's entries for every page there may be, the
  numbers of the pictures met, and the languages' tags. The object is
  408 bytes besides in a desktop build, nearly all of them the chapter's
  record, and less on the device.
- **Text given piece after piece is put together** into items of a
  kilobyte at the most, each ending where a character does: a document
  gives its text in small pieces, and an item for each would be four
  bytes of head for every few words.
- **The pictures' sizes are given at the end**, when the document has been
  read through and whatever reads a picture's file is free. The memory
  the index page had holds them, so they are given only once the blocks
  are ended.
- **It counts what the chapter's record wants** as the text goes by: the
  characters, the text by class of font family (headings left out), and
  which rows of 256 characters the text uses.

## Reading one

```
BeginOpen(file, record), then Step() until it says there is no more
to find a place: BeginFindBlock(), BeginFindCharacters(),
  BeginFindSource() or BeginFindAnchor(), then Step(); then Place()
to read: Seek(place) or SeekStart(); then, for each block,
  ReadBlock() until it is not kMore, and NextItem() until the item
  is the block's end, asking again whenever it says kMore
```

- **Whatever the bytes are, everything asked ends in a status.** A
  segment that fails a checksum, or says of itself what cannot be, is
  damaged; a checksum says when a chapter is to be imported again, and is
  not what makes reading one safe.
- **Its memory is one page** the caller gives, and 720 bytes of its own
  in a desktop build (less on the device), half of them the languages'
  tags. A `Step()`, a `ReadBlock()` and
  a `NextItem()` each ask the file for a page's worth of bytes at the
  most, which is two reads of storage where they lie across two of the
  file's blocks.
- **A block's items are given as they are read**, before its checksum can
  be known: the item that is the block's end is given only if the block
  is what was written, and otherwise the reading ends there as damaged.
  Text is given as the bytes the segment has, which the reader has not
  looked at: whoever sets it has to take any bytes.
- **Finding a place** is a few steps, each of one page:

  | Looked for | Steps |
  |---|---|
  | A block by its number | 2: the page's entry in the directory, and the index page |
  | The block a count of characters falls in | 2: the directory's entries, and the index page |
  | The last block that began at or before a place in the source | 2 for each index page looked at, which is 9 pages at the most of 256 |
  | An id | 2 for each anchor page as far as the one that has it, and 2 for its block |

  A place past the chapter's last character, and an id after its last
  block, are the chapter's end. Where a document has an id twice, the
  first is found.
- **A kind of item the reader does not know** is passed over by its
  length and counted, and a kind of block it does not know is given with
  the number it has.

## The tool

`bn-bundle` prints a chapter as text, writes one from the same text, and
looks for a place in one:

```
./build/host/tools/bundle/bn-bundle write-chapter book.bundle 0 chapter.txt
./build/host/tools/bundle/bn-bundle chapter book.bundle 0
./build/host/tools/bundle/bn-bundle find book.bundle 0 characters 12
```

The text is a line for each thing a chapter is made of, in the order a
document has them (`tools/bundle/chapter.h` has every kind of line). This
is a chapter of three blocks, as `chapter` prints it and as
`write-chapter` reads it:

```
chapter whole 0 4321
language fr
size 9 640 480
id top
block heading 2 100 align=3* breaking=258
text One
end
block paragraph 0 180
text It was a 
style 2 2 2* 1 900
link note 7 n1
text note
id mid
endlink
picture 9 1 64 0 0 0 80 0 a small picture
style 0 0 0 0 1000
text , and then no more.
end
block rule 0 260
end
id after
# 3 blocks, 37 characters, 0 left out, 1 index pages, 1 anchor pages, 3 ids, 0 items of no kind known
# text by class: 28 0 4 0 0 0
# rows of characters: 00
```

The first line is the chapter's state, the reason its record gives, and
the length of its source. A block's line has its kind, its level, where
it began in the source, and the fields of its style, each with a star if
the book's style sheets gave it. A run of text is one line, whatever
items it is kept in, and an id is where it is: in a block, or between
two. The lines that begin with `#` are what the chapter's record and
directory say of it, and `write-chapter` does not read them.

`write-chapter` prints what it wrote:

```
blocks=3 characters=37 bytes=321 directory=55 left_out=0 pictures=1
```

`find` takes `block`, `characters`, `source` or `id` and a number or a
name, and prints where that is: the block, where it begins in the
segment, the characters before it, where it began in the source, its kind
and bits for breaking as the index has them, and how far into the block
the place is. Here: the thirteenth character, the id `mid`, the place 200
bytes into the source, the id after the last block, and an id the chapter
does not have:

```
block=1 offset=30 before=3 source=180 kind=1 breaking=0 into=9
block=1 offset=30 before=3 source=180 kind=1 breaking=0 into=13
block=1 offset=30 before=3 source=180 kind=1 breaking=0 into=0
end block=3 before=37
none
```

`bn-bundle info` has a line for each chapter that was imported:

```
chapter 0: whole, 3 blocks, 37 characters, 0 left out, 321 bytes at 16384, directory of 55 at 266 c2680c0c
```

## Checked

- `tests/unit/epub/segment_test.cpp` gives the writer chapters it makes
  up and reads the bytes back two ways: by the places the format gives
  things, with none of the code under test, and with the reader. A small
  chapter is set down byte for byte. The rest: every limit reached and
  what is left out counted (65,836 blocks, ids enough for 128 pages, 600
  pictures, 16 languages); text cut into items at characters; every call
  held to the pages it may write and read; a place found by each of the
  four ways; storage failing at every write and at each kind of read;
  every byte of a chapter's directory and one in seventeen of the rest
  changed in turn, after which whatever is asked ends in a status or
  gives what the chapter held; and the power going at every change of the
  card while a chapter is written and committed, after which the chapter
  is not there or is whole.
- `e2e.SegmentTool` (`tests/e2e/segment_tool.py`) has a writer and a
  reader of a segment in Python, made from the format. What `bn-bundle`
  writes it reads, and what it writes `bn-bundle` reads: the chapter
  comes back line for line, and `find` says what the script works out.
- The fuzz target `segment` writes what the input describes and reads it
  back, and reads the input's own bytes as a segment
  ([Fuzzing](fuzzing.md)).
