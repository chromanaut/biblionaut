# Timing layout on a device

The breaker, the hyphenator and the setter each state a bound on the work
one call does, in nodes, edges or bytes. A firmware built with
`-DBN_LAYOUT_CHECK=ON` says what that work is in time on the device, and
sets the text of real books there to show that it comes to the same lines
as on the desktop: at start, and again at each press of the Power button.
(On the desktop, set `BN_LAYOUT_CHECK`, with `BN_STORAGE` naming the
folder that stands for the card.) It writes nothing to the card, and goes
with neither the storage check nor the book check.

The text is a file of paragraphs at `/layout/paragraphs.txt`, made from
books on a computer by `tools/layout-text.py`. It reads each book's
documents in the order of its spine, takes a paragraph to be the text of a
block element (`p`, `div`, a heading, a list item, a cell), and writes
them as `bn-lines` reads a file: a `br` is a line end inside a paragraph,
and `i`, `em`, `b`, `strong` and `cite` are the second style. It is a
plain reading of a book's text and not the importer. The text is
hyphenated by the table for American English that is built in, unless
there is a table on the card, at `/layout/hyphenation.bnhy`, as
`bn-hyphen pack` makes them: that one is taken instead, which is how
another language's table, or another version of English's, is timed.

```
tools/layout-text.py ~/Books/card/books --out paragraphs.txt
# copy it to /layout on the card
tools/idf -DBN_LAYOUT_CHECK=ON build flash
./build/host/tools/trace/bn-trace | tee layout.log
tools/idf -DBN_LAYOUT_CHECK=OFF build flash     # back to the normal firmware
```

What it does, in order (`reader/layout_check.h`):

1. Times the made-up face alone over 64 KB of text in memory, as the book
   check times its checksum: the face is what the text is measured in.
2. Breaks the paragraph that brings a call of the breaker's closest to its
   bound, with 64, 256 and 1,024 nodes, timing every call: the longest is
   the bound in time.
3. Reads the table on the card, if there is one, and otherwise takes the
   one that is built in.
4. Sets text made to be awkward for the setter (5,000 letters with no
   space, soft hyphens end to end, line feeds and hyphens and spaces by the
   thousand) at two widths, timing every call; and has the hyphenator find
   the places in eight long words.
5. Sets the text on the card three times: hyphenated, then not, then
   hyphenated again with every call timed, which finds the longest call
   and costs a reading of the clock round each. The lines are 1,760
   quarter pixels wide (440 pixels), with 256 nodes, 768 records and a
   hyphen at 12,100, which are `bn-lines`'s defaults.

What it finds goes out as log text. Over the text of the 28 books in
`corpus/`, on a desktop (`tools/layout-text.py corpus`, and no table on
the card, so the one built in):

```
layout check: the made-up face alone, 65536 bytes in 449 us (142538 KB/s).
layout check: breaker, 64 nodes: the dearest call 6 us, 2146 of 3328 work; 388 calls in 28 us.
layout check: breaker, 256 nodes: the dearest call 63 us, 33154 of 37888 work; 1540 calls in 279 us.
layout check: breaker, 1024 nodes: the dearest call 912 us, 525826 of 544768 work; 6148 calls in 3789 us.
layout check: no table at /layout/hyphenation.bnhy (not found): the built-in table, 38049 bytes; no break before 2 letters or after 3 letters.
layout check: setter, awkward text: the dearest call 28 us, the most work 504 of 1533, the breaker's 11361 of 37888; 35666 calls in 5080 us.
layout check: hyphenator, 8 long words: the dearest 109 us for 64 (1703 ns a word), 1653 of 81915 work.
layout check: text: 17350039 bytes in /layout/paragraphs.txt.
layout check: hyphenated: 50313 paragraphs, 17233557 bytes, 329876 lines (30747 with a hyphen, 0 too wide); checksum 8ee4f78b.
layout check: hyphenated: setting 873 ms (19258 KB/s), 79 us for 30 lines; the face about 100 ms of it; card 5 ms.
layout check: hyphenated: the longest paragraph 1750 us, 141354 bytes in 1585 lines; the most work in a call: setter 494, breaker 3242, hyphenator 1064.
layout check: hyphenated: 2435592 words asked about, 104 last resort, 76811 lines early in 2397, 0 cut short; 256 nodes; 14704031 bytes measured.
layout check: hyphenated: 13872034 calls.
...
layout check: hyphenated, every call timed: 50313 paragraphs, 17233557 bytes, 329876 lines (30747 with a hyphen, 0 too wide); checksum 8ee4f78b.
layout check: passed.
layout check: the longest step: 939 us, breaking the dearest paragraph.
```

The lines for each setting say what it came to, with the checksum of its
lines; how long setting it took, and so a page of 30 lines, beside the
card's time in reading it and the face's share, worked out from the bytes
measured at the rate found first; the paragraph that took longest, from
its start; the counts `bn-lines --figures` prints; and how many calls it
took. `bn-lines` prints the same paragraphs, bytes,
lines, hyphens, words asked about, bytes measured and checksum for the
same file and table with its defaults, so a device's lines can be shown to
be the desktop's. The run passes if the text could be set every way and
the first setting and the last, which differ only in being timed call by
call, came to the same lines. A table on the card that cannot be used is
said, and the built-in one taken; a missing text fails the run, after
everything that does not need it.

It is also in the metrics (`layout_check=1` if it passed,
`layout_kb_per_s` for the first setting, and `layout_longest_call_us`) and
in the trace (`reader/layout_check`, and `reader/layout_check_pass` for each
setting).

What it does not say: what a real face's measuring costs, which waits for
the fonts; nor what reading a paragraph out of a book costs, which waits for
the format books will be imported into. `e2e.LayoutCheck` runs the check
in the desktop application against `bn-lines` on text made from books the
test writes, with a table on the card and with the built-in one, and holds
`tools/layout-text.py` to a book whose paragraphs it writes out by hand.
