# Third-party components

Everything Biblionaut builds against or ships that it did not write. Add an
entry, with the licence checked against the component's own licence file,
before a new dependency lands.

| Component | Used for | Licence | How it is obtained | Shipped in firmware |
|---|---|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) 6.0 | Device SDK: RTOS, drivers, boot, flashing tools | Apache-2.0 | Installed separately; not in this repository | Yes (linked) |
| [FatFs](http://elm-chan.org/fsw/ff/) R0.15, as ESP-IDF carries it | The file system on the SD card | FatFs licence: a one-clause BSD-style licence by ChaN (source must keep the copyright notice; no condition on binaries) | With ESP-IDF, in its `fatfs` component; not in this repository | Yes (linked) |
| [Expat](https://libexpat.github.io) 2.10.0 | The XML parser: reads the files an EPUB describes itself with, and its chapters (`epub/xml.h`) | MIT. Copyright (c) 1998-2000 Thai Open Source Software Center Ltd and Clark Cooper; Copyright (c) 2001-2025 Expat maintainers | Downloaded by CMake when a build is configured, from expat's releases on GitHub, and checked against the SHA-256 in `third_party/expat/CMakeLists.txt`. Compiled as it comes, with our settings (`third_party/expat/expat_config.h`). Not in this repository | Yes (linked), once something in the firmware reads XML. Its licence asks that the notice go with every copy, so it has to go with a firmware that is given out |
| [TeX's hyphenation patterns for American English](https://github.com/hyphenation/tex-hyphen) (`hyph-en-us`, from the hyph-utf8 package at commit `29e003ea`, tagged CTAN-2024.12.31): Gerard D.C. Kuiken's patterns, which hold Donald E. Knuth's from `hyphen.tex` | Where an English word may be broken: `bn-hyphen` makes them into the table that `layout/hyphenator.h` reads, as part of the build, and the table is compiled in (`hyphenation/american_english.h`) | The file's own notice, read from the header of `hyph-en-us.tex` at that commit: "Copyright (C) 1990, 2004, 2005 Gerard D.C. Kuiken. Copying and distribution of this file, with or without modification, are permitted in any medium without royalty provided the copyright notice and this notice are preserved." Knuth's patterns, which are the file's first 4,447 of 4,938, unchanged, come under the notice of `hyphen.tex`, which lets them be changed so long as the result is not called `hyphen.tex`, and has no copyright line | Downloaded by CMake when a build is configured: the plain-text form of the patterns and of the words written out with their breaks (`hyph-en-us.pat.txt`, `hyph-en-us.hyp.txt`), each checked against the SHA-256 in `third_party/hyph-en-us/CMakeLists.txt`. Not in this repository | Yes (as the table), in a firmware that uses it: so far one built with the layout check. The copyright line and notice are compiled in with the table |
| [GoogleTest](https://github.com/google/googletest) 1.17.0 | Host unit tests | BSD-3-Clause | Downloaded by CMake when the tests are configured | No |
| [FreeType](https://freetype.org) 2.15.0 | Draws the glyphs of font files for `bn-font` (`tools/font`), the program that makes the fonts image, on the computer that builds | The FreeType License (`docs/FTL.TXT`), the one chosen of the two its `LICENSE.TXT` offers: BSD-like, with a clause that a product's documentation credits the FreeType Project. Copyright 1996-2002, 2006 by David Turner, Robert Wilhelm, and Werner Lemberg | Downloaded by CMake when a host build is configured, from FreeType's own releases, and checked against the SHA-256 in `third_party/freetype/CMakeLists.txt`. Built by its own CMake files, as a static library with none of the libraries it can use besides. Not in this repository | No. A fonts image holds the glyphs FreeType drew and none of its code. Should `bn-font` itself ever be given out as a program, its documentation has to say it uses FreeType |
| [Courier Prime](https://quoteunquoteapps.com/courierprime) 3.018, at commit `7fd585a2` of [quoteunquoteapps/CourierPrime](https://github.com/quoteunquoteapps/CourierPrime): the regular and the bold | The monospace face of the presets (`tools/font/presets/mono-courier-prime.preset`), unless a preset's own family's is asked for; and in the host tests, the real font the font builder is tried on (`tests/unit/tools/font_tool_test.cpp`, `tests/e2e/font_tool.py`, `tests/e2e/text_viewer.py`) | SIL Open Font License 1.1, read from `OFL.txt` at that commit and from the font files themselves. Copyright 2015 The Courier Prime Project Authors. No reserved font name | For the tests, downloaded by CMake when they are configured, each file checked against the SHA-256 in `third_party/courier-prime/CMakeLists.txt`. For a fonts image: downloaded by CMake when the `fonts` target is built with that preset, from the typeface's repository on GitHub, each file checked against the SHA-256 in `third_party/fonts/CMakeLists.txt`. Not in this repository | In a fonts image made with that preset, as the pictures of its glyphs and their measures, and so in a firmware given out with that image: see Fonts in a firmware, below |
| [Noto Serif, Noto Sans](https://notofonts.github.io) 2.015 and Noto Sans Mono 2.014, at commit `d14cc173` of [notofonts/notofonts.github.io](https://github.com/notofonts/notofonts.github.io): the unhinted TrueType files, four styles of the serif and the sans and two of the monospace | The preset a fonts image is made of unless another is asked for (`tools/font/presets/noto.preset`, `mono-noto.preset`); Noto Sans is the fallback face of the other presets; and in the host tests, Noto Serif Italic and Noto Sans Regular are the real fonts the font builder's reading of kerning is tried on (`tests/e2e/font_kerning.py`) | SIL Open Font License 1.1, read from `fonts/LICENSE` at that commit and from the font files themselves. Copyright 2022 The Noto Project Authors. No reserved font name | Downloaded by CMake when the `fonts` target is built with that preset, and the two files the tests use when a build with tests is built, from the typeface's repository on GitHub, each file checked against the SHA-256 in `third_party/fonts/CMakeLists.txt`. Not in this repository | In a fonts image made with that preset, as the pictures of its glyphs and their measures, and so in a firmware given out with that image: see Fonts in a firmware, below |
| [IBM Plex](https://github.com/IBM/plex) Serif and Sans 3.006 and Mono 2.005, at commit `ba3013d7` of that repository: four styles of the serif and the sans and two of the monospace | The `plex` preset (`tools/font/presets/plex.preset`, `mono-plex.preset`) | SIL Open Font License 1.1, read from `LICENSE.txt` at that commit and from the font files themselves. Copyright © 2017 IBM Corp. with Reserved Font Name "Plex" | Downloaded by CMake when the `fonts` target is built with that preset, from the typeface's repository on GitHub, each file checked against the SHA-256 in `third_party/fonts/CMakeLists.txt`. Not in this repository | In a fonts image made with that preset, as the pictures of its glyphs and their measures, and so in a firmware given out with that image: see Fonts in a firmware, below |
| [Source Serif 4](https://github.com/adobe-fonts/source-serif) 4.005 (commit `80d3f889`), [Source Sans 3](https://github.com/adobe-fonts/source-sans) 3.052 (commit `87b37a2d`) and [Source Code Pro](https://github.com/adobe-fonts/source-code-pro) 2.042 (commit `803b7e23`), each from its repository's `release` branch: four styles of the serif and the sans and two of the monospace | The `source` preset (`tools/font/presets/source.preset`, `mono-source.preset`) | SIL Open Font License 1.1, read from each repository's `LICENSE.md` at that commit and from the font files themselves. Copyright Adobe (http://www.adobe.com/): 2014 - 2023 for the serif, 2010-2024 for the sans and 2023 for the monospace, each with Reserved Font Name 'Source'. Source is a trademark of Adobe | Downloaded by CMake when the `fonts` target is built with that preset, from the typeface's repository on GitHub, each file checked against the SHA-256 in `third_party/fonts/CMakeLists.txt`. Not in this repository | In a fonts image made with that preset, as the pictures of its glyphs and their measures, and so in a firmware given out with that image: see Fonts in a firmware, below |
| [SDL](https://libsdl.org) 3.4.18 | The desktop application's window (`hw/desktop/panel_window_sdl.cpp`), in a build configured with `BN_DESKTOP_WINDOW` | zlib licence. Copyright (C) 1997-2026 Sam Lantinga | Downloaded by CMake when such a build is configured, from SDL's releases on GitHub, and checked against the SHA-256 in `third_party/sdl/CMakeLists.txt`. Built by its own CMake files, as a static library with only what a window needs. Not in this repository | No |
| [zlib](https://zlib.net/) | On the computer that builds only. `bn-font sizes` (`tools/font`) compresses a fonts image's pictures with it, to say what they would take compressed. In the host tests, what our own inflater produces is compared with what zlib does, in the unit tests and in the inflater's fuzz target, and the tests' ZIP files are made with it; so are the PNG reader's test pictures, and its fuzz target reads each picture a second way with it | zlib licence | The computer's own copy, whatever its version: macOS has one (1.2.12 when this was written), and on Linux it is the `zlib1g-dev` package (1.3 on Ubuntu 24.04). Not in this repository | No |
| [libFuzzer](https://llvm.org/docs/LibFuzzer.html), part of LLVM | The fuzzing engine: `tools/fuzz.py` links the fuzz targets (`tests/fuzz`) with it, on a developer's computer, to look for inputs that make a reader of books go wrong. No other build has it | Apache-2.0 with LLVM Exceptions | The computer's own LLVM, of the major version in `tools/llvm-version`, which the format and tidy checks need too. Not in this repository | No |
| [OpenX4 E-Paper Community SDK](https://github.com/open-x4-epaper/community-sdk) | What it worked out about the X4: for the panel, the set-up sequence, the values sent in it, which refresh commands it takes, and the waveform and drive voltages of the gray pass (its own gray table, not the ones it says were taken from the maker's firmware); for the buttons, which pins they are on and the voltage each one gives | MIT | Nothing is downloaded or copied. The values are written into `hw/x4/display.cpp` and `hw/x4/input.cpp`, which are otherwise our own code | Yes (as those values) |

| Granlund and Montgomery's division by multiplication (Torbjörn Granlund and Peter L. Montgomery, *Division by Invariant Integers using Multiplication*, PLDI 1994, figure 4.1) | How `draw/divide.h` divides every pixel of a row by the same number: a multiplier and two shifts worked out once, giving the quotient division gives | A published method. No code was taken | Nothing is downloaded or copied. `Divider` is our own code | Yes (as that method) |
| The JPEG standard (ITU-T Recommendation T.81, *Digital compression and coding of continuous-tone still images*, 1992) | How `draw/jpeg.h` reads a JPEG picture: its markers, Huffman codes, scans and inverse transform. For the tests, the tables of its Annex K (tables K.1 and K.2 of quantization, K.3 to K.6 of Huffman codes) and the way its section K.2 makes a table of codes for a picture, with which `tests/support/jpeg_builder.h` and the encoder in `tests/e2e/jpeg_tool.py` write the tests' pictures | A published standard. No code was taken | Nothing is downloaded or copied. The reader and the tests' encoders are our own code, and the Annex K tables are written into them from the standard | Yes (as that method) |
| Liang's hyphenation method (Franklin Mark Liang, *Word Hy-phen-a-tion by Com-put-er*, Stanford University, 1983), as TeX uses it | How `layout/hyphenator.h` finds where a word may be broken: patterns laid over the word, the largest number at each place deciding | A published method. No code and no patterns were taken | Nothing is downloaded or copied. The hyphenator, the form of its table and `bn-hyphen` are our own code | Yes (as that method) |

ESP-IDF bundles further components under their own licences; those that end
up in the firmware image need their own entries here once the image contents
settle.

## Fonts in a firmware

No font is in this repository, and making a fonts image on one's own
computer asks nothing of anyone. Giving a firmware out with an image in
it is passing the fonts on in another form, and the SIL Open Font License
1.1, which every preset's fonts are under, has terms for that. As its text
reads:

- The fonts may be bundled and given out with any software, and are not to
  be sold by themselves.
- Each copy has to carry the fonts' copyright notices and the licence. So
  a firmware given out with an image made of a preset has to come with
  the notices of that preset's typefaces (the table above has each, and
  Noto's as well for a preset that falls back to it) and the licence's
  text.
- What is made from the fonts stays under the Open Font License. An image
  is such a thing: glyphs chosen from the fonts and drawn at a few sizes.
  It is not under this project's licence, though the firmware beside it
  is.
- A version of a font that has been changed may not go by a reserved font
  name. "Plex" and "Source" are reserved, and an image is a changed
  version. The image holds no names, only roles (serif, sans, monospace,
  fallback), and the firmware is to name faces to whoever reads on it by
  those roles and not by the typefaces' names. A preset's file here is
  named for the fonts it is a recipe for, which are the makers' own files,
  unchanged.

Nothing yet puts the notices into a firmware or beside one: that is to be
done before a firmware with fonts is given out. A preset of one's own may
name fonts under any terms, and what may be done with an image made of
them is for whoever builds it to find out.

No hyphenation patterns are in this repository. A language's patterns
are someone's work and come under terms of their own, which differ from
one language to the next: each set needs an entry here, with its terms
read from its own file, before a table made from it is shipped or kept
here. American English's has one, above.

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
