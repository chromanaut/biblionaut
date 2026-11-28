# A chapter in pages, on a device

`layout/chapter_pager.h` is where the reader of a chapter's segment ([A
chapter's bytes](segment.md)), the paragraph setter ([Setting a
paragraph's text in lines](paragraph-setter.md)) and the page chooser
([Choosing the lines of a page](page-chooser.md)) meet. `ChapterPager`
reads a block, has it set in lines, and gives the lines to a chooser, a
block after another, until a page is known. `bn-pages` ([A chapter in
pages](pages.md)) is a tool over it; nothing in the firmware uses it yet.

A line is known by its block and the character of the block it begins
with, and a page by its first line. A place in a chapter is a block and so
many characters into it, counted as the segment counts them: a byte of
text is one, and so is a picture among the words.

## What it does

- **Three pages, and a block's lines.** `BeginDown()` is the page that
  begins with the line holding a place; `BeginUp()` the page that ends
  with the last line that begins before a place, which for the place a
  line begins at is the line above it; `BeginAbout()` the page a chapter
  entered at a place is shown, which begins with the line holding the
  place or a little above it where that line is kept with those before;
  and `BeginLines()` tells a sink each line of one block. Then `Step()`
  until it says there is no more.
- **A page is given as its lines**, in records the caller provides, 28
  bytes each: the block, the characters of it that are on the line, how
  wide they are, where the line stands on the page, its height, the space
  that belongs above it, how many places between words it has, and what
  is true of it (kept with the line before, starts a page, shows a hyphen,
  ends its paragraph, too wide). It is not given as text: whoever draws
  the page reads the text again.
- **What a block's record comes to is not its business.** Whoever makes
  one gives it a `BlockSetting` to ask: how wide a block's lines may be,
  how high they are, the space above the block and after it, what a
  hyphen costs; for each run of text, the measure's number for its style
  and what breaks its words; and how high the lines of a block that is a
  picture are. The pager knows the marks a block's record carries for
  pages (a page ends before it, it is kept with the next, its lines are
  kept together) and nothing of lengths or faces.
- **What it makes of them**: above a block's first line is the block's
  space before and the space after of the block before it. That line
  starts a page if the block asks for a page to end before it, and so does
  the chapter's first; it is kept with the line before if the block before
  asks to be kept with the next. Every other line of a block that asks to
  be kept together is kept with the line before it. A block that comes to
  no line (a rule, a block of nothing but spaces) is one line with nothing
  on it. A line is a pixel high at the least.
- **A picture among the words is one byte to the setter**, in the style of
  the text about it: the setter puts nothing but text in a line, and this
  stands for the picture until it does. In a block that is a picture,
  where the picture comes before any text, the lines are as high as the
  setting says the picture is.
- **A capital that is dropped is not.** Where a block's record says how
  many of its first characters are one (`kBreakDropped`), they are set
  in the line like the rest of the block's text. The setting is given the
  block's record and can see the mark.

## How a chapter is read

- A block is found by the chapter's index and read from its head, and the
  block after it is read on from there. A block past the last the index
  has is come to by reading on from that one.
- To set a block, two things are wanted of the block before it: whether
  that one is kept with the next, and the space after it. They are
  remembered for the last four blocks whose heads were read; otherwise the
  head of the block before is read for them.
- **Nothing else is kept of a block that has been set.** A page that
  begins part of the way down a block has the block set from its start,
  and the lines above the page's first passed over; and no more of a block
  is set than a page needs. A page filled up is set twice: up from its
  end, to find where it begins, and then down from there for its lines.
  Where filling it up comes to a line that starts a page, it is that
  line's page filled down, as a reader going back to a chapter's start is
  shown the chapter's first page.
- It counts what it does (`BlocksSet()`, `HeadsRead()`, `BlocksFound()`,
  `BlocksPassed()`), so that what keeping more would save can be
  measured.
- The reader may be used for anything else between one piece of work and
  the next. If the reader is given another chapter, or the setting comes
  to say other things, `Forget()` has it forget what it remembered.

## Its limits

- **Memory**: all of it the caller's. The setter with its nodes and
  records; entries for the chooser that fills a page up
  (`PageFromBottom::kMinEntries` at the least); and the records for a
  page's lines, eight at the least. The pager itself is 632 bytes on the
  device. With the setter and what it usually has (256 nodes and 768
  records, 12.2 KB), 128 entries (1.5 KB) and records for 64 lines
  (1.8 KB), that is 15.8 KB, besides the reader and its page.
- **Flash**: 7.1 KB, as compiled for the device.
- **A page has no more lines than there are records.** One filled down
  ends where they run out, as it would where the page was full, lines
  that are kept together left for the next page together; one filled up
  begins with the first line there is a record for; and both say so
  (`ChapterPage::cutShort`), as a page filled up says when the chooser
  ran out of entries. Every line is still on a page.
- **One step's work**: a step makes one call of the reader that reads, or
  one call of the setter that sets, and takes in the lines that call
  gives out, which are no more than the breaker has nodes; for each of
  them it makes one call of a chooser. What those calls do is theirs to bound.
  `LastStep()` says what a step did.
- **Every failure is a status.** The chapter's bytes are not trusted: a
  block that cannot be read ends the work with `kReadFailed`, and
  `ReaderStatus()` says how the reader failed. A block's checksum is at
  its end, so it is only looked at for a block that is read to its end:
  what is read of the block before for its head alone, and of a block a
  page ends part of the way down, is taken as it is found. Lines told to
  a sink before the work failed are of a block that did not check.

## Checked

Its tests write chapters into a bundle with the segment's writer and set
what the pager gives beside a second working-out that has none of it: the
lines of each block from the setter given the block's text whole, and
pages chosen by adding the lines' heights up over the whole chapter. Four
made-up chapters, on pages from 33 pixels high to 760, have every page
down from the first line, the page above every one of those and above
sixty other places (where a line begins, inside one, past its block's
end), and the page about 120 places compared so. Others hold it to what
is said above: a page of more lines than there are records, going down
and going up; a chapter of 65,576 blocks, past the index; storage that
fails at each read a piece of work makes; a bit changed anywhere in a
chapter's blocks; and every step, in all of them, to one call of the
reader or the setter and to two reads of storage.

Its fuzz target (`fuzz_chapter_pager`, [Fuzzing](fuzzing.md)) writes a
chapter from the input and makes the same comparison, with the pager
given as little memory as the input says; or takes the input's bytes as a
segment, where every piece of work has to end in a status and every page
given has to be lines that follow one another down it.
