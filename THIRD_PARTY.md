# Third-party components

Everything Biblionaut builds against or ships that it did not write. Add an
entry, with the licence checked against the component's own licence file,
before a new dependency lands.

| Component | Used for | Licence | How it is obtained | Shipped in firmware |
|---|---|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) 6.0 | Device SDK: RTOS, drivers, boot, flashing tools | Apache-2.0 | Installed separately; not in this repository | Yes (linked) |
| [GoogleTest](https://github.com/google/googletest) 1.17.0 | Host unit tests | BSD-3-Clause | Downloaded by CMake when the tests are configured | No |
| [OpenX4 E-Paper Community SDK](https://github.com/open-x4-epaper/community-sdk) | What it worked out about the X4: for the panel, the set-up sequence, the values sent in it, and which refresh commands it takes; for the buttons, which pins they are on and the voltage each one gives | MIT | Nothing is downloaded or copied. The values are written into `hw/x4/display.cpp` and `hw/x4/input.cpp`, which are otherwise our own code | Yes (as those values) |

ESP-IDF bundles further components under their own licences; those that end
up in the firmware image need their own entries here once the image contents
settle.
