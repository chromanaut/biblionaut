# Importing a chapter

A document of a book is made into a chapter of its bundle once, and read
from the bundle ever after ([The file a book is imported into](bundle.md)).
`epub/chapter_import.h` does the making. A `ChapterImporter` reads the
document twice from its start: its head first, for the style sheets it
asks for ([What a document asks for](document-sheets.md)); then all of it,
with a cascade over those sheets ([A book's rules, and what they come
to](css-cascade.md)) and a `DocumentBlocks`, which says where each block
of text begins and ends and with what record ([A document's
blocks](document-blocks.md)). What they say goes to a `SegmentWriter` with
the text ([A chapter's bytes](segment.md)), and what comes out is the
chapter's record, to be committed.

```
importer.Begin(document, sheets, parser, file, bundle.DataEnd(), language)
while (importer.Step()) {}
bundle.Commit(chapter, importer.Chapter())
```

Blocks, their text, how the text is set, ids and languages are imported.
A picture is written as one the book does not have, with the text that
stands for it, since nothing yet says which of the book's pictures it is;
and no link is written, nor where a cell is in its table.

## What becomes of the text

- **White space** (a space, a tab, a line end, a return) is combined as
  CSS has it. Any run of it in a block is one space, across the elements
  it is written in, and none is kept at a block's start, at its end, or
  to either side of a line's end. A space is written only when something
  is written after it in its line, and is set as the first of the white
  space it stands for was.
- **Where white space is as written** (`pre`) text is kept as it is, but
  for the white space before a block's first text: of that, only what
  follows the last line end is kept, 64 bytes of it at the most, and it is
  set as the text is. So the line end after `<pre>` is no empty line, and
  the first line keeps its indent. Everything after the first text stays,
  white space at the block's end with it.
- **`br`** is a line feed in the text. A line feed that ends a block's
  text ends its last line and adds none, as CSS has it; so a `br` alone in
  a block is one empty line.
- **A character written by a name** HTML gives it (`&nbsp;`, `&mdash;`)
  is that character, where the parser reports the name ([XML](xml.md)).
  The 253 names are those of HTML 4.01 and `apos`
  (`epub/html_entities.h`). A name that is none of them is left out, and
  counted.
- **Nothing else is changed.** A soft hyphen stays, a no-break space is no
  white space, and no character is composed with another.
- **A rule** (`hr`) has nothing in it: what a document writes inside one
  is left out.

## How text is set

A style item is written where the setting of text changes, and none for
text set as a block begins: plain, in the book's own language, at the
block's size. A run's size is so much of its block's, in thousandths. Its
language is the book's own where its tag is the one the caller gives as
the book's, in letters of either case; any other is one of the chapter's
languages, which are numbered in the order text in them is first written.

## Ids

An `id` of any element, and the `name` of an `a` that has no `id`, is an
anchor where the element opens: in the block that is open, so many
characters into it; between two blocks, at the start of the next; and in
what is not shown, the same. An id stands before white space that has not
yet become a space, and at the start of a block whose first characters may
yet be a capital that is dropped.

## A capital that is dropped

A book writes one in two ways, and `DocumentBlocks` says which blocks may
have one. The importer looks at the block's first characters, which it
holds back until it knows:

- **A first letter with a style of its own** (`p::first-letter`). The
  block's first letter, and the one or two quotation marks before it if
  they are set as it is, are written as a run in that style. If the rule
  floats the letter, the block says how many characters that is.
- **Floated text at a block's start** (`<span class="dropcap">`). The
  block says so if the floated text is no more than one letter with one or
  two quotation marks before it, all set alike, or one picture whose `alt`
  is 64 bytes at the most. A block that has both is taken this way, and
  its letter is set as the floated element is.

The count is two bits of the block's "breaking and the rest"
(`kBreakDropped`): none to three characters, which are in the block's
text like any others; a picture is one. A letter is one of the Latin,
Greek or Cyrillic alphabets (U+0041 to U+024F, U+0370 to U+052F and
U+1E00 to U+1FFF, but for the signs among them), and for a first
letter's style a digit as well. A mark is a quotation mark of either
kind, a guillemet, or the mark Spanish opens a question or an exclamation
with.

## Where a block began

A block's place in the document is where the last tag before its first
text began, an element's opening or its closing: for a block that an
`hr`, a `br` or an `img` begins, the tag of that element itself. It is the
same whatever pieces the document is read in and whatever it is encoded
in.

## What goes wrong

A document that goes wrong part way is a chapter **cut short**: what came
before is kept, and the block that was open ends there. One of which
nothing was read is **refused**, and has no segment. Just how much of the
text before the place is kept is the parser's to say, which looks at a
document a piece at a time: a few characters more or less with pieces of
another size. A device's are always 1,024 bytes, so a book comes to the
same chapter every time. The chapter's record gives the reason as a
number:

| Reason | | |
|---|---|---|
| 1 | no file | The book has no such file, or it could not be opened |
| 2 | unreadable | The file could not be read to its end: it is damaged in the book |
| 3 | not XML | It is not XML, from where the chapter ends |
| 4 | no memory | The parser was given too little memory for it |

A document of the spine that is not imported at all has a record too
(`ChapterWithoutDocument()`): one whose file the book does not have is
refused, and one that is not a document of text, such as a picture
standing in the spine, is said to be no document.

Importing itself ends badly only if storage fails or the segment would not
fit in a file. Nothing written is part of the bundle until the chapter is
committed.

**What is left out and counted.** The chapter's record has one count of
what was left out for a limit: what the writer could not keep (an id of
more than 255 bytes, an id past the last page of ids), a name that is no
character's, white space before a `pre`'s first text that there was no
room to hold, and what was too deep or one language too many for the
`StyleStack` and the cascade ([What each element comes
to](element-styles.md)).

**The versions.** A bundle's header page says which rules it was made
under (`kImportTextRules`, `kImportVersion`): the first is raised by a
change that can move text within a paragraph or change which paragraphs
there are, the second by one that makes importing better without moving
any. Both are 1.

## What it costs

**Memory.** The importer takes 24,656 bytes from its caller, none of which
grows with the chapter or with a paragraph: 19,304 for the segment's
writer, 2,048 for the cascade, 2,792 for the blocks (32 open elements) and
512 for the document's path. Beside it stand what reading any file of a
book takes: the parser's memory, the table of the book's rules, the
inflater's 32 KB window, a 4 KB buffer for each of the book's two files
and for the bundle, the finder of sheets' 6 KB and the bundle's page.
`bn-bundle import` gives the parser and the table 24 KB each, 129,104
bytes in all, and says how much of those two the book used.

**A step.** One `Step()` opens the document, or has the finder of sheets
do a step, or has the parser read one piece of the document, and never
two of those: so it reads 1,024 bytes of the book at the most. And it
writes eleven pages of the bundle at the most. A piece of the document can
begin a block with every three bytes (`<hr/>a` is two blocks); a rule and
a block of one character are 235 bytes of the segment between them, with
the longest records a block may have and their records in the index; that
is 40 KB, ten pages, and one more for what the step before left of a
page. A chapter of ordinary prose writes a page for about every four
pieces it reads.

## The tool

`bn-bundle import` imports the chapters of a book into the bundle `make`
made of it, one by its number or all of them, and commits each:

```
./build/host/tools/bundle/bn-bundle make book.epub book.bundle
./build/host/tools/bundle/bn-bundle import book.epub book.bundle all
./build/host/tools/bundle/bn-bundle chapter book.bundle 0
```

For a book whose spine has a chapter, a picture, and a document the book
has not got, `import` prints:

```
chapter 0 whole reason=none blocks=4 characters=70 bytes=521 left_out=0 dropped=1 entities_left_out=0 held_left_out=0 too_deep=0 cascade_too_deep=0 languages=2 steps=33 path=OEBPS/chapter.xhtml
chapter 1 not-a-document reason=none blocks=0 characters=0 bytes=0 left_out=0 dropped=0 entities_left_out=0 held_left_out=0 too_deep=0 cascade_too_deep=0 languages=0 steps=0 path=-
chapter 2 refused reason=no-file blocks=0 characters=0 bytes=0 left_out=0 dropped=0 entities_left_out=0 held_left_out=0 too_deep=0 cascade_too_deep=0 languages=0 steps=0 path=-
memory work=24656 parser=24576 parser_used=10304 rules=24576 rules_used=307 window=32768 buffers=12288 sheets=6144 page=4096 all=129104
```

A line for each chapter: what became of it and why, its blocks and
characters, the bytes of its segment, what was left out (in all, and of
that the names that are no character's, the white space not held, and the
elements too deep for the `StyleStack` and for the cascade), how many
blocks begin with a dropped capital, how many languages the document
names, how many steps it took, and the document's path in the book. Then
the memory importing was given, and how much of the parser's and of the
table's was used at the most.

The chapter is this document, with the sheet in [Style sheets](css.md):

```
<body>
<section epub:type="dedication"><p>For A.</p></section>
<h2 id="one">One</h2>
<p>It   began,
   as these things do, in a <span xml:lang="fr">caf&eacute;</span>.</p>
<p class="first" style="margin-top: 1em">And went <i>on</i>,<br/> and on.</p>
</body>
```

and `chapter` prints what was made of it ([A chapter's
bytes](segment.md#the-tool) has what each line is):

```
chapter whole 0 591
language fr
block paragraph 0 364 align=2* indent=96,0,0* left=0,0,0* right=0,0,0* before=0,0,0*
style 2 2 1* 0 1000
text For A.
end
id one
block heading 2 388 align=2* size=1500 left=0,0,0* right=0,0,0* before=192,0,0* breaking=3
style 1 0 1* 0 1000
text One
end
block paragraph 0 410 indent=0,0,0* left=0,0,0* right=0,0,0* before=96,0,0*
style 0 0 1* 0 1000
text It began, as these things do, in a 
style 0 0 1* 1 1000
text café
style 0 0 1* 0 1000
text .
end
block paragraph 0 497 indent=0,0,0* left=0,0,0* right=0,0,0* before=64,0,0* breaking=256*
style 0 0 1* 0 3000
text A
style 0 0 1* 0 1000
text nd went 
style 2 0 1* 0 1000
text on
style 0 0 1* 0 1000
text ,\x0Aand on.
end
# 4 blocks, 70 characters, 0 left out, 1 index pages, 1 anchor pages, 1 ids, 0 items of no kind known
# text by class: 0 67 0 0 0 0
# rows of characters: 00
```

The white space of the third block is one space at each place, and its
name a character; the French is the chapter's first language, and the
book's own English has no number. The last block's first letter is set
three times the size, as its rule says, and the block says that one
character is dropped (256 is one, in the two bits above the low byte);
its `br` is a line feed, with no space after it. Every run is of a serif
face, which the book's sheet named for the whole body.

## Checked

- `tests/unit/epub/chapter_import_test.cpp` imports documents written for
  each rule above into a bundle on storage the test holds, and reads the
  chapter back with the segment's reader: white space of every kind, text
  as written, names, how text is set, languages, ids, pictures, each
  thing of a block's record, where a block began, and every way a capital
  is and is not dropped. And what goes wrong: a document that is not XML
  from somewhere on, one that cannot be read to its end, one of which
  nothing is read, a parser with too little memory, storage failing at
  each of a chapter's first writes. Every step of every import is held to
  what a step may read and write, and one document is written to come as
  near the most as importing can. A document read a byte at a time comes
  to the chapter it does read whole.
- `e2e.ImportTool` (`tests/e2e/import_tool.py`) works each chapter of a
  book out a second time in Python, another way round. Where the
  firmware's code goes through a document once, holding back only a
  space, a few bytes of white space and a block's first characters, the
  test keeps the whole document, gathers everything that lies in a block
  into one list, and answers from the list. `bn-bundle chapter` has to
  print what that comes to, line for line, and `bn-bundle import` to count
  what it counts: for a document written to bring up every rule, the one
  the test of blocks was written for, 60 documents and their sheets made
  at random, documents cut short part way, and the documents that are
  refused.
- The fuzz target `chapter_import` imports whatever document and sheets
  it is given, with storage and the book failing where the input says,
  and reads the chapter back ([Fuzzing](fuzzing.md)).
