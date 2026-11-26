# A document's blocks

A bundle keeps a chapter as blocks of text, each with a record of how it
is set ([The file a book is imported into](bundle.md)). A document has no
blocks: it has elements, and text standing anywhere among them.
`epub/document_blocks.h` finds the blocks. A `DocumentBlocks` goes
through a document with a `StyleStack` ([What each element comes
to](element-styles.md)), told of each element as it opens and closes and
of where text or a picture comes, and says where a block begins and ends,
of what kind it is and with what record, and how the text at any place
is set. It keeps nothing of the text and writes nothing: that is for
what gives a chapter's blocks to the writer of its segment ([A chapter's
bytes](segment.md)), which is not built.

**The rules it goes by.**

- **A block is made by what is in it, not by an element.** One begins at
  the first text that is more than white space, or picture, or `br`,
  where none is open, and ends where an element that is a block opens or
  closes. So an element with only blocks inside makes none of its own;
  text standing straight in a `div`, an `li` or a `td` is a block; text
  beside blocks in one element is a block before them and one after; a
  line of verse written as a `span` the book makes a block is a block;
  and an element with nothing in it makes none.
- **Its kind** is from the nearest thing around it that names one: a
  heading, with its level, inside `h1` to `h6`; a list item for the first
  block begun inside an item of a list, however deep, and a table's cell
  for the first begun inside a `td` or a `th`; and a paragraph otherwise,
  the later blocks of an item or a cell among them. An `hr` is a block
  by itself, a rule.
- **An item's marker and number** go with the first block begun inside
  the item, whatever its kind: a heading in an item has them. An item's
  number is one more than that of the item before it in its list, the
  first being the list's `start` or 1, unless its own `value` says
  another. A rule takes no marker.
- **`br`** ends a line of the block that is open, and where none is open
  it begins one, which is the empty line books mean by it. It is that
  whatever the book says a `br` is to a page, unless it says it is
  nothing.
- **Its lines** (how they are aligned, their size, the distance between
  them, a first line's indent, their direction and white space) are as
  the nearest element around it that is a block has them. What an
  element that is part of a line says of them is not the block's.
- **To each side** it has the margin and the padding of every element it
  is inside that is a block, added up, in their three units. `auto` is
  nothing there.
- **Above it** it has the largest of the margins that meet there, which
  is CSS's own rule and not their sum: the bottom margins of the elements
  that closed since the block before, and the top margins of those that
  opened. With margins below nothing among them it is the largest above
  nothing and the one furthest below it, added. Where their units
  differ, which is the largest is judged with 16 pixels to the em and a
  page 30 em wide, and the one that wins is given as it was written.
  Padding is added, and keeps apart the margins to either side of it; so
  is the height the book gave an element that has nothing in it. The
  space is said whole with the block it stands above, and nothing is
  said of a space below a block.
- **Where a page may end.** "A page ends after this" is said as "a page
  ends before" of the block that follows. What is to be kept with what
  follows, or kept together, is by the nearest block around.
- **A block that was floated, and one centred.** A block in an element
  that is a block and floated says so. One whose element has a width and
  `auto` for both side margins, which is how a book centres a block, says
  so and has the width.
- **A capital that is dropped**, in the two ways a book writes one. A
  block has the first letter's style (`p::first-letter`) of the nearest
  element around it that has one and that it is the first block of, with
  whether the rule floats the letter. And it is said to begin floated if
  its first text stands in an element that is part of a line and floated
  (`<span class="dropcap">`), with where that floated text ends. Which
  of those are capitals, and not a bullet or a number, is for what reads
  the text.
- **Who said it.** Each thing of a record comes with whether the book
  said it, as for an element. The space above is the book's if the
  margin that won was, or a padding or a height added; if nothing but
  nothing met there, it is the book's if the book said any of it. A side
  is the book's if any part of its sum is.

**What it does not say**: anything of the text itself, a link, a
picture's size, a table's rows and columns, and what an `svg` written in
the document is. Those go with what writes a chapter down.

**How much memory.** 76 bytes for each open element, 28 of them the
`StyleStack`'s, after 360 for the languages' tags, in memory its caller
gives it: 2,792 bytes for 32 open elements. An element deeper than there
is room for is counted, and takes what the deepest kept gives. If it is
a block it still ends the block that is open as it opens, and as it
closes too among the first 64 that deep; and what it says as it opens
still counts (its top margin and padding, a page that ends before it,
its first letter's style). What would have waited for it to close does
not, nor its sides, nor what it would name a block. One call does
what the `StyleStack`'s does, looks at each byte of a text once at the
most, and where a block begins looks once at each open element that is
kept; `MostWorkFor()` says how much at the most.

`bn-css cascade --blocks` and `bn-css document --blocks` print the blocks
of a document:

```
./build/host/tools/css/bn-css cascade --blocks chapter.xhtml book.css
```

For the sheet in [Style sheets](css.md) and the document in [A book's
rules, and what they come to](css-cascade.md):

```
B paragraph
R align: center book
R indent: 1.5em book
R margin-left: 0 book
R margin-right: 0 book
R space-before: 0 book
S text italic; text-book italic; family serif book
E
B heading 2
R align: center book
R size: 1500
R margin-left: 0 book
R margin-right: 0 book
R space-before: 3em book
R break-before
R keep-with-next
S size 1500; text bold; family serif book
E
B paragraph
R indent: 0 book
R margin-left: 0 book
R margin-right: 0 book
R space-before: 1.5em book
S family serif book
E
B paragraph
R indent: 0 book
R margin-left: 0 book
R margin-right: 0 book
R space-before: 1em book
R first-letter floated: size 3000 book; family serif book
S family serif book
S text italic; family serif book
S family serif book
E
```

`B` is a block beginning, with its kind; `R` a thing of its record, a
line for each that is not as a block that says nothing has it, with
`book` after what the book said; `S` how the text from there on is set;
`E` its end. A line's end is `L`, a picture `P`, and `F` is where the
floated text a block began with ends. The heading has 3 em above it, its
own `margin-top: 2em` at one and a half times the reader's size, and the
paragraph after it 1.5 em, which is the heading's bottom margin: the
paragraph's own is nothing. The last paragraph's first letter is set
three times its size and floated. With `--sum` it prints one line, with
the checksum of those lines and what was counted:

```
blocks b58446c6 elements=8 lines=35 table_bytes=307 nodes=2 level_bytes=2 left_out=0 too_deep=0 too_many_classes=0 most_work=20 blocks=4 paragraphs=3 headings=1 list_items=0 rules=0 cells=0 line_ends=0 pictures=0 markers=0 first_letters=1 floated_letters=1 begin_floated=0 floated=0 centred=0 blocks_most_work=5 work_over=0 languages=0 styles_too_deep=0 page_margins=2 inherits_of_own=0 languages_left_out=0 breaks_avoided_before=0 not_shown=0
```

`tools/check-books.py --css-report` adds the blocks up over the first
documents of each book of a folder. Over `corpus/`:

```
The blocks of the 141 documents they were looked for in: 6886 blocks:
  5359 paragraphs, 243 headings, 332 items of lists, 905 cells of tables, 47 rules
       332  in   12 books  the first block of an item, with its marker
      2876  in   17 books  a line ended by br
        31  in    6 books  a picture
         1  in    1 books  a first letter with a style of its own
         1  in    1 books  of those, floated: a dropped capital
         0  in    0 books  a block that begins in floated text
        12  in    1 books  a block in something floated
        53  in    4 books  a block centred by its margins
  the most work for one call: 17 at the median, 18 for nine in ten, 20 at the most
```

**How it is tested.** The unit tests have a test for each rule above. An
end-to-end test (`tests/e2e/blocks_tool.py`) works the blocks out a
second time in Python, another way round: where the firmware's code goes
through a document once and says of each block what it knows as the
block begins, the test keeps the whole document, lays it out as one list
of places, and answers every question from that list. The two have to
agree line for line on a document written to bring up every rule and on
200 documents and their sheets made at random. `tools/check-books.py`
holds them to each other on the first documents of real books. The fuzz
target of the table and the cascade finds a document's blocks too, with
text and pictures in the documents it is given ([Fuzzing](fuzzing.md)).
