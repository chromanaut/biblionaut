#!/usr/bin/env python3
"""End-to-end check of the book check, run by CTest.

The book check (reader/book_check.h) is what a device runs to find out
whether it can read books off its card. Here the desktop application runs
it over a folder standing for the card, and what it reports is compared
with things that share none of its code path: `bn-zip check`, `bn-xml
book` and `bn-epub info` on each of the same files, and Python's zipfile
module. A book should come to the same entries, the same bytes and the
same number of reads of storage in the application as it does in bn-zip,
its XML to the same events as in bn-xml, and its package to the same
checksum in the same number of reads as in bn-epub, which is how a run on
a device is compared with the desktop.

usage: book_check.py APP BN_TRACE BN_ZIP BN_XML BN_EPUB WORK_DIR
"""

from __future__ import annotations

import os
import random
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

WORDS = ["the ", "reader ", "turned ", "a ", "page, ", "and ", "then ", "another; ", "night ", "fell ", "quietly.\n"]


def text(size: int, seed: int) -> bytes:
    """Bytes that compress well."""
    rng = random.Random(seed)
    out = bytearray()
    while len(out) < size:
        out += rng.choice(WORDS).encode()
    return bytes(out[:size])


def noise(size: int, seed: int) -> bytes:
    """Bytes that do not compress at all."""
    return random.Random(seed).getrandbits(8 * size).to_bytes(size, "little")


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def xhtml(size: int, seed: int) -> bytes:
    """A chapter: XML, with `size` bytes of text in it."""
    return (b'<?xml version="1.0" encoding="utf-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml"><body>\n<p class="a">'
            + text(size, seed).replace(b"\n", b"</p>\n<p>") + b" &amp; so on.</p>\n</body></html>\n")


def make_book(path: Path, seed: int, chapters: int) -> None:
    """An EPUB: its package lists what is in it, and its XML is XML, but for one file."""
    manifest = "".join(f'<item id="c{chapter}" href="chapter{chapter:02d}.xhtml" media-type="application/xhtml+xml"/>'
                       for chapter in range(chapters))
    manifest += ('<item id="kept" href="kept-as-it-is.xhtml" media-type="application/xhtml+xml"/>'
                 '<item id="odd" href="not-xml.xhtml" media-type="application/xhtml+xml"/>'
                 '<item id="cover" href="images/cover.jpg" media-type="image/jpeg" properties="cover-image"/>'
                 '<item id="css" href="empty.css" media-type="text/css"/>')
    spine = "".join(f'<itemref idref="c{chapter}"/>' for chapter in range(chapters))
    package = ('<?xml version="1.0" encoding="utf-8"?>\n<package xmlns="http://www.idpf.org/2007/opf" version="3.0">'
               '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
               f"<dc:title>Book {seed} &amp; Its {chapters} Chapters</dc:title><dc:creator>Writer {seed}</dc:creator>"
               f"<dc:language>en</dc:language></metadata>\n<manifest>{manifest}</manifest>\n"
               f'<spine>{spine}<itemref idref="kept" linear="no"/></spine></package>\n')
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
        archive.writestr("META-INF/container.xml", '<container><rootfiles><rootfile full-path="OEBPS/content.opf" '
                         'media-type="application/oebps-package+xml"/></rootfiles></container>', zipfile.ZIP_DEFLATED)
        archive.writestr("OEBPS/content.opf", package, zipfile.ZIP_DEFLATED)
        for chapter in range(chapters):
            archive.writestr(f"OEBPS/chapter{chapter:02d}.xhtml", xhtml(6_000 + 900 * chapter, seed + chapter),
                             zipfile.ZIP_DEFLATED, compresslevel=1 + chapter % 9)
        archive.writestr("OEBPS/kept-as-it-is.xhtml", xhtml(9_000, seed + 50))
        archive.writestr("OEBPS/not-xml.xhtml", b"<html><body><p>" + text(7_000, seed + 60), zipfile.ZIP_DEFLATED)
        archive.writestr("OEBPS/images/cover.jpg", noise(30_000, seed))
        archive.writestr("OEBPS/empty.css", b"", zipfile.ZIP_DEFLATED)


def run_application(app: str, card: Path, capture: Path, milliseconds: int) -> None:
    capture.unlink(missing_ok=True)
    environment = dict(os.environ, BN_SERIAL_OUT=str(capture), BN_STORAGE=str(card), BN_BOOK_CHECK="1",
                       BN_DISPLAY_SPEED="20")
    environment.pop("BN_STORAGE_CHECK", None)
    result = subprocess.run([app, "--run-ms", str(milliseconds)], env=environment, capture_output=True, check=False)
    expect(result.returncode == 0, f"the desktop application ended with {result.returncode}: {result.stderr!r}")


def log_lines(trace_tool: str, capture: Path) -> list[str]:
    """The lines of log text in a capture, as bn-trace prints them, without the time."""
    result = subprocess.run([trace_tool, str(capture)], capture_output=True, check=False)
    expect(result.returncode == 0, f"bn-trace ended with {result.returncode}: {result.stderr!r}")
    lines = []
    for line in result.stdout.decode("utf-8", "replace").splitlines():
        match = re.match(r"\[\s*[\d.]+\] log  (.*)$", line)
        if match:
            lines.append(match.group(1))
    return lines


def zip_check(zip_tool: str, path: Path) -> dict[str, int]:
    """What `bn-zip check` says of a file."""
    result = subprocess.run([zip_tool, "check", str(path)], capture_output=True, check=False)
    out = result.stdout.decode()
    totals = re.search(r"(\d+) entries, (\d+) failed; (\d+) bytes read from (\d+) in the file", out)
    reads = re.search(r"sd_reads=(\d+) sd_read_bytes=(\d+)", out)
    expect(totals is not None and reads is not None, f"bn-zip check {path.name} said {out!r}")
    return {"entries": int(totals[1]), "failed": int(totals[2]), "out": int(totals[3]), "in": int(totals[4]),
            "reads": int(reads[1]), "read_bytes": int(reads[2])}


def xml_check(xml_tool: str, path: Path) -> dict[str, int]:
    """What `bn-xml book` says all the XML of a file comes to."""
    result = subprocess.run([xml_tool, "book", str(path)], capture_output=True, check=False)
    out = result.stdout.decode("utf-8", "replace")
    totals = re.search(r"(\d+) documents, (\d+) failed; (\d+) events, checksum of all ([0-9a-f]{8}); ", out)
    expect(totals is not None, f"bn-xml book {path.name} said {out[-300:]!r}")
    return {"documents": int(totals[1]), "failed": int(totals[2]), "events": int(totals[3]),
            "checksum": int(totals[4], 16)}


def epub_info(epub_tool: str, path: Path) -> dict:
    """What `bn-epub info` says of a book's package, and what reading it cost."""
    result = subprocess.run([epub_tool, "info", str(path)], capture_output=True, check=False)
    out = result.stdout.decode("utf-8", "replace")
    expect(result.returncode == 0, f"bn-epub info {path.name} ended with {result.returncode}: {result.stderr!r}")
    said = dict(line.split(": ", 1) for line in out.splitlines() if ": " in line)
    reads = re.search(r"sd_reads=(\d+) sd_read_bytes=(\d+)", said.get("cost", ""))
    expect(reads is not None and "checksum" in said, f"bn-epub info {path.name} said {out[-400:]!r}")
    return {"title": said["title"], "author": said["author"], "language": said["language"],
            "files": int(said["manifest"]), "documents": int(said["spine"]), "cover": said["cover"].split()[0],
            "nav": said["nav"] != "-", "ncx": said["ncx"] != "-", "checksum": int(said["checksum"], 16),
            "memory": int(said["memory"]), "reads": int(reads[1]), "read_bytes": int(reads[2])}


def reported(lines: list[str]) -> dict[str, dict]:
    """What the book check said of each book, by the book's path."""
    books: dict[int, dict] = {}
    for line in lines:
        match = re.match(r"book (\d+): (.*)$", line)
        if not match:
            continue
        number, rest = int(match[1]), match[2]
        book = books.setdefault(number, {})
        if begun := re.match(r"(\d+) bytes, (/.*)$", rest):
            book.update(size=int(begun[1]), path=begun[2])
        elif ended := re.match(r"(ok|FAILED)\. (?:(\d+) of )?(\d+) entr(?:y|ies), (\d+) bytes out of (\d+)\. "
                               r"Directory found in (\d+) reads?, \d+ ms\.(?: First to fail: entry (\d+), (.*))?$",
                               rest):
            book.update(ok=ended[1] == "ok", failed=int(ended[2] or 0), entries=int(ended[3]), out=int(ended[4]),
                        **{"in": int(ended[5])}, open_reads=int(ended[6]),
                        first_failed=int(ended[7] or 0), failure=ended[8] or "")
        elif times := re.match(r"card \d+ ms for (\d+) reads? of (\d+) bytes \(\d+ KB/s\); inflating \d+ ms: "
                               r"(\d+) bytes in \(\d+ KB/s\), (\d+) out \(\d+ KB/s\); all \d+ ms\.$", rest):
            book.update(reads=int(times[1]), read_bytes=int(times[2]), inflated_in=int(times[3]),
                        inflated_out=int(times[4]))
        elif xml := re.match(r"XML: (\d+) documents?, (\d+) bytes, (\d+) events?, checksum ([0-9a-f]{8}); "
                             r"parsing \d+ ms \(\d+ KB/s\); most memory (\d+)\."
                             r"(?: Not XML: (\d+)\.)?(?: Refused for memory: (\d+)\.)?$", rest):
            book.update(xml_documents=int(xml[1]), xml_bytes=int(xml[2]), xml_events=int(xml[3]),
                        xml_checksum=int(xml[4], 16), xml_memory=int(xml[5]), xml_not=int(xml[6] or 0),
                        xml_refused=int(xml[7] or 0))
        elif unread := re.match(r"FAILED\. It cannot be read: (.*)$", rest):
            book.update(ok=False, unreadable=unread[1])
        elif names := re.match(r"package: (title \".*\"|no title), (author \".*\"|no author), "
                               r"(language \".*\"|no language)\.$", rest):
            book.update(package_names=names[0])
        elif package := re.match(r"package: (\d+) files?, (\d+) documents?, (?:cover (\w+)|no cover), (no nav|nav), "
                                 r"(no ncx|ncx); memory (\d+); checksum ([0-9a-f]{8})\.(.*)$", rest):
            book.update(package_files=int(package[1]), package_documents=int(package[2]), package_cover=package[3] or "-",
                        package_nav=package[4] == "nav", package_ncx=package[5] == "ncx",
                        package_memory=int(package[6]), package_checksum=int(package[7], 16), package_amiss=package[8])
        elif cost := re.match(r"package: \d+ ms: card \d+ ms for (\d+) reads? of (\d+) bytes\.$", rest):
            book.update(package_reads=int(cost[1]), package_read_bytes=int(cost[2]))
        elif unread := re.match(r"package: NOT READ: (.*)\. \d+ ms: card \d+ ms for \d+ reads? of \d+ bytes\.$", rest):
            book.update(package_not_read=unread[1])
        else:
            raise Failure(f"a line about book {number} that is not understood: {line!r}")
    return {book["path"]: book for book in books.values()}


def main() -> int:
    if len(sys.argv) != 7:
        print(__doc__, file=sys.stderr)
        return 2
    app, trace_tool, zip_tool, xml_tool, epub_tool = (str(Path(argument).resolve()) for argument in sys.argv[1:6])
    work = Path(sys.argv[6]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    card = work / "card"
    books = card / "books"
    (books / "more").mkdir(parents=True)

    try:
        # Two sound books, one of them with a name a shell would mind.
        make_book(books / "first.epub", 1, 4)
        make_book(books / "Second Book, the (2nd ed.).EPUB", 100, 12)
        # A book with one byte changed in the middle of its seventh entry's data.
        make_book(books / "damaged.epub", 200, 8)
        with zipfile.ZipFile(books / "damaged.epub") as archive:
            info = archive.infolist()[6]
        damaged = bytearray((books / "damaged.epub").read_bytes())
        damaged[info.header_offset + 30 + len(info.filename) + info.compress_size // 2] ^= 0x55
        (books / "damaged.epub").write_bytes(damaged)
        # A file that only has a book's name.
        (books / "not-a-book.epub").write_bytes(text(5_000, 300))
        # A book in a folder, which is looked in.
        make_book(books / "more" / "In a Folder.epub", 400, 2)
        # And what is not to be read at all: other files, the files some
        # computers leave beside the real ones, a folder with a book's
        # name, a book two folders down, and one that is not in /books.
        (books / "notes.txt").write_bytes(b"not a book")
        (books / "._first.epub").write_bytes(b"left by a computer")
        (books / "folder.epub").mkdir()
        (books / "more" / "deeper").mkdir()
        make_book(books / "more" / "deeper" / "too-deep.epub", 450, 2)
        make_book(card / "top.epub", 500, 2)

        # The application works on the check a little each turn, and how
        # long a turn takes depends on the computer. So it is given longer
        # if it had not finished.
        capture = work / "capture.bin"
        lines: list[str] = []
        for milliseconds in (1_500, 6_000, 30_000):
            run_application(app, card, capture, milliseconds)
            lines = log_lines(trace_tool, capture)
            if any(line.startswith("book check: card ") for line in lines):
                break
        said = "\n".join(lines)
        expect(any(line.startswith("book check: card ") for line in lines),
               f"the book check did not finish in 30 seconds. It said:\n{said}")
        expect(any(re.match(r"book check: the checksum alone, 262144 bytes in \d+ us \(\d+ KB/s\)\.$", line)
                   for line in lines), f"no line about the checksum:\n{said}")

        found = reported(lines)
        names = {"/books/first.epub", "/books/Second Book, the (2nd ed.).EPUB", "/books/damaged.epub",
                 "/books/not-a-book.epub", "/books/more/In a Folder.epub"}
        expect(set(found) == names, f"the books read were {sorted(found)}:\n{said}")

        total = {"entries": 0, "failed": 0, "out": 0, "in": 0, "reads": 0, "read_bytes": 0}
        xml_total = {"documents": 0, "events": 0, "not": 0}
        package_total = {"reads": 0, "read_bytes": 0, "memory": 0}
        for name in sorted(names - {"/books/not-a-book.epub"}):
            path = card / name[1:]
            book = found[name]
            expect(book["size"] == path.stat().st_size, f"{name}: size {book['size']}")
            # Against bn-zip, which reads the file the same way.
            theirs = zip_check(zip_tool, path)
            for key in ("entries", "failed", "out", "in", "reads", "read_bytes"):
                expect(book[key] == theirs[key],
                       f"{name}: the book check has {key} = {book[key]}, bn-zip check has {theirs[key]}:\n{said}")
                total[key] += book[key]
            # Against zipfile, which shares nothing with either.
            with zipfile.ZipFile(path) as archive:
                infos = archive.infolist()
            expect(book["entries"] == len(infos), f"{name}: {book['entries']} entries, zipfile has {len(infos)}")
            expect(book["in"] == sum(info.compress_size for info in infos), f"{name}: bytes in the file")
            deflated = [info for info in infos if info.compress_type == zipfile.ZIP_DEFLATED]
            expect(book["inflated_in"] == sum(info.compress_size for info in deflated), f"{name}: inflated, in")
            # Its XML against bn-xml, which parses the same documents. The
            # book has one file that is not XML, and both stop in it at
            # the same place.
            theirs_xml = xml_check(xml_tool, path)
            # (Everything in the book but its mimetype, its cover and its style sheet.)
            expect(book["xml_documents"] == theirs_xml["documents"] == len(infos) - 3,
                   f"{name}: {book['xml_documents']} XML documents, bn-xml has {theirs_xml['documents']}:\n{said}")
            expect(book["xml_refused"] == 0 and book["xml_memory"] > 2_000, f"{name}: the parser's memory:\n{said}")
            xml_total["documents"] += book["xml_documents"]
            xml_total["events"] += book["xml_events"]
            xml_total["not"] += book["xml_not"]
            if name != "/books/damaged.epub":
                expect((book["xml_events"], book["xml_checksum"]) == (theirs_xml["events"], theirs_xml["checksum"]),
                       f"{name}: the book check found {book['xml_events']} events, checksum "
                       f"{book['xml_checksum']:08x}; bn-xml {theirs_xml['events']}, {theirs_xml['checksum']:08x}:\n{said}")
                expect(book["xml_not"] == theirs_xml["failed"] == 1, f"{name}: one file is not XML:\n{said}")
            # Its package against bn-epub, which opens the book and reads
            # it the same way. (The damaged book's package is whole.)
            theirs_package = epub_info(epub_tool, path)
            expect("package_not_read" not in book and "package_checksum" in book and "package_reads" in book,
                   f"{name}: its package should have been read:\n{said}")
            mine = {"files": book["package_files"], "documents": book["package_documents"],
                    "cover": book["package_cover"], "nav": book["package_nav"], "ncx": book["package_ncx"],
                    "checksum": book["package_checksum"], "memory": book["package_memory"],
                    "reads": book["package_reads"], "read_bytes": book["package_read_bytes"]}
            for key, value in mine.items():
                expect(value == theirs_package[key],
                       f"{name}: the book check has the package's {key} = {value!r}, bn-epub has "
                       f"{theirs_package[key]!r}:\n{said}")
            expect(book["package_names"] == f'package: title "{theirs_package["title"]}", author '
                   f'"{theirs_package["author"]}", language "{theirs_package["language"]}".',
                   f"{name}: the book check has {book['package_names']!r}, bn-epub has {theirs_package!r}")
            expect(book["package_amiss"] == "" and theirs_package["documents"] == len(infos) - 6
                   and theirs_package["files"] == len(infos) - 3 and theirs_package["cover"] == "jpeg",
                   f"{name}: the package should list every file but itself, the container and the mimetype:\n{said}")
            package_total["reads"] += book["package_reads"]
            package_total["read_bytes"] += book["package_read_bytes"]
            package_total["memory"] = max(package_total["memory"], book["package_memory"])
            if name != "/books/damaged.epub":
                expect(book["ok"] and book["failed"] == 0, f"{name} should have passed:\n{said}")
                expect(book["out"] == sum(info.file_size for info in infos), f"{name}: bytes out")
                expect(book["inflated_out"] == sum(info.file_size for info in deflated), f"{name}: inflated, out")
        hurt = found["/books/damaged.epub"]
        expect(not hurt["ok"] and hurt["failed"] == 1 and hurt["first_failed"] == 7,
               f"the damaged book should fail at its seventh entry and nowhere else:\n{said}")
        expect(hurt["failure"] in ("bad checksum", "damaged"), f"the damaged entry failed as {hurt['failure']!r}")
        expect(found["/books/not-a-book.epub"].get("unreadable") == "not a zip file",
               f"the file that is no book:\n{said}")
        expect(not any(key.startswith("package") for key in found["/books/not-a-book.epub"]),
               f"the file that is no book has no package to speak of:\n{said}")

        # All of them together.
        last = (f"book check: FAILED. 2 of 5 books, 1 of {total['entries']} entries, "
                f"{total['out']} bytes out of {total['in']}.")
        expect(last in lines, f"the last line should be {last!r}:\n{said}")
        expect(any(line.startswith(f"book check: card ") and
                   f" ms for {total['reads']} reads of {total['read_bytes']} bytes " in line for line in lines),
               f"the reads of all the books together should be {total['reads']}:\n{said}")
        expect(any(line.startswith(f"book check: XML: {xml_total['documents']} documents, ") and
                   f" bytes, {xml_total['events']} events; parsing " in line and
                   line.endswith(f" bytes of events checksummed. Not XML: {xml_total['not']}.") for line in lines),
               f"the XML of all the books together should be {xml_total}:\n{said}")
        expect(any(re.match(r"book check: XML memory: the most a document used \d+ bytes; documents over 8 KB: \d+, "
                            r"over 12 KB: 0, over 16 KB: 0\.$", line) for line in lines),
               f"the line about the parser's memory:\n{said}")
        expect(any(line.startswith("book check: packages: 4 read; ") and
                   f" ms for {package_total['reads']} reads of {package_total['read_bytes']} bytes; the longest "
                   in line and line.endswith(f" ms; the most memory {package_total['memory']}.") for line in lines),
               f"the packages of all the books together should be {package_total}:\n{said}")
        expect("top.epub" not in said and "too-deep" not in said,
               f"only /books and the folders in it are read:\n{said}")
        expect("book check: 1 folder inside a folder not looked in: only one level down is." in lines,
               f"the folder two levels down should be mentioned:\n{said}")

        # With no /books, the top directory is where it looks.
        shutil.rmtree(books)
        for milliseconds in (1_500, 6_000, 30_000):
            run_application(app, card, capture, milliseconds)
            lines = log_lines(trace_tool, capture)
            if any(line.startswith("book check: card ") for line in lines):
                break
        said = "\n".join(lines)
        expect("book check: there is no /books. Looking in the top directory." in lines, f"no /books:\n{said}")
        expect(list(reported(lines)) == ["/top.epub"], f"the top directory's book:\n{said}")
        expect(any(line.startswith("book check: passed. 1 book, ") for line in lines), f"it should pass:\n{said}")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"the book check agrees with bn-zip, bn-xml, bn-epub and zipfile about 4 books: {total['entries']} entries, "
          f"{total['reads']} reads, {xml_total['documents']} XML documents, {xml_total['events']} events, "
          f"4 packages in {package_total['reads']} reads")
    return 0


if __name__ == "__main__":
    sys.exit(main())
