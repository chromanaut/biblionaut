# Fonts

The firmware does not read font files. Every face it draws text in, at
every size it draws it, is drawn beforehand on the computer that builds,
and kept in one run of bytes, the fonts image, which the firmware reads
where it lies: on the device, in the flash partition called `fonts` (see
"Fonts on a device", below), mapped into the processor's address space a
face at a time; on the desktop, in a file read into memory. So far the
application uses the image
for one thing, showing a file of text to try faces with ([Trying faces on
the panel](text-viewer.md)); the rest of this is the image, the program that makes
it, the reader of it and the measure that sets text by it.

A face is one role (serif, sans, monospace, or the fallback for what the
others lack), one style (regular, italic, bold, bold italic) and one size
in pixels. It has its metrics, a table from code points to glyphs, a record
for each glyph and the glyphs' pictures. A glyph's picture is two bits a
pixel: four levels of ink, from paper to full. Its advance, how far it
moves the pen, is in quarters of a pixel and is the outline's own, not
rounded to the pixels its picture was fitted to: so the widths of a line's
characters add up to what the line is wide at any size. Glyph 0 of a face
is what is drawn for a character it has no glyph for. `draw/font.h`
describes the layout byte by byte.

A face may kern: set two of its glyphs that follow one another closer
together, or further apart, than their advances would. Its glyphs are put
in classes for that, twice over: those that kern alike with whatever
follows them are one class as the first of two, and those that kern alike
after whatever comes before are one class as the second. A glyph's record
has its two classes, a byte each, 0 for a glyph that does not kern; and the
face has a table with a signed byte for each class of first glyphs and
each of second, in quarters of a pixel. So a pair's kerning is one byte
read, found from two the glyphs have. The measure that sets text (below)
adds it to the first glyph's advance before it rounds.

A face may have ligatures: one glyph that it draws in place of two or
three of its glyphs that follow one another, as one for an f and an i.
It has a list of them, eight bytes each: the glyphs a ligature is made
of, by their numbers in the face, and the glyph that stands for them.
The list is in the order the ligatures are tried in, so that one of two
f's and an i comes before one of the two f's. The glyph that stands for a
ligature is a glyph of the face like any other, with an advance of its
own, which is seldom what its letters' come to, and classes for kerning.
It may be a character's own, as Noto's ligature of an f and an i is its
glyph for U+FB01; or one no character has, as Source Serif 4's of an f
and a j is, and then its number is after those of all the characters'
glyphs. The measure that sets text (below) draws a ligature in its
letters' place.

## Making the image

`bn-font` (`tools/font`) makes the image from font files, drawing each
glyph with FreeType, which the host build downloads (`third_party/freetype`)
and which is no part of the firmware. (A build configured with
`-DBN_FONT_TOOL=OFF` has no `bn-font` and downloads neither FreeType nor
the font its tests use; the firmware's build of its own host tools is
one.) Which files, and at which sizes, is said by a preset, a text file (the
build has some of its own: The presets, below):

```
sizes 26 30 34
mono regular  CourierPrime-Regular.ttf
mono bold     CourierPrime-Bold.ttf
```

A line names a face's role (`serif`, `sans`, `mono`, `fallback`), its style
(`regular`, `italic`, `bold`, `bold-italic`) and its font file, which is the
rest of the line and is found from the preset's folder unless it begins
with `/`. `sizes` are the sizes every face is drawn at, in pixels to the
em. `characters U+0020-U+007E U+00A0 ...` chooses the characters that are
drawn; without it they are Latin with its extensions, the phonetic
alphabet, the marks set over letters, Greek, Cyrillic, punctuation,
currency and letterlike symbols, number forms, arrows, mathematical
operators, the Latin ligatures and the replacement character, U+FFFD. A
character the font has no glyph for is left out and counted. Anything from a `#` to the end of a line is passed
over.

```
$ bn-font build courier.preset --out courier.bnfn
mono     regular     26 px:  381 glyphs (2075 lacking), pictures   20833 bytes; ascent 21, descent 9, line 29, x-height 12; no kerning; no ligatures
mono     regular     30 px:  381 glyphs (2075 lacking), pictures   27877 bytes; ascent 24, descent 11, line 34, x-height 14; no kerning; no ligatures
mono     regular     34 px:  381 glyphs (2075 lacking), pictures   35732 bytes; ascent 27, descent 12, line 38, x-height 16; no kerning; no ligatures
mono     bold        26 px:  381 glyphs (2075 lacking), pictures   22159 bytes; ascent 21, descent 9, line 29, x-height 12; no kerning; no ligatures
mono     bold        30 px:  381 glyphs (2075 lacking), pictures   29667 bytes; ascent 24, descent 11, line 34, x-height 14; no kerning; no ligatures
mono     bold        34 px:  381 glyphs (2075 lacking), pictures   37570 bytes; ascent 27, descent 12, line 38, x-height 16; no kerning; no ligatures
faces 6, pictures 173838 bytes, kerning 0 bytes, image 204534 bytes
```

Glyphs are drawn with FreeType's light hinting, which moves an outline up
and down to the pixels and not across, and its 256 grays are made the four
levels. A serif or a sans that is given a regular and not all of the other
three styles is built, with a warning for each: the reader of the image
gives the nearest style a role has. The fonts a preset names are whoever
builds's own business: no font is in this repository, and a preset that
names fonts from the builder's computer need not be either. The pictures
are not compressed; what they would take if they were is for `bn-font
sizes` to say (below).

A face is kerned as its font is. The fonts of the presets keep their
kerning in the table called GPOS, as pair adjustments, which FreeType reads
only a part of, so `bn-font` reads it itself (`tools/font/kerning.h`): the
lookups of the `kern` feature for the default language of every script the
font names; a lookup's subtables in order, a list of pairs passing on a
pair it does not name and a table of classes answering for every pair
whose first glyph it covers; and what several lookups say of a pair added
up. Each pair is then put to the nearest quarter of a pixel at the face's
size, and the face's glyphs into classes that kern no pair otherwise than
the font does. Three things a font may have are left out, the first two
counted in what `build` says of a face: a lookup of kerning that looks at
more than two glyphs (the four fonts of Noto Sans have one each, for an
accent on a dotless i before a bracket, and the four of Source Serif 4
one, for the dot between two l's); whatever a pair moves besides the first
glyph's advance (no font of the presets has any); and kerning across a
mark that stands between two letters, since a table asked about
neighbours cannot. A monospace font has no kerning, and its faces have
none. A face with more than 255 classes of either kind would be built
without kerning, and said; the most any font of the presets comes to is
212.

A face has the ligatures its font makes of its own accord, and no others:
which letters are joined differs from one font to the next, and a font
with a glyph for a ligature need not make it. A font says it in the table
called GSUB, which FreeType does not read, so `bn-font` reads that too
(`tools/font/ligatures.h`): the lookups of the `liga` feature, the one for
ordinary text, for the default language of every script the font names;
of those the ligature substitutions; and each one's ligatures in the
order the font has them, which is the order it tries them in. A ligature
is kept if every glyph it is made of is one a character of the image is
drawn with, and its own glyph is drawn for it if no character's is that
glyph already. The fonts of the presets join only an f with what follows
it: Noto Serif and Noto Sans two f's, an f and an i, an f and an l, and
two f's with an i or an l, each with the glyph of its character (U+FB00
to U+FB04); IBM Plex Serif an f with an i or an l, and IBM Plex Sans with
an i; Source Sans 3 two f's, and an f or two with a t; and Source Serif 4
nine in its upright styles and sixteen in its italics, a j, a b, an h and
a k among what an f is joined to, four of the nine and eleven of the
sixteen with glyphs no character has.
The monospace fonts join nothing. What is left out is counted in what
`build` says of a face: a ligature of more than three glyphs, or one
past the 255 a face holds; anything under `liga` that is not a ligature
substitution; and, where a font has more than one lookup of them, a
ligature of a later lookup that a list cannot say as the font makes it.
A font goes through a line with one lookup and then, through what that
made of it, with the next, and a list is tried once at each place in
turn: so a later lookup's ligature is left out if it is made of a glyph
an earlier lookup makes, or if a ligature of an earlier lookup lies within
it, which the font has joined by then. (One difference is left in:
where an earlier lookup's ligature begins inside a later lookup's and
runs past its end, the font makes the earlier lookup's, and the list
whichever begins first in the line. No font of the presets has more than
one lookup of ligatures.) A lookup that passes over marks is read as one
that does not, so letters with a mark between them are not joined.

`bn-font info` says what an image holds, reading it by the code the
firmware reads it with; with `--glyphs` and some characters, what each face
has for them: the glyph's number, its advance in quarters of a pixel, its
picture's size and where its top left corner lies from the pen on the
baseline, its levels of ink added up, and its two classes for kerning.
A face's own line ends with how many classes of each kind its kerning
has, and how many ligatures.

```
$ bn-font info courier.bnfn --glyphs U+0041 U+0067 U+0020 U+03B1
image 204534 bytes, faces 6
mono     regular     26 px:  381 glyphs; ascent 21, descent 9, line 29, x-height 12; kerning 0 by 0; ligatures 0
  U+0041: glyph 34, advance 62, 16 by 15 at 0, 15, ink 249, kerns as 0 and 0
  U+0067: glyph 72, advance 62, 14 by 17 at 1, 12, ink 288, kerns as 0 and 0
  U+0020: glyph 1, advance 62, 0 by 0 at 0, 0, ink 0, kerns as 0 and 0
  U+03B1: no glyph
mono     regular     30 px:  381 glyphs; ascent 24, descent 11, line 34, x-height 14; kerning 0 by 0; ligatures 0
  U+0041: glyph 34, advance 72, 18 by 18 at 0, 18, ink 340, kerns as 0 and 0
  U+0067: glyph 72, advance 72, 17 by 20 at 1, 14, ink 385, kerns as 0 and 0
```

(and so on for the other four faces).

With `--kerning` and some characters it says every two of them that a face
kerns, and by how many quarters of a pixel the second is set further on:
less than nothing is nearer. Of the image of the `noto` preset (below), an
A, a V, an r, a comma and an o in the serif at 30 pixels: a comma is four
pixels nearer after an r, and a V two and a half after an A.

```
$ bn-font info fonts.bnfn --kerning U+0041 U+0056 U+0072 U+002C U+006F
image 10144822 bytes, faces 50
...
serif    regular     30 px: 1982 glyphs; ascent 33, descent 9, line 41, x-height 17; kerning 111 by 92; ligatures 5
  U+0041 U+0056: -10
  U+0041 U+006F: -2
  U+0056 U+0041: -10
  U+0056 U+002C: -13
  U+0056 U+006F: -7
  U+0072 U+002C: -16
  U+002C U+0056: -13
  U+006F U+0056: -7
```

With `--ligatures` and some characters it says every two and three of
them that a face joins, in the order they were named: the number of the
glyph that stands for them, and its advance. Of the same image, an f, an
i and an l in the serif at 30 pixels:

```
$ bn-font info fonts.bnfn --ligatures U+0066 U+0069 U+006C
image 10144822 bytes, faces 50
...
serif    regular     30 px: 1982 glyphs; ascent 33, descent 9, line 41, x-height 17; kerning 111 by 92; ligatures 5
  U+0066 U+0066: glyph 1974, advance 85
  U+0066 U+0066 U+0069: glyph 1977, advance 120
  U+0066 U+0066 U+006C: glyph 1978, advance 120
  U+0066 U+0069: glyph 1975, advance 80
  U+0066 U+006C: glyph 1976, advance 79
```

And of an image of the `source` preset, in the serif's italic, where the
last glyph a character has is 859 and those after it are the ligatures'
own:

```
$ bn-font info source.bnfn --ligatures U+0066 U+0069 U+006C U+006A U+0074
image 7111217 bytes, faces 55
...
serif    italic      30 px:  871 glyphs; ascent 32, descent 11, line 41, x-height 15; kerning 161 by 145; ligatures 16
  U+0066 U+0066: glyph 855, advance 68
  U+0066 U+0066 U+0066: glyph 861, advance 101
  U+0066 U+0066 U+0069: glyph 858, advance 100
  U+0066 U+0066 U+006C: glyph 859, advance 100
  U+0066 U+0066 U+006A: glyph 863, advance 93
  U+0066 U+0066 U+0074: glyph 865, advance 105
  U+0066 U+0069: glyph 856, advance 67
  U+0066 U+006C: glyph 857, advance 67
  U+0066 U+006A: glyph 868, advance 60
  U+0066 U+0074: glyph 870, advance 72
```

`bn-font sizes` says what an image's pictures would take if they were kept
in other ways than the image keeps them, which is as they are: for
choosing a way, and how many sizes a partition has room for, from the
fonts themselves. It changes nothing, and the firmware reads none of these
forms. For the faces of each size, and for all of them:

- **run-length, a glyph**: each glyph by itself as runs of pixels of one
  level, a byte to a run: the level in two bits and the length, 64 at the
  most, in six;
- **DEFLATE, a glyph**: each glyph by itself, compressed by zlib at its
  best;
- **the best, a glyph**: each glyph in whichever of those, or as it is, is
  smallest for it, and the last line says how many glyphs that was for
  each;
- **DEFLATE, groups** of 16, 64 and 256 glyphs in the order a face has
  them, and **a face** as one: compressed together, so that to draw one
  glyph its group has to be decompressed.

```
$ bn-font sizes courier.bnfn
image 204534 bytes: tables 30696, pictures 173838
26 px: 2 faces, 762 glyphs
  as they are                42992 bytes  100%
  run-length, a glyph        63138 bytes  147%
  DEFLATE, a glyph           39957 bytes   93%
  the best, a glyph          38914 bytes   91%
  DEFLATE, groups of 16      24473 bytes   57%
  DEFLATE, groups of 64      21986 bytes   51%
  DEFLATE, groups of 256     19218 bytes   45%
  DEFLATE, a face            17647 bytes   41%
```

(and the same for 30 px and 34 px)

```
all: 6 faces, 2286 glyphs
  as they are               173838 bytes  100%
  run-length, a glyph       219738 bytes  126%
  DEFLATE, a glyph          146463 bytes   84%
  the best, a glyph         144369 bytes   83%
  DEFLATE, groups of 16      86779 bytes   50%
  DEFLATE, groups of 64      78341 bytes   45%
  DEFLATE, groups of 256     68083 bytes   39%
  DEFLATE, a face            62114 bytes   36%
the best, a glyph: 691 as they are, 58 run-length, 1537 DEFLATE
```

Letters drawn in grays are poor in runs: an edge is a pixel or two of each
level, and a byte to a run comes to more than the picture as it is. A
glyph by itself is too little for DEFLATE to find much in, and what it
saves grows with the size; together, glyphs compress to half and less.
The compressed sizes are zlib's, and another version of zlib may give a
few bytes more or fewer.

## The presets

A fonts image can be made without bringing any fonts: the build has
presets, each a serif and a sans in four styles and a monospace face in
two, whose font files it downloads from their makers' repositories at a
commit it names, each checked against a hash (`third_party/fonts`). They
are downloaded when an image is asked for, not when a build is configured:

```
cmake --build --preset host --target fonts      # writes build/host/fonts/fonts.bnfn
```

Which typefaces is chosen when the build is configured:

| | |
|---|---|
| `-DBN_FONTS=noto` | Noto Serif and Noto Sans. This is what an image is made of unless something else is asked for |
| `-DBN_FONTS=plex` | IBM Plex Serif and IBM Plex Sans |
| `-DBN_FONTS=source` | Source Serif 4 and Source Sans 3 |
| `-DBN_FONTS=/a/preset/of/ones/own` | Whatever that preset names: its own font files, at its own sizes |
| `-DBN_FONTS_MONO=courier-prime` | Courier Prime as the monospace face. This is so unless something else is asked for |
| `-DBN_FONTS_MONO=family` | The family's own: Noto Sans Mono, IBM Plex Mono or Source Code Pro |

A preset here is three files of `tools/font/presets` read as one:
`ladder.preset`, which has the sizes; the family's faces; and a monospace
face. `bn-font build` takes any number of presets and reads them so, and
with `--fonts FOLDER` finds their files from that folder, which is how the
build points them at what it downloaded. The sizes are five, 22, 26, 30,
34 and 38 pixels to the em, each about an eighth larger than the one
before, 30 being the size text is first read in: on the X4's panel, about
7, 8.5, 10, 11.5 and 12.5 points as printed. Plex and Source have fewer of
the characters an image is made with than Noto has (about 900 and about
860 to 1,400 a face, against about 1,980), so their images have Noto Sans
besides, as the fallback face, for what they lack. As built, with the
12 MB (12.58 million bytes) of the `fonts` partition to fit in:

| Preset | Monospace face | Faces | Image | Of it kerning |
|---|---|---|---|---|
| `noto` | Courier Prime | 50 | 10.14 MB | 0.49 MB |
| `noto` | Noto Sans Mono | 50 | 11.98 MB | 0.49 MB |
| `plex` | Courier Prime | 55 | 6.99 MB | 1.43 MB |
| `plex` | IBM Plex Mono | 55 | 7.49 MB | 1.43 MB |
| `source` | Courier Prime | 55 | 7.11 MB | 1.03 MB |
| `source` | Source Code Pro | 55 | 7.75 MB | 1.03 MB |

Ligatures are little of an image: eight bytes each, 2 KB of a `noto`
image and under 1 KB of a `plex`; and 45 KB of a `source`, where the
glyphs of most are drawn for them and kerned.

```
$ bn-font build ladder.preset noto.preset mono-courier-prime.preset --fonts FOLDER --out fonts.bnfn
serif    regular     22 px: 1982 glyphs (474 lacking), pictures  106959 bytes; ascent 24, descent 7, line 30, x-height 12; kerning 42240 pairs, 111 by 92; 5 ligatures
serif    regular     26 px: 1982 glyphs (474 lacking), pictures  143543 bytes; ascent 28, descent 8, line 35, x-height 14; kerning 42270 pairs, 111 by 92; 5 ligatures
...
mono     bold        34 px:  381 glyphs (2075 lacking), pictures   37570 bytes; ascent 27, descent 12, line 38, x-height 16; no kerning; no ligatures
mono     bold        38 px:  381 glyphs (2075 lacking), pictures   47138 bytes; ascent 30, descent 13, line 43, x-height 18; no kerning; no ligatures
faces 50, pictures 8629148 bytes, kerning 494212 bytes, image 10144822 bytes
```

(which is what the `fonts` target runs.)

All of these typefaces are under the SIL Open Font License; what that asks
of a firmware given out with an image made of them is in `THIRD_PARTY.md`
(Fonts in a firmware). The tests read each preset with either monospace
face, and `e2e.FontTool` holds every font file a preset names to being one
the build has a commit and a hash for. They download two of the files and
no more, Noto Serif Italic and Noto Sans Regular, when a build with tests
is built: fonts that kern and that make ligatures, for `e2e.FontKerning`
and `e2e.FontLigatures` (below).

## Reading the image

`draw::FontImage` (`draw/font.h`) opens an image and gives its faces: by
number, by role, style and size, or the nearest style a role has at a size
(bold italic gives way to bold, then to italic, and any of them to
regular). A `draw::Face` gives a glyph for a code point, with its picture
as a span of the image's own bytes; nothing is copied, and nothing is
allocated. `Face::Kern()` says how much further on a glyph is set after
another of the same face, in quarters of a pixel: a glyph says which face
gave it, and one face's glyph is not kerned with another's.
`Face::Ligature()` is given glyphs that follow one another and says
whether the face draws one glyph in place of the first two or three, and
which: the first in its list that is made of them. It joins only its own
glyphs.

The image is not trusted, though the firmware's own build made it: a
partition can hold anything.

- **Opening** looks at the header and at each face's record, 1,024 faces at
  the most, and refuses an image in which any face's tables run outside
  it. It reads only as many bytes as the image says it is long, so a
  partition larger than its image is no trouble.
- **Finding a glyph** halves the face's ranges of code points: 33 steps at
  the most (`Face::kMostSteps`), whatever the tables hold, and `LastSteps()`
  says how many. A glyph whose picture would run outside its face's
  pictures is not given.
- **Kerning** two glyphs reads one byte of the face's table, which opening
  the image saw to be inside it; a glyph whose record says a class the
  table has no row or no place for is not kerned.
- **Finding a ligature** goes through the face's list, which opening the
  image saw to be inside it, once: 255 ligatures at the most. One whose
  glyph the face cannot give is not made. So that not every glyph of a
  text has the list gone through for it, a face gives a mark for the
  glyphs that begin a ligature (`Face::LigatureFirsts()`: 64 bits, one for
  each of what a glyph's number can leave over 64), and a glyph whose mark
  is not set begins none.
- **RAM**: none but the objects themselves: under 300 bytes for an image,
  most of it the list of the faces it holds (below), and 0.6 KB in the
  device's hardware layer for the list of what it has mapped. **Flash**:
  3.4 KB for the reader, 4.9 KB for the measure below and 0.7 KB for the
  partition's mapping, as compiled for the device, besides the image
  itself.

An image is opened in one of two ways. Given a span of bytes, all of it is
there to be read, and every face has its glyphs from the first: so the
tools open one, and the tests. A device has not the address space to map
the whole of an image ([Fonts on a device](#fonts-on-a-device)), so there
an image is opened through a `core::ByteMap` (`core/byte_map.h`), which
gives a stretch of bytes when asked and takes it back, and only the header
and the table of faces are kept readable, 49 KB of them at the most. A
face then says its role, style, size and metrics, and is found by them, but
has no glyphs until it is held (`FontImage::Hold()`): that maps the one
stretch of the image from the first of the face's tables to the end
of the last, which is the face and nothing else in an image `bn-font` made,
since it writes a face's tables and pictures together. Sixteen faces are
held at the most, and `ReleaseFaces()` gives them all up: whoever sets a
page holds the faces the page is set in, and gives them up for the next
page's. A face there is no room to map is not held, and has no glyphs. A
glyph says where in the image its picture is, and `FontImage::Bytes()`
gives the picture again from that while its face is held, which is how a
page keeps a glyph in four bytes. Nothing chooses a face to give up for
another when room runs out; that is for whoever comes to set a page in more
faces than there is room for.

`layout::FaceMeasure` (`layout/face_measure.h`) is the `TextMeasure` that
sets text in such faces. Each of the paragraph setter's styles is given a
face. A character is drawn with its style's face if that has a glyph for
it, with the fallback face if that has, and otherwise with its style's
face's glyph 0; a soft hyphen and a zero width space are not drawn and take
no room; a byte that is not UTF-8 is drawn as U+FFFD, which is found as
any other character is (of the presets' typefaces Noto and IBM Plex have a
glyph for it, and Source and Courier Prime have not).

Every glyph is drawn on a whole pixel, and a glyph is always the same
number of pixels after the one before it, whichever two they are: the
first's advance and what their face kerns the two by, added in quarters of
a pixel and then put to the nearest pixel, a half going up
(`FaceMeasure::PenStep()`). An image has advances in quarters of a pixel,
and the plain way to use them, adding them up and drawing each glyph at
the nearest pixel to where the pen has got to, puts the same two letters a
pixel further apart in one place than in another, by what was left over
before them: with an `o` of 17 and a quarter pixels, two of them are 17
pixels apart three times and 18 the fourth. That is plain to the eye on a
page, so it is the step that is rounded and not the pen; and the kerning
is added before the rounding, since half a pixel of it rounded by itself
would be a whole pixel or none. So a kerning of less than half a pixel
moves a letter a whole pixel or not at all, by what the advance before it
has over a pixel, and the same for that pair everywhere.

Letters a face joins are drawn as its ligature. A stretch of text is gone
through from its start, and at each place the first of the face's
ligatures that is made of the two or three glyphs there is drawn in their
place (`layout::GlyphRun`, which the measure counts by, and whoever draws
the text goes by as well). A ligature moves the pen by its own step, and
is kerned with the glyphs beside it as any glyph is.

Two glyphs are kerned, and letters are joined, when one follows the other
in a stretch the paragraph setter has measured as one, which is as much of
a word as is in one style, and one face gave them. So nothing is kerned or
joined across a space, across a change of style, or between a letter and a
glyph the fallback face lent; nor across a soft hyphen or a zero width
space, since the setter measures what is before one apart from what is
after.

A line's text is drawn as it would be by itself, and counted so. Where a
line ends at a place the hyphenator found, the letters before the place
are joined with one another and not with those after it, the last of them
is kerned with the hyphen and not with the letter that begins the next
line, and the next line's letters are joined from its first. "office" is
an o, one glyph for its two f's and its i, a c and an e where it is whole;
and where a line ends between its f's it is an o, an f and a hyphen, and
then the glyph for an f and an i, a c and an e. So the same letters are
the same distance apart on every line, a hyphen is as far from an f at
every line's end, and nothing is left unjoined for being where a word
might have been broken and was not.

The measure gives the paragraph setter two numbers for each piece of a
word to count that by. One is the piece's width, such that the pieces from
any cut on come to what the text from there on is as wide as, set by
itself. The other is what a hyphen after the piece comes to: what a line
that ends there is wider by than its pieces, which is the hyphen itself,
what the letters before it gain by being drawn apart from those after
it, and what the last of them moves by for having a hyphen beside it. With
those a line is as wide as it was counted whether a word on it is whole,
begins there or ends there; and one that has only the middle of a word,
broken at both ends, wherever a glyph of it is the same glyph as in the
whole word. To know what the letters after a cut inside a ligature come
to, the glyphs after the cut are found afresh until they fall in with
those of the whole word, which with the ligatures fonts have is at once or
a glyph later. That is given up after eight glyphs, so that no text makes
a call long, and the text after the cut is then counted with the ligature
whole, which is wider than it is drawn: it takes a word of nothing but
f's.

A piece of a word is as wide as its glyphs' steps added up, and a space is
rounded as a letter is, so what is drawn from the same glyphs by the same
steps lines up with what was measured; widths are still counted four to
the pixel, as the paragraph setter is given them, each a whole number of
pixels. (A stretch of letters too wide for a line, or too long for the
setter to take as one word, is measured a letter at a time and so without
kerning or ligatures; whoever draws it kerned and joined draws it a
little narrower than it was counted.) The hyphen shown at a line's end is
the style's U+2010, or its hyphen-minus. It is found once for a style, as
are the marks of the glyphs that begin a ligature, and kept until the
measure is given a face: so it is given its faces once they are held, and
again when they have been given up and held anew. Reading UTF-8 a character at a
time, strictly, is `core/utf8.h`, which the hyphenator shares.

The reader's tests write their images by hand from the description of the
layout (`tests/support/font_image.h`): what is found and what is not, an
image cut short at every byte, and one with every byte changed three ways,
from which nothing may be given that lies outside it. Each of those images
is opened through a map as well, one that gives every stretch as a copy of
exactly its size and frees it when it is given up, so that a byte read
outside a face's own stretch, or after the face was given up, is seen: it
has to go as the image opened whole goes, and every face, once held, to
give the same glyphs and to kern them alike. Kerning has tests of its own
besides: a face kerns the pairs it was given and no others, no glyph of
another face, and nothing by a class its table has not. The builder's
reading of a font's kerning is tried on tables written for it a number at
a time, each thing it has to know of them by itself, and on tables cut
short at every byte and with every byte changed; and `e2e.FontKerning`
sets `bn-font` beside a second reading of two real fonts that kern, Noto
Serif Italic and Noto Sans Regular, which shares none of its code: the
fonts' tables are read there from the description of their layout, and
every two characters of every face have to be kerned in the image by what
the tables come to at the face's size, as `bn-font info --kerning` has to
say too. Ligatures are tried in the same three ways: a face makes the
first of its list that fits, of its own glyphs only, and none whose glyph
it cannot give; the builder's reading of a font's ligatures is given
tables written for it, cut short and changed as the kerning's are; and
`e2e.FontLigatures` reads the same two fonts' tables a second way and
holds an image's lists to them, once with the ligatures' own characters
among those drawn and once without, when their glyphs are no
character's, and `bn-font info --ligatures` to saying the same. The
measure's tests give it faces written by hand that kern and join, and
hold what it says of a word to figures worked out in the test; and then
3,000 words made at random of the letters those faces join and kern, cut
at random, where every line a word could be broken into has to be as wide
as a plain second setting of that line's text, by itself, makes it; and a
paragraph of words the hyphenator breaks between two f's, set by the
paragraph setter at many widths, every line of it held to the same. The
measure has a fuzz target of its own (`face_measure`). The
builder's tests,
and `e2e.FontTool`, try `bn-font` on a real font, Courier Prime, which the
build downloads for them (`third_party/courier-prime`): the test takes the
image apart itself, and the font file as far as its tables of characters
and widths, and holds the image to having a glyph for exactly the
characters the font has, each moving the pen as far as the font's width
for it comes to at that size; `bn-font info --glyphs` to saying of
every glyph what the image's bytes say; and `bn-font sizes` to the sizes
the test works out again, the runs written out a second time and the
compressing done by Python's zlib. Nothing draws a glyph a second way:
that a picture is its letter is for whoever looks at it. The reader has a
fuzz target as well (`font`, see [Fuzzing](fuzzing.md)).

## Fonts on a device

The firmware's fonts live in a partition of the flash of their own, read
where they lie through the processor's mapping of flash into its address
space. The firmware's partition table (`app/firmware/partitions.csv`) has
a 4 MB app partition and, after it, a 12 MB data partition, `fonts`.
Flashing a firmware writes the table and leaves that partition as it was:
an image is megabytes, and stays from one firmware to the next. It is
written by a target of its own, from a file `bn-font` made:

```
cmake --build --preset host-release --target fonts
tools/idf -DBN_FONTS_IMAGE=$PWD/build/host-release/fonts/fonts.bnfn fonts-flash
```

```
-- The fonts image: 10144822 bytes of the partition's 12582912.
...
Wrote 10144824 bytes (3666908 compressed) at 0x00400000 in 56.5 seconds (1435.5 kbit/s).
Hash of data verified.
```

A file that is not a fonts image, or one larger than the partition, is
refused before anything is written. The hardware layer gives the partition
to the rest as a `core::ByteMap` (`hw::SystemFonts()`): a stretch of it is
read by being mapped, and takes every page of 64 KB it touches. So far the
one firmware that reads it is the text viewer's ([Trying faces on the
panel](text-viewer.md)), which is where what it costs on an X4 is given.
The mapping is counted: `font_maps`, `font_maps_refused` and, with the most
it has been, `font_mapped_kb` go out with the other metrics.

### The partition check

A firmware built with `-DBN_PARTITION_CHECK=ON` checks the partition itself
on a device: that it is there, how much of it can be mapped, and what
reading it costs.

```
tools/idf -DBN_PARTITION_CHECK=ON build flash
./build/host-release/tools/trace/bn-trace --seconds 20 | tee partition.log
tools/idf -DBN_PARTITION_CHECK=OFF build flash      # back to the normal firmware
tools/idf -DBN_FONTS_IMAGE=... fonts-flash          # and the fonts image again
```

Flashing it fills `fonts` with 12 MB of a pattern, in place of any image, in
which every four bytes are a number worked out from where they are
(`app/firmware/partition_pattern.py`). At start, before the application,
it says how large the flash chip is, what partitions there are, how much
of `fonts` can be mapped at once, how long mapping a window takes, how
fast the whole partition is read and whether every word of it is right,
and what a read of 64 bytes costs where the cache has it and where it does
not (`app/firmware/main/partition_check.h`). On an X4:

```
partition check: flash: the chip says 16384 KB (ESP_OK); the build takes it to be 16384 KB (ESP_OK).
partition check: partition nvs: type 1, subtype 0x02, at 0x009000, 20 KB.
partition check: partition phy_init: type 1, subtype 0x01, at 0x00e000, 4 KB.
partition check: partition factory: type 0, subtype 0x00, at 0x010000, 4032 KB.
partition check: partition fonts: type 1, subtype 0x40, at 0x400000, 12288 KB.
partition check: pages of address space free for data: 123 (7872 KB).
partition check: the whole partition at once, 12288 KB: ESP_ERR_NO_MEM.
partition check: the most mapped at once: 123 pages (7872 KB).
partition check: a window of 64 KB: mapped in 16 us and unmapped in 4 us, on average of 100 (0 failed).
partition check: a window of 1024 KB: mapped in 79 us and unmapped in 7 us, on average of 100 (0 failed).
partition check: read all 12288 KB in 824 ms (14902 KB/s), through windows of 1024 KB.
partition check: every word is what it should be (3145728 words).
partition check: scattered over 4096 KB: 20000 reads of 64 bytes in 87541 us (4377 ns a read).
partition check: within 8 KB: 20000 reads of 64 bytes in 17349 us (867 ns a read).
partition check: passed.
```

The processor has 8 MB of address space for flash, in 128 pages of 64 KB,
shared by the app's code and constant data and whatever else is mapped:
beside an app of 280 KB, 123 pages were free, so a partition larger than
that is mapped a part at a time. Mapping a window of 1 MB takes five times
as long as one of 64 KB, and is still under a tenth of a millisecond. The
mapping is done by the chip's ROM, where the firmware takes its driver for
the flash from ([Device build](building.md#device-build)). Done by ESP-IDF's own driver it left 124 pages
free and took 26 µs for a window of either size, and reading through it
was as fast (822 ms for the 12 MB).
