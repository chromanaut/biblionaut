#!/usr/bin/env python3
"""End-to-end check of what tools/check-books.py makes of a book's chapters, run by CTest.

The script has bn-bundle import each document of a book's spine and sets
how each ended beside a reading of the book by Python, and has bn-pages
walk the first short chapters. This is where that comparison is itself
checked: on a book made here, whose documents end in each of the ways
one can, and then with a tool that says something wrong in the place of
each of the two, which the script has to notice.

The book's spine has a document of three paragraphs; one that is XML as
far as its first paragraph and not after; one that is no XML from its
first byte; a picture; and a file the book does not have.

usage: book_chapters.py CHECK_BOOKS BN_ZIP BN_XML BN_EPUB BN_CSS BN_PNG BN_JPEG BN_BUNDLE BN_PAGES WORK_DIR
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True  # Or Python leaves compiled copies of the tests' scripts beside them.

import epub_tool  # noqa: E402  (found through the path set just above)
from document_count import write_book, xhtml  # noqa: E402
from epub_tool import expect  # noqa: E402

# Three blocks, of 16, 17 and 6 characters.
WHOLE = xhtml("<p>First paragraph.</p>\n<p>Second paragraph.</p>\n<p>Third.</p>\n")
# One block of 5 characters, and then an end tag that ends nothing.
BROKEN = (b'<?xml version="1.0" encoding="UTF-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml"><head><title>t</title></head>\n'
          b"<body><p>Kept.</p>\n</section><p>Never read.</p></body></html>\n")
NOTHING = b"This was never a document of any kind.\n"

# What the script says of the chapters: the three paragraphs and the one
# that was kept are each a line, and each chapter a page.
EXPECTED = [
    "The documents of their spines, imported: 1 whole, 1 cut-short, 2 refused, 1 not-a-document, by the tool and by "
    "Python alike; the chapters have 4 blocks and 44 characters.",
    "2 of the chapters walked, in 2 pages of 4 lines: every page come to three ways, and all agree.",
]


def stand_in(path: Path, tool: str, changes: str) -> str:
    """A tool that runs `tool` and gives out what it prints with `changes` made to it by sed."""
    path.write_text(f'#!/bin/sh\n{shlex.quote(tool)} "$@" | sed {shlex.quote(changes)}\n')
    path.chmod(0o755)
    return str(path)


def main() -> int:
    if len(sys.argv) != 11:
        print(__doc__, file=sys.stderr)
        return 2
    script, zip_tool, xml_tool, epub, css, png, jpeg, bundle, pages = sys.argv[1:10]
    work = Path(sys.argv[10])
    shutil.rmtree(work, ignore_errors=True)
    books = work / "books"
    books.mkdir(parents=True)

    def check(bundle_tool: str, pages_tool: str) -> tuple[int, list[str]]:
        ran = subprocess.run([sys.executable, "-I", script, "--zip", zip_tool, "--xml", xml_tool, "--epub", epub, "--css",
                              css, "--png", png, "--jpeg", jpeg, "--bundle", bundle_tool, "--pages", pages_tool,
                              str(books)], capture_output=True, check=False, env=os.environ)
        return ran.returncode, (ran.stdout + ran.stderr).decode("utf-8", "replace").splitlines()

    try:
        page = "application/xhtml+xml"
        write_book(books / "five-ways.epub", "Five Ways",
                   [("whole", "whole.xhtml", page), ("broken", "broken.xhtml", page), ("nothing", "nothing.xhtml", page),
                    ("plate", "plate.bin", "image/jpeg"), ("gone", "gone.xhtml", page)],
                   ["whole", "broken", "nothing", "plate", "gone"],
                   {"whole.xhtml": WHOLE, "broken.xhtml": BROKEN, "nothing.xhtml": NOTHING, "plate.bin": b"not read"})

        code, said = check(bundle, pages)
        expect(code == 0, "check-books.py ended with " + str(code) + ":\n" + "\n".join(said))
        got = [line for line in said if line.startswith("The documents of their spines, imported") or
               " of the chapters walked, " in line]
        expect(got == EXPECTED, "of the book's chapters check-books.py said:\n" + "\n".join(got) +
               "\nand it should say:\n" + "\n".join(EXPECTED))

        # A tool that says something wrong has to be noticed, each in its place.
        wrong = [
            ("a chapter that is whole said to be cut short",
             stand_in(work / "bundle-1", bundle, "s/^chapter 0 whole reason=none/chapter 0 cut-short reason=not-XML/"),
             pages, "bn-bundle import: chapter 0 (OPS/whole.xhtml) is cut-short"),
            ("a chapter cut short said to be whole",
             stand_in(work / "bundle-2", bundle, "s/^chapter 1 cut-short reason=not-XML/chapter 1 whole reason=none/"),
             pages, "bn-bundle import: chapter 1 (OPS/broken.xhtml) is whole"),
            ("a picture said to be refused",
             stand_in(work / "bundle-3", bundle, "s/^chapter 3 not-a-document reason=none/chapter 3 refused reason=no-file/"),
             pages, "bn-bundle import: chapter 3 (OPS/plate.bin) is refused"),
            ("a file the book has not said to be no document",
             stand_in(work / "bundle-4", bundle, "s/^chapter 4 refused reason=no-file/chapter 4 not-a-document reason=none/"),
             pages, "bn-bundle import: chapter 4 "),
            ("a chapter of another document",
             stand_in(work / "bundle-5", bundle, "s/path=OPS\\/whole.xhtml$/path=OPS\\/broken.xhtml/"),
             pages, "bn-bundle import: chapter 0 is of 'OPS/broken.xhtml'"),
            ("a chapter left out of what importing prints",
             stand_in(work / "bundle-6", bundle, "/^chapter 2 /d"),
             pages, "bn-bundle import: 4 chapters, and the book's spine has 5 items"),
            ("pages that do not agree",
             bundle, stand_in(work / "pages-1", pages, "s/: all agree with the pages$/: 1 of them differ/"),
             "bn-pages walk 0: its last lines are"),
            ("a chapter read with a block more than was imported",
             bundle, stand_in(work / "pages-2", pages, "s/, blocks 3, /, blocks 4, /"),
             "bn-pages walk 0: 1 chapters of 4 blocks and 39 characters; importing said 1 of 3 blocks and 39"),
            ("a walk that printed nothing",
             bundle, stand_in(work / "pages-3", pages, "d"), "bn-pages walk 0: ended with 0"),
        ]
        for what, bundle_tool, pages_tool, wanted in wrong:
            code, said = check(bundle_tool, pages_tool)
            found = [line for line in said if line.startswith("DIFFERENT") and wanted in line]
            expect(code == 1 and found, f"with {what}, check-books.py ended with {code} and said nothing of it "
                   f"({wanted!r}):\n" + "\n".join(said))
    except epub_tool.Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print(f"check-books.py says how the 5 documents of a spine ended as chapters, and notices each of {len(wrong)} "
          "wrong things a tool says of them.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
