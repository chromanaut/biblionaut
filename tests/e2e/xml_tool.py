#!/usr/bin/env python3
"""End-to-end check of bn-xml, and through it of the XML parser, run by CTest.

The unit tests say what the parser should make of documents written for
them. Here what it makes of a document is compared with what another
parser makes of the same one: the expat that Python carries, which is
another build of the library, of another version, set up another way, and
driven by other code. Every event has to agree: each element, each
attribute in its order, all the text, each entity left unexpanded.

It also checks the documents an EPUB holds, read out of the ZIP file as
the firmware reads them, and that what is not XML is refused.

usage: xml_tool.py BN_XML WORK_DIR
"""

from __future__ import annotations

import random
import shutil
import subprocess
import sys
import zipfile
import zlib
from pathlib import Path
from xml.parsers import expat

ESCAPES = str.maketrans({"\\": "\\\\", "\n": "\\n", "\t": "\\t", "\r": "\\r"})
XHTML_DOCTYPE = ('<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" '
                 '"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">')


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def reference_events(data: bytes) -> tuple[bool, list[str]]:
    """What Python's expat makes of a document, in the lines bn-xml prints. False if it is not XML."""
    lines: list[str] = []
    text: list[str] = []

    def flush() -> None:
        if text:
            lines.append("-" + "".join(text).translate(ESCAPES))
            text.clear()

    def start(name: str, attributes: list[str]) -> None:
        flush()
        lines.append("(" + name)
        for at in range(0, len(attributes), 2):
            lines.append("A" + attributes[at] + " " + attributes[at + 1].translate(ESCAPES))

    def end(name: str) -> None:
        flush()
        lines.append(")" + name)

    def skipped(name: str, is_parameter_entity: int) -> None:
        if not is_parameter_entity:
            flush()
            lines.append("&" + name)

    parser = expat.ParserCreate()
    parser.ordered_attributes = True
    parser.StartElementHandler = start
    parser.EndElementHandler = end
    parser.CharacterDataHandler = text.append
    parser.SkippedEntityHandler = skipped
    try:
        parser.Parse(data, True)
        well_formed = True
    except expat.ExpatError:
        well_formed = False
    flush()
    return well_formed, lines


def checksum(lines: list[str]) -> int:
    crc = 0
    for line in lines:
        crc = zlib.crc32((line + "\n").encode("utf-8"), crc)
    return crc


def words(rng: random.Random, count: int) -> str:
    pool = ["the", "reader", "turned", "a", "page", "and", "then", "another", "naïve", "café", "—",
            "文字", "night", "fell", "quietly", "\U0001F600"]
    return " ".join(rng.choice(pool) for _ in range(count))


def chapter(seed: int, paragraphs: int) -> str:
    """XHTML as a chapter of an EPUB 2 book has it, with something of everything."""
    rng = random.Random(seed)
    parts = ['<?xml version="1.0" encoding="utf-8"?>\n', XHTML_DOCTYPE, "\n",
             '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="en">\n',
             f"<head><title>{words(rng, 3)} &amp; {words(rng, 2)}</title>",
             '<link rel="stylesheet" type="text/css" href="../styles/main%20one.css"/></head>\n',
             "<body class=\"chapter  wide\" id='c1'>\n<!-- a comment, which is not told of -->\n"]
    for number in range(paragraphs):
        kind = rng.randrange(8)
        if kind == 0:
            parts.append(f'<h2 id="h{number}">{words(rng, 4)}</h2>\n')
        elif kind == 1:
            parts.append(f'<p class="first">{words(rng, 6)}&nbsp;&mdash;&nbsp;{words(rng, 5)}&hellip;</p>\n')
        elif kind == 2:
            parts.append(f"<p>{words(rng, 5)} <i>{words(rng, 2)} <b>{words(rng, 1)}</b></i> &lt;{words(rng, 3)}&gt; "
                         f"&#169; &#x2014; &quot;{words(rng, 2)}&quot;</p>\n")
        elif kind == 3:
            parts.append(f'<div epub:type="footnote"><img src="images/pic{number}.jpg" alt="{words(rng, 3)}"\n'
                         f'     title="a\ttab and a\nline end"/><br/></div>\n')
        elif kind == 4:
            parts.append(f"<p><![CDATA[{words(rng, 4)} < & > ]]]]><![CDATA[>]]>{words(rng, 2)}</p><?pi {number}?>\n")
        else:
            parts.append(f"<p>{words(rng, 30 + rng.randrange(60))}</p>\n")
    parts.append("</body>\n</html>\n")
    return "".join(parts)


def package(items: int) -> str:
    """An OPF file, the way a book lists what it holds."""
    lines = ['<?xml version="1.0" encoding="UTF-8"?>',
             '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">',
             '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>A &amp; B</dc:title>'
             '<dc:identifier id="uid">urn:uuid:0</dc:identifier><meta property="dcterms:modified">2026</meta></metadata>',
             "<manifest>"]
    lines += [f'<item id="c{n}" href="text/chapter%20{n}.xhtml" media-type="application/xhtml+xml"/>'
              for n in range(items)]
    lines += ["</manifest>", '<spine toc="ncx">'] + [f'<itemref idref="c{n}"/>' for n in range(items)]
    lines += ["</spine>", "</package>", ""]
    return "\n".join(lines)


def documents() -> dict[str, bytes]:
    """The documents that are XML, by file name."""
    long_tag = '<p><img alt="' + "long " * 900 + '" src="data:image/png;base64,' + "QUJD" * 1200 + '"/></p>'
    deep = "".join(f"<d{n % 7}>" for n in range(300)) + "x" + "".join(f"</d{n % 7}>" for n in reversed(range(300)))
    made = {
        "short.xhtml": chapter(1, 3).encode(),
        "chapter.xhtml": chapter(2, 400).encode(),
        "content.opf": package(250).encode(),
        "bom.xml": b"\xef\xbb\xbf" + chapter(3, 5).encode(),
        "utf16-le.xml": b"\xff\xfe" + ("<a t=\"café\">" + "文 — \U0001F600 " * 500 + "</a>").encode("utf-16-le"),
        "utf16-be.xml": b"\xfe\xff" + ("<a t=\"café\">" + "文 — \U0001F600 " * 500 + "</a>").encode("utf-16-be"),
        "latin1.xml": ('<?xml version="1.0" encoding="ISO-8859-1"?><a t="café">naïve ' + "é" * 3000
                       + "</a>").encode("latin-1"),
        "long-tag.xhtml": (XHTML_DOCTYPE + "<html><body>" + long_tag + "</body></html>").encode(),
        "deep.xml": deep.encode(),
        "no-doctype.xhtml": chapter(4, 30).replace(XHTML_DOCTYPE, "").replace("&nbsp;", "&#160;")
                                          .replace("&mdash;", "&#8212;").replace("&hellip;", "&#8230;").encode(),
        "crlf.xml": chapter(5, 40).replace("\n", "\r\n").encode(),
        "empty-root.xml": b"<a/>",
        "one-long-line.xhtml": chapter(6, 300).replace("\n", " ").encode(),
    }
    return made


def bad_documents() -> dict[str, bytes]:
    """Documents that are not XML, each in its own way."""
    good = chapter(7, 20)
    return {
        "bad-mismatch.xml": b"<a><b>text</c></a>",
        "bad-cut-short.xhtml": good[: len(good) // 2].encode(),
        "bad-two-roots.xml": b"<a/><b/>",
        "bad-not-xml.xml": b"just some words, and a < sign",
        "bad-empty.xml": b"",
        "bad-utf8.xml": b"<a>bad \xc3\x28 bytes</a>",
        "bad-entity-no-dtd.xhtml": b"<html><p>one&nbsp;two</p></html>",
        "bad-duplicate.xml": b'<a x="1" x="2"/>',
    }


def events_of(tool: str, path: Path, *options: str) -> subprocess.CompletedProcess:
    return subprocess.run([tool, "events", str(path), *options], capture_output=True, check=False)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tool = str(Path(sys.argv[1]).resolve())
    work = Path(sys.argv[2]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    try:
        good = documents()
        bad = bad_documents()
        events = 0
        for name, data in good.items():
            path = work / name
            path.write_bytes(data)
            well_formed, expected = reference_events(data)
            expect(well_formed, f"{name}: Python's expat does not take it for XML, so the test is wrong")
            result = events_of(tool, path)
            expect(result.returncode == 0, f"{name}: bn-xml ended with {result.returncode}: {result.stderr!r}")
            got = result.stdout.decode("utf-8").split("\n")
            expect(got[-1] == "", f"{name}: the last line is not ended")
            got.pop()
            for number, (mine, theirs) in enumerate(zip(got, expected)):
                expect(mine == theirs, f"{name}: event {number + 1} is {mine!r}, Python's expat has {theirs!r}")
            expect(len(got) == len(expected), f"{name}: {len(got)} events, Python's expat has {len(expected)}")
            expect(f"{len(expected)} events, checksum {checksum(expected):08x}, ".encode() in result.stderr,
                   f"{name}: the summary is {result.stderr!r}")
            events += len(got)

        # What is not XML is refused, with what came before the mistake still told.
        for name, data in bad.items():
            path = work / name
            path.write_bytes(data)
            well_formed, expected = reference_events(data)
            expect(not well_formed, f"{name}: Python's expat takes it for XML, so the test is wrong")
            result = events_of(tool, path)
            expect(result.returncode == 1 and b"not well-formed at line " in result.stderr,
                   f"{name}: bn-xml ended with {result.returncode}: {result.stderr!r}")
            got = result.stdout.decode("utf-8", "replace").split("\n")[:-1]
            expect(got == expected, f"{name}: before the mistake bn-xml told {got!r}, Python's expat {expected!r}")

        # The same parser in little memory: enough for an ordinary chapter,
        # not for a tag of 9 KB, which is refused and not half read.
        result = events_of(tool, work / "chapter.xhtml", "--memory", "16384")
        expect(result.returncode == 0, f"a chapter in 16 KB: {result.stderr!r}")
        result = events_of(tool, work / "long-tag.xhtml", "--memory", "16384")
        expect(result.returncode == 1 and b"no memory" in result.stderr, f"a long tag in 16 KB: {result.stderr!r}")
        result = events_of(tool, work / "deep.xml", "--memory", "12288")
        expect(result.returncode == 1 and b"no memory" in result.stderr, f"300 levels in 12 KB: {result.stderr!r}")
        expect(events_of(tool, work / "no-such-file.xml").returncode == 1, "a missing file should be refused")

        # In an EPUB: read out of the ZIP file, inflated and parsed in one go.
        book = work / "book.epub"
        with zipfile.ZipFile(book, "w") as archive:
            archive.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
            archive.writestr("META-INF/container.xml", '<container><rootfiles><rootfile full-path="OEBPS/content.opf"/>'
                             "</rootfiles></container>", zipfile.ZIP_DEFLATED)
            for name, data in good.items():
                archive.writestr("OEBPS/" + name, data, zipfile.ZIP_DEFLATED if len(name) % 2 else zipfile.ZIP_STORED)
            archive.writestr("OEBPS/Capitals.XHTML", chapter(8, 10), zipfile.ZIP_DEFLATED)
            archive.writestr("OEBPS/images/cover.jpg", b"\xff\xd8 not XML and not looked at")
            archive.writestr("OEBPS/style.css", "p { margin: 0 }")
        result = subprocess.run([tool, "book", str(book)], capture_output=True, check=False)
        expect(result.returncode == 0, f"book.epub: bn-xml ended with {result.returncode}: {result.stdout!r}")
        lines = result.stdout.decode("utf-8").splitlines()
        said = {}
        for line in lines[:-1]:
            how, crc, count, _memory, _line, name = line.split(None, 5)
            said[name] = (how, int(crc, 16), int(count))
        with zipfile.ZipFile(book) as archive:
            wanted = [info for info in archive.infolist()
                      if info.filename.lower().endswith((".xhtml", ".html", ".htm", ".xml", ".opf", ".ncx"))]
            expect(len(wanted) == len(good) + 2 and set(said) == {info.filename for info in wanted},
                   f"book.epub: the documents parsed were {sorted(said)}")
            for info in wanted:
                _, expected = reference_events(archive.read(info))
                expect(said[info.filename] == ("ok", checksum(expected), len(expected)),
                       f"book.epub: {info.filename}: bn-xml says {said[info.filename]}, "
                       f"Python's expat ok, {checksum(expected):08x}, {len(expected)}")
        expect(lines[-1].startswith(f"{len(wanted)} documents, 0 failed; "), f"book.epub: the last line is {lines[-1]!r}")

        # And an EPUB with a document in it that is not XML.
        with zipfile.ZipFile(work / "bad.epub", "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("good.xhtml", good["short.xhtml"])
            archive.writestr("bad.xhtml", bad["bad-cut-short.xhtml"])
            archive.writestr("also-good.opf", good["content.opf"])
        result = subprocess.run([tool, "book", str(work / "bad.epub")], capture_output=True, check=False)
        lines = result.stdout.decode("utf-8").splitlines()
        expect(result.returncode == 1 and lines[-1].startswith("3 documents, 1 failed; "),
               f"bad.epub: bn-xml ended with {result.returncode}: {result.stdout!r}")
        expect(lines[1].startswith("not-well-formed ") and lines[1].endswith(" bad.xhtml"), f"bad.epub: {lines[1]!r}")
        expect(lines[0].startswith("ok ") and lines[2].startswith("ok "), f"bad.epub: the others: {lines!r}")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"{len(good)} documents, {events} events: bn-xml agrees with Python's expat about all of them; "
          f"{len(bad)} that are not XML refused")
    return 0


if __name__ == "__main__":
    sys.exit(main())
