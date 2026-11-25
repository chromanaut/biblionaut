# Choosing the lines of a page

`layout/page_chooser.h` decides which lines go on a page. Breaking a
paragraph into lines does not depend on where a page ends, so a chapter is
one run of lines whatever pages they fall on, a page is known by its first
line, and choosing pages is a matter of how many lines fit. Like the line
breaker it knows nothing of text. A line is a height, the gap that belongs
between it and the line before, and whether a page may end between the two.

There are three ways to come to a page, and something for each:

- **Forward**, a page is filled down from its first line (`PageFromTop`).
  Lines are offered in order until one is refused, and the next page
  begins with the line after the last one on this.
- **Back**, to a page that has not been laid out before, it is filled up
  from its last line: the one above the first line of the page being left
  (`PageFromBottom`). The lines above a page are only known by breaking
  their paragraphs from the start, so they are offered a block at a time,
  the nearest block first and the lines of each in their own order. A long
  paragraph is not held: only the lines that could still be the page's
  first are remembered, in entries the caller provides, 12 bytes each. 128
  of them (1.5 KB) do for a page of 128 lines.
- **Into the middle** of a chapter, at some line, the page begins with that
  line if a page may, and otherwise with the nearest line above it that
  one may begin with (`PageHolding`).

The rules are the same whichever way a page is come to:

- **Gaps.** The gap before a line is there while the line before is on the
  same page, and is left out above the first line of a page. The exception
  is a line that starts a page whatever comes before it, such as the first
  of a chapter: there it is the space the book wants above that line.
- **Lines kept together.** A line can be kept with the one before it, as a
  heading is with the paragraph under it, and the lines beside a large
  first letter are with each other. No page ends or begins between them.
  Filled down, a page goes back to the latest place it may end. Filled up,
  it begins with the first line below that may begin one. If the lines kept
  together are more than a page holds, it ends, or begins, where it is
  full.
- **A line that starts a page** always begins one. A page filled up stops
  there, with room to spare below if that is how it falls, and says that
  it did: whoever wants such a page full fills it down from that line.
- **A line taller than the page** has a page to itself.
- **The two directions agree.** A page filled up to where a page filled
  down ended holds that page's lines, and perhaps more above them; and a
  page filled down from where one filled up begins reaches at least as far
  as that one's end. The one exception is a page that had to begin or end
  between lines kept together, for want of anywhere better.
- **One call's work**: a call to `PageFromTop` or `PageHolding` does a
  fixed amount, and a call to `PageFromBottom` goes over no more entries
  than it was given.
- **Flash**: 1.7 KB, as compiled for the device.

Its tests fill pages written out by hand for each rule, and compare all
three with adding the heights up over the whole run: 300 made-up runs of
up to 600 lines, with lines of no height and lines taller than the page,
gaps, lines kept together and lines that start a page, cut into blocks of
1 to 300 lines, and 40 places tried in each. A page is filled up from the
end of a block of 20,000 lines in the entries an ordinary page gets, and
blocks whose heights add up to more than 32 bits hold are counted rightly.
