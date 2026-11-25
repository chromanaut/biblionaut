# Storage

Files are reached through three things in `core`:

- `core/storage.h` is the bare interface a hardware port implements: open,
  read and write at an offset, sync, the handful of calls that work on
  names, and listing a directory. Every call says how it ended
  (`StorageStatus`), and every port accepts the same paths, which are the
  ones an SD card can hold. A file is opened one of three ways: to be read
  (`kRead`); new and empty, to be written (`kCreate`); or as it is, to be
  read and written over and added to (`kUpdate`), which is how a file that
  grows over many sessions is kept without being written out again each
  time. A file has one writer or any number of readers, never both, on
  every port: that is what FatFS allows. Eight things can be open at once
  (`Storage::kMaxOpen`), a directory being listed counting as one, and two
  of them can be directories. `Stat()` and the listing say of a file its size
  and when it was last written (`FileInfo::modified`: FAT's date and time
  on a card, seconds on the desktop), which is how something made from a
  file, such as a picture in the cache, is known to be out of date.
- `core/block_file.h` is what code reads and writes files with. It keeps a
  position and a buffer of its own, asks storage for whole blocks at
  multiples of the block size, and counts what it asks for (`sd_reads`,
  `sd_read_bytes`, `sd_seeks`, `sd_writes`, `sd_write_bytes`, `sd_syncs`).
  It is also a `ByteSource`, so a parser can read a file without knowing it
  is one.
- `core/file_replacement.h` writes a file so that it is never found half
  written, whenever the power goes. An SD card cannot put one file in the
  place of another in one step, so it takes several, and
  `RecoverReplacement()` finishes or undoes whatever was cut short.

On a device the card is on the wires it shares with the panel, driven by
ESP-IDF's SD driver and FatFS, which is called directly
(`hw/esp32c3/sd_storage.cpp`). Only FAT file systems are understood, which
is what cards of up to 32 GB are sold with; a bigger card comes with exFAT
and has to be formatted as FAT32 first.

The driver moves whole sectors straight between the card and the caller's
memory only if that memory begins at a multiple of four. If it does not,
it takes a block as large as the sectors from the heap, goes by way of
that, and gives it back: memory nobody here asked for, at a moment nobody
chose. FatFS hands the driver the whole sectors of a read or a write
where they fall in the caller's memory, so that happens when a read or
write begins part way through a sector, at a place in the file that is no
multiple of four, and goes on over a whole sector or more. The metrics
count them (`sd_reads_copied`, `sd_writes_copied`) and say how many bytes
the last took and the most any did (`sd_copied_bytes`). A `BlockFile`
asks for whole blocks, and the cache of pictures reads and writes whole
sectors through memory of its own ([Keeping what is
shown](picture-cache.md)).

FAT keeps a file as a chain of pieces, and to go to a place in a file
FatFS follows the chain there, from the file's start if the place is
behind where it is, reading the card's table as it goes: the larger the
file, the longer that takes. On an X4, going from the start of a book of
353 MB to its end took 67 ms. So the driver gives a large file that is
opened to be read a table, which FatFS fills with a map of the file's
pieces and goes by afterwards, reading nothing: a place in that book then
costs what it does in a small one, 3.5 ms with the 4 KB read there. Making
the map follows the chain once, so it is paid for when the file is opened
(62 ms for that book). A file is large, here, if its chain is longer than
one sector of the card's table holds, which is 128 clusters (4 MB where a
cluster is 32 KB): a shorter chain is one read of the table however it is
followed, and such a file is opened as it always was. A table has room for
a file in 63 pieces, and is half a kilobyte; there are four of them, for
the eight files that can be open, since a map is for a large file read
from all over, which is a book, and FatFS cannot make a file longer that
has one, so a file opened to be written is given none. A file in more
pieces, and one opened while all four are in use, goes without a map and
is only slower. The metrics say how many files were given a map
(`sd_maps`), how many were in too many pieces (`sd_maps_refused`), how
many found no table free (`sd_maps_none_free`), the most numbers a file's
map needed (`sd_map_items`, two for each piece and two more), and the size
of the card's clusters (`sd_cluster_bytes`).

The card is clocked at 20 MHz, which is the driver's own choice
(`sd_clock_khz` in the metrics says what it was set to), though the panel
on the same wires is sent to at 40. A card may only be clocked faster once
it has been switched to its high speed, and the 16 GB card in the X4 this
was tried with offers none over these wires: asked which speeds it has, it
names the ordinary one alone, and told to switch, it refuses. So no card
is asked.

On the desktop the card is a folder (`hw/desktop/folder_storage.h`), the
one named by the `BN_STORAGE` environment variable; without it there is no
card. It is made to behave as a card does where that is cheap: the same
paths are refused, only eight files can be open at once (a directory being
listed counts as one, and only two can be), a file open to be written
cannot be opened again nor an open file opened to be written, an open file
cannot be removed or renamed, and a rename will not replace a file.

The application gets storage ready at start and says whether it is
(`storage_ok` in the metrics, `reader/storage_start` in the trace). It
does nothing else with it yet. On a device the trace also says what was
found, and why not if nothing usable was:

```
hw/sd_card megabytes=30436          the card's size
hw/sd_file_system kind=3            1: FAT12, 2: FAT16, 3: FAT32
hw/sd_start status=0  312.5 ms      0: ready, 1: no card, 8: a card that cannot be used
hw/sd_card_error code=263           the card did not answer
hw/sd_mount_error code=13           no file system FatFS knows: exFAT, most likely
```

## The storage check

A firmware built with `-DBN_STORAGE_CHECK=ON` puts the card through its
paces (on the desktop, set `BN_STORAGE_CHECK`): at start, and again at each
press of the Power button, which also looks for a card again if none was
found before. It writes a 256 KB file in 4 KB blocks, reads it back
checking every byte, and reads short pieces from all over it. It opens the
file again as it is, finds that it cannot then be opened a second time,
writes over one block in the middle and adds one at the end, and reads
those and the blocks either side back. It has eight different files open
at once and finds that a ninth is refused. It replaces a small file twice,
and removes what it made (`reader/storage_check.h`). It does a
little each turn, so the panel and the buttons carry on around it. Each
read, write and sync of the card is in the trace with the time it took
(`hw/sd_read`, `hw/sd_write`, `hw/sd_sync`), and the outcome is there and
in the metrics:

```
reader/storage_check passed=1
reader/storage_check_failed step_and_status=307     step 3 (writing), status 7 (no space)
storage_check=1[1..1]
```

```
tools/idf -DBN_STORAGE_CHECK=ON build flash
./build/host/tools/trace/bn-trace | tee card.log
tools/idf -DBN_STORAGE_CHECK=OFF build flash     # back to the normal firmware
```

It can be combined with `-DBN_PLANE_CHECK=ON`, to see whether using the
card disturbs what the panel holds.
