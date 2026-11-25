# A book's rules, and what they come to

`epub/css_rules.h` keeps the rules that the reader of CSS tells of, and
says what they come to for each element of a document.

- **`CssRules`** is the table. It is what a `CssReader` is handed as its
  handler, a sheet at a time, and it keeps every sheet of a book in one
  block of memory that the caller gives it: each name once, a selector in
  about 12 bytes and a declaration in about 4. Of a list of font families
  it keeps the kind of face, in a byte, and none of the names ([Kinds of
  face](css-families.md)). Of a rule that sets a
  property more than once it keeps the declaration that would win. A rule
  there is no room for is left out whole and counted, and the rules
  before it are as they were.
- **`CssCascade`** goes through a document beside its parser. Told of each
  element as it opens and closes, it fills in a `CssStyle`: for each
  property that some rule set, the value that wins, and the same again
  for the element's first letter (`p::first-letter`). What wins is
  decided as CSS has it: `!important` first, then the element's own
  `style` attribute over any rule, then the selector's rank, then the
  later sheet, then what was written later.
- **A document is given the sheets it asks for** and no others, in the
  order it asks for them: every rule is marked with its sheet, and a
  cascade passes over the rules of sheets it was not given. Which sheets
  a document asks for is found by `DocumentSheets` ([What a document asks
  for](document-sheets.md)).
- **A document's own rules** (its `<style>` elements) are added after the
  book's and taken away again: `Here()` marks where the table stands and
  `BackTo()` forgets everything added since.
- **Nothing of the document is kept** but a few bytes for each element
  that is open. A selector can ask anything of the elements an element is
  inside and of the ones before it (`hgroup > h2 + p + p`, `tr + tr td`);
  each different thing asked is a *node*, and for an open element there
  is a bit for each node, twice: which nodes the element is, and which
  the last element to close inside it was. A node for `h2 + p` is "a `p`,
  after an element that is the node `h2`", so one remembered element
  serves a row of `+` of any length. A book with no such selectors, which
  is most trade books, keeps nothing at all. An element deeper than there
  is memory for is still given the rules that ask nothing of the elements
  around it.
- **Values are as the sheets wrote them.** `inherit`, an em and "larger"
  are given back as they are: what an element takes from the one it is
  inside is for whatever keeps the styles of the open elements, which is
  not built. The one exception is a list of font families, which is given
  as the kind of face it comes to: the `body` of the document below, which
  the sheet gives `Georgia, "Times New Roman", serif`, has
  `font-family: serif`.
- **Work is bounded.** A name is looked for among the table's by halving;
  only the selectors kept under an element's id, its classes or its name
  are tried on it, and those kept under none. `CssCascade::MostWorkFor()`
  says the most one element can cost for a table of a given size, and
  the tests and the fuzz target hold every element to it. In real books
  the dearest element costs a few hundred steps.

`bn-css cascade` runs it on a document and the sheets it is to be given:

```
./build/host/tools/css/bn-css cascade chapter.xhtml book.css
./build/host/tools/css/bn-css cascade --in book.epub OEBPS/chapter.xhtml OEBPS/book.css
```

With the sheet in [Style sheets](css.md), and this for the document,

```
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<body>
<section epub:type="dedication"><p>For A.</p></section>
<h2>One</h2>
<p>It began.</p>
<p class="first" style="margin-top: 1em">And went <i>on</i>.</p>
</body>
</html>
```

it prints a line for each element that opens (`<`) and closes (`>`), and
between them what is set for it (`P`) and for its first letter (`F`). Here
from the heading on:

```
< h2
P text-align: center
P margin-top: 2em
P margin-right: 0px
P margin-bottom: 1em
P margin-left: 0px
P break-before: always
>
< p
P text-indent: 0px
P margin-top: 0px
P margin-right: 0px
P margin-bottom: 0px
P margin-left: 0px
>
< p
P text-indent: 0px
P margin-top: 1em
P margin-right: 0px
P margin-bottom: 0px
P margin-left: 0px
F font-size: 3em
F line-height: 1
F float: left
< i
>
>
```

The first paragraph has no indent because it comes after the heading
(`h2 + p`), the second because of its class; the second's top margin is
from its own `style`. With `--sum` it prints one line instead, with the
checksum of those lines and what the table and the cascade took:

```
cascade 85c143af elements=8 lines=47 table_bytes=307 nodes=2 level_bytes=2 left_out=0 too_deep=0 too_many_classes=0 most_work=20
```

**How much memory.** `bn-css book` puts every sheet of a book into one
table and says what that took, and `tools/check-books.py --css-report`
adds it up over a folder. Over `corpus/`:

```
A table of all a book's rules, in the 28 books with style sheets:
  bytes: 2172 at the median, 4090 for nine in ten, 4498 at the most
  rules kept: 43 at the median, 87 for nine in ten, 105 at the most
  nodes: 7 at the median, 42 for nine in ten, 51 at the most
  books whose table is over 8 KB, 12 KB, 16 KB, 20 KB and 24 KB: 0, 0, 0, 0, 0
  rules left out of a table of 64 KB: 0
  bytes kept for each open element, in the documents the cascade was tried on: 2 at the median, 10 for nine in ten, 14 at the most
  the most work for one element: 42 at the median, 119 for nine in ten, 150 at the most
```

**How it is tested.** The unit tests give it sheets and documents written
for each thing it has to get right, and beside it a second working-out of
the same rules (`tests/support/css_reference.h`) that keeps the whole
document as a tree and tries every selector on every element; the two
have to agree on every element of four hundred sets of sheets and twelve
hundred documents made at random, with the cascade's memory sometimes cut
to a few bytes. `e2e.CssCascade` compares `bn-css cascade` with a third
working-out, in Python (`tests/e2e/css_cascade_tool.py`), which takes
its rules from the second reading of CSS, so that nothing of the path
from a sheet's bytes to an element's style is checked only against
itself. `tools/check-books.py` runs that comparison on real books: on
the first few documents of each, with the sheets each one asks for, which
the tool finds for itself ([What a document asks for](document-sheets.md)).
