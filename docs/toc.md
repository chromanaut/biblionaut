# The table of contents

`epub/toc.h` reads a book's table of contents: its parts by name, each with
the place in the book where the part begins. The package reader finds the
file; `TocReader` reads it.

- **Two kinds.** EPUB 3 has a page of XHTML with a list of links in it (the
  "nav"), and EPUB 2 an XML file made for the purpose (the "NCX"). Many
  books have both, and the two are often not the same: of 138 real books
  with both, 53 had differences between them. A book with both is read from
  its nav unless the NCX is asked for.
- **Entry by entry.** Each entry is given to a `TocHandler` as it is found,
  an entry before the entries under it: what it is called, how far in it
  is, the document it points into, and the place within that document. None
  of it is kept, so a table of contents of any length costs the same
  memory: the reader, which is about 1 KB, and what the XML parser needs.
- **What an entry is.** In an NCX: each `navPoint` in the `navMap`, by the
  text of its label and the `src` of its content, nested as the points
  are. In a nav: within the `nav` element whose `epub:type` is `toc`, each
  link in a list, and each `span` that stands where a link would as a
  heading with no link, nested as the lists are. Page lists, landmarks and
  whatever else the two files hold are passed over.
- **Where an entry points.** Its link is worked out as any link in a book
  is, from the folder the table of contents is in, and looked up in the
  package (`Package::SpineIndexOf`): the answer is the document's place in
  the spine, with what the link has after its `#` as the place within the
  document. An entry that points at nothing, or at something that is no
  document of the spine, has no document.
- **Labels** have every run of spaces and line ends made one space, and the
  text of whatever is inside a link (italics, a line break) is part of
  the label. One longer than 256 bytes is cut at a character and marked.
- **In pieces.** `Begin()` and then `Step()`, which reads 1 KB of the file
  each time, so that a long table of contents can be read a little a turn;
  or `Read()` for all of it at once.
- **Speed**: on an X4, reading the table of contents took 4 to 159 ms for
  each of 33 books, 18 ms at the median, for tables of 1 to 385 entries:
  about 0.5 ms an entry over all of them. Under a fifth of it is the
  card, which gave most tables in one read or two; the rest, inflating
  and parsing the file and placing each entry, went at about 450 KB of
  the file a second. Counted from opening the book, with its package read
  first, the contents were in hand after 36 to 433 ms, 64 ms at the
  median.
- **Bad input**: the file is not trusted. Whatever bytes it holds, reading
  ends with an answer: `kNoToc` for a book with none, `kNotToc` for a file
  with no table of contents in it, `kNoMemory`, `kUnreadable`. A file that
  stops being XML part way gives the entries that came before the mistake,
  and `CutShort()` says that it did.

`bn-epub` prints one:

```
./build/host/tools/epub/bn-epub toc book.epub
./build/host/tools/epub/bn-epub toc book.epub --ncx
```

For a small book with an NCX, in two parts:

```
toc: ncx
0	0	-	Part 1
1	0	s0	Chapter 0: The Tale & Its Telling
1	1	-	Chapter 1: The Tale & Its Telling
1	2	-	Chapter 2: The Tale & Its Telling
1	3	s3	Chapter 3: The Tale & Its Telling
1	4	-	Chapter 4: The Tale & Its Telling
1	5	-	Chapter 5: The Tale & Its Telling
1	6	s6	Chapter 6: The Tale & Its Telling
1	7	-	Chapter 7: The Tale & Its Telling
1	8	-	Chapter 8: The Tale & Its Telling
1	9	s9	Chapter 9: The Tale & Its Telling
0	10	-	Part 2
1	10	-	Chapter 10: The Tale & Its Telling
1	11	-	Chapter 11: The Tale & Its Telling
entries: 14
checksum: 5d9afae8
cost: packages_read=1 packages_failed=0 xml_documents=3 xml_failures=0 zip_finds=0 zip_records=21 sd_reads=2 sd_read_bytes=7338 sd_writes=0 sd_write_bytes=0 sd_syncs=0 sd_seeks=0 xml_memory=10144[8752..10144]
```

The first line says which kind was read. Then a line an entry, with tabs
between: how far in it is, from 0; the document it points into, by its
place in the spine from 0, or `-` for none; the place within the document,
or `-` for its start; and what it is called. Then how many there were, a
checksum of them (`epub/toc_digest.h`), which is what a device's book
check reports for the same book, and the counters after reading the
package and the table. `--nav` and `--ncx`
ask for that kind; it exits with 1, and says why, if there is none to read.

The unit tests say what should be made of tables of contents written for
them, of both kinds, and of every bit of one turned over and every length
it can be cut to. The end-to-end test (`tests/e2e/epub_tool.py`) makes
books whose two tables differ, and compares `bn-epub toc` with a reading of
each by Python's own XML parser, written separately for the test: every
entry has to agree.
