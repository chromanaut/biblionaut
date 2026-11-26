# What each element comes to

A cascade says what a book's rules set for an element, as the sheets wrote
it: `1.5em`, `larger`, `inherit`, and nothing at all for most properties
of most elements ([A book's rules, and what they come
to](css-cascade.md)). `epub/style_stack.h` works the rest out. A
`StyleStack` goes through a document beside the cascade and says of each
element, as it opens, what it comes to:

- **what it takes from the element it is inside**: the size of its text
  and how the text is set, its language, how its lines are aligned and
  spaced and how far a first line is set in, and what stands before the
  items of a list;
- **what it is given by being the element it is**: an `h1` is large and
  bold, a `blockquote` stands in from both sides, `i` is italic, a `pre`
  keeps its white space;
- **what the book's values come to as amounts**: a size as so many
  thousandths of the reader's own, and a length as three amounts added
  together, so much of the reader's text size (in 64ths), so many pixels
  (in 8ths) and so much of the page's width (in 64ths of a percent), so
  that lengths in different units can be added up.

Each thing it says comes with whether the book said it or the element
gives it, so that a reader's own choices can stand in for a book's styles
without the book being read again. What makes blocks of text out of
what it says, which elements' text goes into which paragraph and the
space between one and the next, goes through a document with it ([A
document's blocks](document-blocks.md)).

**The rules it goes by.**

- **A block, part of a line, or nothing.** The book's `display` decides
  where it says `block`, `inline`, `list-item` or `none`; otherwise it is
  what the element is, by HTML's own list. `inline-block` changes
  nothing, and `initial` is part of a line, as the property starts. An
  element with `hidden`, and everything inside an element that is
  nothing, is nothing; so are `head`, `script`, `style` and their kin.
  Mathematics (`math`) and the notes of a `ruby` (`rt`) are
  nothing too, since a page of text cannot show them, and each is
  counted; a ruby's own text is kept. A drawing written in a document
  (`svg`) is not told apart from other elements yet.
- **Sizes.** An em and a percentage are of the size of the element
  outside, a rem of the size of the document's own element, a pixel a
  sixteenth of the reader's size. `smaller` and `larger` are a step of
  1.2. The sizes by name are CSS's, with `medium` the reader's: `small`
  is 813 thousandths, `x-large` 1,500. A heading's own size, and those
  of `small`, `big`, `sup` and `sub`, are of the size outside; what the
  book says of such an element's size is of the size outside too.
- **How text is set.** Bold is a weight of 600 or over, or `bolder`.
  Underlining and striking add up through the elements a text is inside,
  and nothing takes away a line drawn by an element outside. Text is
  raised or lowered by `vertical-align` on something that is part of a
  line (`super`, `top`, `text-top` or a length above nothing raise it;
  `sub`, `bottom`, `text-bottom` or a length below nothing lower it), and
  stays so through what is inside it as far as the next block. The kind
  of face is the book's ([Kinds of face](css-families.md)), or the
  element's: `pre`, `code`, `kbd`, `samp` and `tt` are monospace.
- **A language** is the nearest said, by `xml:lang` before `lang`. A
  document's elements may name 15 between them, of 23 bytes each.
- **A block's lines.** Their alignment is the book's, by a rule or by
  `align`; `center`, `th` and `caption` are centred by being what they
  are. A first line's indent is an amount once it is said, and passes to
  the elements inside as that amount. The distance between lines is a
  multiple of the text's size (a plain number, an em or a percentage) or
  a length.
- **Lists.** What stands before an item is by its list, a disc, a circle
  or a square by how deep among lists a `ul` is and a number for an
  `ol`; then as `type` says; then as the book says
  (`list-style-type`).
- **The space around an element** is its own. The book's lengths are
  turned into the three amounts, an em by the element's own size. What
  elements are given is HTML's: a paragraph has an em above and below, a
  heading its own, a `blockquote` 40 pixels to each side, a list 40
  pixels of padding, and a list inside a list, a `dl` among them, no
  space above or below.
- **Where a page may end.** A break before or after is told as that;
  "avoid" after an element is "kept with what follows", and inside it
  "kept together".
- **A first letter** (`p::first-letter`) is told as the text of that
  letter is to be set, beginning from its element's text, and with
  whether the rule floats it, which is how a book asks for a capital that
  is dropped.
- **`inherit`** is what the element outside has, for what passes from
  one element to those inside it. For what is an element's own (its
  margins and padding, its width and height, `display`, `float`, where a
  page breaks) it is taken as nothing said, and counted. `initial` is
  the property's own start.

**Where this is not what a browser does**, each chosen, beside the
mathematics, the notes of a ruby and the `inherit` above:

- `body` has no margin of its own, where a browser gives it 8 pixels.
- The margins and the padding the book gives `html` and `body` are left
  out, and counted. They are the book's page margins, and a page's
  margins are the reader's.
- A link is not underlined for being one. An underline the book asks for
  is kept, as the book's.
- A heading is kept with what follows it.
- A distance between lines given in em or as a percentage stays a
  multiple of the text's size for the elements inside, as a plain number
  does, where a browser fixes it as a length at the element.

**What is not taken as written is counted**, by kind: an element deeper
than is kept, a margin or a padding of the page, `inherit` for what is an
element's own, a language there was no room for, "no page may end
before this", which nothing keeps, and mathematics or a note of a ruby.

**How much memory.** 28 bytes for each open element, after 360 for the
languages' tags, in memory its caller gives it. An element deeper than
there is room for takes what the deepest that is kept gives, as if it
were straight inside that one. One call goes once through the element's
attributes, and through the languages that are kept if the element names
one, and does a fixed amount of work besides; `MostWorkFor()` says how
much at the most, and nothing in a document or a sheet makes it more.

`bn-css cascade --styles` and `bn-css document --styles` print what each
element comes to, in place of what the rules set for it:

```
./build/host/tools/css/bn-css cascade --styles chapter.xhtml book.css
```

For the sheet in [Style sheets](css.md) and the document in [A book's
rules, and what they come to](css-cascade.md), from the heading on:

```
< h2
C display: block
C size: 1500
C text: bold
C family: serif book
C align: center book
C margin-top: 3em book
C margin-right: 0 book
C margin-bottom: 1.5em book
C margin-left: 0 book
C break-before
C keep-with-next
>
< p
C display: block
C family: serif book
C indent: 0 book
C margin-top: 0 book
C margin-right: 0 book
C margin-bottom: 0 book
C margin-left: 0 book
>
< p
C display: block
C family: serif book
C indent: 0 book
C margin-top: 1em book
C margin-right: 0 book
C margin-bottom: 0 book
C margin-left: 0 book
F first-letter: floated
F size: 3000 book
F family: serif book
< i
C text: italic
C family: serif book
C indent: 0 book
>
>
```

A line is said for each thing that is not as an element that says nothing
and is inside nothing would have it, with `book` after what the book said.
The heading's size is 1,500 thousandths of the reader's, by being an
`h2`, and its `margin-top: 2em` is 3 em of the reader's size. The kind of
face is the book's all the way down, from the `body`; the `body`'s own
side margins, 5% each, are left out. `F` lines are for a first letter:
three times its paragraph's size. With `--sum` it prints one line, with
the checksum of those lines and what was left out:

```
styles 102c8947 elements=8 lines=62 table_bytes=307 nodes=2 level_bytes=2 left_out=0 too_deep=0 too_many_classes=0 most_work=20 languages=0 styles_too_deep=0 page_margins=2 inherits_of_own=0 languages_left_out=0 breaks_avoided_before=0 not_shown=0
```

`tools/check-books.py --css-report` adds up what the stack of styles says
over the first documents of each book of a folder. Over `corpus/`:

```
What each element comes to, in the 141 documents the stack of styles was tried on: 15299 elements, of which 75699 things are said:
  languages a document's elements name between them: 1 at the median, 2 for nine in ten, 3 at the most
  left out:    140  in   14 books  a margin or a padding of the page
  left out:     72  in    1 books  mathematics, or a note of a ruby
  left out:      2  in    2 books  no page to end before an element
```

**How it is tested.** The unit tests have a test for each rule above,
with what a few elements written for it come to. An end-to-end test
(`tests/e2e/styles_tool.py`) works the same out a second time in Python,
another way round: it takes what the rules set from the second
working-out of the cascade, keeps the whole document as a tree, has its
own list of what HTML's elements are given, and goes down the tree. The
two have to agree line for line on a document written to bring up every
rule and on 160 documents and their sheets made at random, some of them
deeper than is kept and naming more languages than there is room for.
`tools/check-books.py` holds them to each other on the first documents of
real books. The fuzz target of the table and the cascade takes a stack of
styles through every document too ([Fuzzing](fuzzing.md)).
