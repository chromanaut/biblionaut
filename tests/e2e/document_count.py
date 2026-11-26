#!/usr/bin/env python3
"""End-to-end check of what tools/check-books.py counts in the documents of a spine, run by CTest.

The script counts the paragraphs and the ids of every document of each
book's spine, and compares them with nothing: no tool reads a document's
text yet. So this is where the counting itself is checked: it is run over
two books made here, whose paragraphs and ids were counted by hand as
they were written, and what it prints is set beside that.

Between them the books have a paragraph inside a paragraph's block, a
line end, text in the head, in a style and in an SVG picture, white space
in rows, characters of more than one byte and characters written as
references, an id given twice, an anchor named and not given an id, a
document the spine names twice, an item of the spine that is a picture,
and one that is no file of the book.

usage: document_count.py CHECK_BOOKS BN_ZIP BN_XML BN_EPUB BN_CSS BN_PNG BN_JPEG BN_BUNDLE WORK_DIR
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True  # Or Python leaves compiled copies of the tests' scripts beside them.

import epub_tool  # noqa: E402  (found through the path set just above)
from epub_tool import expect  # noqa: E402


def xhtml(body: str, head: str = "<title>None of this is text</title>") -> bytes:
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml"><head>{head}</head>\n'
            f"<body>{body}</body></html>\n").encode()


# Eight paragraphs, the largest of 38 bytes ("Plain text, on two lines," and,
# after a line end, "and a third."), and six ids whose names come to 24
# bytes, the longest of them 6.
FIRST = xhtml(
    '<h1 id="top">A   heading</h1>\n'                                    # "A heading"
    "<p>Plain   text,\n   on two\tlines,<br/>and a third.</p>\n"
    '<div>Before<p id="two">Inside é</p>after</div>\n'                # "Before", "Inside é" (9 bytes), "after"
    "<p>One<br/>two</p>\n"                                                 # "One", a line end, "two": 7 bytes
    '<p><a name="three">x</a><a id="four" name="not-counted">y</a><span id="top">z</span></p>\n'  # "xyz"
    "<p>   </p>\n"                                                         # No text: no paragraph.
    '<p><img src="plate.bin" alt="No text either"/></p>\n'
    '<svg xmlns="http://www.w3.org/2000/svg"><text id="in-svg">Not a paragraph</text></svg>\n'
    '<p id="one">&amp; &#x4E2D;</p>\n',                                    # "& 中": 5 bytes
    head="<title>None of this is text</title><style>p { margin: 0 }</style>")
# Three paragraphs, the largest of 16 bytes, and no ids.
SECOND = xhtml("<p>Short.</p>\n<p>A little longer.</p>\n<p>Short again.</p>\n")
# One paragraph of 18,000 bytes, and 3,000 ids of 6 bytes each.
THIRD = xhtml("<p>" + "".join(f'<span id="n-{number:04d}">中文</span>' for number in range(3000)) + "</p>")

# Two paragraphs, the larger of 4,999 bytes, and no ids.
FOURTH = xhtml("<p>" + "word " * 1000 + "</p>\n<p>Short.</p>\n")
SIZES = sorted(len(document) for document in (FIRST, SECOND, THIRD, FOURTH))

PACKAGE = """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:title>%s</dc:title><dc:language>en</dc:language><dc:identifier id="uid">urn:x:%s</dc:identifier>
</metadata>
<manifest>
%s
</manifest>
<spine>
%s
</spine>
</package>
"""


def write_book(path: Path, title: str, items: list[tuple[str, str, str]], spine: list[str], files: dict[str, bytes]) -> None:
    """A book whose manifest has `items`, each an id, a name and a media type, and whose spine names `spine`."""
    manifest = "\n".join(f'<item id="{given}" href="{name}" media-type="{kind}"/>' for given, name, kind in items)
    order = "\n".join(f'<itemref idref="{given}"/>' for given in spine)
    epub_tool.write_book(path, "OPS/package.opf", PACKAGE % (title, path.stem, manifest, order),
                         {"OPS/" + name: data for name, data in files.items()}, None)


EXPECTED = f"""\
A book's bundle before any chapter is imported, in bytes: 16384 at the median, 16384 for nine in ten, 16384 at the \
most; its pictures: 1 at the median, 1 for nine in ten, 1 at the most; the entries of its contents: 0 at the median, 0 \
for nine in ten, 0 at the most.
Of the 4 documents of their spines, a book's largest paragraph, in bytes: 18000 at the median, 18000 for nine in ten, \
18000 at the most; the most paragraphs in one of its documents: 8 at the median, 8 for nine in ten, 8 at the most; the \
most ids in one: 3000 at the median, 3000 for nine in ten, 3000 at the most.
Items of the spines that are not XHTML by their media type, and were not counted: 1 of the kind jpeg.
Items of the spines that are files their books do not have: 1.

The documents of the spines: 4 of them, in 2 books:
  bytes of a document: {SIZES[2]} at the median, {SIZES[3]} for nine in ten, {SIZES[3]} at the most
  paragraphs in a document: 3 at the median, 8 for nine in ten, 8 at the most
  bytes of a document's largest paragraph: 4999 at the median, 18000 for nine in ten, 18000 at the most
  ids in a document: 6 at the median, 3000 for nine in ten, 3000 at the most
  bytes of the names of a document's ids: 24 at the median, 18000 for nine in ten, 18000 at the most
  bytes of the longest name of an id in a document: 6 at the median, 6 for nine in ten, 6 at the most
  documents with a paragraph over 4, 8, 16, 32 and 64 KB: 2, 1, 1, 0, 0
  the books with the largest paragraphs, in bytes:
       18000  long-paragraphs.epub: OPS/third.xhtml
          38  two-documents.epub: OPS/first.xhtml
  the books with the most paragraphs in one document:
           8  two-documents.epub: OPS/first.xhtml
           2  long-paragraphs.epub: OPS/fourth.xhtml
  the books with the most ids in one document:
        3000  long-paragraphs.epub: OPS/third.xhtml
           6  two-documents.epub: OPS/first.xhtml
  the books with the largest documents, in bytes:
    {len(THIRD):8}  long-paragraphs.epub: OPS/third.xhtml
    {len(FIRST):8}  two-documents.epub: OPS/first.xhtml
The tools and Python agree about every book.
"""


def main() -> int:
    if len(sys.argv) != 10:
        print(__doc__, file=sys.stderr)
        return 2
    script, zip_tool, xml_tool, epub, css, png, jpeg, bundle = sys.argv[1:9]
    work = Path(sys.argv[9])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        page = "application/xhtml+xml"
        # The spine names the first document twice, a picture, and a file
        # the book does not have; the document it leaves out is not counted.
        write_book(work / "two-documents.epub", "Two Documents",
                   [("first", "first.xhtml", page), ("second", "second.xhtml", page), ("plate", "plate.bin", "image/jpeg"),
                    ("gone", "gone.xhtml", page), ("aside", "aside.xhtml", page)],
                   ["first", "plate", "second", "gone", "first"],
                   {"first.xhtml": FIRST, "second.xhtml": SECOND, "plate.bin": b"not read",
                    "aside.xhtml": xhtml('<p id="aside">' + "Not of the spine. " * 4000 + "</p>")})
        write_book(work / "long-paragraphs.epub", "Long Paragraphs",
                   [("third", "third.xhtml", page), ("fourth", "fourth.xhtml", page)], ["third", "fourth"],
                   {"third.xhtml": THIRD, "fourth.xhtml": FOURTH})
        ran = subprocess.run([sys.executable, "-I", script, "--zip", zip_tool, "--xml", xml_tool, "--epub", epub, "--css",
                              css, "--png", png, "--jpeg", jpeg, "--bundle", bundle, "--document-report", str(work)],
                             capture_output=True, check=False)
        said = ran.stdout.decode("utf-8", "replace")
        expect(ran.returncode == 0, f"check-books.py ended with {ran.returncode}:\n{said}{ran.stderr.decode()}")
        # The first line counts what the tools read, which is not this test's.
        expect(said.split("\n", 1)[1] == EXPECTED,
               f"what check-books.py said of the two books:\n{said}\nand what it should say after its first line:\n"
               f"{EXPECTED}")
    except epub_tool.Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("check-books.py counts the paragraphs and the ids of 4 documents in 2 books as they were written.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
