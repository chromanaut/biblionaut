# Third-party components

Everything Biblionaut builds against or ships that it did not write. Add an
entry, with the licence checked against the component's own licence file,
before a new dependency lands.

| Component | Used for | Licence | How it is obtained | Shipped in firmware |
|---|---|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) 6.0 | Device SDK: RTOS, drivers, boot, flashing tools | Apache-2.0 | Installed separately; not in this repository | Yes (linked) |
| [FatFs](http://elm-chan.org/fsw/ff/) R0.15, as ESP-IDF carries it | The file system on the SD card | FatFs licence: a one-clause BSD-style licence by ChaN (source must keep the copyright notice; no condition on binaries) | With ESP-IDF, in its `fatfs` component; not in this repository | Yes (linked) |
| [Expat](https://libexpat.github.io) 2.10.0 | The XML parser: reads the files an EPUB describes itself with, and its chapters (`epub/xml.h`) | MIT. Copyright (c) 1998-2000 Thai Open Source Software Center Ltd and Clark Cooper; Copyright (c) 2001-2025 Expat maintainers | Downloaded by CMake when a build is configured, from expat's releases on GitHub, and checked against the SHA-256 in `third_party/expat/CMakeLists.txt`. Compiled as it comes, with our settings (`third_party/expat/expat_config.h`). Not in this repository | Yes (linked), once something in the firmware reads XML. Its licence asks that the notice go with every copy, so it has to go with a firmware that is given out |
| [GoogleTest](https://github.com/google/googletest) 1.17.0 | Host unit tests | BSD-3-Clause | Downloaded by CMake when the tests are configured | No |
| [zlib](https://zlib.net/) | Host tests only: what our own inflater produces is compared with what zlib does, in the unit tests and in the inflater's fuzz target, and the tests' ZIP files are made with it | zlib licence | The computer's own copy, whatever its version: macOS has one (1.2.12 when this was written), and on Linux it is the `zlib1g-dev` package (1.3 on Ubuntu 24.04). Not in this repository | No |
| [libFuzzer](https://llvm.org/docs/LibFuzzer.html), part of LLVM | The fuzzing engine: `tools/fuzz.py` links the fuzz targets (`tests/fuzz`) with it, on a developer's computer, to look for inputs that make a reader of books go wrong. No other build has it | Apache-2.0 with LLVM Exceptions | The computer's own LLVM, of the major version in `tools/llvm-version`, which the format and tidy checks need too. Not in this repository | No |
| [OpenX4 E-Paper Community SDK](https://github.com/open-x4-epaper/community-sdk) | What it worked out about the X4: for the panel, the set-up sequence, the values sent in it, and which refresh commands it takes; for the buttons, which pins they are on and the voltage each one gives | MIT | Nothing is downloaded or copied. The values are written into `hw/x4/display.cpp` and `hw/x4/input.cpp`, which are otherwise our own code | Yes (as those values) |

| Liang's hyphenation method (Franklin Mark Liang, *Word Hy-phen-a-tion by Com-put-er*, Stanford University, 1983), as TeX uses it | How `layout/hyphenator.h` finds where a word may be broken: patterns laid over the word, the largest number at each place deciding | A published method. No code and no patterns were taken | Nothing is downloaded or copied. The hyphenator, the form of its table and `bn-hyphen` are our own code | Yes (as that method) |

ESP-IDF bundles further components under their own licences; those that end
up in the firmware image need their own entries here once the image contents
settle.

No hyphenation patterns are in this repository or in the firmware. A
language's patterns are someone's work and come under terms of their own,
which differ from one language to the next: each set needs an entry here,
with its terms read from its own file, before a table made from it is
shipped or kept here.

## Test books

`corpus/` holds 28 EPUB files that the tests read. They are not this
project's work and are not under its licence. Each is kept unchanged, under
the terms it came with. None of it is in the firmware.

| From | Files | Terms |
|---|---|---|
| [Project Gutenberg](https://www.gutenberg.org) | 16 | The Project Gutenberg License, which is in each file. The works are in the public domain in the United States |
| [Standard Ebooks](https://standardebooks.org) | 7 | Texts and artwork in the public domain in the United States; Standard Ebooks' own work on each dedicated to the public domain (CC0 1.0) |
| [IDPF's EPUB 3 samples](https://github.com/IDPF/epub3-samples), release `20230704` | 5 | Three under CC BY-SA 3.0, one under CC BY-SA 2.5, one under the GNU Free Documentation License 1.2, as each file states. Two carry fonts under the fonts' own free licences (DejaVu Sans; STIX) |

`corpus/README.md` lists every file, with its authors, where it came from
and its terms.
