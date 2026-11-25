# XML

`epub/xml.h` reads XML: the files an EPUB describes itself with, and its
chapters. The parsing is done by [expat](https://libexpat.github.io);
`XmlParser` is what the rest of the code sees of it.

- A document is read from a `ByteSource`, 1 KB at each `Step()`, and never
  held whole, so a chapter can be parsed as it comes out of the ZIP file
  and the inflater. What is found is told to an `XmlHandler` as it is
  found: an element beginning, with its attributes; an element ending;
  text; an entity that was not expanded.
- **Memory**: the parser is given a block of memory when it is made, and
  everything expat needs comes out of that, through a small heap of our
  own that lives inside the block (`core/region_heap.h`). Nothing comes
  from the general heap. That heap checks every piece it is handed back
  by going along its own records to it, so a mistake of the library's
  with a pointer (one given back twice, or kept from an earlier
  document, or pointing into the middle of something) is turned away
  and counted, and cannot make the heap give out memory outside the
  block. A check goes past 17 pieces on average, and the parser asks
  for one about once in every 12 to 25 things it reports, which on the
  desktop is under 1% of its time. A document that needs more than the
  block is
  refused (`kNoMemory`), which in practice means one with a single tag
  or comment several kilobytes long, since a whole one has to be held at
  once, or with elements nested hundreds deep. Text can be any length. On
  an X4, of the 2,072 documents of 34 books chosen to differ from one
  another, all but 8 used less than 12 KB. The most was 17.1 KB, for a
  document that begins with a comment of 5.5 KB.
- **Speed**: on an X4, over the same documents, it took in about 960 KB a
  second with every event counted and checksummed. Book by book that was
  from 640, for one thick with markup, to 1,890, for Chinese prose with
  few tags, and 940 at the median.
- **Bad input**: the document is not trusted. Whatever bytes it holds,
  parsing ends with an answer, and one that is not XML is refused with
  expat's error number and the line it is on. What was found before the
  mistake has been told by then.
- **Entities are not expanded.** The five every XML document has (`&amp;`
  and the rest) and numbered characters (`&#160;`) come out as text. An
  entity of HTML's, such as `&nbsp;`, is told to the handler by name if
  the document names a DTD, as XHTML with a `PUBLIC` doctype does; in a
  document that names none it is a mistake by XML's rules, and parsing
  stops there. An entity a document declares for itself comes out as the
  reference was written. So there is nothing a document can make expand
  without end. In the value of an attribute, which is given whole, there
  is nowhere to tell of a reference by name: one the document does not
  declare is left out of the value, and nothing says so
  (`alt="caf&eacute;"` arrives as `caf`).
- **Stopping, and the handler's manners**: a handler may stop the parser
  from inside one of its own calls, and is then told nothing more, not
  even the end of an element written as one tag (`<br/>`) whose beginning
  it was being told of; the parsing ends as `kStopped` whatever else was
  wrong with the document at that place. A handler may not begin or step
  the parser it is being called by: that is refused (`kInUse`), and the
  document in hand goes on.
- **Names** are given as written, prefix and all (`dc:title`). Namespaces
  are not worked out, which also means a book that uses a prefix it never
  declared can still be read.
- **Encodings**: UTF-8, UTF-16, ISO-8859-1 and US-ASCII; everything is
  given out as UTF-8. Any other is refused (`kUnsupportedEncoding`).

expat is not in this repository: CMake downloads the release named in
`third_party/expat/CMakeLists.txt`, checks its hash, and compiles it with
the settings in `third_party/expat/expat_config.h`, the same for the
desktop and the device. The settings leave out what an EPUB does not need:
entity expansion, parameter entities and external DTDs, and namespace
processing. It is 71 KB of flash once something in the firmware uses it.

`bn-xml` runs the same code on the desktop:

```
./build/host/tools/xml/bn-xml events chapter.xhtml
./build/host/tools/xml/bn-xml book book.epub
./build/host/tools/xml/bn-xml book book.epub --memory 16384
```

`events` prints what the parser finds in one file, an event a line. `book`
parses every XML file in an EPUB as it is read out of the ZIP file, and
prints for each how it ended, a checksum of its events, and the most
memory the parser used; and at the end the count and the checksum of all
of them run together (`epub/xml_digest.h`), which is what the firmware's
book check reports for a book. `--memory` holds the parser to that many bytes, as
a device would.

The unit tests say what the parser should make of documents written for
them, and damage one in every way they can think of. An end-to-end test
(`tests/e2e/xml_tool.py`) compares `bn-xml` with the expat that Python
carries, which is another build and another version driven by other code:
every event of every document has to agree.
