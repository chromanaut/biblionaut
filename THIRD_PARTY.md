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
| [zlib](https://zlib.net/) | Host unit tests only: what our own inflater produces is compared with what zlib does | zlib licence | The computer's own copy, whatever its version: macOS has one (1.2.12 when this was written), and on Linux it is the `zlib1g-dev` package (1.3 on Ubuntu 24.04). Not in this repository | No |
| [OpenX4 E-Paper Community SDK](https://github.com/open-x4-epaper/community-sdk) | What it worked out about the X4: for the panel, the set-up sequence, the values sent in it, and which refresh commands it takes; for the buttons, which pins they are on and the voltage each one gives | MIT | Nothing is downloaded or copied. The values are written into `hw/x4/display.cpp` and `hw/x4/input.cpp`, which are otherwise our own code | Yes (as those values) |

ESP-IDF bundles further components under their own licences; those that end
up in the firmware image need their own entries here once the image contents
settle.
