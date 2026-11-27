# Tools

The host build makes a program for each reader of a book's files, and a
few more, each running the firmware's own code on one file on the desktop.
They are in `build/host/tools/` (and in `build/host-release/tools/`, which
is the build to run them from over many files). Each is described with the
piece it runs.

| Program | Source | What it does | Described in |
|---|---|---|---|
| `bn-trace` | `tools/trace` | Decodes the diagnostic stream, from a device or from a capture | [Diagnostics](diagnostics.md) |
| `bn-zip` | `tools/zip` | Reads a ZIP file as the firmware does | [ZIP files](zip.md) |
| `bn-xml` | `tools/xml` | Does the same for XML | [XML](xml.md) |
| `bn-epub` | `tools/epub` | Says what a book says of itself, and reads its table of contents | [What a book says of itself](package.md), [The table of contents](toc.md) |
| `bn-bundle` | `tools/bundle` | Makes the bundle of a book, and says what a bundle holds; imports the book's chapters into it; prints a chapter of one as text, writes one from that text, and looks for a place in one | [The file a book is imported into](bundle.md#a-bundle-made-from-a-book), [Importing a chapter](chapter-import.md#the-tool), [A chapter's bytes](segment.md#the-tool) |
| `bn-css` | `tools/css` | Reads a style sheet, says which kind of face a `font-family` comes to, finds the sheets a document asks for, and says what their rules come to, what each element then comes to, and what blocks a document is made into | [Style sheets](css.md), [Kinds of face](css-families.md), [A book's rules](css-cascade.md), [What a document asks for](document-sheets.md), [What each element comes to](element-styles.md), [A document's blocks](document-blocks.md) |
| `bn-svg` | `tools/svg` | Draws an SVG file as the firmware does | [Drawings](svg.md) |
| `bn-png` | `tools/png` | Reads a PNG picture as the firmware does | [Pictures](pictures.md) |
| `bn-jpeg` | `tools/jpeg` | Does the same for a JPEG picture | [Pictures](pictures.md#jpeg) |
| `bn-hyphen` | `tools/hyphen` | Makes a language's hyphenation table, and breaks words with one | [Where a word may be broken](hyphenation.md) |
| `bn-lines` | `tools/lines` | Sets text in lines as the firmware does | [Setting a paragraph's text in lines](paragraph-setter.md) |
| `bn-pages` | `tools/pages` | Sets a chapter of a bundle in pages, with the format's reader and the firmware's layout, and prints them as text | [A chapter in pages](pages.md) |
| `bn-font` | `tools/font` | Makes the fonts image the firmware draws text from | [Fonts](fonts.md) |

And the scripts:

| Script | What it does | Described in |
|---|---|---|
| `tools/bn-symbolize` | Turns the addresses in a crash report into functions and lines | [Diagnostics](diagnostics.md#crash-reports) |
| `tools/check-books.py` | Runs the tools over a folder of real books, and compares what each says with a reading by Python | [Real books](real-books.md) |
| `tools/layout-text.py` | Writes books' paragraphs to a file, for the layout check | [Timing layout on a device](layout-check.md) |
| `tools/fuzz.py` | Builds the fuzz targets with a fuzzing engine, and runs them | [Fuzzing](fuzzing.md) |
| `tools/queued` | Has the heavy checks take turns on a machine | [Checks](checks.md) |
| `tools/setup-idf.sh`, `tools/idf` | Installs the pinned ESP-IDF, and runs `idf.py` with it | [Building](building.md#device-build) |
| `tools/check-format.sh`, `tools/check-banned.py`, `tools/check-tidy.sh`, `tools/check-firmware-tidy.py`, `tools/check-firmware-symbols.sh`, `tools/linux-check.sh` | The checks CI runs | [Checks](checks.md) |
