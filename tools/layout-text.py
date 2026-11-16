#!/usr/bin/env python3
"""Writes the paragraphs of EPUB books to a file, for the layout check.

The layout check (reader/layout_check.h) sets the text at
/layout/paragraphs.txt on a device's card in lines, and `bn-lines` sets the
same file on a computer, so that the two can be compared. This makes that
file out of books: every document of each book's spine, in order, taken
apart into paragraphs as a reader would see them.

It is not the importer, and nothing here is what the firmware will do with
a book. It is a plain reading, enough to give the layout code a book's text
in a book's proportions:

  - a paragraph is the text between the starts and ends of block elements
    (p, div, h1 to h6, li, blockquote, td and the like);
  - white space in a row is one space, as a browser takes it;
  - a br is a line end inside the paragraph;
  - what is in i, em, b, strong or cite is in the second style, between two
    asterisks, and an asterisk of the text itself is written as U+2217;
  - head, script and style are left out, and so are pictures.

The file is as `bn-lines` reads one: UTF-8, an empty line between two
paragraphs, a line end inside a paragraph where it ends a line, and
asterisks round the second style. Two line ends in a row inside a
paragraph are written with a space between them, so that they are not
taken for the end of it.

usage: layout-text.py [--out FILE] BOOK_OR_FOLDER...

A folder is looked through for files ending in .epub, at any depth, in
order of their paths. Without --out the text goes to standard output. What
was found is said on standard error.
"""

from __future__ import annotations

import argparse
import posixpath
import re
import sys
import zipfile
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote
from xml.etree import ElementTree

BLOCKS = {
    "address", "article", "aside", "blockquote", "body", "caption", "dd", "div", "dl", "dt", "figcaption", "figure",
    "footer", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "li", "nav", "ol", "p", "pre", "section", "table",
    "tbody", "td", "tfoot", "th", "thead", "tr", "ul",
}
LEFT_OUT = {"head", "script", "style", "svg", "math"}
SECOND_STYLE = {"i", "em", "b", "strong", "cite"}
SPACE = re.compile(r"[ \t\r\n\f]+")


class Paragraphs(HTMLParser):
    """Takes one document apart into paragraphs."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.paragraphs: list[str] = []
        self.pieces: list[str] = []
        self.left_out = 0     # How deep inside elements whose text is left out.
        self.styled = 0       # How deep inside elements of the second style.
        self.written_styled = False  # Whether the paragraph's text so far ends in the second style.

    def _end_paragraph(self) -> None:
        text = "".join(self.pieces)
        self.pieces = []
        self.written_styled = False
        # Line ends at either end are dropped; two in a row get a space
        # between them; and a paragraph of spaces and asterisks is none.
        text = text.strip("\n")
        while "\n\n" in text:
            text = text.replace("\n\n", "\n \n")
        if text.replace("*", "").strip(" \n"):
            self.paragraphs.append(text)

    def _add(self, text: str) -> None:
        want_styled = self.styled > 0
        if want_styled != self.written_styled:
            self.pieces.append("*")
            self.written_styled = want_styled
        self.pieces.append(text)

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        tag = tag.lower().split(":")[-1]
        if tag in LEFT_OUT:
            self.left_out += 1
        elif tag in BLOCKS:
            self._end_paragraph()
        elif tag in SECOND_STYLE:
            self.styled += 1
        elif tag == "br" and not self.left_out:
            self.pieces.append("\n")

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        tag = tag.lower().split(":")[-1]
        if tag == "br" and not self.left_out:
            self.pieces.append("\n")
        elif tag in BLOCKS:
            self._end_paragraph()

    def handle_endtag(self, tag: str) -> None:
        tag = tag.lower().split(":")[-1]
        if tag in LEFT_OUT:
            self.left_out = max(0, self.left_out - 1)
        elif tag in BLOCKS:
            self._end_paragraph()
        elif tag in SECOND_STYLE:
            self.styled = max(0, self.styled - 1)

    def handle_data(self, data: str) -> None:
        if self.left_out:
            return
        self._add(SPACE.sub(" ", data).replace("*", "∗"))

    def close(self) -> None:
        super().close()
        self._end_paragraph()


def local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def documents_of(book: zipfile.ZipFile) -> list[str]:
    """The names of the book's documents, in the order of its spine; every
    XHTML file in the ZIP's order if its package cannot be found or read."""
    names = book.namelist()
    try:
        container = ElementTree.fromstring(book.read("META-INF/container.xml"))
        package_path = next(element.get("full-path") for element in container.iter()
                            if local(element.tag) == "rootfile" and element.get("full-path"))
        package = ElementTree.fromstring(book.read(package_path))
        base = posixpath.dirname(package_path)
        manifest = {}
        for element in package.iter():
            if local(element.tag) == "item" and element.get("id") and element.get("href"):
                manifest[element.get("id")] = posixpath.normpath(posixpath.join(base, unquote(element.get("href"))))
        spine = [manifest[element.get("idref")] for element in package.iter()
                 if local(element.tag) == "itemref" and element.get("idref") in manifest]
        found = [name for name in spine if name in names]
        if found:
            return found
    except (KeyError, StopIteration, ElementTree.ParseError, zipfile.BadZipFile):
        pass
    return [name for name in names if name.lower().endswith((".xhtml", ".html", ".htm"))]


def paragraphs_of(path: Path) -> tuple[list[str], int]:
    """A book's paragraphs, and how many documents they came from."""
    paragraphs: list[str] = []
    with zipfile.ZipFile(path) as book:
        documents = documents_of(book)
        for name in documents:
            reader = Paragraphs()
            reader.feed(book.read(name).decode("utf-8", "replace"))
            reader.close()
            paragraphs.extend(reader.paragraphs)
    return paragraphs, len(documents)


def books_in(paths: list[str]) -> list[Path]:
    books: list[Path] = []
    for given in paths:
        path = Path(given)
        if path.is_dir():
            books.extend(sorted(found for found in path.rglob("*") if found.suffix.lower() == ".epub"
                                and found.is_file() and not found.name.startswith(".")))
        else:
            books.append(path)
    return books


def main() -> int:
    parser = argparse.ArgumentParser(description="Writes the paragraphs of EPUB books to a file, for the layout check.")
    parser.add_argument("paths", nargs="+", metavar="BOOK_OR_FOLDER")
    parser.add_argument("--out", help="where to write the text (default: standard output)")
    arguments = parser.parse_args()

    out = open(arguments.out, "w", encoding="utf-8", newline="\n") if arguments.out else sys.stdout
    books = documents = paragraphs = 0
    written = 0
    try:
        for path in books_in(arguments.paths):
            try:
                found, count = paragraphs_of(path)
            except (OSError, zipfile.BadZipFile) as error:
                print(f"layout-text: {path}: {error}", file=sys.stderr)
                continue
            books += 1
            documents += count
            for paragraph in found:
                text = ("\n\n" if written else "") + paragraph
                out.write(text)
                written += len(text.encode("utf-8"))
                paragraphs += 1
        if written:
            out.write("\n")
            written += 1
    finally:
        if out is not sys.stdout:
            out.close()
    print(f"layout-text: {books} books, {documents} documents, {paragraphs} paragraphs, {written} bytes",
          file=sys.stderr)
    return 0 if books else 1


if __name__ == "__main__":
    sys.exit(main())
