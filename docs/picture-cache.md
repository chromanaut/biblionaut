# Keeping what is shown

A drawing made once can be shown again without being made again:
`draw::PictureCache` (`draw/picture_cache.h`) keeps it on the card, as it
was shown. A picture read and scaled is kept the same way.

- **What is kept**: the picture at the size it is shown and in the
  panel's levels, a pixel's level packed into 1, 2, 4 or 8 bits, row after
  row. It is found by a key: the path of the file it was made from, the
  file's size and when it was last written, its own width and height, the
  levels, and how it lies on the panel. A file changed since is not the
  same key.
- **Where**: a folder of 64 files, each holding one picture, so the cache
  never holds more than 64. The key chooses a picture's home file, and it
  may be kept in any of the 8 files from there on. Of those it takes the
  place of what was kept before as the same picture, else of the first
  that holds nothing whole, else of the picture kept longest ago. Looking
  for a picture, or for a place to keep one, stops at the first of its
  places that has no file, since nothing is kept past one.
- **Each file**: a header, which says what the picture is and when it
  was kept, and carries a checksum of itself and one of the rows; then
  the rows. The header is a sector of the card, 512 bytes, of which it
  uses 44. The
  rows are written first and the header last, so a file left half written
  by a press, or by the power going, has no header. Files are not synced:
  what the card had not written when the power went shows in the rows'
  checksum, which is checked as the last row is read.
- **Whole sectors**: a card is read and written in sectors, and a write
  that begins or ends part way through one costs it a read of that sector
  and a write of all of it. So the rows are gathered, in memory the
  caller gives (`GatherIn()`; the viewer gives 4 KB), and go to the card
  as many whole sectors at a time as that holds, each write begun where a
  sector begins; only the last write of a picture's rows ends part way
  through one. They are read back the same way. The X4's driver then
  moves every sector straight between the card and that memory. Given a
  band of rows at a time, it had to go by way of a block of its own from
  the heap, of up to 3 KB, for 84 of the reads and 28 of the writes that
  showing the card's 29 pictures took.
- **Bad input**: whatever a file holds, reading it ends in a status:
  `kMissing`, `kOther` (another picture, or this one before its file
  changed), `kDamaged`, `kStoreFailed`; and no row is given that is not
  whole. The viewer draws again what it finds damaged, and keeps it again.

On the X4's panel held as it is read, the viewer keeps a drawing as it
lies on the panel, so a page's 480 by 800 is 800 by 480 in the cache,
47 KB at a bit a pixel, and a drawing narrower than the page keeps only
its own part. It keeps a picture as it lies on the page, its rows in the
order they come from the file, which is the order its strips are written
in again: so the key of a picture says it is not turned, whichever way
the panel is.

A picture has 8 places from its home, and not one file chosen by its key,
because two pictures are likely to choose the same file and each take the
place of the other. Gone round twice, 18 files chosen at random have
something made again the second time in nine tours out of ten with one
file each, and in none with 8 places from a home. Looking at a file costs
about 3.4 ms on the X4's card, so
looking at all 8 places, as when the picture is not there and they are
all full, costs about 27 ms.

The unit tests keep pictures in every number of bits and read them back in
pieces, look for them with each part of the key changed, fill more files
than there are, keep as many pictures with one home as it has places and
then more, keep one where a file holds nothing whole, and damage the files
in each place a byte can be damaged.
