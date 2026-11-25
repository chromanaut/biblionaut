# Where a word may be broken

`layout/hyphenator.h` finds the places a word may be broken across two
lines, by the method TeX uses (Franklin Mark Liang, 1983). A language is a
set of patterns: short runs of letters with a number at each place between
them, such as `hy3ph` or `1tio`. Every pattern that occurs in a word is
laid over it, each place takes the largest number any pattern puts there,
and the word may be broken where that number is odd. So one pattern can
allow a break and a longer one forbid it again.

- **A table for each language**, made from its patterns by `bn-hyphen` and
  read where its bytes lie: in the firmware itself, or wherever else it is
  kept. Nothing is copied out of it and nothing is allocated. The patterns
  are a tree, read a letter at a time, and branches that are alike are
  kept once.
- **American English is built in.** No patterns are in this repository:
  the build downloads TeX's for American English (4,938 patterns, and 14
  words written out with their breaks) and makes them into a table of
  38,049 bytes, which is compiled in as constant data
  (`hyphenation/american_english.h`). On the device it stays in flash and
  is read there. It is only in a program that asks for it: so far the
  layout check (`reader/layout_check.h`), and not yet a normal firmware,
  in which nothing sets text. Its copyright notice is compiled in with it.
  A table made from TeX's patterns for German (36,709) is 234,154 bytes.
- **A word** is given as UTF-8, and the places come back as how many bytes
  of it come before each break. Capitals count as their small letters. A
  character the language has no letter for divides the word, and each side
  is looked at by itself; so do bytes that are not UTF-8. A word of more
  than 63 characters is given no places.
- **Words written out** with their breaks, as a language's exceptions are,
  go into the table as patterns of their own that no other pattern can
  change.
- **A table is not trusted.** Whatever its bytes are, nothing outside it
  is read, and a word is answered within the work the table's own header
  allows: no more starts than the word has letters, no further from each
  than the longest pattern, no more edges at a step than the widest state
  has (`Hyphenator::MostWorkFor`). With the American English table that is
  81,915 edges and numbers for a word of 63 letters. Over the 235,976
  words of a dictionary, the most any one took was 633, and they took 214
  each on average.
- **What is noted of a table** when it is opened, 384 bytes in the
  object: which letter each character below 128 is, and which edge leaves
  the first state on each letter. Every start in a word begins at the
  first state, which has an edge for nearly every letter (27 in English),
  so finding the one wanted there was a third of all the looking over
  that dictionary. And a
  word of fewer bytes than a break needs letters about it is sent back
  before it is read: 47% of the words of the Project Gutenberg books in
  `corpus/` are such. What a table says is not changed by any of it: the
  first state's edges are noted in the order they would be gone through.
  On the X4, setting the text of 34 books with the words hyphenated
  took 159 s, and 100 s with none: 18 µs for each of the 3.3 million
  words asked about. Going through the first state's edges and looking
  every character up in the letter list, it was 187 s, 26 µs a word.
- **Flash**: 1.8 KB, as compiled for the device, without a table.

`bn-hyphen` makes a table and breaks words with one:

```
./build/host/tools/hyphen/bn-hyphen pack patterns.txt --out table.bnhy
./build/host/tools/hyphen/bn-hyphen words table.bnhy words.txt
./build/host/tools/hyphen/bn-hyphen info table.bnhy
```

`pack` takes patterns as they are written for TeX, with spaces or line
ends between them, and optionally the language's exceptions
(`--exceptions`), a list of its letters' other forms such as their capitals
(`--letters`), and the fewest letters it leaves before and after a break
(`--left`, `--right`: 2 and 3 unless told). For the nine patterns of TeX's
that the word "hyphenation" meets, it says:

```
patterns 9, exceptions 0
letters 9, as 18 characters
states 13, edges 18 (18 before like branches were kept once), widest 5
longest pattern 5, values 21 bytes
table 199 bytes
```

`words` prints each word of a file, or of what it is given to read, with
a `-` at every place it may be broken: `hy-phen-ation`.

The build makes the American English table the same way, from the
plain-text files that TeX's hyphenation package (hyph-utf8) keeps beside
each language's TeX file: `hyph-en-us.pat.txt` for the patterns and
`hyph-en-us.hyp.txt` for `--exceptions`. The TeX file itself will not do,
as its patterns are inside `\patterns{...}`, which `pack` does not read.
`third_party/hyph-en-us/CMakeLists.txt` names the commit they are taken
from and the hash of each. The table is left in the build, at
`build/host/parts/third_party/hyph-en-us/american_english.bnhy`, for
`bn-lines` or `bn-hyphen words` to be given; a unit test holds what is
compiled in to that file, byte for byte, and to words whose breaks TeX
gives.

Its tests write tables of their own, the plain way and with nothing
shared, and compare the places found with laying every pattern over every
place: 300 made-up sets of patterns, 60 words each. A table with any one
byte changed, and tables of bytes that are no table at all, have to be
answered within the work they state and without a read outside them. A
table whose first state is not what `bn-hyphen` would write (two edges on
one letter, no edge marked last, more edges than the table says a state
has, bytes changed anywhere among its edges) is held to a second reading
of it, an edge at a time (`tests/support/hyphen_patterns.h`), in some
three thousand such tables; and a table opened on a hyphenator that had
another open to nothing being left of the first. The
tables `bn-hyphen` makes are checked the same way, against the plain ones
too. And an end-to-end test (`tests/e2e/hyphen_tool.py`) sets `bn-hyphen`
beside a second way of breaking the same words that shares none of its
code, on 150 made-up languages with letters of one byte to four. Tables
and words that nobody wrote are the fuzz target's (see [Fuzzing](fuzzing.md)).
