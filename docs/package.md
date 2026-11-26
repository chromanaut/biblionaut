# What a book says of itself

`epub/package.h` reads the two files an EPUB describes itself with:
`META-INF/container.xml`, which says where the package file is, and the
package file (the OPF), which lists every file of the book and the order
its documents are read in. It is read a piece at a time, `Package::Begin`
and then `Package::Step` until there is no more (`Package::Read` is both,
for a tool or a test), and gives back:

- the title, the author and the language: the first of each that has
  anything in it, with its spaces tidied;
- the identifier: the `dc:identifier` the package names as its unique one
  (`unique-identifier`), if that has anything in it, and otherwise the
  first that has. It is what the book is known by to the world, such as
  an ISBN or a UUID; two books can give the same, and a book need not
  give one;
- the spine, which is the documents in reading order, each with the ZIP
  entry it is in, its kind, and whether the book counts it as part of the
  run of the text (`linear="no"` marks one that is not);
- the entry of the cover picture, of the EPUB 3 table of contents (the
  nav document) and of the EPUB 2 one (the NCX), where the book has them.
  The tables of contents are found here and read by `TocReader` ([The
  table of contents](toc.md));
- how many of the manifest's files are pictures (JPEG, PNG, GIF or SVG by
  what the manifest says of them), and which of them a path is
  (`PictureOf()`): a picture is known by its place among them, in the
  manifest's order. Nothing more is kept for that: the place is counted,
  from the manifest's 16 bytes a file, when it is asked for. Where the
  manifest has a file more than once, the first of those is the picture.

How it does that:

- **The manifest is kept as numbers, not as text.** A book lists its files
  twice over: the package names each by an id and a path, and the ZIP file
  has an entry for each path. For every file the package lists, 16 bytes
  are kept: a number of 48 bits worked out from its id, another from its
  path (`NameHash`, in `epub/book_path.h`), its kind, and a few notes. A
  document of the spine is 28 bytes. So a book of 800 files and 400
  documents is read in 24 KB, whatever the length of its names.
- **The ZIP directory is gone through once.** When the package has been
  read, every entry of the ZIP file is taken in the file's own order, its
  name turned into the same kind of number, and that looked for among
  the files that are wanted: the documents of the spine, the cover and the
  tables of contents. It stops when it has them all. Looking each file up
  by name instead would read the directory again for every one, since a
  ZIP file's order is seldom the package's.
- **A piece at a time.** How long the whole reading takes is the book's
  to decide, and a book can be made to make it long: a package file of
  megabytes, a ZIP file of tens of thousands of entries, one listed file
  that is not there. How long a step takes is not the book's to decide. A
  step looks at 16 entries of the ZIP directory, or as many as lie in
  4 KB of it; or parses 1 KB of a file; or makes about 16,000
  comparisons among the files of the manifest. With files read 4 KB at a
  time, that is five reads of storage at the most, and usually one or
  none. `container.xml` and the package file are found by going through
  the directory in such steps too, the second from where the first was,
  and not by the ZIP reader's `Find`, which goes on until it has an
  answer. Until the last step the book's documents, cover and tables of
  contents are not to be had: the spine is empty.
- **Paths** in the package are worked out the way a link is
  (`ResolveBookPath`): from the folder the package file is in, with `..`
  and `.` followed (never to above the top of the book), `%20` and the
  like turned back into bytes, and anything after a `#` dropped. A
  reference to somewhere else, such as an address on the internet, is not
  a file of the book and is left out.
- **Memory**: the caller gives a block, and the records are made in it,
  files from one end and documents from the other. Nothing comes from the
  general heap. A book that needs more than the block is refused
  (`kNoMemory`). The parser's memory is the caller's as well
  (`XmlParser`).
- **Speed**: on an X4, opening a book and reading its package took 26 to
  301 ms for each of 33 books, 43 ms at the median; the largest lists 745
  files, and used 22.4 KB of the 24 KB it was given. Nearly half of the
  time is the card, at about 5 ms a read, and most of the rest is parsing
  the package file. Matching the spine to the manifest is a few
  milliseconds at the most. A thirty-fourth book, which lists 2,384
  files, was refused for want of memory after 0.5 s. None of that is one
  long call: over those 34 books the longest single step of the whole
  book check was 24.6 ms, and it was opening a file, which is the card's
  file system finding a name in its folder. Read in one go, as it used to
  be, the same packages took 8% less time in all and up to 463 ms in one
  call.
- **Two names with one number.** Nothing checks a name against the text
  it came from, so two different names in one book that came to the same
  number would be taken for each other. With 48 bits, that is one book in
  some 500 million for a book of a thousand files. `ReadZipEntryName`
  reads an entry's name back out of the ZIP file, for whoever needs to be
  sure.
- **What is put up with**, because real books do it: metadata written
  with capitals or with any prefix (`dc:Title`); an id in the spine that
  the manifest has not got (left out, and counted); a file the package
  lists that is not in the ZIP file (kept, and marked as not there); a
  cover named by EPUB 2's `<meta name="cover">` or by EPUB 3's
  `properties="cover-image"`, the second being believed first; a
  container that names its package with `%20` in the path; a package
  that stops being XML after its spine (`CutShort()`).
- **Bad input**: neither file is trusted. Whatever bytes they hold,
  reading ends with an answer: `kNoContainer`, `kNoPackage`,
  `kNotPackage` for one with no documents in its spine, `kUnreadable` for
  a ZIP file damaged where it had to be read, or `kNoMemory`. After any
  of those the spine is empty. Its tests read every book in steps and
  hold each step to five reads of storage: the books made to be awkward
  (five thousand entries, a megabyte of package file, a directory whose
  records lie 40 KB apart) and the damaged ones alike.

`bn-epub` runs the same code on the desktop:

```
./build/host/tools/epub/bn-epub info book.epub
./build/host/tools/epub/bn-epub info book.epub --memory 4096
```

For a small book with some things wrong with it, it prints:

```
title: Untidy
author: -
language: -
package: deep/er/package.opf
manifest: 5
cover: -
nav: -
ncx: deep/er/toc.ncx
spine: 4
+ xhtml deep/er/cover.xhtml
+ xhtml ?
+ xhtml deep/er/one.xhtml
- xhtml deep/er/cover.xhtml
unknown: 2
checksum: cd21187e
memory: 248
cost: packages_read=1 packages_failed=0 xml_documents=2 xml_failures=0 zip_finds=0 zip_records=16 sd_reads=2 sd_read_bytes=2504 sd_writes=0 sd_write_bytes=0 sd_syncs=0 sd_seeks=0 xml_memory=9536[8752..9536]
```

A `-` for the author, the cover or any of the first lines means the book has
none. Each line of the spine is `+` for a document in the run of the text or
`-` for one set aside, then its kind, then the name of its entry in the ZIP
file, or `?` where the ZIP file has no such file. `unknown` (printed only
when there are any) counts the documents the spine named that the manifest
has not got. `checksum` is of everything above it (`epub/package_digest.h`),
with each file given by where its entry is and not by its name: it is what a
device's book check reports for the same book, so the two can be compared.
`memory` is how many bytes of the block were used, and `cost` is the
counters after the reading: here 16 records of the directory and two reads
of the file. (`zip_finds` counts searches by the ZIP reader's `Find`, which
reading a package does not use: it looks for its two files itself, a piece
at a time.)
`--memory` holds it to a block of that many bytes, as a device would; it
exits with 1, and says why, for a file it cannot read as a book.

The unit tests say what should be made of books written for them, in
either version of EPUB and with their files in any order in the ZIP file,
and of every bit of a package file turned over and every length it can be
cut to. An end-to-end test (`tests/e2e/epub_tool.py`) makes books and
compares `bn-epub` with a reading of them by Python's own XML parser and
ZIP reader, written separately for the test: title, author, cover,
tables of contents and every document of the spine have to agree.
