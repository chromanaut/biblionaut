# ZIP files

An EPUB is a ZIP file, and `epub/zip.h` reads one, going by the directory
at the end of the file:

- `ZipArchive` finds that directory and then finds entries in it, by name
  (`Find`) or one after another (`Next`). It is 24 bytes: it keeps no list
  of the entries, however many there are, and reads the directory from the
  file when asked. Each search starts where the last one ended, so asking
  for entries in the file's own order reads the directory once, not once
  for each.
- `ZipEntry` is what the directory says of one entry: 24 bytes that can be
  kept, in memory or in a file, to read the entry later without finding it
  again.
- `ZipDirectorySum` works out the checksum of the directory, the bytes as
  they are in the file, a block of the file a step. The directory has
  every entry's name, size and checksum in it, so it stands for the whole
  file: a book's bundle keeps it, to know later whether the EPUB is still
  the one it was made from ([The file a book is imported
  into](bundle.md)).
- `ZipEntryReader` gives an entry's bytes as a `ByteSource`, read from the
  file and inflated as they are asked for. It gives out no more than the
  directory says the entry holds, and at the end checks that the data
  stopped there and has the checksum the directory gives. It is 2.7 KB on
  the device and needs the inflater's 32 KB window.
- A name is the bytes the file has for it, and two names are the same if
  their bytes are. A ZIP file has a flag to say that its names are UTF-8,
  which nothing here looks at: a book's names are UTF-8 by the EPUB
  standard, and some books' ZIP files leave the flag off all the same (one
  Japanese book of some 500 tried does). A reader that goes by the flag
  cannot find such a book's chapters.

All three read through a `BlockFile`. Finding entries and reading them can
share one, but each step from an entry back to the directory then reads a
block again: reading every entry of a 3,000-entry file and looking each up
by name took 6,189 reads of storage that way and 242 with a `BlockFile`
each for the directory and the entries.

The file is not trusted: whatever it holds, every call ends with an answer.
What cannot be read is reported as such (`kUnsupported`): ZIP64, a file in
several parts, an encrypted entry, and compression other than DEFLATE.

`bn-zip` runs the same code on the desktop:

```
./build/host/tools/zip/bn-zip list book.epub
./build/host/tools/zip/bn-zip check book.epub
./build/host/tools/zip/bn-zip cat book.epub META-INF/container.xml
```

`list` prints a line for each entry. `check` reads every entry to its end,
looks each one up by name, and says how many reads of the file that took.
`cat` writes one entry out.

The unit tests make their ZIP files with a writer of their own, so that
they can get any one field wrong. Because a writer and a reader by the same
hand could share a mistake, an end-to-end test (`tests/e2e/zip_tool.py`)
also compares `bn-zip`, entry by entry and byte by byte, with Python's
`zipfile` on files made by `zipfile` and, where there is one, by the `zip`
command.
