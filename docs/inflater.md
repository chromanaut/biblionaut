# Decompression

`core/inflate.h` undoes DEFLATE (RFC 1951), the compression inside a ZIP
file and so inside an EPUB. `Inflater` is a `ByteSource` that reads another:
it draws compressed bytes from its source as it needs them and gives the
plain bytes out in pieces of whatever size is asked for, so a file is never
whole in memory in either form. It is our own code, the same on the desktop
and the device.

- **Memory**: the caller provides the 32 KB window the format needs. The
  object itself is 2.6 KB on the device. It allocates nothing.
- **Flash**: 4.7 KB, as compiled for the device.
- **Speed**: on an X4, over 34 books, it gives out about 2,920 KB a
  second with the ZIP checksum included (which is a quarter of that
  time), taking in about 1,450 KB a second, where the SD card delivers
  about 970. Three things got it there from the 1,550 KB a second it gave out
  at first. The loop that reads a compressed block keeps the state of
  the input in local variables and not in the object, where the compiler
  had to load and store it again for every symbol, since any byte
  written might have changed it. What is produced is written once, into
  the window, and copied to the caller in one piece when the call ends.
  And a repeat of earlier output, or the bytes of a stored block, are
  copied in runs and not a byte at a time. On the desktop the same
  change doubles its speed on the text of the books in `corpus/`, and
  multiplies it by more than eleven on the data of their PNG pictures,
  which is mostly long runs.
- **The checksum** that goes with it, of every byte an entry gives
  (`core/checksum.h`), is worked out from a table of 256 numbers, a
  lookup a byte: 1 KB of flash, and 11.7 MB a second by itself on an X4.
  Of the three ways timed there, it reads books the soonest: a table of
  16 is slower, and four tables of 256, though faster by themselves,
  slow the parsing that goes on between one checksum and the next.
- **Bad input**: whatever bytes it is given, it either produces the data
  they stand for or stops, saying why (`kBadData`, `kTruncated`,
  `kSourceFailed`). It does not check that the data is the right data: that
  is for the checksum a ZIP file carries.
- **One call's work**: a stream cannot make one `Read()` long. DEFLATE
  lets a stream be written so that every byte of output costs a block of
  its own, with its own codes to read and build, or so that block after
  block holds nothing at all; a megabyte of either would keep a call that
  waited to fill its destination busy for as long as the megabyte lasted.
  So a call starts 17 blocks at the most and then comes back with what it
  has, which may be fewer bytes than it was asked for, and a stream with
  more than 16 blocks in a row that produce nothing is refused as bad
  data (a compressor writes such a block where it was told to flush:
  three together at the most). A call asked for `n` bytes therefore takes
  no more than `2n + 5,632` bytes from its source, whatever the stream
  (`Inflater::MostInputFor`).
- **Reading ahead**: it may take up to 260 bytes from its source that come
  after the end of the stream, so the source should end where the stream
  does.

Its tests build streams by hand for each rule of the format and each way of
breaking it, and compare it with zlib on streams zlib compresses: 800 of
them across kinds of data, sizes, compression levels and strategies, and
again with the input and output cut into pieces of many sizes. Since
output is copied in runs within a window that is a ring, there are streams
made for the places where a run has to stop: repeats short and long, from
one byte back to 32,768, each laid across the window's end twice, and
stored blocks of many lengths, some begun with two of their bytes already
taken into the buffer of bits and some with none. They also turn over
every bit of a stream, one at a time, and cut it short at every length, to
see that the inflater always stops. Every call in every one of
those tests is held to what one call may take from its source; and there
are streams made for that alone: a megabyte of blocks with nothing in
them, and blocks of one byte each whose codes are written the longest way
the format has, which bring a call to within a tenth of the bound.
