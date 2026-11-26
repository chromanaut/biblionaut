# Keeping a drawing

A drawing read again for each strip costs its whole file each time: on
the X4, the largest drawing in the public books, 3.5 MB, took 354 s in 30
bands, about 5 s of reading the card and 5 s of rendering a pass. So a
drawing can be read once and kept, and each strip drawn from what was
kept. Kept so, with what is kept written 8 KB at a time and the file
read 8 KB at a time, it takes 14 s from the press to the refresh.

The X4's card reads faster the more it is asked for at once, 2 MB read
straight through: 0.73 MB/s at 512 bytes a read, 0.89 MB/s at a
kilobyte, 1.38 MB/s at 4 KB, 1.52 MB/s at 8 KB and 1.64 MB/s at 32 KB.
Reads of 512 bytes scattered over the file came to the same rate as reads
of 512 bytes one after another: what costs is how many reads there are.
While a drawing is kept, the file's reads take turns with the writes of
what is kept, and come to less than these rates.

What a renderer draws into is a `Canvas` (`draw/fill.h`): the lines of
each shape's outline, in pixels, and then the shape's filling. A `Filler`
is one, and draws into its strip as the lines come. `SpoolWriter`
(`draw/spool.h`) is another, which keeps them:

- **What it keeps**: each line placed as a filler places it, at a 256th of
  a pixel, with the line that closes an outline left open; and each line
  for every band of the picture whose rows it crosses, since a filler does
  nothing with a line that lies wholly above or below its strip. Which
  bands those are is two divisions a line, made in 32 bits, which hold a
  place: the X4 divides 64 bits by a routine many times slower, and with
  that out of it the card's nine drawings are read into what is kept in
  12.5 s where they took 13.6 s. Each
  filling is kept for every band that has lines it uses up. A band's lines
  and fillings, given back in the order they came to a filler set to that
  band (`BeginBand()`, `ReplayStep()`), draw it byte for byte as drawing
  into it would.
- **Where**: each band has a chunk of 512 bytes in hand, in memory the
  caller gives; a full chunk is added to a store, the caller's
  (`SpoolStore`: a file on a card, or memory), and a table of links, two
  bytes for each chunk the store may hold, chains each band's chunks in
  order.
- **How long a record is**: nearly all of what a drawing keeps is lines
  that go on from where the band's last ended, and in an engraving most
  are shorter than a pixel. So those have no byte to say what they are:
  the top bits of a record's first byte say that it is such a line and how
  long its steps are. It takes 2 bytes if it goes less than a quarter of a
  pixel across and half a pixel down, 3 if less than four pixels each way,
  and 5 or 9 beyond that. A line that begins somewhere else is a move to
  there first: 4 bytes if that is within eight pixels of where the last
  ended, as it is where an outline comes back into a band it left, and 7
  or 9 if not. A filling is 4 to 94.
- **What it comes to**: drawn 480 pixels wide in bands of 16 rows, the
  3.5 MB engraving keeps 315,112 lines in 0.86 MB, the 1.8 MB one 0.52 MB,
  and the Alice engraving 0.32 MB; the drawings of shapes and strokes keep
  tens of kilobytes. About 2.8 bytes a line. On the X4, where the page is
  turned onto the panel, the largest keeps 827 KB, written in 1.0 s and
  read back in 1.3 s.
- **What a call costs**: `ReplayStep()` reads one chunk at the most, and
  fills once at the most.
- **Bad input**: the links given can be too few (`kNoRoom`), the store can
  fail (`kStoreFailed`), and what is read back can be damaged
  (`kBadRecord`); each is a status, and a damaged chunk is never read past
  its end. The object is about a kilobyte.

`bn-svg draw --spool` reads the file once into a recording in memory, and
draws each strip from it:

```
./build/host/tools/svg/bn-svg draw drawing.svg --width 240 --rows 32 --spool --out drawing.pgm
```

```
spool: ok, 5 chunks of 512 bytes, 140 lines and 8 fillings kept
a drawing of 60 by 40, as 240 by 160 in 5 strips of 32 rows: 6 elements, 3 shapes filled, 0 stroked, 20 commands, 138 lines in all, 15488 bytes of cells
left out, or not drawn as written: text
```

The unit tests set drawings kept and given back in bands of 1, 7 and 16
rows and of the whole picture beside the same drawn straight into those
bands, byte for byte: fillings of every kind, boxes, gradients, strokes and
dashes, what lies beyond the picture, what is used again, and drawings made
at random; and give back a recording damaged at random. The renderer's
fuzz target does the same for every file it is given. The nine drawings
timed on the X4 come out the same, byte for byte, with and without
`--spool`.
