# Setting a paragraph's text in lines

`layout/paragraph_setter.h` stands between a paragraph's text and the line
breaker. It takes the text as it comes, in runs that each have a style;
finds the words; asks the hyphenator where each may be broken, and a
measure how wide its pieces are; gives the breaker its items; and says of
each line that comes back which bytes of the paragraph's text are on it.
It knows nothing of fonts. How wide text is, is asked of a `TextMeasure`
that the caller provides, in whatever unit the lines' widths are in.

- **Spaces** are where a line may end. Several in a row are one space as
  wide as all of them. Those at the start of a paragraph, and after a line
  end the text asks for, are left out. A line feed is such a line end.
- **Words** are everything else, a no-break space among it. A word may
  change style part way through and is one word still: the hyphenator is
  asked about the whole of it, and each part is measured in its own style.
  A character is in the style its first byte came in: if a run ends part
  way through a character, the bytes that go on with it are taken in that
  style. So a word changes style only where a character begins; and a word
  in more than eight styles, which is taken in parts as a run too long for
  a word is, is cut only there.
- **Where a line may end inside a word**: where the hyphenator says, at a
  price the caller sets, with a hyphen shown. At a soft hyphen, the same
  way; and a word that has one is broken at those and nowhere the
  hyphenator would have chosen. A hyphen is as wide as the measure says
  one is at that place: the measure gives, with each piece's width, what a
  hyphen shown after the piece comes to, so that one which draws a word's
  letters otherwise where a line ends among them (kerned with the hyphen,
  or not joined across the line's end) can have the line counted as it
  will be drawn. After a hyphen or a dash that is written in
  the word, at no price and with nothing more shown: but not after one the
  word begins with, between two of them, or between a minus and a figure.
  And at a zero width space.
- **What it says of a line**: where its text begins and ends in the
  paragraph, how wide it is, how many places between words it has (which
  is what spreading it out to the full width needs), whether a hyphen is
  to be shown after it, and whether it ends the paragraph or ends where the
  text asked. A line begins with the first character of its first word and
  ends with the last of its last.
- **Nothing runs off the end of a line.** A stretch with nowhere to end a
  line that is wider than the paragraph's narrowest line may be ended
  between any two of its characters, as a last resort, and the lines it
  runs over are then filled as lines of words are. So is a run of bytes too
  long to be taken as one word (252), which is how text in a script written
  without spaces comes out as full lines. A line may also end without a
  hyphen where a word could be broken and the narrowest line has no room
  for one. Only a character that is itself wider than a line makes a line
  too wide.
- **Memory**: the caller's. The breaker's nodes, and a record of 4 bytes
  for each item that is not yet on a line given out: 768 of them (3 KB) go
  with 256 nodes. When the records run out, lines are given out early. The
  setter itself is about 1 KB, most of it the word it has in hand.
- **Flash**: 5.6 KB, as compiled for the device.
- **One call's work**: a paragraph takes many calls, and a call need not
  take all the text it is offered. Each looks at no more than a word's
  worth of bytes (255), asks the hyphenator about one word at the most,
  has the measure go over a word's worth of bytes at the most, and gives
  the breaker one item at the most (`ParagraphSetter::MostWorkFor`). What
  the hyphenator and the breaker then do, they bound themselves.
- **Not done**: anything on a line that is not text, such as a picture
  among the words; the spaces at the start of a line of text that is set
  just as it is written; the rules for ending lines in scripts written
  without spaces between words; and spreading a line out, which is for
  whoever draws it.

`bn-lines` (`tools/lines`) sets a file of paragraphs by this code. There
are no fonts yet to measure text with, so it measures in a made-up face
whose widths it states (`layout/made_up_face.h`: a small letter is 36
quarters of a pixel, an `i` 18, an `m` 54), which lets a reader add a line
up. Text between two asterisks is in a second style, a little wider. With
the table made in [Where a word may be broken](hyphenation.md) from the
nine patterns that "hyphenation" meets:

```
$ cat text.txt
Nobody on the quay had a word for hyphenation, and the well-known lamp was *lit* before anyone asked.
$ ./build/host/tools/lines/bn-lines text.txt --table table.bnhy --width 640
0	18	584	3	-	Nobody on the quay
19	36	562	4	h	had a word for hy-
36	54	540	2	-	phenation, and the
55	70	508	1	-	well-known lamp
71	85	426	2	-	was lit before
86	99	432	1	l	anyone asked.
```

Each line is where it begins and ends in the paragraph's text, its width,
its places between words, what is true of it (`h`: a hyphen is shown; `l`:
it is a last line; `w`: it is too wide) and its text. `--figures` prints
what setting the paragraphs took instead, with a checksum of all the lines
(`layout/line_digest.h`: where each begins and ends, its width, its number,
its places between words and what is true of it):

```
$ ./build/host/tools/lines/bn-lines text.txt --table table.bnhy --width 640 --figures
paragraphs 1, bytes of text 99, lines 6, the most in one paragraph 6
lines with a hyphen shown 1, too wide 0
words the hyphenator was asked about 18, places made as a last resort 0
lines given out early 0, in 0 paragraphs; ended for want of records 0
nodes in use at the most 22 of 256
calls 98; the most work in one, by the setter 36 of 1533, by the breaker 17 of 37888
bytes measured 82; checksum of the lines 65aeed32
```

Its tests set paragraphs written out by hand for each rule, and compare it
with a second way of making a paragraph's items, written for the tests:
the whole paragraph at once, every word found, cut and measured before the
breaker is given anything, on 600 made-up paragraphs. Another 400, with
lines as narrow as a letter, little memory, and the text offered a byte or
a few at a time, have to come out as lines that hold all the text once and
add up. Every call is held to what one call may do, on text made to be
awkward as well: one run of 5,000 letters with every letter wider than the
line, 500 soft hyphens end to end, 700 line feeds, a word in a new style
at every other letter.

An end-to-end test (`tests/e2e/lines_tool.py`) sets `bn-lines` beside a
second reading of the same paragraphs in Python that shares none of its
code. It takes each paragraph apart into the stretches that no line ends
inside and the places between them, with a made-up language's hyphenation,
and finds what the cheapest way of ending the lines costs by trying, for
every place, every place a line ending there could have begun. Over 480
paragraphs the lines `bn-lines` prints cost exactly that.
