# What a document asks for

`epub/document_sheets.h` finds the style sheets a document is to be read
with. A document is given the rules it asks for and no others, in the
order CSS gives them: `DocumentSheets` reads the document's head, reads
each sheet the head names into the book's table of rules, once for the
whole book, and gives the list that `CssCascade::Begin()` takes.

- **The sheets it links**, in the order of the links. A `link` counts if
  its `rel` has the word `stylesheet` and not the word `alternate`, its
  `type` is `text/css` or is not given, and it has an `href`. The sheet
  is the file of the book that the `href` names from the document's
  folder, whether the book's manifest lists it or not.
- **The sheets those import**, each before the sheet that imports it, as
  CSS has them, and theirs before them: four down from a link at the
  most. An import is found from the folder of the sheet that names it.
- **Its own `<style>` rules**, after all the linked sheets, as one sheet
  that stays in the table only until the next document is begun. An
  element's own `style` attribute comes over all of these, and is the
  cascade's to read.
- **A sheet named twice**, by two links or by two sheets that import it,
  stands in the list once, where it was named last: that is where its
  rules would count if it were read again.
- **Only the head is read.** The document is parsed until its `<body>`
  opens and no further, so a chapter of megabytes costs what its head
  does. A document whose first element is not `html` has no head, and is
  given no sheets.
- **Each sheet is read once for a book.** A book's sheets are known by
  their paths, and a second document that links one opens nothing but
  itself. The files are reached through `epub/book_files.h`: a book's
  files by their paths, one open at a time, which `ZipBookFiles` gives
  for a ZIP file and a test gives from memory.
- **A document can be handed over open.** `Begin()` takes a path, and the
  document is found in the book by it; or a path and the document
  already open, and then nothing is looked for but its sheets, each once
  for the book. Whoever has the book's package has where each document
  of the spine is in the ZIP file, and need not find it by its name. The
  path still says where the document's links are from. The document is
  read only as far as its head goes, and never after a sheet has been
  opened, so the one reader a book's files are opened with can hand it
  over.
- **It is done in steps.** A step opens one file of the book, or has the
  XML parser read 1 KB of the document, or the reader of CSS 1 KB of a
  sheet, or makes one move in working the list out; and never two of
  those. The tests and the fuzz target hold every step to it.

A link's `media`, and a `<style>`'s, go by the rule for `@media` blocks
([Style sheets](css.md)): taken for `all`, for `screen`, or for "not" some
other kind of device, and never with a condition in brackets, so that
what is made of a book does not depend on the device that made it. So a
sheet linked for `amzn-mobi` is not read. Neither is one linked for
`(min-device-width: 0px)`, which one public book, in seven languages, uses
to say "any newer reader" of a second sheet; its first is read.

**Nothing is left out without being counted.** Each thing that is passed
over is counted by its kind, and told to whoever is listening with its
name:

| What is left out | The kind it is counted as |
|---|---|
| A link, or a `<style>`, for some other kind of device | `link for another medium`, `style for another medium` |
| A link or an import of something that is no file of the book (`https://...`), or of a file the book does not have | `link to no file of the book`, `import of no file of the book` |
| A link after the first 8 of a document | `link after the most followed` |
| An import more than 4 sheets down from a link | `import too deep` |
| An import of a sheet that is already on the way to it | `import of a sheet on the way to it` |
| An import after the first 64 of a book | `import after the most remembered` |
| A sheet for which there is no room: the book has 64 known already, or the memory for their names is full, or the table has the 64 it holds | `sheet with no room` |
| A sheet whose file could not be read to its end; what was read of it is kept | `sheet not read to its end` |
| A `<style>` whose text does not fit in what is left of the memory for it | `style too long` |
| An `@import` inside a `<style>`, which is not followed | `import in a style` |
| A head that could not be read through, the document not being XML or its file failing; the sheets named before that are still given. A file that fails just after its head may be counted too: the parser tells of a tag some bytes after reading it, so how the bytes came decides | `head not read through` |

No book of the 548 it has been run on comes near a limit: a
document is given 7 sheets at the most, a book has 18, imports go one
down, and the largest `<style>` in a head is 2.0 KB.

**Where this is not what CSS has.** Three things, each chosen:

- A `<style>` that stands before a link is still read after the linked
  sheets, where CSS would have it before. No document of the 548 books
  has one.
- A `<style>` in the body is not seen, since the head is all that is
  read. One public book, of fixed pages, has 13.
- `<?xml-stylesheet?>` is not read. Among the 548 it is only in the SVG
  pages of one book whose spine is SVG.

And one thing that looks like a difference and is not: text inside a
comment is no `<style>`, to XML, to a browser or to this. Two books made
from Word have `<!--[if gte mso 9]><style>...` in 123 documents.

**How much memory.** The object is about 2 KB. It takes two pieces from
its caller: for the paths of the book's sheets (2 KB is advised: the
longest path in the books is 74 bytes, and a book has 18 sheets at the
most), and for one document's `<style>` text, each with a byte to end it
(4 KB is advised). It reads through the XML parser, the reader of CSS and
the table that its caller has anyway. In the books the names of a book's
sheets take 562 bytes at the most, and 31 at the median.

`bn-css sheets` runs it over every document of a book, those whose names
end in `.xhtml`, `.html` or `.htm`, in the order the ZIP file has them:

```
./build/host/tools/css/bn-css sheets book.epub --dropped
```

For a book of two documents, the first with this head,

```
<link rel="stylesheet" href="../css/book.css"/>
<link rel="stylesheet" href="../css/kindle.css" media="amzn-mobi"/>
<link rel="stylesheet" href="gone.css"/>
<style>p.first { text-indent: 0 }</style>
```

and the second with this,

```
<link rel="alternate stylesheet" href="../css/night.css"/>
<link rel="stylesheet" href="../css/poems.css"/>
```

where `book.css` imports `fonts.css`, and `poems.css` imports
`book.css`, it prints each document (`D`), the sheets it is given in the
order their rules apply (`S`), and with `--dropped` what was left out for
it (`X`); then what it all came to, with a checksum of it; the reads of
the file that the finding took; each kind of thing left out with how
many; and what the table of the book's rules takes:

```
D OEBPS/text/one.xhtml
S OEBPS/css/fonts.css
S OEBPS/css/book.css
S <style>
X link for another medium: ../css/kindle.css
X link to no file of the book: OEBPS/text/gone.css
D OEBPS/text/two.xhtml
S OEBPS/css/fonts.css
S OEBPS/css/book.css
S OEBPS/css/poems.css
sheets_documents=2 sheets_unopened=0 sheets_given=6 sheets_read=3 sheets_names_bytes=75 sheets_left_out=2 sheets_own=1 sheets_absent=0 sheets_checksum=b2af330b
sheets_reads=1 sheets_read_bytes=2478
left_out link_for_another_medium=1 link_to_no_file_of_the_book=1
css_table_bytes=100 css_table_names=3 css_table_nodes=0 css_table_rules=3 css_table_selectors=3 css_table_left_out=0
```

Three sheets were read for the six that were given, and one document has
rules of its own. The night sheet is an alternate, and was never asked
for: it is not among what was left out.

**The spine.** With `--spine` the documents are those of the book's
spine, in reading order, and each is opened from where the book's package
says it is and handed over open: which is how a reader goes through a
book, and what the book check does on a device ([Reading the books on a
card](book-check.md)). The tool reads the package and the table of contents first,
as the check does, so that its reads are the check's reads. For the same
book, whose spine has the second document first:

```
./build/host/tools/css/bn-css sheets book.epub --spine --dropped
```

```
D OEBPS/text/two.xhtml
S OEBPS/css/fonts.css
S OEBPS/css/book.css
S OEBPS/css/poems.css
D OEBPS/text/one.xhtml
S OEBPS/css/fonts.css
S OEBPS/css/book.css
S <style>
X link for another medium: ../css/kindle.css
X link to no file of the book: OEBPS/text/gone.css
sheets_documents=2 sheets_unopened=0 sheets_given=6 sheets_read=3 sheets_names_bytes=75 sheets_left_out=2 sheets_own=1 sheets_absent=0 sheets_checksum=b55acb95
sheets_reads=0 sheets_read_bytes=0
left_out link_for_another_medium=1 link_to_no_file_of_the_book=1
css_table_bytes=100 css_table_names=3 css_table_nodes=0 css_table_rules=3 css_table_selectors=3 css_table_left_out=0
```

Each document is given what it was given before; the order is the
spine's, and so the checksum is another. `sheets_absent` counts the
documents a spine names that the book has not got. (The file is so small
that the finding read nothing: it was all in the two blocks that reading
the package had left in memory.) With `--by-name` as well, each document
of the spine is found in the ZIP file by its name all the same, which
gives the same lists and costs more reads: "On real books", below, has how
many.

**The checksum** (`epub/sheets_digest.h`) is how what the documents of a
book were given is said in one number, so that a device's finding can be
set beside the desktop's, or beside another finding altogether. It is the
CRC-32 of these lines, each with its line end: for each document in turn
`D` and its path; `S` and a path for each sheet in its list, `S <style>`
for its own; and `X`, a kind and a count for each kind of thing left out
for it, in the order of the table above (`X link for another medium 1`).
A document that could not be opened is its `D` line and a line `N`. Two
findings with the same checksum gave the same documents, in the same
order, the same sheets in the same order, and left out as many things of
each kind for each.

`bn-css document` finds one document's sheets in the same way, and then
goes through the document with them as `bn-css cascade` does with the
sheets it is handed:

```
./build/host/tools/css/bn-css document --sum book.epub OEBPS/text/one.xhtml
```

```
document d3517ed8 elements=8 lines=22 sheets=3 own_style=1 sheets_left_out=2 table_bytes=97 nodes=0 level_bytes=0 left_out=0 too_deep=0 too_many_classes=0 most_work=11
```

**How it is tested.** The unit tests give it books held in memory
(`tests/support/book_fakes.h`), which count every file opened and every
byte read: that is how a test knows that a sheet was read once, that a
body of 200 KB was not read at all, and that no step did more than a step
may. Beside it goes a plain working-out of the same thing
(`tests/support/sheets_reference.h`), which reads a whole head into
lists, calls itself for each import and reads a sheet again every time it
comes to it; the two have to give every document of 300 books made at
random the same list, leave the same things out, and find as many rules
in its own `<style>` elements, every other document handed over open.
`e2e.SheetsTool`
compares `bn-css sheets` with a third finding, in Python
(`tests/e2e/sheets_tool.py`), which reads the head with Python's expat
and a sheet's imports with the second reader of CSS, on books it makes;
and compares `bn-css document` with the second working-out of the
cascade given the sheets the Python found, so that the path from a
document's bytes to an element's style is checked end to end. It gives
each of its books a package as well, whose spine has the documents in
another order, leaves some out and names one that is not there, and
holds `bn-css sheets --spine` to that order both ways, by place and by
name. The checksum is worked out three times over: by the unit tests
from lines written out by hand, by the fuzz target from what the plain
working-out gives, and by that end-to-end test from the Python's finding
and not from anything the tool printed.
`tools/check-books.py` runs these comparisons on real books ([Real
books](real-books.md)).

**On real books.** `tools/check-books.py --css-report` says what the
documents of a folder of books ask for. Over `corpus/`:

```
What documents ask for: 859 documents are given 1676 sheets, 0 of them with rules of their own:
  sheets a book's documents ask for between them: 3 at the median, 4 for nine in ten, 4 at the most
  the most sheets one document is given: 3 at the median, 4 for nine in ten, 4 at the most
  bytes for the names of a book's sheets: 38 at the median, 50 for nine in ten, 50 at the most
  nothing was left out

Through the spines, in reading order: 840 documents. Reads of the file to find their sheets:
  each document opened from where the package says it is: 938 in all; for a book, 16 at the median, 102 for nine in ten, 154 at the most
  each document found in the ZIP file by its name: 1080 in all; for a book, 17 at the median, 115 for nine in ten, 185 at the most
```

What it costs to find them is in the last two lines of the report: a
document found in the ZIP file by its name costs more reads than one
opened from where the package says it is. A ZIP file's directory is
searched from where the last search ended, which costs next to nothing
while names are asked for in the directory's own order; a spine is
often in another order, and a sheet read between two documents moves
the search as well. So a reader that has the package opens a document
from its place.
