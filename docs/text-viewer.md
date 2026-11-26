# Trying faces on the panel

In place of the test pattern, the application can show text set in a fonts
image's faces (`reader/text_viewer.h`): the card's
`/layout/paragraphs.txt`, the file the [layout check](layout-check.md) sets, a page at a time.
It is for seeing faces and sizes on a panel, and the first place the
hyphenator, the paragraph setter and the fonts work together in the
application's loop. On the desktop, with `BN_TEXT` naming an image `bn-font`
made and `BN_STORAGE` the folder that stands for the card:

```
BN_STORAGE=card BN_TEXT=courier.bnfn ./build/window/app/desktop/biblionaut-desktop --window
```

On a device, a firmware built for it shows the same file from the card in
the faces of the image in its `fonts` partition ([Fonts on a
device](fonts.md#fonts-on-a-device) says how an image gets there):

```
tools/idf -DBN_TEXT_VIEWER=ON build flash        # back to normal: -DBN_TEXT_VIEWER=OFF
```

| Button | What it does |
|---|---|
| Right, Down | The next page, by a half refresh |
| Left, Up | The page before, of those gone through |
| Confirm | The next of the image's sizes, from where the page shown begins |
| Back | The sans in place of the serif, or the serif again, from the same place |

The text is read as `bn-lines` reads it: an empty line ends a paragraph, a
line end inside one is a line the text ends, and what is between two
asterisks is in italic. A paragraph is set by the paragraph setter with the
image's faces for its measure and the built-in table of American English,
its lines spread to the full width but the last, with 24 pixels left round
the text and a third of a line between paragraphs. Every glyph is on a
whole pixel, and as many pixels after the one before it wherever the two
come, kerned as their face kerns them ([the
measure](fonts.md#reading-the-image) says how, and why). What a spread
line has over goes between its words in whole pixels: the same to each
place, and one more to as many places as there are pixels still left,
taken evenly along the line.

No picture of the page is kept. Setting a page leaves a list of its glyphs,
each with where its picture goes (12 bytes a glyph, 3,072 of them at the
most), and the panel's rows are made from the list a band of 16 at a time,
turned the way the device is held to be read, with the darker ink where
two glyphs meet. On a panel that shows four levels, as the X4's does, a
page is shown first with every pixel that has any ink black, by a full
refresh the first time and a half refresh after; then the bands are made
again as the marks of the pixels with one level of ink and with two, and
the gray pass (see [Grays](panel-and-buttons.md#grays)) makes those light and dark gray, the drive
voltages kept up between the two. A page with no gray in it has no gray
pass. On a panel of two levels, a pixel with two or three levels of ink of
the three is black.

Nothing in it waits. A turn of the loop works for about 20 ms: a paragraph
is read 256 bytes at a step and given to the setter a call at a step, its
lines are placed one at a step and the bands written one at a step, so a
paragraph of any length leaves the buttons answering. What a button asks
for is done once the page under way is set and the panel is free: a page
whose rows are still being written is given up for the one asked for, and
one already shown gives up its grays. For each page shown, a line says
what it was and where the time went:

```
text: page 1, serif 30 px, 21 lines, 368 glyphs: set 2 ms, planes 17 ms, refresh 3592 ms, gray planes 11 ms, gray pass 214 ms
text: page 2, serif 30 px, 21 lines, 388 glyphs: set 1 ms, planes 12 ms, refresh 1605 ms, gray planes 12 ms, gray pass 225 ms; the end of the text
text: page 1, serif 22 px, 29 lines, 709 glyphs: set 3 ms, planes 9 ms, refresh 1582 ms, gray planes 9 ms, gray pass 217 ms
text: page 1, sans 22 px, 29 lines, 709 glyphs: set 4 ms, planes 11 ms, refresh 1584 ms, gray planes 9 ms, gray pass 218 ms
```

(From the desktop, in a build with the sanitizers: the refreshes are the
simulated panel's, and the rest is no measure of a device.) It takes about
54 KB from the heap for the X4's panel, most of it the list of glyphs, and
keeps the paragraph being set, 64 KB of text at the most.

The image is read a face at a time ([Reading the image](fonts.md#reading-the-image)), since a
device cannot map the whole of one: the viewer holds the regular and the
italic of the serif or the sans at the size text is set in, and the
fallback face of that size if the image has one, and gives them up for the
next when the size or the face is changed. A line says what it holds then:
how many faces, how much of the image they are, and how much is mapped,
which is more, since flash is mapped in pages of 64 KB and the table of
faces takes one. A face there is no room for is said in that line, and
what would have been set in it is not drawn. The desktop counts room as the
device it simulates does.

With `-DBN_TEXT_TOUR=ON` as well (`BN_TEXT_TOUR` on the desktop) the viewer
goes through the text by itself, for timing it with nobody at the buttons:
from the image's smallest size to its largest, two pages in the serif or
the sans and two in the other. On an X4, with the image of the `noto`
preset in its partition and a file of text on its card:

```
text: serif 22 px: 2 faces held, 288 KB of the image; 448 KB mapped
text: page 1, serif 22 px, 22 lines, 610 glyphs: set 23 ms, planes 74 ms, refresh 3580 ms, gray planes 71 ms, gray pass 207 ms
text: page 2, serif 22 px, 22 lines, 456 glyphs: set 18 ms, planes 66 ms, refresh 1574 ms, gray planes 64 ms, gray pass 204 ms
text: sans 22 px: 2 faces held, 266 KB of the image; 448 KB mapped
text: page 1, sans 22 px, 22 lines, 458 glyphs: set 19 ms, planes 63 ms, refresh 1574 ms, gray planes 61 ms, gray pass 207 ms
```

(and so on through 26, 30 and 34 px)

```
text: serif 38 px: 2 faces held, 691 KB of the image; 896 KB mapped
text: page 1, serif 38 px, 14 lines, 211 glyphs: set 18 ms, planes 67 ms, refresh 1571 ms, gray planes 65 ms, gray pass 203 ms
text: page 2, serif 38 px, 14 lines, 219 glyphs: set 1 ms, planes 69 ms, refresh 1575 ms, gray planes 67 ms, gray pass 201 ms
text: sans 38 px: 2 faces held, 640 KB of the image; 832 KB mapped
text: page 1, sans 38 px, 14 lines, 201 glyphs: set 19 ms, planes 64 ms, refresh 1572 ms, gray planes 62 ms, gray pass 206 ms
text: page 2, sans 38 px, 13 lines, 194 glyphs: set 6 ms, planes 62 ms, refresh 1578 ms, gray planes 61 ms, gray pass 207 ms
text: the tour is done.
```

Over the twenty pages, setting one took 36 ms at the most, kerning and
all, of which reading its paragraphs off the card is a part; making its
rows from the list of glyphs and writing them to the panel took 60 to 74
ms, and as long again for the marks of its grays; the rest of the 1.9
seconds a page takes is the panel's own: a half refresh of 1.57 seconds
and a gray pass of 0.2. Ten times two faces were held, 22 stretches of the
image mapped in all and none refused, 896 KB mapped at the most, of an
image of 10.1 MB; and 233,692 bytes of the heap stayed free. Whether the letters are good to
read at each size is for whoever looks at the panel.

Its tests give it images written by hand, on panels turned each of the
four ways, and hold every pixel the panel shows to the level of its ink,
worked out in the test; and `e2e.TextViewer` runs the desktop application
on an image `bn-font` makes of Courier Prime and adds up the ink in the
pictures the simulated panel saves: two pages of text hold the ink that
`bn-font info` gives for each of its letters, and a hyphen's for each word
broken, and nothing else.
