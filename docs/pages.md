# A chapter in pages

`bn-pages` (`tools/pages`) sets a chapter of a bundle in pages and prints
them as text. The pages are the pager's ([A chapter in pages, on a
device](chapter-pager.md)): the chapter comes through the reader of its
segment ([A chapter's bytes](segment.md)), each block is set in lines by
the paragraph setter ([Setting a paragraph's text in
lines](paragraph-setter.md)), and pages are chosen among the lines by the
page chooser ([Choosing the lines of a
page](page-chooser.md)). What the tool adds is what a block's
record comes to on a page, which the pager asks and the tool answers
with rules of its own in the made-up face; the text of each line, which
the pager does not give and the tool reads again; and the printing. It
is how the format, its reader and the layout are tried together on the
desktop before there is a reader on a device.

## What it does

- **It reads a chapter only in the ways a device has**, since what reads
  it is what a device will read it with.
- **It checks the pager against itself as it goes**: every line of a
  page has to be a line of its block as the pager gives the block's lines
  when asked for those alone, with the same width, height, space above
  and marks.
- **A page is known by its first line**, and a line by its block and the
  character of the block it begins with. So a place in a chapter is a
  block and how many characters into it, however the place was given:
  as that, as a count of the chapter's characters, as a place in the
  source document with so many characters after it (which is what a
  saved position is), or as an id.
- **Three ways to a page**, each the page chooser's own:
  - *down* from a line, taking lines until one does not fit;
  - *up* to a line, for the page above one that was not come to from
    above. Where filling it up comes to a line that starts a page, it is
    that line's page filled down, as a reader going back to a chapter's
    start is shown the chapter's first page. Above a chapter's first
    line it is the last page of the chapter before that was imported;
  - *about a place* the chapter is entered at: the page begins with the
    line the place is on, or a little above it where that line is kept
    with those before.
- **`walk` tries them against each other** over a chapter or a whole
  book: every page going down from the first line, which have to hold
  every line once; then the page above each, which has to end there and
  to hold the page before; then the page each of a great many places is
  entered at, which has to hold the place and to begin with its line if
  a page may.

## How a block's record becomes a page's measures

This is the tool's own, until something sets real text in a real face:

- Widths are in quarters of a pixel, as the made-up face has them, and
  heights in pixels. The page's text is 440 pixels wide and 760 high
  unless told otherwise, and the reader's text size is 16 pixels.
- A length is its three amounts added: so much of the reader's text
  size, so many pixels, and so much of the page's width. What a book
  wrote in em is of the reader's size by the time a block is written,
  whatever the size of the block's own text.
- A block's lines are as wide as the page's text less its margins, and
  its first line less its indent as well; never wider than the page's
  text and never narrower than a tenth of it.
- A line is as high as the block's line height says (a multiple of its
  text size, or a length), and one and a quarter times its text size
  where the block says nothing. In a block that is a picture, where the
  picture comes before any text, a line is as high as the picture made to
  fit the page's width and at the most its height; as high as it is
  declared, where its size is not known; and 100 pixels where neither is
  said.
- Above a block's first line is the block's space before and the space
  after of the block before it.
- A block's first line starts a page if the block asks for a page to end
  before it, and so does a chapter's first line. It is kept with the
  line before if the block before asks to be kept with the next, and
  every other line of a block that asks to be kept together is kept with
  the line before it.
- A run's style, to the made-up face, is 1 if it is bold, 2 if italic
  and 3 if both. A picture among words is one character.

What it does not do: set text as written where a block asks for its
white space to be kept, which the setter does not do either; show a
link, a list's marker or a table as anything but text; or break words,
unless it is given a hyphenation table, and then by the one table
whatever language a run is in.

## Running it

```
./build/host/tools/pages/bn-pages page book.bundle 0 start
./build/host/tools/pages/bn-pages page book.bundle 0 id the-road
./build/host/tools/pages/bn-pages forward book.bundle 0 1 62
./build/host/tools/pages/bn-pages back book.bundle 0 2 0
./build/host/tools/pages/bn-pages walk book.bundle --pages
./build/host/tools/pages/bn-pages lines book.bundle 0
./build/host/tools/pages/bn-pages contents book.bundle 3
```

`--width` and `--height` set the page (in quarters of a pixel, and in
pixels), and `--table` gives a hyphenation table, the output of
`bn-hyphen make`.

A page is printed as a line that says the chapter, where the page begins
and where it ends (a block and a character of it, or `end`), how many
lines it has, how high they stand, and whether it was filled `down` or
`up`. Then its lines: the block, the characters of the block that are on
the line (from, and up to), how wide they are, how high the line is, the
space above it, letters for what it is (`k` kept with the line before,
`s` starts a page, `h` a hyphen is shown at its end, `l` the last of its
paragraph, or `-`), and after a bar its text. This is the first page of a
short chapter, on a page 250 pixels wide and 120 high (`--width 1000
--height 120`, as in all that follows): a heading, which is kept with
what follows, and the lines of the first paragraph that fit.

```
page chapter 0 from 0+0 to 1+127 lines 5 height 112 down
0 0 18 618 20 0 sl |The Second Chapter
1 0 31 908 20 12 k |It was a bright cold day in the
1 32 65 990 20 0 - |middle of the month, and the road
1 66 98 954 20 0 - |ran on between the fields as far
1 99 126 872 20 0 - |as anyone could see. Nobody
```

The page that begins with the paragraph's second line (`forward ... 1
62`, the line that holds its 63rd character):

```
page chapter 0 from 1+32 to 2+54 lines 6 height 120 down
1 32 65 990 20 0 - |middle of the month, and the road
1 66 98 954 20 0 - |ran on between the fields as far
1 99 126 872 20 0 - |as anyone could see. Nobody
1 127 137 288 20 0 l |was on it.
2 0 20 692 20 0 - |The second paragraph
2 21 53 918 20 0 - |begins a little way in, and says
```

And the page above the second paragraph (`back ... 2 0`). The page has
room for the paragraph's first line as well, but that line is kept with
the heading, and both do not fit: so the page begins with the first line
below that a page may begin with.

```
page chapter 0 from 1+32 to 2+0 lines 4 height 80 up
1 32 65 990 20 0 - |middle of the month, and the road
1 66 98 954 20 0 - |ran on between the fields as far
1 99 126 872 20 0 - |as anyone could see. Nobody
1 127 137 288 20 0 l |was on it.
```

`walk --pages` prints where each page going down begins, and then what
it found:

```
page chapter 0 from 0+0 lines 5 height 112
page chapter 0 from 1+127 lines 6 height 120
page chapter 0 from 3+0 lines 4 height 96
chapters 1, pages 3, lines 15, blocks 5, characters 359
pages above a page 3, and places entered at 9: all agree with the pages
checksum of where the pages begin 9388bb52
```

It ends with 1, and says which, if a page above a page or a place
entered at does not agree with the pages going down, or if the chapter
cannot be read.

## Checked

`e2e.PagesTool` (`tests/e2e/pages_tool.py`) writes chapters with
`bn-bundle write-chapter`, some made up at random and one written for
particular things, and sets what `bn-pages` prints beside a second
working-out in Python:

- the lines of every block against the block as the script has it: their
  characters, in order and with nothing but spaces left out; their
  widths, added up in the made-up face in each character's own style;
  their heights, the spaces above them, and what is kept and what starts
  a page, worked out from the block's record by the rules above;
- every page going down, the page above a line, and the page entered at
  a place (by its block, by a count of characters, by a place in the
  source, by an id), against pages the script chooses by adding the
  lines' heights up over the whole chapter; and the same on a page of
  another size;
- the page an entry of the table of contents leads to, the page above a
  chapter's first line, and a saved place found again after its chapter
  is written anew.

Whether a paragraph's lines are the best there are is not checked there:
that is `e2e.LinesTool`'s, on the setter itself.

`tools/check-books.py` has `walk` go through chapters imported from real
books, the first short ones of each or with `--walk-all` every one
([Real books](real-books.md)).
