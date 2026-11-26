# Reading the books on a card

A firmware built with `-DBN_BOOK_CHECK=ON` reads every book on the SD card
from end to end: at start, and again at each press of the Power button. (On
the desktop, set `BN_BOOK_CHECK`, with `BN_STORAGE` naming the folder that
stands for the card.) It is how to find out whether a device reads books
correctly and how fast. It writes nothing to the card.

It takes for a book every file whose name ends in `.epub` in `/books` and
in each folder in `/books`, one level down and no further. If there is no
`/books`, or no book there, it looks in the top directory and its folders
instead (`reader/book_check.h`). A file or folder whose name begins with a
dot is hidden and passed over, as everything that lists the card does: a
Mac writes a `._` file beside each file it copies to a card. With each book it does two things. First
it opens the book as a reader would: finds the ZIP directory and reads the
book's package, which says what the book is called and where its documents
are (see [What a book says of itself](package.md)); then, with the book
still open, reads its table of contents ([The table of contents](toc.md));
then finds the style sheets that each document of its spine asks for, in
reading order ([What a document asks for](document-sheets.md)), opening each document
from where the package says it is and reading its head;
and last, before closing it, reads a byte at each of sixteen places far
apart in the file, to and fro: the start, the end, the middle, and so on
by halves. That is what going to a place in a file costs, which on a card
depends on how large the file is (see [Storage](storage.md)). Then it closes the
book, opens it afresh, reads every entry to its end, which checks the
entry's size and checksum, and looks each entry up by name.
Every entry with an XML file's name (one ending in `.xhtml`, `.html`,
`.htm`, `.xml`, `.opf` or `.ncx`) is also parsed as it is read. It works
for about 20 ms a turn, so the panel and the buttons carry on around it,
and a run takes longer by the clock than the times it reports. All of it
is done in steps, each one call into what is being checked: a read from
an entry, a piece of a document parsed, a piece of a package or a table
of contents read, a document opened or a step of finding its sheets. A
turn runs over its 20 ms by however long its last
step takes, so the last line of a run says which step took longest, and
how long: on a device, that is whether those calls are as short as an
application that has a panel and buttons to see to needs them to be.

What it finds goes out as log text: a line when a book is begun, three for
its package, one for its table of contents, two for the sheets its
documents ask for, with one more for each kind of thing left out in
finding them, one for the places, four when it ends, and eleven at the
end for all of them, with those kinds again.

```
book check: the checksum alone, 262144 bytes in 798 us (320802 KB/s).
book 1: 1832425 bytes, /books/core/The First Book.epub
book 1: package: title "The First Book", author "A. Writer", language "en".
book 1: package: 374 files, 368 documents, cover jpeg, nav, ncx; memory 16288; checksum 7b162ac4.
book 1: package: 1 ms: card 0 ms for 24 reads of 90555 bytes.
book 1: contents: nav, 385 entries, 1 level; checksum 2eeff666; 0 ms: card 0 ms for 2 reads of 8192 bytes.
book 1: styles: 368 documents, 1102 sheets given, 3 read, 0 with rules of their own; checksum c31e8f44.
book 1: styles: table 2272 bytes, 46 rules, names 38 bytes; 9 ms: card 1 ms for 374 reads of 1531904 bytes.
book 1: places: 16 far apart, 16 reads in 17 us, the longest 3 us.
book 1: ok. 377 entries, 4353104 bytes out of 1769871. Directory found in 1 read, 0 ms.
book 1: card 0 ms for 468 reads of 1909179 bytes (1996180 KB/s); inflating 35 ms: 1661195 bytes in (46199 KB/s), 4244428 out (118042 KB/s); all 68 ms.
book 1: opened 4 times in 188 us, the longest 99 us; number 1 in its folder.
book 1: XML: 372 documents, 4240089 bytes, 93155 events, checksum a7e0cc8f; parsing 31 ms (131118 KB/s); most memory 11632.
...
book check: passed. 34 books, 5152 entries, 63899132 bytes out of 39526769.
book check: card 22 ms for 10091 reads of 41148789 bytes (1748666 KB/s); inflating 371 ms: 24130522 bytes in (63399 KB/s), 48502885 out (127435 KB/s); all 689 ms.
book check: XML: 2072 documents, 31585733 bytes, 1059050 events; parsing 241 ms (127717 KB/s); 32821373 bytes of events checksummed.
book check: XML memory: the most a document used 18952 bytes; documents over 8 KB: 2072, over 12 KB: 24, over 16 KB: 5.
book check: packages: 33 read; 28 ms: card 18 ms for 258 reads of 901750 bytes; the longest 2 ms; the most memory 22448. Not read: 1.
book check: contents: 33 read (25 nav), 2666 entries; 7 ms: card 0 ms for 49 reads of 195690 bytes; the longest 0 ms.
book check: styles: 1916 documents of 33 books, 3582 sheets given, 76 read, 4 with rules of their own.
book check: styles: 68 ms: card 23 ms for 1808 reads of 7368383 bytes; the longest 9 ms; the most a table took 19821 bytes, names 117.
book check: files opened 136 times in 6 ms; the longest 262 us.
book check: places: 544 reads in 19 ms; the longest 681 us.
book check: the longest step: 1132 us, opening the directory.
```

(Those figures are a desktop's, and the book's names are made up.) A
book's file is opened four times, twice to read its package and twice to
read it through; the line about that says how long those took, and where
the book comes in the listing of its folder, since a file is found by
going past the names before it. The line about the places says how many
reads of storage the sixteen came to (fewer in a file so small that one
place is in the block the one before was in), how long they took, and the
longest of them; none of that is in the book's other figures. There are
four times besides. `card` is the time
spent inside storage. `inflating` is the time spent reading out the
compressed entries with storage's share taken off, which is the processor
inflating and checksumming, beside what went into the inflater and what
came out. `parsing` is the time the XML parser took over what it was given,
with the reading and inflating of it taken off; the check's own counting
and checksumming of what the parser found is in that time, and the last
line but one says how many bytes that was. `all` is everything spent on the
book. A KB is 1024 bytes. The checksum is timed by itself first, over
256 KB held in memory, so that its share of `inflating` and of `parsing`
can be worked out. A book or an entry that cannot be read is reported with
`FAILED` and the reason, and the check goes on to the next. A file that
turns out not to be XML is counted (`Not XML: 2.`), and so is one that
wanted more than the 32 KB the parser is given (`Refused for memory: 1.`),
but neither makes the book a failure: the file is read out and checked all
the same.

The package's three lines stand apart from the rest. The first has what
the book calls itself, each name cut short with `...` if it is long. The
second has how many files its manifest lists and how many documents its
spine, whether it has a cover and each kind of table of contents, how much
of the 24 KB the package reader is given it used, and a checksum of
everything the package was found to hold (`epub/package_digest.h`);
then, if there are any, how many documents are not in the book or not in
the manifest, and whether the package file was cut short. The third has
what it cost to open the book and read that: the time, the card's share of
it, and the reads, finding the ZIP directory included. None of that is in
the book's other figures. A book whose package cannot be read has one line
that says why (`package: NOT READ: no container.`), and that does not make
the book a failure either.

The line for the table of contents has which kind was read (the nav, if
the book has one, or the NCX), how many entries it has and how many levels
they go to, a checksum of the entries (`epub/toc_digest.h`), and what
reading it cost, which is counted by itself as the package's is. It is
read a piece at a time, as the entries are. After that come how many
entries point into no document, if any do, and whether the file was cut
short. A book with no table of contents says `contents: none.`, and one
with a file that could not be read as one says why (`contents: NOT READ:
not a table of contents.`); neither makes the book a failure.

The two lines for the styles have what finding the sheets of the spine's
documents came to. The first: how many documents of the spine were gone
through; how many sheets they were given between them, a sheet counted
once for each document that has it; how many sheets were read, each once
for the book; how many documents have `<style>` rules of their own; and
a checksum of what every document was given (`epub/sheets_digest.h`).
After that, if there are any: how many documents could not be opened,
how many the spine names that the book has not got, and how many rules
the table had no room for. The second: what the table of the book's
rules takes of the 32 KB the check gives it, and how many rules are in
it, once the last document's own rules are out of it; what the names of
its sheets take; and what the finding cost, which is counted by itself
as the package's is. Then comes a line for each kind of thing that was
left out, with how many (`styles: left out 12: import of no file of the
book.`). A book whose package could not be read has no spine to go
through, and nothing is said of its styles. What the finding needs is a
second block of memory, of about 42 KB, which the check gets when it is
made: if that cannot be had, the run says so (`styles: NOT READ: no
memory for them.`) and finds no sheets.

`bn-zip check` reads a file the same way, read for read. So the same book
on the desktop should come to the same entries, the same bytes and the same
reads (`sd_reads` and `sd_read_bytes`, in the last line `bn-zip check`
prints) as it does on the device. `bn-xml book` parses the same documents,
and its last line has the count and the checksum of everything found in
them, which should be the book's on the device too. `bn-epub info` opens
the book and reads its package the same way, and prints the checksum, the
memory and the reads that the package's lines should have. `bn-epub toc`
goes on to read the table of contents, and prints its kind, its entries
and their checksum; the reads it reports are the package's and the
table's together. `bn-css sheets --spine` opens the book, reads its
package and its table of contents, and then finds the sheets of the
spine's documents the same way: its `sheets_checksum`, its counts, its
`css_table_bytes` and its `sheets_reads` are what the lines for the
styles should have.

The outcome is also in the metrics (`book_check=1` if every book was sound,
`book_check_books`, `inflate_kb_per_s`, `checksum_kb_per_s`,
`xml_kb_per_s`, `xml_memory` for the most the parser has used, and
`css_table_bytes` for the most the table of one book's rules took) and in
the trace (`reader/book_check`, and `reader/book_check_book` for each
book), where every read of the card is too (`hw/sd_read`), and the reading
of each package with its four parts (`epub/package_read`, and
`epub/package_container`, `package_file`, `package_resolve` and
`package_find_files`), and the finding of each document's sheets
(`epub/document_sheets`, with how many it was given).

```
tools/idf -DBN_BOOK_CHECK=ON build flash
./build/host/tools/trace/bn-trace | tee books.log
tools/idf -DBN_BOOK_CHECK=OFF build flash     # back to the normal firmware
```

It cannot be combined with `-DBN_STORAGE_CHECK=ON`: the two would get in
each other's way on the card. A firmware built with neither has the code
of neither in it.

With `-DBN_BOOK_CHECK_BRIEF=ON` as well (on the desktop,
`BN_BOOK_CHECK_BRIEF` set as well) the check is a brief one: each book is
opened, its package and its table of contents are read and its sixteen
places gone to, and no book is read through, nor are any document's
sheets found, which would be a reading of every document's head. That takes seconds where the
whole takes minutes, and still says whether each book opens, what it
holds, and what opening it and going about in it cost. A book's file is
then opened twice, its last line is `book 1: ok. Not read through.`, and
the run's is `book check: passed. 34 books opened, none read through.`
with nothing after it about entries or XML.

```
tools/idf -DBN_BOOK_CHECK=ON -DBN_BOOK_CHECK_BRIEF=ON build flash
```
