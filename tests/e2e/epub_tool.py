#!/usr/bin/env python3
"""End-to-end check of bn-epub, and through it of the package reader, run by CTest.

The unit tests say what the reader should make of packages written for
them. Here books are made the way books are, by Python's zipfile module,
and what bn-epub says of each is compared, line for line, with what a
reading of the same book by Python's own XML parser says: the title, the
author, the language, the cover, the tables of contents, and every
document of the spine in its order.

usage: epub_tool.py BN_EPUB WORK_DIR
"""

from __future__ import annotations

import posixpath
import random
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
import zlib
from pathlib import Path
from urllib.parse import unquote, urlsplit

KINDS = {"application/xhtml+xml": "xhtml", "text/html": "xhtml", "text/css": "css", "application/x-dtbncx+xml": "ncx",
         "image/jpeg": "jpeg", "image/jpg": "jpeg", "image/png": "png", "image/gif": "gif", "image/svg+xml": "svg"}


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def kind(media_type: str | None) -> str:
    name = (media_type or "").lower()
    if name in KINDS:
        return KINDS[name]
    return "font" if any(word in name for word in ("font", "opentype", "truetype")) else "other"


def tidy(element: ET.Element | None) -> str:
    return " ".join("".join(element.itertext()).split()) if element is not None else ""


def reference(path: Path) -> list[str]:
    """What bn-epub info should print for the book at `path`, but for its last two lines.

    The line before those is the checksum, which is worked out here from
    lines of its own: the same things, with each file given not by its name
    but by where zipfile says its entry is, how long it is and what its
    contents' checksum is (epub/package_digest.h).
    """
    with zipfile.ZipFile(path) as archive:
        names = {info.orig_filename for info in archive.infolist()}
        # The first entry of each name, as three numbers.
        entries: dict[str, str] = {}
        for info in archive.infolist():
            entries.setdefault(info.orig_filename, f"{info.header_offset} {info.file_size} {info.CRC:08x}")
        container = ET.fromstring(archive.read("META-INF/container.xml"))
        package_path = next(e.get("full-path") for e in container.iter() if local(e.tag) == "rootfile")
        # The one entity of HTML's that turns up in titles is told to the
        # parser, which would stop at it otherwise.
        parser = ET.XMLParser()
        parser.entity["nbsp"] = " "
        parser.feed(archive.read(package_path))
        package = parser.close()
    base = posixpath.dirname(package_path)

    def full(href: str) -> str:
        path = unquote(urlsplit(href).path)
        if path.startswith("/"):
            return posixpath.normpath(path.lstrip("/"))
        return posixpath.normpath(posixpath.join(base, path))

    def where(item: ET.Element | None) -> str | None:
        return full(item.get("href")) if item is not None and full(item.get("href")) in names else None

    metadata = next(e for e in package if local(e.tag) == "metadata")

    def first(tag: str) -> ET.Element | None:
        return next((e for e in metadata.iter() if local(e.tag).lower() == tag and tidy(e)), None)

    items = [e for e in package.iter() if local(e.tag) == "item" and e.get("href") and not urlsplit(e.get("href")).scheme]
    by_id: dict[str, ET.Element] = {}
    for item in items:
        if item.get("id"):
            by_id.setdefault(item.get("id"), item)
    lines = [f"title: {tidy(first('title')) or '-'}", f"author: {tidy(first('creator')) or '-'}",
             f"language: {tidy(first('language')) or '-'}", f"package: {package_path}", f"manifest: {len(items)}"]
    summed = list(lines)

    def entry(item: ET.Element | None) -> str | None:
        return entries[where(item)] if where(item) else None

    cover = next((e for e in items if "cover-image" in (e.get("properties") or "").split()), None)
    if cover is None:
        meta = next((e for e in metadata.iter() if local(e.tag) == "meta" and e.get("name") == "cover"
                     and e.get("content")), None)
        named = by_id.get(meta.get("content")) if meta is not None else None
        if named is not None and kind(named.get("media-type")) in ("jpeg", "png", "gif", "svg"):
            cover = named
    lines.append(f"cover: {kind(cover.get('media-type'))} {where(cover)}" if where(cover) else "cover: -")
    summed.append(f"cover: {kind(cover.get('media-type'))} {entry(cover)}" if where(cover) else "cover: -")
    nav = next((e for e in items if "nav" in (e.get("properties") or "").split()), None)
    lines.append(f"nav: {where(nav) or '-'}")
    summed.append(f"nav: {entry(nav) or '-'}")
    spine = next(e for e in package if local(e.tag) == "spine")
    ncx = by_id.get(spine.get("toc")) if spine.get("toc") else None
    if ncx is None:
        ncx = next((e for e in items if kind(e.get("media-type")) == "ncx"), None)
    lines.append(f"ncx: {where(ncx) or '-'}")
    summed.append(f"ncx: {entry(ncx) or '-'}")
    documents = [e for e in spine if local(e.tag) == "itemref" and e.get("idref") in by_id]
    lines.append(f"spine: {len(documents)}")
    summed.append(lines[-1])
    for document in documents:
        item = by_id[document.get("idref")]
        start = f"{'-' if document.get('linear') == 'no' else '+'} {kind(item.get('media-type'))} "
        lines.append(start + (where(item) or "?"))
        summed.append(start + (entry(item) or "?"))
    unknown = sum(1 for e in spine if local(e.tag) == "itemref" and e.get("idref") and e.get("idref") not in by_id)
    if unknown:
        lines.append(f"unknown: {unknown}")
    # None of the books made here has a package that is cut short.
    summed += [f"unknown: {unknown}", "cut short: 0"]
    lines.append(f"checksum: {zlib.crc32(''.join(line + chr(10) for line in summed).encode()):08x}")
    return lines


def container(package_path: str) -> str:
    return ('<?xml version="1.0"?>\n<container version="1.0" '
            'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>'
            f'<rootfile full-path="{package_path}" media-type="application/oebps-package+xml"/>'
            "</rootfiles></container>\n")


def write_book(path: Path, package_path: str, package: str, files: dict[str, bytes], order: random.Random | None) -> None:
    """A ZIP file of the book, its files in the order given, or shuffled by `order`."""
    entries = [("META-INF/container.xml", container(package_path).encode()), (package_path, package.encode())]
    entries += list(files.items())
    if order is not None:
        order.shuffle(entries)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
        for name, data in entries:
            archive.writestr(name, data, zipfile.ZIP_DEFLATED if len(name) % 3 else zipfile.ZIP_STORED)


def novel(chapters: int, version: str) -> tuple[str, dict[str, bytes]]:
    """A book of `chapters` chapters as EPUB 2 or EPUB 3 lays one out, its package at OEBPS/content.opf."""
    files: dict[str, bytes] = {"OEBPS/styles/main.css": b"p { margin: 0 }", "OEBPS/images/cover.jpg": b"jpeg",
                               "OEBPS/toc.ncx": b"<ncx/>"}
    manifest = ['<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>',
                '<item id="css" href="styles/main.css" media-type="text/css"/>']
    spine = []
    cover_properties = ' properties="cover-image"' if version == "3.0" else ""
    manifest.append(f'<item id="cover-image" href="images/cover.jpg" media-type="image/jpeg"{cover_properties}/>')
    if version == "3.0":
        files["OEBPS/nav.xhtml"] = b"<html/>"
        manifest.append('<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>')
    for number in range(chapters):
        name = f"text/chapter {number:03d}.xhtml" if number % 7 == 3 else f"text/chapter{number:03d}.xhtml"
        files["OEBPS/" + name] = f"<html>{number}</html>".encode()
        manifest.append(f'<item id="ch{number}" href="{name.replace(" ", "%20")}" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="ch{number}"{" linear=" + chr(34) + "no" + chr(34) if number % 11 == 5 else ""}/>')
    meta = '<meta name="cover" content="cover-image"/>' if version == "2.0" else ""
    package = (f'<?xml version="1.0" encoding="UTF-8"?>\n<package xmlns="http://www.idpf.org/2007/opf" '
               f'version="{version}" unique-identifier="uid">\n'
               '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
               f"<dc:title>A Novel of {chapters} Chapters\n   &amp; an Epilogue</dc:title>\n"
               "<dc:creator>Renée   Author</dc:creator><dc:creator>A Second</dc:creator>\n"
               f"<dc:language>en-GB</dc:language><dc:identifier id=\"uid\">urn:x</dc:identifier>{meta}\n</metadata>\n"
               "<manifest>\n" + "\n".join(manifest) + '\n</manifest>\n<spine toc="ncx">\n' + "\n".join(spine)
               + '\n</spine>\n<guide><reference type="cover" href="text/chapter000.xhtml"/></guide>\n</package>\n')
    return package, files


def books(work: Path) -> list[Path]:
    """Books of several kinds, written to `work`."""
    made = []
    shuffle = random.Random(7)

    def add(name: str, package_path: str, package: str, files: dict[str, bytes], shuffled: bool = True) -> None:
        write_book(work / name, package_path, package, files, shuffle if shuffled else None)
        made.append(work / name)

    for version in ("2.0", "3.0"):
        package, files = novel(40, version)
        add(f"novel-{version}.epub", "OEBPS/content.opf", package, files)
    package, files = novel(600, "3.0")
    add("many-chapters.epub", "OEBPS/content.opf", package, files)
    package, files = novel(12, "2.0")
    add("in-order.epub", "OEBPS/content.opf", package, files, shuffled=False)

    # The package at the top of the book, with the opf: prefix, a mark at
    # its front that some editors put there, and a doctype.
    package = ('﻿<?xml version="1.0"?>\n<!DOCTYPE package PUBLIC "+//ISBN 0-9673008-1-9//DTD OEB 1.2 Package//EN" '
               '"http://openebook.org/dtds/oeb-1.2/oebpkg12.dtd">\n'
               '<opf:package xmlns:opf="http://www.idpf.org/2007/opf" xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
               "<opf:metadata><dc-metadata><dc:Title>Old &nbsp; Style</dc:Title><dc:Creator>Old Hand</dc:Creator>"
               "<dc:Language>de</dc:Language></dc-metadata></opf:metadata>\n<opf:manifest>\n"
               '<opf:item id="a" href="a.html" media-type="text/html"/>\n'
               '<opf:item id="b" href="sub/../b.html#top" media-type="text/html"/>\n'
               '<opf:item id="pic" href="/pictures/c.gif" media-type="image/gif"/>\n'
               '<opf:item id="font" href="f.otf" media-type="application/vnd.ms-opentype"/>\n'
               '<opf:item id="t" href="contents.ncx" media-type="application/x-dtbncx+xml"/>\n'
               "</opf:manifest>\n<opf:spine>\n"
               '<opf:itemref idref="b"/><opf:itemref idref="a"/><opf:itemref idref="font"/>'
               "</opf:spine>\n</opf:package>\n")
    add("old-style.epub", "book.opf", package,
        {"a.html": b"a", "b.html": b"b", "pictures/c.gif": b"gif", "f.otf": b"font", "contents.ncx": b"ncx"})

    # Things that are not as they should be: a file the package lists and
    # the book does not have, documents the spine names and the manifest
    # does not, a file somewhere else, two files with one id, a cover that
    # is a page and not a picture, a table of contents the spine misnames.
    package = ('<package xmlns="http://www.idpf.org/2007/opf"><metadata><title>  Untidy  </title>'
               '<meta name="cover" content="page"/></metadata>\n<manifest>\n'
               '<item id="page" href="cover.xhtml" media-type="application/xhtml+xml"/>\n'
               '<item id="lost" href="lost.xhtml" media-type="application/xhtml+xml"/>\n'
               '<item id="web" href="https://example.com/x.xhtml" media-type="application/xhtml+xml"/>\n'
               '<item id="twice" href="one.xhtml" media-type="application/xhtml+xml"/>\n'
               '<item id="twice" href="two.xhtml" media-type="application/xhtml+xml"/>\n'
               '<item id="toc" href="toc.ncx" media-type="application/x-dtbncx+xml"/>\n'
               "</manifest>\n<spine toc=\"no-such-id\">\n"
               '<itemref idref="page"/><itemref idref="lost"/><itemref idref="web"/><itemref idref="twice"/>'
               '<itemref idref="nothing"/><itemref idref="page" linear="no"/>\n</spine></package>\n')
    add("untidy.epub", "deep/er/package.opf", package,
        {"deep/er/cover.xhtml": b"c", "deep/er/one.xhtml": b"1", "deep/er/two.xhtml": b"2", "deep/er/toc.ncx": b"n"})
    return made


def info(tool: str, path: Path, *options: str) -> subprocess.CompletedProcess:
    return subprocess.run([tool, "info", str(path), *options], capture_output=True, check=False)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tool = str(Path(sys.argv[1]).resolve())
    work = Path(sys.argv[2]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    try:
        made = books(work)
        documents = 0
        for path in made:
            expected = reference(path)
            result = info(tool, path)
            expect(result.returncode == 0, f"{path.name}: bn-epub ended with {result.returncode}: {result.stderr!r}")
            lines = result.stdout.decode("utf-8").splitlines()
            expect(len(lines) >= 2 and lines[-2].startswith("memory: ") and lines[-1].startswith("cost: "),
                   f"{path.name}: the last two lines are {lines[-2:]!r}")
            got = lines[:-2]
            for number, (mine, theirs) in enumerate(zip(got, expected)):
                expect(mine == theirs, f"{path.name}: line {number + 1} is {mine!r}, a reading by Python has {theirs!r}")
            expect(len(got) == len(expected), f"{path.name}: {len(got)} lines, a reading by Python has {len(expected)}")
            documents += int(next(line for line in got if line.startswith("spine: ")).split()[1])

        # What it costs: a book of 600 chapters, its files in no order, is
        # found with two lookups by name and one reading of the directory,
        # in 16 bytes a file and 28 a document.
        lines = info(tool, work / "many-chapters.epub").stdout.decode("utf-8").splitlines()
        expect("manifest: 604" in lines and "spine: 600" in lines, f"many-chapters.epub: {lines[:12]!r}")
        expect(lines[-2] == f"memory: {604 * 16 + 600 * 28}", f"many-chapters.epub: {lines[-2]!r}")
        cost = dict(item.split("=") for item in lines[-1].split()[1:] if "=" in item)
        expect(cost["zip_finds"] == "2" and int(cost["zip_records"]) <= 3 * 607,
               f"many-chapters.epub: the cost was {lines[-1]!r}")

        # With less memory than that the book is refused, and with exactly that it is not.
        result = info(tool, work / "many-chapters.epub", "--memory", str(604 * 16 + 600 * 28))
        expect(result.returncode == 0, f"many-chapters.epub in exactly enough memory: {result.stderr!r}")
        result = info(tool, work / "many-chapters.epub", "--memory", str(604 * 16 + 600 * 28 - 4))
        expect(result.returncode == 1 and b"no memory" in result.stderr and result.stdout == b"",
               f"many-chapters.epub in too little memory: {result.stderr!r} {result.stdout[:80]!r}")

        # What is not a book is refused, and why is said.
        with zipfile.ZipFile(work / "no-container.epub", "w") as archive:
            archive.writestr("content.opf", "<package/>")
        with zipfile.ZipFile(work / "no-package.epub", "w") as archive:
            archive.writestr("META-INF/container.xml", container("missing.opf"))
        with zipfile.ZipFile(work / "empty-spine.epub", "w") as archive:
            archive.writestr("META-INF/container.xml", container("content.opf"))
            archive.writestr("content.opf", "<package><metadata/><manifest/><spine/></package>")
        (work / "not-a-zip.epub").write_bytes(b"this is not a ZIP file at all" * 10)
        for name, why in (("no-container.epub", b"no container"), ("no-package.epub", b"no package"),
                          ("empty-spine.epub", b"not a package"), ("not-a-zip.epub", b"not a zip file"),
                          ("no-such-file.epub", b"cannot read")):
            result = info(tool, work / name)
            expect(result.returncode == 1 and why in result.stderr and result.stdout == b"",
                   f"{name}: bn-epub ended with {result.returncode}: {result.stderr!r}")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"{len(made)} books, {documents} documents: bn-epub agrees with a reading by Python about all of them; "
          f"5 that are not books refused")
    return 0


if __name__ == "__main__":
    sys.exit(main())
