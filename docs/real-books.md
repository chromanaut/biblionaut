# Real books

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

`tools/check-books.py` runs `bn-zip`, `bn-xml`, `bn-epub`, `bn-css`,
`bn-png` and `bn-jpeg` over every EPUB under a folder, and compares what each says of a
book with a reading of the same book by Python:

```
tools/check-books.py corpus
tools/check-books.py --tools build/host ~/books
```

```
28 books: 1228 entries in their ZIP files, 945 XML documents with 1899986 events, 840 documents in their spines, 79 readings of tables of contents with 3914 entries, 77 style sheets with 1406 rules, 1676 sheets asked for by 859 documents, 42379 properties set for 15299 elements of 141 documents, 80 PNG pictures of 62626058 pixels, 45 JPEG pictures of 46766091 pixels.
Memory for the rows of a book's widest PNG picture, in bytes: 9602 at the median, 11202 for nine in ten, 11202 at the most; over the 18 books that have one.
Memory for the rows of a book's widest JPEG picture read at its whole size (a progressive one at an eighth), in bytes: 22528 at the median, 29952 for nine in ten, 40960 at the most; over the 17 books that have one.
Of the 834 documents of their spines, a book's largest paragraph, in bytes: 3824 at the median, 21717 for nine in ten, 141354 at the most; the most paragraphs in one of its documents: 123 at the median, 1050 for nine in ten, 2108 at the most; the most ids in one: 15 at the median, 123 for nine in ten, 501 at the most.
Items of the spines that are not XHTML by their media type, and were not counted: 6 of the kind svg.
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
- **The style sheets**: every sheet `bn-css book` reads has the checksum,
  the rules, the declarations and the count of things passed over that a
  second reading of it comes to ([Style sheets](css.md) has what that
  reading is); and the book's `font-family` values come to the kinds of
  face, by keyword, by a known name or by nothing, that a second
  working-out of them comes to ([Kinds of face](css-families.md)).
- **What a document asks for**: every document `bn-css sheets` goes
  through is given the sheets, in the order, that a second finding in
  Python gives it, and what was left out is the same kind for kind ([What
  a document asks for](document-sheets.md) has what that finding is). A book where
  the tool says it had no room for a sheet, or could not read one to its
  end, is passed over and counted: the second finding does not work
  those out. The same is done for the documents of the book's spine, in
  reading order, by the checksum of what they are all given: `bn-css
  sheets --spine` against that checksum worked out from the second
  finding; and the tool has to come to it both ways, with each document
  opened from its place and with each found by its name.
- **What the rules come to**: for the first few documents of a book, up
  to 1,500 elements between them, what `bn-css document` gives each
  element is what a second working-out gives it, by checksum: the tool
  finding the document's sheets itself, the second working-out given
  those the second finding has. The second working-out is slow, which is
  why it is not every document.
- **The pictures**: every PNG `bn-png book` reads has the header, the
  ending, the rows and the checksum of their gray that a second reading of
  it by Python comes to ([Pictures](pictures.md) has what that reading is); and
  so has every JPEG `bn-jpeg book --reduce 8` reads, at an eighth of its
  size, where the reader's arithmetic and the second reading's are the
  same.

**What it counts and compares with nothing.** No tool reads a document's
text yet, so there is nothing to set these beside; they are counted so
that what real books hold is at hand when something is sized for it. Of
every XHTML document of each book's spine, read once however often the
spine names it, the script counts the paragraphs and the ids:

- **A paragraph** is where `tools/layout-text.py` has one: the text
  between the starts and ends of block elements (the script takes that
  list from there). Its size is that text in UTF-8, with white space in a
  row made one space and a `br` made a line end. What is in `head`,
  `script`, `style`, `svg` or `math` is no part of it, and an element
  with no text, a picture alone, is no paragraph.
- **An id** is the value of any element's `id`, wherever in the document
  it is, or of an `a`'s `name` where it has no `id`. One given twice in
  a document counts once.

The line that begins `Of the` has, book by book, the largest paragraph
and the most paragraphs and ids in one document. An item of a spine that
is not XHTML by its media type is not read: the line after it says how
many there are of each kind (the six in `corpus/` are the pages of the
book laid out in SVG), and another says how many are files the book does
not have, where any are. `--document-report` says more: the spread of
each figure over the documents, how many have a paragraph over each of a
few sizes, and the five books with the most of each, by the document
that has it:

```
tools/check-books.py --document-report corpus
```

```
The documents of the spines: 834 of them, in 27 books:
  bytes of a document: 10769 at the median, 132402 for nine in ten, 2020786 at the most
  paragraphs in a document: 21 at the median, 109 for nine in ten, 2108 at the most
  bytes of a document's largest paragraph: 1195 at the median, 2939 for nine in ten, 141354 at the most
  ids in a document: 1 at the median, 16 for nine in ten, 501 at the most
  bytes of the names of a document's ids: 10 at the median, 162 for nine in ten, 4508 at the most
  bytes of the longest name of an id in a document: 10 at the median, 22 for nine in ten, 47 at the most
  documents with a paragraph over 4, 8, 16, 32 and 64 KB: 46, 29, 18, 5, 3
  the books with the largest paragraphs, in bytes:
      141354  pg24264-images-3.epub: OEBPS/2938539401917988555_24264-0-1.txt.xhtml
       26241  pg14741-images-3.epub: OEBPS/6968912479258425706_14741-h-0.htm.xhtml
       21717  pg7000-images-3.epub: OEBPS/6692091177720485162_7000-h-2.htm.xhtml
       21383  pg4300-images-3.epub: OEBPS/1471490836921852263_4300-h-18.htm.xhtml
       16499  adam-smith_the-wealth-of-nations.epub: epub/text/chapter-5-1.xhtml
  the books with the most paragraphs in one document:
        2108  adam-smith_the-wealth-of-nations.epub: epub/text/chapter-1-11.xhtml
        1438  pg4300-images-3.epub: OEBPS/1471490836921852263_4300-h-15.htm.xhtml
        1050  pg1012-images-3.epub: OEBPS/7872976103240418525_1012-h-2.htm.xhtml
         750  pg17489-images-3.epub: OEBPS/8334475935261823288_17489-h-3.htm.xhtml
         537  william-shakespeare_hamlet.epub: epub/text/act-3.xhtml
  the books with the most ids in one document:
         501  adam-smith_the-wealth-of-nations.epub: epub/text/endnotes-1.xhtml
         142  moby-dick.epub: OPS/toc.xhtml
         123  pg24264-images-3.epub: OEBPS/2938539401917988555_24264-0-5.txt.xhtml
          82  pg22367-images-3.epub: OEBPS/380337140494991823_22367-h-0.htm.xhtml
          67  pg17489-images-3.epub: OEBPS/8334475935261823288_17489-h-0.htm.xhtml
  the books with the largest documents, in bytes:
     2020786  linear-algebra.epub: EPUB/xhtml/fcla-xml-2.30li58.xhtml
      358772  adam-smith_the-wealth-of-nations.epub: epub/text/chapter-1-11.xhtml
      258985  pg24264-images-3.epub: OEBPS/2938539401917988555_24264-0-7.txt.xhtml
      255032  pg4300-images-3.epub: OEBPS/1471490836921852263_4300-h-15.htm.xhtml
      210832  pg26731-images-3.epub: OEBPS/408209634455667199_26731-h-0.htm.xhtml
```

The reading is Python's `html.parser`, which takes what a document has
however badly it is formed; one it gives up on is counted and said.
`e2e.DocumentCount` checks the counting on two books it makes, whose
paragraphs and ids were counted as they were written: a paragraph inside
another's block, a line end, text in the head and in a picture, an id
given twice, a document the spine names twice, a picture in the spine and
an item that is no file.

Where they differ it prints a line for each difference, starting
`DIFFERENT`, and ends with 1. It takes the tools from `build/host-release`
if they are built there, or else from `build/host`; `--tools` names another
build, and `--zip`, `--xml`, `--epub`, `--css`, `--png` and `--jpeg` one tool each.

A folder with a file named `SHA256SUMS` in it is checked against that file
as well: every file listed has to be there with that SHA-256, and every
EPUB in the folder has to be listed. That is how a book in `corpus/` that
has been changed, or added without its line, is found.

CTest runs it over `corpus/` as `e2e.Corpus`. It is as useful over any
folder of books on the computer at hand: a few hundred take some seconds
with the `host-release` tools.

Its readings by Python have to be as careful as the tools are. The first
book it met whose names are UTF-8 without the ZIP file saying so was read
rightly by the tools and wrongly by Python's `zipfile`, which takes such
names for another encoding; the readings now take a name's bytes as UTF-8
whatever the flag says, and `e2e.EpubTool` makes a book of that kind.
