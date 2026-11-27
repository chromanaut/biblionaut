# Drawings and pictures on the panel

In place of the test pattern ([The panel and the buttons](panel-and-buttons.md)),
the application can show the SVG, PNG and JPEG
files in the card's `drawings` folder (`reader/drawing_viewer.h`): a
drawing drawn as a drawing in a book is, and a picture read (`draw/png.h`,
`draw/jpeg.h`, a JPEG made as small as it can be as it is read and still be
as large as it is shown) and scaled (`draw/scale.h`). Each is fitted to the page the way the device is held to
be read, as large as fits and in the middle of it, turned into the panel's
rows, and its grays dithered to the panel's black and white
(`draw/dither.h`). It is for seeing the renderer's and the picture
reader's work on a panel, and the first place they run in the
application's loop. On the desktop, with `BN_STORAGE` naming the folder
that stands for the card:

```
BN_STORAGE=card BN_DRAWINGS=1 ./build/window/app/desktop/biblionaut-desktop --window
```

| Button | What it does |
|---|---|
| Down, Right | The next drawing or picture, by a half refresh, going round from the last to the first |
| Up, Left | The one before |
| Confirm | The same one again, by a half refresh |
| Back | The same one again, by a full refresh |

A firmware built with `-DBN_DRAWING_VIEWER=ON` does the same with
`/drawings` on its card. Nothing else drives the panel then, and none of
the checks can be asked for with it. With `-DBN_DRAWING_TOUR=ON` as well,
or `BN_DRAWING_TOUR` set on the desktop, it goes round the drawings twice
by itself, the second time from the cache (below), each begun as its last
refresh ends, as if Down had been pressed then, and a line says when the
tour is over: for timing them with nobody at the buttons. One that cannot be shown is passed over in the same way,
once its line has said why.

```
tools/idf -DBN_DRAWING_VIEWER=ON -DBN_DRAWING_TOUR=ON build flash
./build/host/tools/trace/bn-trace --seconds 600 --save tour.bin
```

A drawing is drawn a band of 16 of the panel's rows at a time, its file
read once and what it draws kept (below), and each band dithered to the
panel's levels and written to both of the panel's planes, so that the half
or full refresh that shows it leaves both right; the drive voltages go off
as the refresh ends. On a panel with grays, as the X4's, each gray is black
in those planes, and a gray pass after the refresh brings the grays out,
as it does for a picture (below). A turn of
the loop draws for about 20 ms and returns, so a drawing of megabytes takes
many turns and the buttons are heard throughout: a press while a drawing is
being drawn gives it up for the one asked for. For each drawing a line of
log text says what it was, how it ended, how many shapes it filled and
stroked, how long it took from the press to its refresh, and what the
renderer left
out:

```
drawing 2 of 3: lewis-carroll-illustration-7.svg, ok, 17 shapes, 0 strokes, 37 ms
```

That is an engraving of 932 KB from a public book, drawn by the optimised
desktop application. No stroke
is drawn thinner than a pixel (`SetThinnestStroke()`, in [Drawings](svg.md#a-whole-file)): one much
thinner would be a light gray, which the panel's levels show as a broken
line.

A drawing is read once, and kept on the card while it is shown, in a
hidden file at the top of the card (`/.drawing-spool`, made again for each
drawing): what it draws for each band of the panel ([Keeping a
drawing](drawing-spool.md)). Each band is then drawn from what was kept, its own part and no
more. If the card will not keep it, or it would be more than 2 MB, which
is twice what the largest drawing in the public books keeps, the
drawing is read again for each band instead, as it could always be, and a
line says why. The viewer takes about 101 KB of the heap when it starts:
26 KB for the filler's cells, which also hold what each band has in hand
while the file is read, 24 KB for what a drawing defines, 8 KB for the
links of what is kept, 13 KB for a band of grays, which also gathers what
is kept into writes of up to 8 KB while the file is read, since the card
writes a few kilobytes at once faster than a chunk at a time, 8 KB for the renderer, a
kilobyte for the recorder, 6 KB for what is carried in dithering and a
band of bits, 8 KB of the file read at a time, 3 KB for a band of what
goes to the cache or comes from it, at two bits a pixel, and 4 KB for the
cache to gather such bands into whole sectors of the card.

What is shown is kept as well, in the cache of pictures on the card
(`/.picture-cache`, [Keeping what is shown](picture-cache.md)), the size it is
shown and in its levels, two bits a pixel in the X4's four: a drawing as
it lies on the panel, and a
picture as it lies on the page, its rows in the order they came. Shown again, while its file is the same size and was last written
at the same time, it is read from there a band at a time and written to
the panel, and nothing is drawn. What the cache does not have is drawn,
and kept as its bands are written; what it has damaged, or half written
by a press that gave it up, is drawn and kept again. A drawing whose file
could not all be read, or a picture not read whole, is not kept. A line
says what the cache did:

```
drawing 2 cache: kept, 45 KB in 1 ms
drawing 2 of 3: lewis-carroll-illustration-7.svg, from the cache, 3 ms
drawing 2 cache: read 45 KB in 0 ms
```

That is the same engraving, the first time and then gone back to, on the
optimised desktop application.
The trace has a span for each drawing or picture, from the press to the
refresh, and the metrics the time the last took (`drawing_ms`).

Once a drawing's refresh is over, a second line says where its time went,
added up by the viewer, since a long drawing reads the card more times
than a capture can keep a span for: how many bands it was drawn in, how
much of the file was read in all, and how long went in reading it, in
rendering, in dithering, in writing the bands to the panel, in waiting
for the panel to take a band, in the viewer's own turns otherwise
(`other`), in the loop between the viewer's turns, and in the refresh. A
third says how much was kept and read back, how long each took (and in
how many writes to the card) and how
long drawing the bands from it took; or why the drawing was not kept. A
last says how long writing the planes of its gray pass took, and the pass,
or why there was none. A picture's says how long its refresh took, and
then the same of its gray pass.

```
drawing 2 timed: 30 bands, 918 KB read in 0 ms; rendering 19 ms, dithering 8 ms, writing 0 ms, waiting 6 ms, other 0 ms, between turns 0 ms, refreshing 1571 ms
drawing 2 timed: spool 623 KB written in 0 ms (78 writes), 623 KB read in 1 ms; replaying 4 ms
drawing 2 timed: gray planes 1 ms, gray pass 218 ms
```

That is the same engraving as above, 932 KB read once for its size and
once to keep it, on the optimised desktop application, where the panel's
refresh is as long as the X4's.

A picture's rows come from its file in the order the page is read, which
is not the panel's order when the panel is turned to be read, as the X4's
is. Its file is read through once: its rows are read, made the size they
are shown at and dithered, and written to both planes as they come, a
strip at a time. A strip is the rows of the page that fall in one byte of
the panel's rows: on a panel turned a quarter, eight of the page's rows
are a column of the panel one byte wide and the panel's height, written as
one window ([Windows of a plane](panel-and-buttons.md#windows-of-a-plane)); otherwise they are eight of the
panel's rows. The strips above and below the picture are written white. So
nothing of the panel is kept but the strip being filled. A picture whose
rows would need more than 16 KB of the reader's memory, an interlaced PNG,
a JPEG of a kind that is not read, and a file that is not a picture are
not shown, and a line says why.

A picture is dithered to as many levels as the panel shows
(`Display::Levels()`): on the X4, and the desktop's X4, four, the grays by
a gray pass ([Grays](panel-and-buttons.md#grays)). It is shown first by a half or full refresh
in black and white, each of its grays black, and kept in the cache at two
bits a pixel; the drive voltages stay up. Then it is read back from the
cache, its grays are marked in the two planes, strip by strip as before,
and the gray pass brings them out. A picture with no grays has no gray
pass, and one that could not be kept stays in black and white. A press
during the first refresh, for another, is not kept waiting for the grays.

For each picture shown, a line says what it was, the size it was shown
at, and how long the reading, the scaling, the dithering (with the setting
of the panel's bits) and the writing took, and then from the press to its
refresh; and once the panel is done, a second line says how long the
refresh took, and the gray pass: writing its planes, and the pass itself,
or why there was none:

```
picture 3 of 3: c-gutenberg.png, ok, 332x500 palette shown 480x723: reading 1 ms, scaling 0 ms, dithering 6 ms, writing 0 ms; 10 ms
picture 3 timed: refreshing 1577 ms; gray planes 4 ms, gray pass 209 ms
```

That is an illustration from a Project Gutenberg book in `corpus/`, made
larger to fill the page, on the optimised desktop application, where the
refresh and the gray pass take as long as on the X4. What a picture needs
is taken from the heap the first time one is shown, and kept: about 46 KB,
32 KB of it PNG's window, 9 KB the JPEG reader, 3 KB the PNG reader, and
2.6 KB for a row at the size shown, the two strips being filled and two
rows of bits. The rest of what a picture needs it has only while it is
read, and that lies in memory the drawings have, which a picture leaves
idle, since one thing is shown at a time and each begins with nothing left
of the one before: the reader's rows, 16 KB, and the scaler's sums, 6 KB,
in the 24 KB for what a drawing defines, and the 4 KB buffer its file is
read through in the 8 KB a drawing's file is read into. (On a panel longer
than 1,024 pixels the sums would not fit there, and are taken from the
heap.)
