# Style sheets

`epub/css.h` reads CSS: a book's style sheets, the `<style>` elements of
its documents and the `style` attributes of their elements. It reads a
sheet and says what is in it. Which rules apply to which element is not
its business: that is the cascade's ([A book's rules, and what they come
to](css-cascade.md)).

- A sheet is read from a `ByteSource`, 1 KB at each `Step()`, and never
  held whole. What is found is told to a `CssHandler` as it is found: a
  rule's beginning, each of its selectors, each thing it sets, its end; an
  `@import`; and everything that was passed over, with what it was.
- **Selectors**: by element, class and id, joined by descent (`div p`),
  as child (`div > p`) or as the element straight after (`h2 + p`), with
  `*` for any element. `:link` is taken as always so, and `::first-letter`
  is read, for drop caps. A selector with anything else in it is dropped:
  any other pseudo-class or pseudo-element, `~`, a namespace before an
  element's name. The others of its rule are kept, and a rule left with
  none is dropped whole. Each selector comes with its rank as CSS has it:
  its counts of ids, of classes and of element names.
- **Attributes**: a part of a selector may ask of an attribute that it is
  there (`[hidden]`), or hold its value against another in any of CSS's
  six ways (`=`, `~=`, `|=`, `^=`, `$=`, `*=`), with the value in quotes or
  as a name, and `i` after it for any case. An attribute's name is given
  as a document writes it, in small letters: `epub|type` in a sheet is
  `epub:type`, since the XML parser gives names with their prefixes and
  does not work namespaces out. So a sheet and a document have to use the
  same prefix. In 520 books the sheets' prefixes are `epub` and `xml` and
  no other, and every document that has such an attribute writes
  `epub:type`. `[*|type]` is the name with any prefix, or none. Each thing
  asked counts in the rank as a class does. A selector may ask 8 things of
  attributes; none in those books asks more than 3.
- **Properties**: `font-style`, `font-weight`, `font-variant`,
  `font-family`, `font-size`, `line-height`, `text-align`, `text-indent`,
  `text-decoration`, `vertical-align`, the four sides of `margin` and of
  `padding`, `display`, `float`, `width`, `height`, and where a page may
  break (`page-break-*` and `break-*`, as one). `margin`, `padding` and
  `font` are taken apart into those. Any other property is dropped, and
  so is a value that is not understood. `!important` is told with the
  declaration.
- **Lengths** are given as a number of thousandths and one of four units:
  pixels, ems, rems and percent. Points, inches and the rest are turned
  into pixels (12pt is 16px), and `ex` into half an em. A length that
  depends on the screen (`vh`, `vw`) is not read.
- **`@media`**: the rules in a block are read if the block is for `all` or
  `screen`, or for "not" some other kind of device (`not amzn-mobi`, which
  six of 335 trade books use, 709 times between them), and has no
  condition in brackets. A
  block for print, for one maker's device, or for a screen of some size is
  passed over. The screen is not asked its size because what is made of a
  book must not depend on which device made it.
- **Other at-rules**: `@import` is told to the handler, if it is for a
  reader of books by the same test. The rest are passed over, with their
  blocks: `@font-face`, `@page`, `@supports`, `@namespace`, `@charset`.
- **Mistakes** stop nothing. Whatever bytes a sheet has, reading goes on
  to its end, dropping the declaration or the rule that cannot be made
  sense of, as CSS's own rules for mistakes have it. In one thing it is
  kinder than they are: a bracket left open ends with its rule, where by
  the rules it would take the rest of the sheet with it. A list of
  selectors over 1,024 bytes, or a declaration over 512, is dropped; the
  longest in the 1,027 sheets of 520 books are 649 and 179.
- **Memory**: about 2 KB of its own, for the selectors or the declaration
  in hand. Nothing from the heap.
- **What is passed over is counted**, by kind, and told to the handler
  with its text, so that what a collection of books loses to all of the
  above can be added up (below).

`bn-css` runs the same code on the desktop:

```
./build/host/tools/css/bn-css rules book.css
./build/host/tools/css/bn-css style 'margin: 1em 0; font-weight: 700 !important'
./build/host/tools/css/bn-css book book.epub --dropped
```

`rules` prints what the reader finds in one sheet, a thing a line. For
this sheet

```
@import url("fonts.css");
body { margin: 0 5%; font-family: Georgia, "Times New Roman", serif }
h2 { text-align: center; page-break-before: always; margin: 2em 0 1em }
p { text-indent: 1.5em; margin: 0; widows: 2 }
h2 + p, p.first, p:first-child { text-indent: 0 }
section[epub|type~="dedication"] > p { text-align: center; font-style: italic }
p.first::first-letter { float: left; font-size: 3em; line-height: 1 }
@media amzn-mobi { p.first::first-letter { float: none } }
a:link { text-decoration: none; color: #036 }
```

it prints

```
I fonts.css
{
S 0,0,1 body
D margin-top: 0px
D margin-right: 5%
D margin-bottom: 0px
D margin-left: 5%
D font-family: georgia, times new roman, serif
}
{
S 0,0,1 h2
D text-align: center
D break-before: always
D margin-top: 2em
D margin-right: 0px
D margin-bottom: 1em
D margin-left: 0px
}
{
S 0,0,1 p
D text-indent: 1.5em
D margin-top: 0px
D margin-right: 0px
D margin-bottom: 0px
D margin-left: 0px
X property: widows
}
{
S 0,0,2 h2 + p
S 0,1,1 p.first
X pseudo-class: :first-child
D text-indent: 0px
}
{
S 0,1,2 section[epub:type~="dedication"] > p
D text-align: center
D font-style: italic
}
{
S 0,1,2 p.first ::first-letter
D float: left
D font-size: 3em
D line-height: 1
}
X media block: amzn-mobi
{
S 0,1,1 a
D text-decoration: none
X property: color
}
```

`{` and `}` are a rule's beginning and end, `S` a selector after its rank,
`D` something the rule sets, `I` an import and `X` something passed over,
with its kind. `style` prints the same for a `style` attribute. `book`
reads every sheet in an EPUB as it comes out of the ZIP file, and prints
for each how it ended, the checksum of what `rules` would print for it,
its rules, the declarations in them and how many things were passed over;
with `--dropped`, also each different thing that was passed over and how
many times. For Standard Ebooks' *Flatland*, which is in `corpus/`:

```
ok 660f898c     38     89     53 epub/css/core.css
ok b1313d47      9     27     18 epub/css/local.css
ok 4bbcc118     15     33     13 epub/css/se.css
css_sheets=3 css_failed=0 css_bytes=10955 css_rules=62 css_declarations=149 css_dropped=84
css_table_bytes=3148 css_table_names=56 css_table_nodes=31 css_table_rules=55 css_table_selectors=95 css_table_left_out=0 css_table_sheets_left_out=0
```

The last line is what keeping all of those rules in one table takes ([A
book's rules, and what they come to](css-cascade.md)).

The unit tests say what the reader should make of sheets written for
them: every property and unit, every kind of selector and of mistake, the
same sheet cut into pieces of every size, and four hundred sheets damaged
at random. An end-to-end test (`tests/e2e/css_tool.py`) compares `bn-css`
with a second reading of the same sheets. Python has no reader of CSS of
its own, so the second reading is written in the test, another way round
(the whole sheet at once, comments out first); every line has to agree,
on sheets the test makes, on 600 of them damaged at random, and, through
`tools/check-books.py`, on the sheets of real books.

**What books lose to it.** `tools/check-books.py --css-report` adds up
what was passed over across a folder of books. Over `corpus/` it begins:

```
Style sheets: 1406 rules read, with 3660 declarations; 963 things passed over, in 28 books:
       604  in   28 books  property
       134  in   15 books  pseudo-class
        78  in   26 books  at-rule
        76  in   17 books  rule
        33  in    9 books  pseudo-element
        19  in    9 books  media block
        17  in   17 books  value
         2  in    1 books  sibling selector
  property, the 30 passed over most of 40:
          75  in   15 books  all
          57  in   19 books  color
          39  in   24 books  border
          39  in   19 books  background-color
```

and goes on to name the pseudo-classes, at-rules and media blocks. These
books are from Project Gutenberg, Standard Ebooks and IDPF, whose sheets
lean on selectors more than most: one rule in twenty is dropped for its
selectors, most of them for `:root`, which begins the rules that
Standard Ebooks' sheets keep for one maker's reading application.
