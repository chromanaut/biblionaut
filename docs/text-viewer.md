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
the text and a third of a line between paragraphs.

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
text: page 1, serif 30 px, 21 lines, 368 glyphs: set 2 ms, planes 6 ms, refresh 3587 ms, gray planes 6 ms, gray pass 212 ms
text: page 2, serif 30 px, 21 lines, 381 glyphs: set 1 ms, planes 7 ms, refresh 1585 ms, gray planes 6 ms, gray pass 216 ms; the end of the text
text: page 1, serif 22 px, 29 lines, 709 glyphs: set 3 ms, planes 7 ms, refresh 1579 ms, gray planes 7 ms, gray pass 217 ms
text: page 1, sans 22 px, 29 lines, 709 glyphs: set 3 ms, planes 8 ms, refresh 1587 ms, gray planes 9 ms, gray pass 216 ms
```

(From the desktop, in a build with the sanitizers: the refreshes are the
simulated panel's, and the rest is no measure of a device.) It takes about
54 KB from the heap for the X4's panel, most of it the list of glyphs, and
keeps the paragraph being set, 64 KB of text at the most. No firmware has
it yet: the fonts image is not yet in a device's flash.

Its tests give it images written by hand, on panels turned each of the
four ways, and hold every pixel the panel shows to the level of its ink,
worked out in the test; and `e2e.TextViewer` runs the desktop application
on an image `bn-font` makes of Courier Prime and adds up the ink in the
pictures the simulated panel saves: two pages of text hold the ink that
`bn-font info` gives for each of its letters, and a hyphen's for each word
broken, and nothing else.
