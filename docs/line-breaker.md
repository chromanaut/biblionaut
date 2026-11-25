# Breaking paragraphs into lines

`layout/line_breaker.h` chooses where the lines of a paragraph end. It is
the first piece of `layout`, and it knows nothing of text, fonts or pixels.
A paragraph is given to it as a run of items, each with a width in whatever
unit the caller measures in: boxes, which are never broken; spaces, where a
line may end; breaks, which are places inside a word where a line may end
at a price, showing a hyphen if it does; and forced breaks. It gives back
the lines, each as a range of those items.

- **Lines are chosen together.** Filling each line as full as it will go
  leaves some lines tight and others with a great gap. `LineBreaker` makes
  the sum of the squares of the space left on each line as small as it can
  be, so a word is taken down early where that evens out the lines after
  it. The last line of a paragraph is allowed to be short.
- **The first lines can have widths of their own**, up to eight of them,
  and the rest share one. That is how a first line is indented, and how
  room is left beside a large first letter that stands several lines high.
- **Memory**: the caller provides an array of nodes, 32 bytes each, one for
  each place a line could end that is still in play. It allocates nothing.
  256 nodes (8 KB) hold a paragraph of about 250 such places whole.
- **Flash**: 4.1 KB, as compiled for the device.
- **A paragraph longer than its memory.** How even the lines can be made
  depends on the whole paragraph: one more word at the end can shift a
  little of every line before it. So nothing is given out before the end
  unless something makes it. When the nodes run out, the lines of the
  earlier half of them are settled as they would be if the paragraph ended
  there, and the rest is worked out again from that point. A paragraph of
  any length is broken in the same memory, and the lines come out no
  further behind the items than the nodes allow. A caller with room for
  only so many items can ask for lines sooner (`Settle()`).
- **What that costs.** A paragraph that fits its nodes is broken the
  cheapest way there is. For one that does not, the lines settled early
  cannot always be the ones the whole paragraph would have chosen. Over 30
  made-up paragraphs of 1,000 to 4,000 words with 256 nodes they cost 2%
  more than the cheapest breaking on average, and 10% at worst.
- **One call's work**: a paragraph cannot make one call long. Nearly every
  call weighs the nodes within one line's width and does no more. The call
  that does most is one that finds the nodes used up: it settles lines, and
  then works out again the cheapest way to every place after them, each
  place looking back over the nodes before it. Counted in nodes gone over,
  a call with `n` nodes does no more than `n(n + 40) / 2`, whatever the
  paragraph (`LineBreaker::MostWorkFor`): 37,888 with 256 nodes. Settling
  always makes room in one go. If one line's width holds more places than
  there are nodes, a line is made to end at the latest of them.
- **What will not fit.** A stretch of boxes with nowhere to break that is
  wider than its line goes on a line of its own, which says that it is too
  wide. Nothing else is ever wider than its line.
- **Not done**: spaces are not squeezed to get more on a line, two
  hyphenated lines in a row cost no more than two apart, and a last line of
  one short word is not avoided.

Its tests break paragraphs written out by hand for each rule, and compare
it with two other ways of finding the cheapest breaking, written for the
tests. One tries every way there is, on 600 made-up paragraphs short enough
for that. The other works forwards through the whole paragraph, keeping
everything, on paragraphs of up to 60,000 words. With the fewest nodes it
accepts, and with a caller that asks for lines early, the lines still have
to fit and to hold every item once.

Every call in those tests is held to the work one call may do. One
paragraph is made for that alone: a first line that is exactly full, and
after it more places to end a line than there are nodes, all within one
line's width. With 256 nodes its dearest call goes over 33,154 nodes,
which is within an eighth of the bound; the dearest call of a made-up
paragraph of ordinary shape goes over 2,600 to 10,400. Another 400
paragraphs are made to be awkward, with boxes of no width, boxes wider than
any line, spaces of no width and breaks at every price, in 32 to 300
nodes, half of them with a caller that asks for lines early.
