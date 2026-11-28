# Fuzzing

Each reader of a book's own files, from the inflater to the table of
contents, has a fuzz target in `tests/fuzz`, and so have the heap the XML
parser's memory comes from, the hyphenator, the paragraph setter and the
measure of text in a fonts image's faces, which
are given a book's words, the pager, which is given a chapter's bytes from
the card, each reader of a drawing, the readers of PNG
and JPEG pictures and the scaler, and the reader of the fonts image, which
a partition can hold anything in place of: one function that
is given some bytes, puts them through the reader, and ends the program if
the reader does anything it should not.
Going outside its memory, or into undefined behaviour, is seen by the
sanitizers. The rest a target checks for itself: what the reader's header
promises of it, and that no single call does more than the header says a
call may.

| Target | What it reads its input with | What it holds the reader to, besides staying in its memory |
|---|---|---|
| `inflate` | `core::Inflater`, twice, with the stream and the output cut up two ways | No call takes more from its source than `MostInputFor()` allows. Both readings give the same bytes. They are the bytes zlib gives for the same stream, and a stream is whole only if zlib says so |
| `zip` | `epub::ZipArchive`, `ZipEntryReader` and `ZipBookFiles`: the directory from end to end, and the first few entries looked up and read, and then opened as a book's files are, from their places and by their names | What each call may read from storage (19 reads to open a file, 2 for an entry of the directory, and for a call on an entry what the inflater's bound comes to in blocks). An entry called sound gave as many bytes as the directory says, with its checksum. A file opened from its place has the path its own header gives, as much of it as there is room for, and is opened if and only if that path was read whole and the entry can be opened |
| `xml` | `epub::XmlParser`, with more or less memory, and a handler that may stop it or try to use it from inside | A step asks its source once, for 1 KB. The parser's heap is in order after every step. Text comes in whole characters, and never more of it than twice the bytes read, which is the most a change of encoding makes of them. A handler that stops the parser is told nothing more. The parser reads a good document rightly afterwards |
| `package` | `epub::Package`, a step at a time, twice | No step reads more than five blocks from storage. Nothing is said of the book until the reading is over. A book that was read has documents, each found again by its entry's name. A second reading says what the first said |
| `toc` | `epub::TocReader`, a step at a time, twice, with a handler that may ask for no more | No step reads more than five blocks. Every label is whole characters with its spaces collapsed, within its length; every entry points at a document of the book or at none. A stopped reading says so |
| `css` | `epub::CssReader`, twice, with the sheet cut up two ways; and the same bytes as a `style` attribute | A step asks its source once, for 1 KB. A rule is told whole and in order. A selector has no more parts or classes than there is room for, asks no more of attributes, and has a rank its parts account for. A number is within its bounds, and not below zero where the property cannot be. What stands before an item of a list is one of the words for that, which are for nothing else. A list of font families is given the kind of face of the first of its names that says, each name asked about by itself. The reader's counts are the counts of what it told. Both readings tell the same |
| `css_rules` | `epub::CssRules`, `epub::CssCascade`, `epub::StyleStack` and `epub::DocumentBlocks`: up to four sheets into a table of one of four sizes, a document gone through with a cascade that has one of four amounts of memory, the table taken back to before its last sheet, and the document gone through again | The table stays inside its memory. If it left no rule out, every element and every first letter is given exactly what a plain working-out of the same rules gives it (`tests/support/css_reference.h`). No element takes more work than `MostWorkFor()` says. A table taken back is the size it was and says of the document what a table never told of the last sheet says; and it takes the sheet again to the same end. A stack of styles beside the cascade (`epub::StyleStack`), keeping one of four numbers of open elements, counts those that are open and those too deep to keep, keeps no more languages than it has room for, does no more work for an element than `MostWorkFor()` says, says of an element only kinds and bits there are, says that an element inside one that is nothing is nothing, never raises or lowers a block's text, gives a distance between lines an amount of its own kind and no other, tells of a first letter just where a rule set something for one, says that an open element passes on what was said of it as it opened, and says the same of the document gone through again. What finds the document's blocks (`epub::DocumentBlocks`), with text and pictures among the document's lines, says of each element what the stack of styles says; ends a block only where one is open and an element opens or closes, and begins one only where none is open; begins none for white space, and always makes other text and a picture part of one unless they are inside what is nothing; gives a record of kinds there are, a level to a heading alone, and no marker or first letter to a rule; ends floated text only in a block that began with it; and does no more work in a call than `MostWorkFor()` says. |
| `document_sheets` | `epub::DocumentSheets`: a book of up to eight files held in memory, with one of four amounts of memory for the sheets' names, for a document's `<style>` text and for the table, read in pieces of one of four sizes, one file failing part way; the sheets of each document found, every other one handed over open, the first given up part way and begun again, and the first found again at the end | Every document ends with a list. A step opens one file at the most or reads `kMostBytesPerStep` of the book at the most, and never both. A list has no sheet twice, and every sheet in it is in the table and, but for the document's own, is a file of the book. The table has the sheets that were read and no others. What is said to be left out is what the counts come to. Until something is left out for want of room, the list and the counts are those of a plain working-out (`tests/support/sheets_reference.h`), and where the table kept every rule it was given, the document's own rules are as many as its `<style>` texts come to, each read to its own end. A document found again has the same list, and nothing is opened for it but itself. No file but a document is opened twice for the book, the sheet in hand when a document was given up apart. The digest of what the documents were given (`epub/sheets_digest.h`) counts what was found, and is the checksum of the lines the plain working-out comes to |
| `chapter_import` | `epub::ChapterImporter`: a book of up to four files held in memory, a document and its sheets, read in pieces of one of four sizes, with one of four amounts of memory for the parser and for the table of rules, one of four languages as the book's own, the document failing part way and storage failing at one of its writes; the document imported into a bundle, committed, and read back | The importing ends. A step opens one file at the most or reads `kMostBytesPerStep` of the book at the most, and never both, and writes `kMostPagesPerStep` pages at the most; once ended, a step does nothing. Unless storage failed it ends well, with a chapter that is whole, cut short or refused: whole only if the document was read to its end, refused with no segment, and for a reason there is. A chapter that was imported reads back, every checksum holding, with the blocks and characters its record counts; a block is of a kind importing writes, a heading alone has a level, and a rule has nothing in it; text is whole characters with no byte no text has; a style item is followed by text and says something the one before it did not; a picture is one the book has not, with no more text standing for it than is kept; a block has as many characters as it says are dropped; an id is in a block there is, no further in than the block is long, and the ids are in the document's order; where a block began is in the document and not before where the last did. A document that is XML to its end is so in pieces of any size, and comes to the same chapter |
| `bundle` | `epub::Bundle`: a file opened a step at a time, its records read, its tables checked, a chapter committed to it and the file opened again. The input is the file's length and the pieces of it that are not nothing, a bundle being mostly empty pages; the checksums of its header page and slots are put right or left, as the input says | A step is one read of storage at the most, of a page, and opening writes nothing. A file that does not begin as a bundle is none, and one that does not open leaves nothing to be asked. In one that opens, the tables follow one another before the slots, the segments end on a sector within the file, and every chapter's record has its segment among the segments and its directory inside it. A table checks as whole or as damaged, as it is. A chapter committed is found again when the file is opened afresh, with the next sequence number and every other record as it was; the commit wrote a page of a slot a step, to one slot, and nothing over the header page or the tables; and with the slot it wrote spoiled, the file opens as the bundle it was before. A file that opens has a header page and a slot that pass their checksums, which the target works out for itself |
| `book_bundle` | `epub::BookBundler`: the package of a book read, and its bundle made a step at a time, twice. The book is a sound ZIP file the target makes round the package, the NCX and the nav the input gives, with a few documents and pictures for them to name; or the input itself | A book whose package was not read has nothing written for it. A step reads no more than five blocks of the book and writes no more than two whole pages of the bundle. A bundle that was made opens, and says what the book does: its strings, a record for each document where the package says its file is, a record for each picture of the first file the ZIP directory has for it and none for a picture with no file, the contents a reading of the table of contents gives, and whether those are the whole. Every table passes its checksum, the directory's checksum is the one worked out from the file's bytes, and the second making is the first byte for byte |
| `segment` | `epub::SegmentWriter` and `epub::SegmentReader`: a chapter's segment written from what the input describes (blocks, text, styles, links, pictures, ids, and runs of small blocks) and read back; or the input's own bytes read as a segment, with the checksums the chapter's record and the directory carry put right or not | Of a segment that was written: every call of the writer writes two whole pages at the most, in order; the record says what was given; every block reads back as it was given, with no text item of more than a kilobyte or cut part way through a character, and each block, count of characters, place in the source and id is found where it is; and with one byte changed, whatever is asked ends in a status that is not kOk or gives what the chapter held. Of any bytes: everything ends in a status, in no more steps than the format allows and with no call asking the file for more than a page's worth; going from block to block ends; no item, name or language is past its limit; and a place that is found is of a block before the directory |
| `region_heap` | `core::RegionHeap`, given a made-up run of requests and of mistakes | Pieces lie inside the block and share no byte. Each holds what was put in it. Nothing outside the block is written. Every mistake is counted and changes nothing. The heap's records agree after every request |
| `hyphenator` | `layout::Hyphenator`, with a language's table as the input has it, or one made of patterns the input writes out, and the words that follow | No word takes more work than `MostWorkFor()` allows a word of as many characters. A word's places are in order, each at the start of a character, with letters enough directly on each side of it by the target's own reading of the word; a word too long to look at has none. With room for fewer places, the first are given. The same table somewhere else in memory, in a hyphenator that had another table open before it, answers the same. With a table, the places are the ones the target finds by reading the table an edge at a time. With patterns, they are the ones found by laying every pattern over every place in the word |
| `paragraph` | `layout::ParagraphSetter`, with a paragraph's text in runs of up to seven styles and line ends asked for, on lines from 1 wide to 30,000, measured four ways | A paragraph is done with in a number of calls that depends only on its length. No call does more than `MostWorkFor()` allows, gives the breaker more than one item, asks the hyphenator about more than one word, or has more than a word's worth of bytes measured. The lines hold all the text once and in order. Each is as wide as its text adds up to and has the places between words it says; one is only too wide for a single character that is. The measure is never given a soft hyphen, nor a piece that begins inside a character, in however many styles a character's bytes are given: a character is in the style of its first byte. No line ends after a hyphen or a dash that a word joiner or U+FEFF follows, unless the text asks for it to, where every word is one the setter takes whole and no line was ended where nothing lets one end. The lines are the same when the text is offered all at once |
| `face_measure` | `layout::FaceMeasure` and `layout::GlyphRun`, in faces made for the target that kern and join letters, with a stretch of text in one of four styles, cut into pieces, with the fallback face or without | Going through the text a glyph at a time gives the glyphs of a plain second setting of it, written in the target from the faces as they were described: each stands for the same bytes, every byte is stood for once, nothing is joined across what is not drawn or across two faces, and a character that is never drawn has no glyph though a face has one for it. Each piece is as wide, and each piece's hyphen comes to as much, as that setting makes them: the text from a cut on set by itself, the text up to a cut with a hyphen after it, and the glyphs after a cut inside a ligature followed for as many glyphs as the header says and no further. Every width is a whole number of pixels, nothing is written past the pieces asked for, and the pieces of a stretch cut only between its glyphs add up to the whole |
| `chapter_pager` | `layout::ChapterPager`: a chapter written from what the input describes (blocks with what they ask of pages and lengths of their own, text, styles, pictures, runs of small blocks) and set in pages, with as few records for a page's lines, entries for the chooser and nodes for the breaker as the input says; or the input's own bytes read as a segment and set in pages | Of a chapter that was written: every piece of work ends kOk. A block's lines are the ones the setter gives of the block's text given whole, each with the height, the space above and the marks its block and the block before it come to. The page down from a line, the page up to one and the page about a place are the lines that adding heights up over the whole chapter chooses, each where that puts it; a page of more lines than there are records ends, or begins, where they run out and says so; going down from the first line each page begins where the one before ended; and with one bit of the chapter changed, a block's lines are not given, or are the lines they were but for what the first has from the block before. Of any bytes: every piece of work ends kOk, or says the chapter could not be read or has no such block, and a page that is given is lines that follow one another down it. Of both: no step makes more than one call of the reader or of the setter, asks the file for more than a page's worth, or takes in more lines than the breaker has nodes; and no piece of work takes more steps than the chapter's bytes allow |
| `svg_tags` | `draw::SvgTagReader`, twice: whole, and cut up, with the ends of the pieces it gives handed back to it | A call takes a byte or tells something. Things are told in an order tags can have, and `Depth()` is how many elements were opened and not closed. Each tag is in the file where `TagOffset()` says it began. No more bytes are told of than were taken, and what an entity stands for is one character. Once reading has failed nothing more is told. Both readings tell the same things |
| `svg_sheet` | `draw::SvgSheetReader`, twice: the sheet whole, and cut into pieces of one of eight sizes | A call takes a byte or tells something. A rule is told as its selectors, what it sets and its end, in that order. A selector is a class or an element's name made of what a name may be made of; a property's name is in small letters with no space; neither is longer than it may be, nor a value. Both readings tell the same things and say the same of what they dropped |
| `svg_values` | Each reader of what an attribute says (`draw/svg_values.h`), and `ParseSvgTransform` | A reading that refuses the text leaves what it was to fill as it was. Space round the text changes nothing, nor capitals a colour. A list of lengths has some, and no more than it has room for, and what reads as a length reads as a list of one. A view box is fitted where SVG says it goes, worked out again in floating point |
| `svg_path` | `draw::SvgPathReader`, `PathFlattener` and `Filler`: a shape drawn whole, and again in strips with its data cut up | A call gives no more commands than it was asked for and one. A curve is 1,024 lines at the most and an arc 32,768. A line adds to no more cells than `MostCellsPerLine()`. Nothing is written outside the strip, and no cell is left with anything in it. The two pictures are the same, byte for byte |
| `svg` | `draw::SvgRenderer`: a file read with nothing to draw into, drawn whole, and drawn in strips from a file cut up | No call gives an outline more than `kMostCommandsPerStep` commands, gives the filler more than `kMostLinesPerStep` lines, fills more than one shape or stroke, reads more than `kReusedPerStep` bytes of what the file defined, or looks through what it kept more than three times. No more than 32 groups are open, and no more is kept than there is memory for, which is of eight sizes and is filled with something else before each pass. A file that is not a drawing leaves the picture alone. Every strip's reading of the file is the same reading, rules and gradients kept included, and the two pictures are the same, byte for byte; and so is the file kept by a `SpoolWriter` and given back a band at a time, each step of which reads a chunk and fills once at the most |
| `png` | `draw::PngReader`, four ways: for the header alone with no memory, with the room and the pieces the input chooses, with all the room it needs, and against a plain reading of the same file with zlib (`tests/support/png_reference.h`) | No step takes more from its source than `MostInputPerStep()`, and the header alone takes 33 bytes. Rows come in order, each as wide as the picture, and a picture is whole only with all of them. It has no room only when it has less than `RowBytesFor()`, and a picture is interlaced only if it says so. The header alone says what the whole reading says of it. With the room it needs it gives the same rows in any pieces. The rows are the plain reading's, and it ends where and as that does, but for data the inflater refuses as bad that zlib takes |
| `jpeg` | `draw::JpegReader`, four ways: for the header alone with no memory, with the reduction, room and pieces the input chooses, with all the room it needs in pieces of 4 KB, and at an eighth | No step takes more from its source than `MostInputPerStep()`, nor gives more than one row. Rows come in order, each as wide as the picture is read, and a picture is whole only with all of them. The reduction taken is the one asked, or an eighth for a progressive picture, and the sides read are the picture's divided by it. It has no room only when it has less than `RowBytesFor()`, and a header it takes is one it can say the room for. The header alone says what the whole reading says of it, and fails as it does. It gives the same rows in any pieces, and ends at an eighth as it ends at the reduction asked |
| `picture_cache` | `draw::PictureCache`, reading a file the card holds, as the picture it says it is or as another, its checksums mended or not | Reading ends in a status, and no file is left open. Another picture can be kept among the same files, of more rows than the cache has memory for as often as not, and is read back the same; keeping it and reading it back, storage is written and read from where a sector begins and in whole sectors but for the last of the rows, what the header leaves of its sector is nothing, and given up part way it is not found whole. The memory the cache gathers in is one sector to eight, some of them with part of one over. Rows are given only whole, and only as many as the picture has. A picture read through without complaint is the file's own rows, the picture asked for, with the checksum its header gives; and kept again it reads back the same |
| `scale` | `draw::Scaler`, from sizes the input gives (sides of nothing and past the largest among them) and its pixels, sometimes all one gray, sometimes with an old row given out of turn | A side of nothing or too long is refused and nothing is given. `Row()` gives no new row and `Step()` one at the most; the new rows come in order, each as wide as the new picture, as many as it is high. A row out of turn stops it. Every new pixel is within a gray of the working-out in real numbers, and a picture of one gray stays that gray |
| `font` | `draw::FontImage` and `draw::Face`: an image opened twice, its first faces and its last looked into, and each asked for the code points at the edges of its ranges and four the input names | Opening goes the same way twice, and an image that is refused has no faces. A face is found by its own role, style and size. Finding a glyph takes no more steps than halving the face's ranges does. A glyph that is given has a picture of the size its width and height make, wholly inside the image, and is the glyph its number gives. In a face whose ranges are in order and apart, a code point has a glyph exactly when going through the ranges one by one finds it, and the glyph is what the image's bytes say, read the plain way. Every two glyphs found in a face are kerned by what the image's bytes say, which is nothing for a class the face's table has not, and no glyph of another face is kerned. Every two and three are joined as the image's bytes say: by the first of the face's ligatures that is made of them, if that one's glyph can be had, which then lies inside the image as any glyph does; and no glyph of another face is joined; and the glyphs that begin a ligature are marked as the image's bytes say. Opened a second time through a map with as much room as the input says, where each stretch is a copy of its own size: only the table of faces is mapped at first; a face is held exactly when there is room for its stretch of the image, and that stretch is all that is mapped for it; held, it gives the glyphs the image opened as a span gives, and kerns and joins them alike; and nothing is left mapped at the end |

The `package` and `toc` targets do not take a ZIP file as their input:
nearly every change to one would be stopped at an entry's checksum, and
the reader behind it never reached. They take the text of the files, and
make a sound ZIP file round them each time (`package` can also be given
the ZIP file itself). The `png` target does the same for a picture: unless
its input says to take the file as it is, it puts every chunk's checksum
right, and may take what the `IDAT` chunks hold as the rows themselves and
compress them before the reader sees them, cut into chunks of the size the
input chooses. Each target's source says how its input is laid out.
The first byte or two are its choices: how much memory the reader has,
how big a piece is asked for at a time, when the handler stops, how many
rows a strip of a picture has.

The targets that draw have a picture of 64 pixels by 48 at the most, and
leave a drawing off once it has come to 200,000 lines, which a few bytes
of arcs can ask for: its pictures are then not compared. The target for a
whole file leaves off as well once 200,000 bytes of what the file defines
have been read again, which a few bytes of `use` can ask for.

The hyphenator's bound is one figure for a whole word, and most tables
leave room to spare under it: work done past what a table says of itself
would not show. So some of its seeds are tables that say the least a table
can (patterns of one letter, states of one edge) and hold more, where
going by what the table holds is over the bound at once.

A target is built two ways.

- **In every ordinary build** it is a program that goes through the inputs
  kept for it in `tests/fuzz/inputs/<target>/`, once, and CTest runs it as
  `fuzz.<Target>`. So the targets are built by both compilers and run with
  the sanitizers and without, on every change, and a fault that an input
  once found stays found. The kept inputs are the seeds, which
  `tests/fuzz/make_seeds.py` makes out of text written for the purpose
  (a stream of each kind, a book of each kind, a table of contents with
  entries in it, a language's table and words for it, paragraphs of text,
  a drawing with something of everything), and any input that ever found
  a fault, cut down to the least that shows it and named `found-` and
  what it found.
- **For fuzzing** it is linked with LLVM's fuzzing engine, libFuzzer,
  which makes up new inputs by changing the ones it has, keeps each one
  that reaches code none reached before, and goes on from those.
  `tools/fuzz.py` builds the targets that way and runs them:

  ```
  tools/fuzz.py                  # every target, a minute each
  tools/fuzz.py --time 3600      # an hour each
  tools/fuzz.py xml zip          # only those two
  ```

  It ends with a line for each target in each build: how many inputs it
  tried, how many of those it kept, and whether anything was found.

  It takes its own turns among the heavy checks ([the queue](checks.md)):
  one with the whole machine to build the targets, and then one in the
  background for the runs, which are in no hurry and hold nothing up
  there. In the background a run has the lowest priority the machine
  gives, and as many go on at once as the background has processors, two
  on a Mac with two efficiency cores. So a run tries far fewer inputs in
  its minute than it would on a processor to itself, about a ninth as
  many on a Mac that is busy, and a pass over many targets is long: one
  that is to get further is given more time, or is started in a turn of
  the ordinary kind (`tools/queued --cores 4 tools/fuzz.py --time 600
  svg`), where it does all of its work at the ordinary priority.

  It runs each target in two builds at once: one with AddressSanitizer,
  and one without, because the heap inside a block is laid out one way
  with it and another way without, and the firmware has it the second
  way. What the engine has kept is in `.fuzz/corpus/`, and the next run
  starts from it, so a run gets further the more have gone before. An
  input that makes a target fail is saved in `.fuzz/found/`, and the
  summary says how to see it fail again and how to cut it down
  (`tools/fuzz.py --shrink FILE TARGET`). It needs the LLVM named in
  `tools/llvm-version`: Apple's compiler has no fuzzing engine.

Fuzzing is not one of the checks a pull request has to pass. A minute a
target finds only what lies near the seeds, and it is hours, from a set of
inputs kept between runs, that find the rest. So it has a workflow of its
own, `.github/workflows/fuzz.yml`, which runs `tools/fuzz.py` on GitHub's
Linux machines, with the targets shared out four to a job and the jobs all
running at once:

- for a pull request that changes anything a target is built from (the
  portable parts, `tests/fuzz`, `third_party`, the build's own files),
  five minutes a target in each build;
- for main, every night, half an hour;
- started by hand, for as long as is asked, on the targets that are
  named, as for an hour a target before a set of readers is called
  finished: `gh workflow run fuzz.yml -f seconds=3600 -f targets='svg png'`.

A run for main keeps what the engine has kept, and the next run, a pull
request's too, goes on from that. A run that finds something fails. Its
summary says which target and which input, in the lines `tools/fuzz.py`
ends with, and the input is among the run's files, with what each run
printed: `gh run download` fetches them, and the summary's own lines say
how to see the input fail on a developer's computer and how to cut it
down. Nothing waits for these runs, so a pull request's is looked at
before the pull request is merged. The targets are whatever `tools/fuzz.py`
lists: a new one is fuzzed there without being named there.

On a developer's computer it is run for a minute on the targets of what
was changed, which finds what lies near; and for as long as anyone likes,
from what that computer has kept.

What it does not find:

- **How long a call takes.** A target counts what a call reads, not its
  time.
- **A bound that only a large input can reach.** The engine's inputs are
  16 KB at the most, and it takes a ZIP directory of more than five
  blocks, or a megabyte of blocks that produce nothing, to tempt a step
  past its bound. The unit tests make those files and hold the same
  bounds over them. The megabyte that a drawing may have read again of
  what it defines is the same: the target leaves a drawing off long before
  it, and a unit test takes a file up to it.
- `ZipArchive::Find`, which reads as much of a directory as it has to,
  and is not held to anything here.
- **Whether a drawing is drawn rightly.** The targets for SVG hold a
  picture to being the same however it is cut up, not to being the
  picture the file describes. That is for the unit tests and the
  end-to-end tests, which have something to set it beside.
- **A drawing of any size.** The pictures are small, and path data long
  enough to be drawn as it arrives is a good part of the longest input
  the engine makes.
- **What takes several tags that agree with one another**, short of a
  long run: a `use` that names another `use`, a group inside what is
  used, `inherit` for a property. The engine is slow to come to these by
  changing bytes, and what it has not come to it has not tried. The
  seeds for a drawing have one of each such thing that the inputs it had
  kept were found not to reach, by building the targets with clang's
  source coverage and taking them through `.fuzz/corpus/`. With those,
  what the engine's inputs and the seeds together do not reach of the
  renderer and the reader of style sheets is the megabyte above, the
  names the renderer gives things in its reports, and checks that nothing
  written in a file should be able to trip. The seeds alone reach less.
