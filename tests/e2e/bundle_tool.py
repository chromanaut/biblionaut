#!/usr/bin/env python3
"""End-to-end check of `bn-bundle`, run by CTest.

The tool makes a book's bundle with the code the firmware makes it with
(epub/book_bundle.h, epub/bundle.h). This reads the bundle's bytes a
second way, by the places the format gives them and with none of that
code, and sets what they say beside a reading of the book by Python's
zipfile and ElementTree:

  - the header page: the EPUB's size, when it was written, the checksum of
    its ZIP directory, the checksum of its package (as epub_tool.py works
    it out), how many documents, pictures and entries, and what was amiss;
  - the strings: the title, the author, the language and the identifier;
  - the spine: for each document where its file is in the EPUB, as
    zipfile says, what kind it is, and whether it is in the run of the
    book;
  - the pictures: the same for each of the manifest's pictures, in the
    manifest's order, with the cover marked;
  - the contents: every entry of the table the book is read by;
  - that the file is laid out as a new bundle is: the tables one after
    another, each with the checksum the header gives for it, two slots
    that pass their own, every chapter not imported, and nothing after.

Then `bn-bundle info` has to print what this reading comes to, line for
line.

The same comparison is what tools/check-books.py makes on real books,
with its own reading of their tables of contents.

usage: bundle_tool.py BN_BUNDLE WORK_DIR
"""

from __future__ import annotations

import os
import posixpath
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
import zlib
from pathlib import Path
from urllib.parse import unquote, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True  # Or Python leaves compiled copies of the tests' scripts beside them.

import epub_tool  # noqa: E402  (found through the path set just above)
from epub_tool import Failure, expect, kind, local, tidy  # noqa: E402

PAGE = 4096
# A kind of file, as a bundle has it: the number the package reader gives it.
KIND_NUMBERS = {"other": 0, "xhtml": 1, "css": 2, "ncx": 3, "jpeg": 4, "png": 5, "gif": 6, "svg": 7, "font": 8}
KIND_NAMES = {number: name for name, number in KIND_NUMBERS.items()}
PICTURES = ("jpeg", "png", "gif", "svg")
IN_RUN, PRESENT, COVER = 1, 2, 4
CONTENTS_NOT_WHOLE, PACKAGE_CUT_SHORT = 1, 2
NO_CHAPTER = 0xFFFF
# When the tool is told the EPUB was written, so that a bundle is the same
# whenever it is made.
WRITTEN = 1_795_000_000


def cut(text: str, most: int) -> bytes:
    """Text as the package reader keeps it: no more than `most` bytes, cut at a character."""
    data = text.encode("utf-8")
    if len(data) <= most:
        return data
    at = most
    while at > 0 and (data[at] & 0xC0) == 0x80:
        at -= 1
    return data[:at].rstrip(b" ")


def printable(text: bytes) -> str:
    """As bn-bundle prints a name: control characters and the backslash by their numbers."""
    return b"".join(b"\\x%02X" % byte if byte < 0x20 or byte == 0x5C else bytes([byte])
                    for byte in text).decode("utf-8", "replace")


# ---- A bundle, read by the places the format gives ----

def read_bundle(data: bytes) -> dict:
    """What a new bundle's bytes say. Raises Failure where they are not laid out as a new bundle is."""
    expect(len(data) >= PAGE and data[:4] == b"BNBK", "the file does not begin as a bundle does")
    version, text_rules, importer = struct.unpack_from("<HHH", data, 4)
    expect(version == 1, f"the format's version is {version}")
    expect(struct.unpack_from("<I", data, 4092)[0] == zlib.crc32(data[:4092]), "the header page fails its checksum")
    epub_bytes, epub_written, epub_directory, package = struct.unpack_from("<IIII", data, 12)
    documents, pictures, contents, amiss = struct.unpack_from("<HHHH", data, 28)
    slot_at, slot_bytes, data_begin = struct.unpack_from("<III", data, 36)
    made_by = data[96:128].split(b"\0", 1)[0]
    expect(not any(data[128:4092]), "the header page has something after what it says")

    tables = []
    at = PAGE
    for number in range(4):
        offset, length, checksum = struct.unpack_from("<III", data, 48 + 12 * number)
        expect(offset == at, f"table {number} is at {offset}, and the one before ends at {at}")
        expect(zlib.crc32(data[offset:offset + length]) == checksum, f"table {number} fails its checksum")
        tables.append(data[offset:offset + length])
        at += length
    expect(slot_at == at + (-at % PAGE), f"the slots are at {slot_at}, and the tables end at {at}")
    expect(not any(data[at:slot_at]), "there is something between the tables and the slots")
    used = 24 + 88 * documents
    expect(slot_bytes == used + (-used % PAGE), f"a slot is {slot_bytes} bytes for {documents} documents")
    expect(data_begin == slot_at + 2 * slot_bytes == len(data), "the file does not end where its slots do")
    for slot, sequence in ((0, 2), (1, 1)):
        begin = slot_at + slot * slot_bytes
        expect(data[begin:begin + 4] == b"BNSL", f"slot {slot} does not begin as one does")
        expect(struct.unpack_from("<IIIHH", data, begin + 4) == (sequence, used, data_begin, documents, 88),
               f"slot {slot} begins {struct.unpack_from('<IIIHH', data, begin + 4)}")
        expect(not any(data[begin + 20:begin + used - 4]), f"slot {slot} has a chapter that is imported")
        expect(struct.unpack_from("<I", data, begin + used - 4)[0] == zlib.crc32(data[begin:begin + used - 4]),
               f"slot {slot} fails its checksum")
        expect(not any(data[begin + used:begin + slot_bytes]), f"slot {slot} has something after what it uses")

    strings = []
    at = 0
    while at < len(tables[0]):
        length = struct.unpack_from("<H", tables[0], at)[0]
        strings.append(tables[0][at + 2:at + 2 + length])
        at += 2 + length
    expect(len(strings) == 4 and at == len(tables[0]), f"the strings are {strings!r}")

    def files(table: bytes, count: int, what: str) -> list[tuple]:
        expect(len(table) == 24 * count, f"the table of {what} is {len(table)} bytes for {count}")
        return [struct.unpack_from("<IIIIHHHBB", table, 24 * number) for number in range(count)]

    entries = []
    at = 0
    while at < len(tables[3]):
        depth, fragment, chapter, label = struct.unpack_from("<BBHH", tables[3], at)
        entries.append((depth, None if chapter == NO_CHAPTER else chapter, tables[3][at + 6:at + 6 + fragment],
                        tables[3][at + 6 + fragment:at + 6 + fragment + label]))
        at += 6 + fragment + label
    expect(len(entries) == contents and at == len(tables[3]),
           f"the contents are {len(entries)} entries in {len(tables[3])} bytes, and the header says {contents}")
    return {"text_rules": text_rules, "importer": importer, "made_by": made_by, "epub_bytes": epub_bytes,
            "epub_written": epub_written, "epub_directory": epub_directory, "package": package, "amiss": amiss,
            "strings": strings, "spine": files(tables[1], documents, "the spine"),
            "pictures": files(tables[2], pictures, "the pictures"), "contents": entries, "slot_bytes": slot_bytes,
            "data_begin": data_begin}


# ---- The book, read by Python ----

def directory_checksum(path: Path) -> int:
    """The CRC-32 of the ZIP file's directory, from where its end record says the directory is."""
    data = path.read_bytes()
    end = data.rfind(b"PK\x05\x06")
    length, offset = struct.unpack_from("<II", data, end + 12)
    return zlib.crc32(data[offset:offset + length])


def book_says(path: Path) -> dict:
    """What a bundle of the book at `path` should hold, but for its contents."""
    with zipfile.ZipFile(path) as archive:
        # The first entry of each name.
        names: dict[str, zipfile.ZipInfo] = {}
        for info in archive.infolist():
            names.setdefault(epub_tool.entry_name(info), info)
        container = ET.fromstring(archive.read("META-INF/container.xml"))
        package_path = next(e.get("full-path") for e in container.iter() if local(e.tag) == "rootfile")
        parser = ET.XMLParser()
        parser.entity["nbsp"] = " "
        parser.feed(archive.read(package_path))
        package = parser.close()
    base = posixpath.dirname(package_path)

    def full(href: str) -> str:
        file = unquote(urlsplit(href).path)
        return posixpath.normpath(file.lstrip("/") if file.startswith("/") else posixpath.join(base, file))

    def record(file: str | None, kind_number: int, flags: int) -> tuple:
        if file is None or file not in names:
            return (0, 0, 0, 0, 0, 0, 0, kind_number, flags)
        info = names[file]
        return (info.header_offset, info.compress_size, info.file_size, info.CRC, info.compress_type, info.flag_bits,
                len(file.encode("utf-8", "surrogateescape")), kind_number, flags | PRESENT)

    metadata = next(e for e in package if local(e.tag) == "metadata")

    def first(tag: str) -> str:
        return next((tidy(e) for e in metadata.iter() if local(e.tag).lower() == tag and tidy(e)), "")

    # The identifier the package names as its own, if that has anything in
    # it; failing that, the first that has.
    identifiers = [e for e in metadata.iter() if local(e.tag).lower() == "identifier"]
    unique = package.get("unique-identifier")
    identifier = next((tidy(e) for e in identifiers if unique and e.get("id") == unique and tidy(e)), "") or \
        next((tidy(e) for e in identifiers if tidy(e)), "")

    items = [e for e in package.iter() if local(e.tag) == "item" and e.get("href") and not urlsplit(e.get("href")).scheme]
    by_id: dict[str, ET.Element] = {}
    for item in items:
        if item.get("id"):
            by_id.setdefault(item.get("id"), item)
    cover = next((e for e in items if "cover-image" in (e.get("properties") or "").split()), None)
    if cover is None:
        meta = next((e for e in metadata.iter() if local(e.tag) == "meta" and e.get("name") == "cover"
                     and e.get("content")), None)
        named = by_id.get(meta.get("content")) if meta is not None else None
        if named is not None and kind(named.get("media-type")) in PICTURES:
            cover = named

    spine = []
    for reference in next(e for e in package if local(e.tag) == "spine"):
        if local(reference.tag) == "itemref" and reference.get("idref") in by_id:
            item = by_id[reference.get("idref")]
            spine.append(record(full(item.get("href")), KIND_NUMBERS[kind(item.get("media-type"))],
                                0 if reference.get("linear") == "no" else IN_RUN))
    # A picture has its file if it is the first of the manifest's pictures
    # with that path, and the EPUB has the file.
    pictures = []
    seen = set()
    for item in items:
        if kind(item.get("media-type")) not in PICTURES:
            continue
        file = full(item.get("href"))
        if file in seen or file not in names:
            pictures.append(record(None, 0, 0))
        else:
            pictures.append(record(file, KIND_NUMBERS[kind(item.get("media-type"))], COVER if item is cover else 0))
        seen.add(file)
    lines = epub_tool.reference(path)
    return {"strings": [cut(first("title"), 256), cut(first("creator"), 128), cut(first("language"), 16),
                        cut(identifier, 128)],
            "spine": spine, "pictures": pictures, "epub_bytes": path.stat().st_size,
            "epub_directory": directory_checksum(path),
            "package": int(next(line for line in lines if line.startswith("checksum: ")).split()[1], 16),
            "package_cut_short": False}


def info_lines(bundle: dict) -> list[str]:
    """What `bn-bundle info` should print of a bundle read as read_bundle() reads one."""
    def place(file: tuple) -> str:
        return f"{file[0]} {file[2]} {file[3]:08x}" if file[8] & PRESENT else "?"

    amiss = [what for bit, what in ((CONTENTS_NOT_WHOLE, "contents not whole"), (PACKAGE_CUT_SHORT, "package cut short"))
             if bundle["amiss"] & bit]
    lines = ["format: 1", f"text rules: {bundle['text_rules']}", f"importer: {bundle['importer']}",
             f"made by: {printable(bundle['made_by']) or '-'}",
             f"epub: {bundle['epub_bytes']} bytes, written {bundle['epub_written']}, "
             f"directory {bundle['epub_directory']:08x}",
             f"package: {bundle['package']:08x}", f"amiss: {', '.join(amiss) or '-'}"]
    lines += [f"{name}: {printable(text) or '-'}"
              for name, text in zip(("title", "author", "language", "identifier"), bundle["strings"])]
    lines.append(f"spine: {len(bundle['spine'])}")
    lines += [f"{'+' if file[8] & IN_RUN else '-'} {KIND_NAMES.get(file[7], 'unknown')} {place(file)}"
              for file in bundle["spine"]]
    lines.append(f"pictures: {len(bundle['pictures'])}")
    lines += [f"{KIND_NAMES.get(file[7], 'unknown')} {place(file)}{' cover' if file[8] & COVER else ''}"
              if file[8] & PRESENT else "?" for file in bundle["pictures"]]
    lines.append(f"contents: {len(bundle['contents'])}")
    lines += [f"{depth}\t{'-' if chapter is None else chapter}\t{printable(fragment) or '-'}\t{printable(label)}"
              for depth, chapter, fragment, label in bundle["contents"]]
    lines += [f"slot: sequence 2, {bundle['slot_bytes']} bytes, segments from {bundle['data_begin']} to "
              f"{bundle['data_begin']}",
              f"chapters: {len(bundle['spine'])} not imported, 0 whole, 0 cut short, 0 not documents, 0 refused",
              "tables: ok ok ok ok"]
    return lines


def compare(tool: str, book: Path, made: Path, contents: list[tuple] | None, contents_whole: bool) -> list[str]:
    """Makes the bundle of `book` at `made` with the tool, and says what is not as the book has it.

    `contents` is the entries of the table of contents the book is read
    by, each how far in it is, the document it points into or None, its
    fragment and its label, the last two as bytes; and `contents_whole`
    whether that is the whole of the book's table.
    """
    ran = subprocess.run([tool, "make", str(book), str(made), "--written", str(WRITTEN)], capture_output=True, check=False)
    if ran.returncode != 0:
        return [f"bn-bundle make ended with {ran.returncode}: {ran.stderr[-160:]!r}"]
    try:
        bundle = read_bundle(made.read_bytes())
        expected = book_says(book)
    except Failure as failure:
        return [f"bn-bundle make: {failure}"]
    except Exception as error:  # Whatever a book can do to a parser.
        return [f"bn-bundle: Python could not read the book or its bundle: {error!r}"]
    problems = []
    for what in ("epub_bytes", "epub_directory", "package", "strings"):
        if bundle[what] != expected[what]:
            problems.append(f"bn-bundle make: {what} is {bundle[what]!r}, and the book has {expected[what]!r}")
    if bundle["epub_written"] != WRITTEN or bundle["made_by"] != b"bn-bundle":
        problems.append(f"bn-bundle make: written {bundle['epub_written']}, made by {bundle['made_by']!r}")
    for what in ("spine", "pictures"):
        if len(bundle[what]) != len(expected[what]):
            problems.append(f"bn-bundle make: {len(bundle[what])} records of the {what}, and the book has "
                            f"{len(expected[what])}")
            continue
        different = next((number for number, pair in enumerate(zip(bundle[what], expected[what])) if pair[0] != pair[1]),
                         None)
        if different is not None:
            problems.append(f"bn-bundle make: record {different} of the {what} is {bundle[what][different]}, and the "
                            f"book has {expected[what][different]}")
    if contents is not None:
        if bundle["contents"] != contents:
            different = next((number for number, pair in enumerate(zip(bundle["contents"], contents))
                              if pair[0] != pair[1]), min(len(bundle["contents"]), len(contents)))
            problems.append(f"bn-bundle make: {len(bundle['contents'])} entries of the contents and the book has "
                            f"{len(contents)}; entry {different} is {bundle['contents'][different:different + 1]} and "
                            f"{contents[different:different + 1]}")
        if bool(bundle["amiss"] & CONTENTS_NOT_WHOLE) == contents_whole:
            problems.append(f"bn-bundle make: amiss is {bundle['amiss']}, and the book's contents are "
                            f"{'whole' if contents_whole else 'not whole'}")
    said = subprocess.run([tool, "info", str(made)], capture_output=True, check=False)
    lines = said.stdout.decode("utf-8", "replace").split("\n")[:-1]
    mine = info_lines(bundle)
    if said.returncode != 0 or lines != mine:
        different = next((number for number, pair in enumerate(zip(lines, mine)) if pair[0] != pair[1]),
                         min(len(lines), len(mine)))
        problems.append(f"bn-bundle info ended with {said.returncode}; line {different + 1} is "
                        f"{lines[different:different + 1]!r}, and the bundle's bytes say {mine[different:different + 1]!r}")
    return problems[:4]


# ---- The test ----

def contents_of(book: Path) -> tuple[list[tuple], bool]:
    """A made book's table of contents as compare() takes it, from epub_tool's reading of it."""
    table = epub_tool.toc_reference(book, None)
    if isinstance(table, str):
        return [], table == "no table of contents"
    entries = []
    for line in table[1:-2]:
        depth, document, fragment, label = line.split("\t", 3)
        entries.append((int(depth), None if document == "-" else int(document),
                        b"" if fragment == "-" else fragment.encode("utf-8"), label.encode("utf-8")))
    return entries, True


def picture_book(work: Path) -> Path:
    """A book with pictures of every kind, in an order of its own in the ZIP file: one twice in the manifest, one
    the EPUB does not have, one named with its bytes written out, a cover named the old way, and two identifiers
    of which the package names the second."""
    manifest = ['<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>',
                '<item id="one" href="text/one.xhtml" media-type="application/xhtml+xml"/>',
                '<item id="p1" href="img/a.jpg" media-type="image/jpeg"/>',
                '<item id="p2" href="img/b%20c.png" media-type="image/png"/>',
                '<item id="css" href="main.css" media-type="text/css"/>',
                '<item id="p3" href="img/a.jpg" media-type="image/jpeg"/>',
                '<item id="p4" href="img/gone.gif" media-type="image/gif"/>',
                '<item id="p5" href="img/drawing.svg" media-type="image/svg+xml"/>',
                '<item id="p6" href="img/misnamed.jpg" media-type="image/jpg"/>']
    manifest += [f'<item id="q{number}" href="img/q{number}.png" media-type="image/png"/>' for number in range(400)]
    package = ('<?xml version="1.0"?>\n<package xmlns="http://www.idpf.org/2007/opf" version="2.0" '
               'unique-identifier="uuid">\n<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
               "<dc:title>Pictures</dc:title><dc:language>en</dc:language>\n"
               '<dc:identifier id="isbn">9780000000002</dc:identifier>'
               '<dc:identifier id="uuid">  urn:uuid:0aa1\n  </dc:identifier>\n'
               '<meta name="cover" content="p5"/>\n</metadata>\n<manifest>\n' + "\n".join(manifest)
               + '\n</manifest>\n<spine toc="ncx"><itemref idref="one"/><itemref idref="p5" linear="no"/></spine>\n'
               "</package>\n")
    contents = ('<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/"><navMap>'
                '<navPoint><navLabel><text>One</text></navLabel><content src="text/one.xhtml#a%20place"/>'
                '<navPoint><navLabel><text>A   part\tof it</text></navLabel><content src="text/one.xhtml"/></navPoint>'
                "</navPoint><navPoint><navLabel><text>Elsewhere</text></navLabel>"
                '<content src="https://example.com/"/></navPoint></navMap></ncx>')
    files = {"OPS/toc.ncx": contents.encode(), "OPS/text/one.xhtml": b"<html/>", "OPS/main.css": b"p {}",
             "OPS/img/drawing.svg": b"<svg/>", "OPS/img/misnamed.jpg": b"6", "OPS/img/b c.png": b"2",
             "OPS/img/a.jpg": b"1"}
    for number in range(399, -1, -1):
        if number % 9 != 4:
            files[f"OPS/img/q{number}.png"] = b"picture %d" % number
    book = work / "pictures.epub"
    epub_tool.write_book(book, "OPS/content.opf", package, files, None)
    return book


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tool = sys.argv[1]
    work = Path(sys.argv[2])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        books = epub_tool.books(work) + [picture_book(work)]
        records = 0
        for book in books:
            entries, whole = contents_of(book)
            made = work / (book.stem + ".bundle")
            problems = compare(tool, book, made, entries, whole)
            expect(not problems, f"{book.name}: " + "; ".join(problems))
            bundle = read_bundle(made.read_bytes())
            records += len(bundle["spine"]) + len(bundle["pictures"]) + len(bundle["contents"])

        # The book of pictures is what it was written to be.
        bundle = read_bundle((work / "pictures.bundle").read_bytes())
        expect(bundle["strings"] == [b"Pictures", b"", b"en", b"urn:uuid:0aa1"], f"its strings: {bundle['strings']!r}")
        pictures = bundle["pictures"]
        expect(len(pictures) == 406, f"it has {len(pictures)} pictures")
        expect([file[7:] for file in pictures[:6]] == [(4, PRESENT), (5, PRESENT), (0, 0), (0, 0), (7, PRESENT | COVER),
                                                      (4, PRESENT)], f"its first pictures: {pictures[:6]}")
        expect(sum(1 for file in pictures[6:] if file[8] & PRESENT) == 400 - 44, "its other pictures")
        expect([file[7:] for file in bundle["spine"]] == [(1, IN_RUN | PRESENT), (7, PRESENT)],
               f"its spine: {bundle['spine']}")
        expect(bundle["contents"] == [(0, 0, b"a place", b"One"), (1, 0, b"", b"A part of it"), (0, None, b"", b"Elsewhere")],
               f"its contents: {bundle['contents']}")
        said = subprocess.run([tool, "make", str(work / "pictures.epub"), str(work / "again.bundle"), "--written",
                               str(WRITTEN)], capture_output=True, check=False).stdout.decode()
        expect("pictures=406 pictures_present=360 contents=3 passes=3 amiss=0" in said, f"what making it said: {said!r}")
        expect((work / "again.bundle").read_bytes() == (work / "pictures.bundle").read_bytes(),
               "the same book made a different bundle the second time")

        # A book with a table of contents that is no table: said in the header.
        bundle = read_bundle((work / "old-style.bundle").read_bytes())
        expect(bundle["amiss"] == CONTENTS_NOT_WHOLE and not bundle["contents"], f"old-style.epub: amiss {bundle['amiss']}")

        # Without --written, the time is the file's own.
        os.utime(books[0], (1_700_000_000, 1_700_000_123))
        ran = subprocess.run([tool, "make", str(books[0]), str(work / "timed.bundle")], capture_output=True, check=False)
        expect(ran.returncode == 0, f"make without --written ended with {ran.returncode}")
        expect(read_bundle((work / "timed.bundle").read_bytes())["epub_written"] == 1_700_000_123, "the file's own time")

        # What is no book, and what is no bundle.
        (work / "not-a-book.epub").write_bytes(b"not a ZIP file at all")
        ran = subprocess.run([tool, "make", str(work / "not-a-book.epub"), str(work / "none.bundle")],
                             capture_output=True, check=False)
        expect(ran.returncode == 1 and b"error: " in ran.stderr, f"a file that is no book: {ran.returncode} {ran.stderr!r}")
        ran = subprocess.run([tool, "info", str(books[0])], capture_output=True, check=False)
        expect(ran.returncode == 1 and b"not a bundle" in ran.stderr, f"info of an EPUB: {ran.returncode} {ran.stderr!r}")
        spoiled = bytearray((work / "pictures.bundle").read_bytes())
        spoiled[4096 + 40] ^= 0x01  # In the spine's table.
        (work / "spoiled.bundle").write_bytes(spoiled)
        ran = subprocess.run([tool, "info", str(work / "spoiled.bundle")], capture_output=True, check=False)
        expect(ran.returncode == 1 and ran.stdout.endswith(b"tables: ok damaged ok ok\n"),
               f"info of a bundle with a table spoiled: {ran.returncode} {ran.stdout[-60:]!r}")
        ran = subprocess.run([tool, "make", str(books[0])], capture_output=True, check=False)
        expect(ran.returncode == 2 and b"usage: " in ran.stderr, f"too few arguments: {ran.returncode}")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print(f"bn-bundle and Python agree about {len(books)} books: {records} records of their spines, their pictures "
          "and their contents.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
