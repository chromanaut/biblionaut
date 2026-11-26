#!/usr/bin/env python3
"""Writes the seed inputs of the fuzz targets, into inputs/<target>/seed-*.

A seed is an input that is known to take its target somewhere worth
starting from: a sound stream, a sound book, a table of contents with
entries in it, a language's hyphenation table and words for it, a drawing
with something of everything. The fuzzing engine makes its new inputs by
changing the ones it has, so what the seeds reach is where it begins. The
program that goes through the kept inputs (see tests/fuzz/CMakeLists.txt)
takes them too.

Every seed is made here, out of text written for the purpose: nothing in
them comes from a book. Each target's file says how its input is laid out;
the first byte or two are the target's choices, and the helpers below put
them there.

Run it after changing how a target reads its input, and commit what it
writes. The files named found-* beside the seeds are inputs that once found
a fault: this script leaves them alone.

The compressed seeds are made with Python's zlib, whose output can differ
from one version of zlib to the next. Nothing depends on the bytes being
the same: any sound stream is as good a seed as another.

Usage: tests/fuzz/make_seeds.py
"""

from __future__ import annotations

import random
import struct
import sys
import zlib
from pathlib import Path

INPUTS = Path(__file__).resolve().parent / "inputs"

TEXT = (
    "The lamp was lit before anyone had asked for it, and the harbour went on with its evening. "
    "Nobody on the quay looked up. The lamp was lit, and the harbour went on, and the evening with it. "
) * 3


def deflate(data: bytes, level: int = 9, strategy: int = zlib.Z_DEFAULT_STRATEGY) -> bytes:
    """`data` as a DEFLATE stream with nothing around it."""
    compressor = zlib.compressobj(level, zlib.DEFLATED, -15, 8, strategy)
    return compressor.compress(data) + compressor.flush()


def write(target: str, name: str, data: bytes) -> None:
    directory = INPUTS / target
    directory.mkdir(parents=True, exist_ok=True)
    (directory / f"seed-{name}").write_bytes(data)


def clear(target: str) -> None:
    directory = INPUTS / target
    if directory.is_dir():
        for old in directory.glob("seed-*"):
            old.unlink()


class DeflateWriter:
    """A DEFLATE stream written a field at a time, for what no compressor
    can be made to write."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.used = 0

    def bit(self, bit: int) -> None:
        if self.used % 8 == 0:
            self.data.append(0)
        self.data[-1] |= bit << (self.used % 8)
        self.used += 1

    def bits(self, value: int, count: int) -> None:
        """A field, lowest bit first: how the format packs all but a pattern."""
        for i in range(count):
            self.bit((value >> i) & 1)

    def pattern(self, pattern: int, count: int) -> None:
        """A prefix code's pattern, highest bit first."""
        for i in reversed(range(count)):
            self.bit((pattern >> i) & 1)


def patterns_of(lengths: list[int]) -> list[int]:
    """The bit patterns of a code, from how long each symbol's is (0 for a
    symbol with none): the rule of RFC 1951, section 3.2.2."""
    counts = [0] * 17
    for length in lengths:
        counts[length] += 1
    counts[0] = 0
    following = [0] * 17
    pattern = 0
    for bits in range(1, 16):
        pattern = (pattern + counts[bits - 1]) << 1
        following[bits] = pattern
    patterns = [0] * len(lengths)
    for symbol, length in enumerate(lengths):
        if length:
            patterns[symbol] = following[length]
            following[length] += 1
    return patterns


# ---- inflate: byte 0 (how output and input are cut up), then a stream ----


def inflate_seeds() -> None:
    clear("inflate")
    text = TEXT.encode()

    def seed(name: str, output_piece: int, source_piece: int, stream: bytes) -> None:
        write("inflate", name, bytes([output_piece | (source_piece << 3)]) + stream)

    seed("stored", 6, 6, deflate(text[:120], level=0))
    seed("fixed-codes", 0, 6, deflate(text[:90], strategy=zlib.Z_FIXED))
    seed("dynamic-codes", 4, 3, deflate(text))
    seed("nothing-in-it", 1, 0, deflate(b""))
    seed("followed-by-more", 3, 5, deflate(text[:200]) + b"what comes after the stream")

    # Three blocks, with the empty stored block a compressor writes when it
    # is told to bring its output up to a whole byte between them.
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    blocks = compressor.compress(text[:150]) + compressor.flush(zlib.Z_FULL_FLUSH)
    blocks += compressor.compress(text[150:300]) + compressor.flush(zlib.Z_SYNC_FLUSH)
    blocks += compressor.compress(text[300:]) + compressor.flush()
    seed("three-blocks", 5, 4, blocks)

    # A repeat from a long way back: some bytes with no pattern, then the
    # first of them again.
    chooser = random.Random(7)
    noise = bytes(chooser.randrange(256) for _ in range(1500))
    seed("far-back", 7, 6, deflate(noise + noise[:400]))

    # Blocks that produce nothing: the most in a row that are allowed, then
    # one more than that, which the inflater refuses and zlib does not.
    empty = bytes([0x00, 0x00, 0x00, 0xFF, 0xFF])  # A stored block, not the last, of no bytes.
    last = bytes([0x01, 0x03, 0x00, 0xFC, 0xFF]) + b"end"  # The last block, stored: three bytes.
    seed("empty-blocks", 2, 6, empty * 16 + last)
    seed("too-many-empty-blocks", 6, 6, empty * 17 + last)
    # And 6 KB of them, read a byte at a time: more than a call that asks
    # for one byte may take from its source. An inflater that did not
    # refuse them would have to come back for breath among them.
    seed("a-long-run-of-empty-blocks", 0, 6, empty * 1200 + last)

    # A stored block that lies across the end of the 32 KB window. Output
    # is copied into the window in runs, and a run has to stop where the
    # window does and go on from its start. It is asked for 257 bytes at a
    # time: a size the window's is a multiple of, such as 1 KB, would end
    # every run at the window's end of itself.
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    across = compressor.compress((text * 80)[:32700]) + compressor.flush(zlib.Z_FULL_FLUSH)
    kept = bytes(chooser.randrange(256) for _ in range(200))
    across += bytes([0x01]) + struct.pack("<HH", len(kept), len(kept) ^ 0xFFFF) + kept
    seed("stored-across-the-windows-end", 5, 6, across)

    # Distances written the longest way there is: a pattern of 15 bits and
    # 13 more that go with it, 28 in all, which is more than the inflater
    # has in hand once the pattern is read, so that it has to fill its
    # buffer again in the middle of one. No compressor gives the furthest
    # distances the longest patterns, so the block's codes are written
    # here: two letters, the end, repeats of 3 and of 258; and distances
    # of about 2,000 in one bit, the two furthest in 15, and thirteen more
    # that nothing uses, to use up the patterns between.
    literals = [0] * 286
    for symbol, length in ((ord("a"), 2), (ord("b"), 2), (256, 2), (257, 3), (285, 3)):
        literals[symbol] = length
    distances = [0] * 30
    distances[21] = 1
    for symbol in range(13):
        distances[symbol] = symbol + 2
    distances[28] = distances[29] = 15
    literal_patterns = patterns_of(literals)
    distance_patterns = patterns_of(distances)
    far = DeflateWriter()
    far.bits(1, 1)  # The last block,
    far.bits(2, 2)  # with codes of its own:
    far.bits(len(literals) - 257, 5)
    far.bits(len(distances) - 1, 5)
    far.bits(19 - 4, 4)
    # every length given by four bits that are the length itself, which is
    # the code-length code where the lengths 0 to 15 have four bits each
    # and the three ways of repeating, which come first, have none.
    for i in range(19):
        far.bits(0 if i < 3 else 4, 3)
    for length in literals + distances:
        far.pattern(length, 4)
    for _ in range(2000):
        letter = ord("a") + chooser.randrange(2)
        far.pattern(literal_patterns[letter], 2)
    for _ in range(100):  # 258 from 2,000 back, until there is something 24,577 back.
        far.pattern(literal_patterns[285], 3)
        far.pattern(distance_patterns[21], 1)
        far.bits(2000 - 1537, 9)
    for turn in range(24):  # Each begun at a different bit of a byte.
        for _ in range(turn % 4):
            far.pattern(literal_patterns[ord("a")], 2)
        far.pattern(literal_patterns[257], 3)
        far.pattern(distance_patterns[28 + turn % 2], 15)
        far.bits(turn * 101, 13)
    far.pattern(literal_patterns[256], 2)
    # From a source that gives a byte at a time, so that the inflater's own
    # store of input runs out wherever it can, the middle of a distance
    # among the places; and once more with the stream's end missing, which
    # is the middle of the last of them.
    seed("distances-written-the-longest-way", 7, 0, bytes(far.data))
    seed("cut-short-in-a-long-distance", 7, 6, bytes(far.data)[:-2])

    # Streams that are wrong in one place, which the inflater has to see.
    # A stored block gives its length twice, the second time with every
    # bit turned over: here the two do not agree.
    seed("stored-with-lengths-that-disagree", 6, 6, bytes([0x01, 0x03, 0x00, 0xFC, 0xFE]) + b"end")
    # A repeat from further back than there is anything: the first thing
    # in the stream, with the fixed codes, is "the 3 bytes 1 back".
    seed("repeat-from-before-the-start", 6, 6, bytes([0x03, 0x02, 0x00]))
    # A good stream with its last bytes missing.
    seed("cut-short", 6, 6, deflate(text)[:-9])


# ---- zip: byte 0 (how entries are read), then a ZIP file ----


class Zip:
    """A ZIP file written a field at a time, so that a seed can have exactly what it is for."""

    def __init__(self) -> None:
        self.data = b""
        self.records: list[bytes] = []

    def add(
        self,
        name: str,
        contents: bytes,
        *,
        deflated: bool = False,
        descriptor: bool = False,
        extra: bytes = b"",
        comment: bytes = b"",
        method: int | None = None,
        flags: int = 0,
        crc: int | None = None,
        size: int | None = None,
    ) -> None:
        """Adds an entry. `crc` and `size` are what the file says of the
        contents, where that is to be something other than the truth."""
        stored = deflate(contents) if deflated else contents
        method = (8 if deflated else 0) if method is None else method
        flags |= 0x0008 if descriptor else 0
        crc = zlib.crc32(contents) if crc is None else crc
        size = len(contents) if size is None else size
        encoded = name.encode()
        sizes = (0, 0, 0) if descriptor else (crc, len(stored), size)
        offset = len(self.data)
        self.data += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, flags, method, 0, 0x21, *sizes, len(encoded), len(extra))
        self.data += encoded + extra + stored
        if descriptor:
            self.data += struct.pack("<IIII", 0x08074B50, crc, len(stored), size)
        self.records.append(
            struct.pack(
                "<IHHHHHHIIIHHHHHII",
                0x02014B50, 20, 20, flags, method, 0, 0x21, crc, len(stored), size,
                len(encoded), len(extra), len(comment), 0, 0, 0, offset,
            )
            + encoded + extra + comment
        )

    def finish(self, comment: bytes = b"", count: int | None = None) -> bytes:
        directory = b"".join(self.records)
        count = len(self.records) if count is None else count
        end = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, count, count, len(directory), len(self.data), len(comment))
        return self.data + directory + end + comment


def zip_seeds() -> None:
    clear("zip")
    text = TEXT.encode()

    def seed(name: str, piece: int, file: bytes) -> None:
        write("zip", name, bytes([piece]) + file)

    book = Zip()
    book.add("mimetype", b"application/epub+zip")
    book.add("META-INF/", b"")
    book.add("OEBPS/one.xhtml", text, deflated=True)
    book.add("OEBPS/two.xhtml", text[:80])
    seed("stored-and-deflated", 5, book.finish())

    late = Zip()
    late.add("first", text[:100], deflated=True, descriptor=True)
    late.add("second", text[100:160], descriptor=True, extra=b"\x55\x54\x05\x00\x01\x00\x00\x00\x00")
    seed("sizes-after-the-data", 3, late.finish())

    commented = Zip()
    commented.add("only", text[:60], comment=b"an entry's own comment")
    seed("comment-after-the-end", 0, commented.finish(comment=b"a ZIP file's comment, which a reader has to look past"))

    seed("no-entries", 1, Zip().finish())

    many = Zip()
    for number in range(40):
        many.add(f"folder/{number:02d}-{'x' * (number % 7)}.txt", text[number : number + 30], deflated=number % 3 == 0)
    seed("forty-entries", 6, many.finish())

    same = Zip()
    same.add("twice", b"the first of two with one name")
    same.add("twice", b"the second")
    seed("one-name-twice", 2, same.finish())

    odd = Zip()
    odd.add("another-way", text[:40], method=12)  # A way of compressing that is not read.
    odd.add("locked", text[:40], flags=0x0001)  # Marked as encrypted.
    odd.add("plain", text[:40])
    seed("entries-that-are-not-read", 4, odd.finish())

    # Entries of which the directory does not say the truth.
    wrong = Zip()
    wrong.add("wrong-checksum", text[:70], crc=0x12345678)
    wrong.add("wrong-checksum-compressed", text, deflated=True, crc=0x12345678)
    wrong.add("holds-more-than-it-says", text, deflated=True, size=len(text) - 20)
    wrong.add("holds-less-than-it-says", text, deflated=True, size=len(text) + 20)
    wrong.add("sound", text[:50])
    seed("entries-that-are-not-what-the-directory-says", 3, wrong.finish())

    seed("more-entries-than-it-has", 7, book.finish(count=9))
    seed("as-many-entries-as-a-field-holds", 5, book.finish(count=0xFFFF))  # What a ZIP64 file says.


# ---- xml: byte 0 (memory, how the source gives, what the handler does), then a document ----

XHTML = b"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="en">
<head><title>The Lamp</title><link rel="stylesheet" type="text/css" href="../style.css"/></head>
<body>
<h1 id="start" class="chapter">One</h1>
<p class="first">The lamp was lit before anyone had asked for it,&nbsp;and the harbour went on &amp; on.</p>
<!-- a remark that is not part of the text -->
<p>Caf&#233; &#x2014; <em>nobody</em> <span epub:type="noteref">looked</span> up.<br/><img src="lamp.jpg" alt="A lamp"/></p>
<?page 2?><![CDATA[text that is <not> markup]]>
</body>
</html>
"""


def xml_seeds() -> None:
    clear("xml")

    def seed(name: str, memory: int, source: int, behaviour: int, document: bytes) -> None:
        write("xml", name, bytes([memory | (source << 2) | (behaviour << 5)]) + document)

    seed("a-chapter", 0, 0, 0, XHTML)
    seed("a-chapter-a-byte-at-a-time", 0, 1, 0, XHTML)
    seed("stopped-at-the-third-thing", 0, 0, 3, XHTML)
    seed("stopped-at-an-element-with-nothing-in-it", 0, 0, 2, b"<a><b/><c/></a>")
    seed("used-from-inside", 1, 4, 6, XHTML)
    seed("stopped-between-steps", 0, 5, 7, XHTML * 3)
    seed("too-little-memory", 2, 0, 0, b"<a>" + b"<b c='d'>" * 300 + b"</b>" * 300 + b"</a>")
    seed("no-memory-to-begin", 3, 0, 0, XHTML)
    seed("two-bytes-a-character", 0, 3, 0, "﻿<?xml version='1.0' encoding='UTF-16'?><a b='c'>dé中\U0001f600</a>".encode("utf-16-le"))
    seed("one-byte-a-character", 0, 2, 0, b"<?xml version='1.0' encoding='ISO-8859-1'?><a b='\xe9'>caf\xe9 \xfc</a>")
    seed(
        "declarations-of-its-own",
        1,
        0,
        0,
        b'<!DOCTYPE a [<!ENTITY me "a long piece of text"><!ATTLIST a kind CDATA "plain">'
        b"<!ELEMENT a (#PCDATA|b)*>]><a>it is &me;.<b kind='other'/>&nbsp;</a>",
    )
    seed("many-attributes", 0, 6, 0, b"<a " + b" ".join(b'n%d="value %d"' % (i, i) for i in range(40)) + b">text</a>")
    seed("not-xml", 0, 0, 0, b"<a><b>one</c></a>")
    seed("nothing", 0, 0, 0, b"")


# ---- package: byte 0, then (for a made book) container.xml and the package file ----

CONTAINER = (
    b'<?xml version="1.0"?>\n<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
    b'<rootfiles><rootfile full-path="%s" media-type="application/oebps-package+xml"/></rootfiles></container>\n'
)

PACKAGE_3 = b"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="id">urn:uuid:00000000-0000-4000-8000-000000000000</dc:identifier>
<dc:title>A  Made
 Book</dc:title><dc:creator>Nobody at All</dc:creator><dc:language>en</dc:language>
<meta property="dcterms:modified">2026-01-01T00:00:00Z</meta>
</metadata>
<manifest>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
<item id="cover" href="cover.jpg" media-type="image/jpeg" properties="cover-image"/>
<item id="c1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
<item id="c2" href="text/ch2.xhtml" media-type="application/xhtml+xml"/>
<item id="c3" href="ch%203.xhtml" media-type="application/xhtml+xml"/>
<item id="n" href="notes.xhtml" media-type="application/xhtml+xml"/>
<item id="css" href="style.css" media-type="text/css"/>
<item id="up" href="../ch4.xhtml" media-type="application/xhtml+xml"/>
<item id="gone" href="gone.xhtml" media-type="application/xhtml+xml"/>
<item id="c2-again" href="text/ch2.xhtml" media-type="application/xhtml+xml"/>
</manifest>
<spine toc="ncx">
<itemref idref="c1"/><itemref idref="c2-again"/><itemref idref="c2"/><itemref idref="c3"/><itemref idref="up"/>
<itemref idref="n" linear="no"/><itemref idref="gone"/><itemref idref="nothing"/><itemref idref="c1"/>
</spine>
</package>
"""

PACKAGE_2 = b"""<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
<dc:title>An Older Made Book</dc:title><dc:creator opf:role="aut">Nobody</dc:creator><dc:language>en-GB</dc:language>
<meta name="cover" content="cover"/>
</metadata>
<manifest>
<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
<item id="cover" href="cover.jpg" media-type="image/jpeg"/>
<item id="c1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
<item id="c2" href="text/ch2.xhtml" media-type="application/xhtml+xml"/>
</manifest>
<spine toc="ncx"><itemref idref="c1"/><itemref idref="c2"/></spine>
<guide><reference type="cover" title="Cover" href="ch1.xhtml"/></guide>
</package>
"""


def package_seeds() -> None:
    clear("package")

    def made(name: str, choices: int, container: bytes, package: bytes) -> None:
        write("package", name, bytes([choices]) + struct.pack("<H", len(container)) + container + package)

    usual = CONTAINER % b"OEBPS/content.opf"
    made("epub-3", 0x00, usual, PACKAGE_3)
    made("epub-2", 0x00, usual, PACKAGE_2)
    made("compressed-and-last-first", 0x30, usual, PACKAGE_3)
    made("zip-with-a-comment", 0x50, usual, PACKAGE_2)
    made("little-room-for-the-package", 0x04, usual, PACKAGE_3)
    made("no-room-for-the-package", 0x06, usual, PACKAGE_2)
    made("little-room-for-the-parser", 0x08, usual, PACKAGE_3)
    made("package-named-with-percent", 0x00, CONTAINER % b"OEBPS/content%2Eopf", PACKAGE_2)
    made("package-that-is-not-there", 0x00, CONTAINER % b"OEBPS/other.opf", PACKAGE_2)
    made("container-that-names-nothing", 0x00, b"<container><rootfiles/></container>", PACKAGE_2)
    made("package-cut-short", 0x10, usual, PACKAGE_3[: PACKAGE_3.index(b"</spine>") + 8])
    made("package-with-no-spine", 0x00, usual, PACKAGE_3[: PACKAGE_3.index(b"<spine")] + b"</package>")

    # The same kind of book as a ZIP file of the bytes themselves.
    book = Zip()
    book.add("mimetype", b"application/epub+zip")
    book.add("META-INF/container.xml", usual, deflated=True)
    book.add("OEBPS/content.opf", PACKAGE_2, deflated=True)
    book.add("OEBPS/toc.ncx", b"<ncx/>")
    book.add("OEBPS/cover.jpg", b"not a picture")
    book.add("OEBPS/ch1.xhtml", b"<html/>", deflated=True)
    book.add("OEBPS/text/ch2.xhtml", b"<html/>")
    write("package", "a-whole-zip-file", bytes([0x01]) + book.finish())


# ---- toc: bytes 0 and 1, then the nav, the NCX, or both ----

NAV = b"""<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Contents</title></head>
<body>
<nav epub:type="toc" id="toc"><h1>Contents</h1>
<ol>
<li><a href="ch1.xhtml">One</a></li>
<li><span>Part  the
 Second</span>
<ol>
<li><a href="text/ch2.xhtml#start">Two</a></li>
<li><a href="ch%203.xhtml#a-place">Three &amp; <em>a half</em></a><ol><li><a href="ch%203.xhtml#deeper">Deeper</a></li></ol></li>
</ol></li>
<li><a href="notes.xhtml">Notes</a></li>
<li><a href="nowhere.xhtml#x">Nowhere</a></li>
<li><a href="#toc">Here</a></li>
</ol></nav>
<nav epub:type="landmarks"><ol><li><a epub:type="bodymatter" href="ch1.xhtml">Start</a></li></ol></nav>
</body>
</html>
"""

NCX = b"""<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
<head><meta name="dtb:depth" content="3"/></head>
<docTitle><text>A Made Book</text></docTitle>
<navMap>
<navPoint id="p1" playOrder="1"><navLabel><text>One</text></navLabel><content src="ch1.xhtml"/></navPoint>
<navPoint id="p2" playOrder="2"><navLabel><text>Part the
 Second</text></navLabel><content src="text/ch2.xhtml"/>
<navPoint id="p3" playOrder="3"><navLabel><text>Two</text></navLabel><content src="text/ch2.xhtml#start"/></navPoint>
<navPoint id="p4" playOrder="4"><navLabel><text>Three</text></navLabel><content src="ch%203.xhtml#a-place"/>
<navPoint id="p5" playOrder="5"><navLabel><text>Deeper</text></navLabel><content src="ch%203.xhtml#deeper"/></navPoint>
</navPoint></navPoint>
<navPoint id="p6" playOrder="6"><navLabel><text>Notes</text></navLabel><content src="notes.xhtml"/></navPoint>
</navMap>
<pageList><pageTarget id="pg1" type="normal" value="1"><navLabel><text>1</text></navLabel><content src="ch1.xhtml#p1"/></pageTarget></pageList>
</ncx>
"""


def toc_seeds() -> None:
    clear("toc")

    def seed(name: str, tables: int, kind: int, stop: int, *files: bytes, deflated: bool = False, parser: int = 0) -> None:
        choices = tables | (kind << 2) | (0x10 if deflated else 0) | (stop << 5)
        body = files[0] if len(files) == 1 else struct.pack("<H", len(files[0])) + files[0] + files[1]
        write("toc", name, bytes([choices, parser]) + body)

    seed("nav", 0, 0, 0, NAV)
    seed("ncx", 1, 0, 0, NCX)
    seed("both-and-the-best-is-asked-for", 2, 0, 0, NAV, NCX, deflated=True)
    seed("both-and-the-ncx-is-asked-for", 2, 2, 0, NAV, NCX)
    seed("a-nav-and-the-ncx-is-asked-for", 0, 2, 0, NAV)
    seed("nav-stopped-after-two", 0, 1, 2, NAV, deflated=True)
    seed("ncx-stopped-after-one", 1, 2, 1, NCX)
    seed("nav-cut-short", 0, 0, 0, NAV[: NAV.index(b"Notes")])
    seed("ncx-with-little-room-for-the-parser", 1, 0, 0, NCX, parser=1)
    long_label = ("A label that goes on, and on, with café and 中文 in it; " * 8).encode()
    seed("nav-with-a-long-label", 0, 0, 0, NAV.replace(b"One", long_label, 1))
    seed("not-a-table-of-contents", 0, 0, 0, XHTML)


# ---- region_heap: byte 0 (the block), then requests of three bytes ----


def region_heap_seeds() -> None:
    clear("region_heap")

    def seed(name: str, block: int, offset: int, requests: list[tuple[int, int]]) -> None:
        body = b"".join(bytes([what]) + struct.pack("<H", number) for what, number in requests)
        write("region_heap", name, bytes([block | (offset << 3)]) + body)

    chooser = random.Random(11)

    def some(kinds: list[int], count: int) -> list[tuple[int, int]]:
        return [(chooser.choice(kinds), chooser.randrange(65536)) for _ in range(count)]

    # A piece given back twice, where its old record still says "in use":
    # two pieces are asked for and given back in that order, so that the
    # second joins the free space the first left and its record is left
    # lying inside that space. Then the second is given back again (the
    # request 4, with a number that picks the second of the pieces given
    # back). A heap that goes by the bytes at the place takes it.
    seed("a-piece-given-back-twice", 0, 0, [(0, 32), (0, 32), (2, 0), (2, 0), (4, 4), (0, 48), (0, 16), (5, 4)])

    # As a parser uses it: mostly asking, some resizing, some giving back.
    seed("as-a-parser-would", 0, 0, some([0, 0, 0, 1, 2, 3, 3, 7], 80))
    seed("with-mistakes", 1, 0, some([0, 0, 1, 2, 2, 3, 4, 4, 5], 80))
    seed("a-block-that-starts-anywhere", 2, 3, some([0, 1, 2, 3, 4, 5, 7], 40))
    seed("a-small-block", 3, 0, some([0, 2, 3, 4, 7], 30))
    seed("blocks-too-small-to-use", 6, 1, some([0, 2, 4, 5, 7], 12))
    seed("no-block", 7, 0, some([0, 2, 4], 6))
    seed("everything-taken-back", 0, 0, some([0, 0, 1, 2], 20) + [(6, 0)] + some([0, 3, 4, 5], 20))
    seed("the-largest-block", 4, 0, some([0, 0, 1, 3], 60))


# ---- hyphenator: byte 0 (how the language is given, room for places), a table or patterns, then words ----

# A language made up for the purpose: its patterns, as TeX writes them, and
# words for them to meet.
PATTERNS = [
    "1a", "1e", "1i", "1o", "1u", "hy3ph", ".un1", "4m1p", "n2at", "he2n", "hena4", "1na", "2io", "o2n", "1tio",
    "2ph.", "a1b2c", "l1l", "t2h", ".ha2", "e4d.", "1b", "1d", "2bb", "r1b",
]
WORDS = ["hyphenation", "Unhappy", "lamp", "HARBOUR", "evening", "the", "nobody", "lighted", "a", "", "millennium"]


class HyphenTable:
    """A hyphenation table (layout/hyphenator.h) written the plain way: the
    tree of the patterns as it stands, a state for every branching, nothing
    kept once that occurs twice. Its pieces can be changed before it is put
    together, for a seed whose table is to be wrong somewhere.

    `alphabet` is the language's small letters. Each is a letter of the
    table, and so is its capital where it has one."""

    def __init__(self, patterns: list[str], left: int, right: int, alphabet: str = "abcdefghijklmnopqrstuvwxyz") -> None:
        number = {letter: at + 1 for at, letter in enumerate(alphabet)}
        # The tree: where each letter leads from each node, and the numbers
        # of the pattern that ends at it, as they go into the table.
        children: list[dict[int, int]] = [{}]
        numbers_at = [b""]
        self.deepest = 1
        for text in patterns:
            letters: list[int] = []
            numbers = [0]
            for character in text:
                if character.isdigit():
                    numbers[-1] = int(character)
                else:
                    letters.append(0 if character == "." else number[character])
                    numbers.append(0)
            node = 0
            for letter in letters:
                if letter not in children[node]:
                    children[node][letter] = len(children)
                    children.append({})
                    numbers_at.append(b"")
                node = children[node][letter]
            # Each number that is not nothing, with how far back from the
            # pattern's end its place is.
            pairs = sorted((len(letters) - place, value) for place, value in enumerate(numbers) if value)
            numbers_at[node] = bytes([len(pairs)]) + bytes(byte for pair in pairs for byte in pair) if pairs else b""
            self.deepest = max(self.deepest, len(letters))

        # Each node that branches is a state. Where each begins, the first
        # node's first. (The list grows as it is gone through.)
        start = {0: 0} if children[0] else {}
        order = list(start)
        count = len(children[0])
        for node in order:
            for letter in sorted(children[node]):
                child = children[node][letter]
                if children[child]:
                    start[child] = count
                    count += len(children[child])
                    order.append(child)
        self.edges: list[list[int]] = []  # Each: the letter, the next state, the value.
        self.values = b""
        for node in order:
            for letter in sorted(children[node]):
                child = children[node][letter]
                value = 0
                if numbers_at[child]:
                    value = len(self.values) + 1
                    self.values += numbers_at[child]
                if letter == max(children[node]):
                    value |= 0x8000
                self.edges.append([letter, start.get(child, 0xFFFF), value])

        listed = {}
        for letter, which in number.items():
            listed[ord(letter)] = which
            if len(letter.upper()) == 1:
                listed[ord(letter.upper())] = which
        self.listed = sorted(listed.items())  # Each: the code point, which letter.
        self.version = 1
        self.left = left
        self.right = right
        self.widest = max([len(leading) for leading in children if leading], default=1)

    def data(self) -> bytes:
        header = b"BNHY" + bytes([self.version, self.left, self.right, self.deepest])
        header += struct.pack("<HHH", len(self.listed), len(self.edges), len(self.values)) + bytes([self.widest, 0])
        listed = b"".join(struct.pack("<I", code_point)[:3] + bytes([which]) for code_point, which in self.listed)
        edges = b"".join(struct.pack("<BHH", *edge) for edge in self.edges)
        return header + listed + edges + self.values


def hyphenator_seeds() -> None:
    clear("hyphenator")
    words = "\n".join(WORDS).encode()

    def table(name: str, room: int, language: HyphenTable, asked: bytes = words) -> None:
        write("hyphenator", name, bytes([room << 1]) + language.data() + asked)

    def patterns(name: str, room: int, left: int, right: int, written: str, asked: bytes = words) -> None:
        fewest = (left - 1) | ((right - 1) << 3)
        write("hyphenator", name, bytes([1 | (room << 1), fewest]) + written.encode() + b"\n" + asked)

    table("a-language", 7, HyphenTable(PATTERNS, 2, 3))
    table("little-room-for-places", 1, HyphenTable(PATTERNS, 1, 1))
    table("no-patterns", 7, HyphenTable([], 2, 2))

    # Letters of two, three and four bytes, some with capitals, and one
    # (the sharp s) with none.
    wider = HyphenTable(["1é", "é1t", "λ1λ", "1𐐨", "s2ß", "д1д", "1a", ".r2", "ß1e"], 1, 1, "aeilmnrstéßλд𐐨")
    table("letters-of-several-bytes", 7, wider,
          "résumé\nÉTÉ\nλλλλ\nΛΛλλ\nStraße\n𐐨𐐨𐐨𐐀\nдддаДД\nété-λλλ".encode())

    # Characters past the plain ones that the language has no letter for,
    # each the same in its low seven bits as a letter it has: é as i, ï as
    # o, ü as a capital of the kind, ā as a, and the long s as a word's end
    # would be. With a break before every letter, one would come beside
    # each if it were taken for that letter.
    table("characters-past-the-plain-ones", 7, HyphenTable(["1a", "1e", "1i", "1o", "1u", "1n", "1v", "1r"], 1, 1),
          "aéa\nnaïve\nüber\nrāna\noſo\nné\néé\nvoilà\n".encode())

    # A first state that is not what would be written for it: two of its
    # edges on one letter, of which the first is the one taken; and none
    # marked last with fewer said to be in a state than it has, so that
    # some of its letters cannot be reached.
    twice = HyphenTable(PATTERNS, 1, 1)
    twice.edges[3][0] = twice.edges[1][0]
    twice.edges[6][0] = twice.edges[2][0]
    table("two-edges-of-the-first-state-on-one-letter", 7, twice)
    narrow = HyphenTable(PATTERNS, 1, 1)
    narrow.widest = 4
    table("a-first-state-wider-than-the-table-says", 7, narrow)
    onward = HyphenTable(PATTERNS, 1, 1)
    first_state = next(at for at, edge in enumerate(onward.edges) if edge[2] & 0x8000)
    onward.edges[first_state][2] &= 0x7FFF
    onward.widest = first_state + 6
    table("a-first-state-that-runs-on-into-the-next", 7, onward)

    # Words with things in them that are not letters, and with bytes that
    # are not UTF-8: a character written in more bytes than it needs (a
    # stroke, and then a letter, where a break would otherwise come), a
    # byte that only continues one, half of a pair, a first byte that
    # begins nothing, a code point past the last, and a character cut short
    # by the word's end.
    table("words-with-other-things-in-them", 7, HyphenTable(PATTERNS, 2, 2),
          b"well-known\ndon't\ne-mail\n3rd\nhalf/way\nlamp.\n(harbour)\nna\xc3\xafve")
    table("words-that-are-not-utf-8", 7, HyphenTable(PATTERNS, 2, 2), b"\n".join([
        b"lamp\xc0\xaflight", b"ev\xc1\xa5ning", b"even\x80ing", b"harb\xed\xa0\x80our", b"hyph\xf5\x80\x80\x80enation", b"\xff",
        b"noth\xf4\x90\x80\x80ing", b"nob\xe0\x80\xafody", b"lighted\xe2\x82",
    ]))

    # As long as a word may be, one letter longer, and far longer.
    table("words-too-long", 7, HyphenTable(PATTERNS, 2, 2),
          ("ab" * 31 + "a" + "\n" + "ab" * 32 + "\n" + "hyphenation" * 30).encode())

    # Tables that open, and are wrong further in. Numbers that are not
    # their pattern's: a place further back than the pattern is long, more
    # of them than there are bytes for, and an edge that says its numbers
    # are past the end of them all.
    wrong = HyphenTable(PATTERNS, 2, 2)
    values = bytearray(wrong.values)
    values[1] = 200
    values[3] = 0x40
    wrong.values = bytes(values)
    wrong.edges[4][2] |= 0x7FFF
    table("numbers-that-are-not-their-patterns", 7, wrong)

    # No edge is marked as the last of its state, and the table says a
    # state may have 255: a state is read on into the ones after it, and
    # the last of them to the end of the edges.
    endless = HyphenTable(PATTERNS, 2, 2)
    for edge in endless.edges:
        edge[2] &= 0x7FFF
    endless.widest = 255
    table("states-with-no-last-edge", 7, endless)

    astray = HyphenTable(PATTERNS, 2, 2)
    astray.edges[0][1] = 0xFFFE
    astray.edges[2][1] = len(astray.edges)
    astray.edges[5][1] = len(astray.edges) - 1
    table("edges-that-lead-past-the-last", 7, astray)

    # A tree that is none: each edge leads back to the first state, so
    # that every run of these two letters is a pattern, as long as the
    # table says a pattern may be.
    around = HyphenTable(["a1", "1b"], 1, 1, "ab")
    for edge in around.edges:
        edge[1] = 0
    around.deepest = 65
    around.widest = 255
    table("a-table-that-goes-round", 7, around, ("a" * 63 + "\n" + "ab" * 31 + "\nABBA\n" + "b" * 64).encode())

    most = HyphenTable(PATTERNS, 2, 3)
    most.deepest = 65
    most.widest = 255
    table("as-much-work-as-a-table-may-ask-for", 7, most)

    # Tables that hold more than they say, where the work a word may take
    # is so little that going by what a table holds, and not by what it
    # says, is at once too much: patterns longer than the longest is said
    # to be, states with more edges than the widest is said to have, and a
    # pattern with more numbers than it has places.
    longer = HyphenTable(["a1", "1b"], 1, 1, "ab")
    for edge in longer.edges:
        edge[1] = 0
    longer.deepest = 2
    longer.widest = 1
    table("patterns-longer-than-the-table-says", 7, longer, ("a" * 63 + "\n" + "ab" * 20 + "\nabba").encode())
    wide = HyphenTable(PATTERNS, 2, 3)
    wide.deepest = 1
    wide.widest = 1
    table("states-wider-than-the-table-says", 7, wide)
    crowded = HyphenTable(["a1"], 1, 1, "ab")
    crowded.values = bytes([200]) + bytes([1, 1]) * 200
    table("more-numbers-than-a-pattern-has-places", 7, crowded, ("a" * 40 + "\nabab\na").encode())

    # Tables that are refused.
    unordered = HyphenTable(PATTERNS, 2, 3)
    unordered.listed[3], unordered.listed[4] = unordered.listed[4], unordered.listed[3]
    table("letters-out-of-order", 7, unordered)
    other = HyphenTable(PATTERNS, 2, 3)
    other.version = 2
    table("another-version", 7, other)
    none_before = HyphenTable(PATTERNS, 2, 3)
    none_before.left = 0
    table("no-letters-before-a-break", 7, none_before)
    end_listed = HyphenTable(PATTERNS, 2, 3)
    end_listed.listed[0] = (end_listed.listed[0][0], 0)
    table("a-letter-that-is-a-words-end", 7, end_listed)
    write("hyphenator", "cut-short", bytes([7 << 1]) + HyphenTable(PATTERNS, 2, 3).data()[:-7])
    write("hyphenator", "nothing", b"")

    patterns("patterns", 7, 2, 3, " ".join(PATTERNS))
    patterns("patterns-and-little-room", 2, 1, 1, " ".join(PATTERNS))
    patterns("patterns-that-disagree", 7, 1, 1, "1a 2a a1 a2 1a1 3a4 a3b b4a .a5 5a. .1b b1. 1ab1 a6b",
             b"abba\nbaobab\nAb\nba\na\nabababababab\nbbbbaaaa")
    patterns("no-patterns-in-text", 7, 2, 2, "")
    patterns("patterns-that-are-not", 7, 2, 2, "123 . .. a..b 4 hy3phenationally9longer {}~ 1é",
             b"hyphenationally\nlonger\nab\n" + b"a" * 70)


# ---- css: byte 0 (how the source gives), then a style sheet ----

SHEET = b"""@charset "utf-8";
@import url("more.css");
/* The body, and what is in it. */
body { margin: 0 5%; font: 1em/1.4 Georgia, "Times New Roman", serif; text-align: justify }
h1, h2, h3.sub { font-weight: bold; text-align: center; page-break-before: always; margin: 2em 0 1em }
p { text-indent: 1.5em; margin: 0; widows: 2 }
p.first, h1 + p, blockquote > p:first-child { text-indent: 0 }
p.first::first-letter { float: left; font-size: 3em; line-height: 1; padding: 0 0.1em 0 0 }
div#cover img, .full { width: 100%; height: auto }
a:link, a:visited { text-decoration: none; color: #036 }
.sc { font-variant: small-caps } sup { vertical-align: super; font-size: 75% }
table td[colspan], li::before { display: none }
"""

MEDIA_SHEET = b"""@media not amzn-mobi { .k { margin-left: 12pt } }
@media amzn-kf8 { .k { margin-left: 0 } }
@media screen and (max-width: 420px) { body { margin: 0 } }
@media all { @media print { p { display: none } } i { font-style: normal !important } }
@font-face { font-family: "A Face"; src: url(data:font/ttf;base64,AAAA;BBBB) }
@page { margin: 1em; @bottom-center { content: counter(page) } }
@import 'late.css' screen;
.after { float: right; margin: 1px 2px 3px 4px; break-inside: avoid-page }
"""

MISTAKES_SHEET = (b"\xef\xbb\xbf<!--\n} p { margin-top: 1em margin-bottom: 2em; text-indent: 0 }\n"
                  b"p { b { margin: 0 } float: left } i { width: calc(1em; float: left }\n"
                  b"@import url(a.css; q { } u { font-family: \"open;\n margin: 1 em; margin-top: 1em }\n"
                  b"s, , t,, { padding: 1px -1px; height: 5vh; width: 99999999999999px; line-height: 1e3 }\n"
                  b".\\31 0, .a\\:b, DIV.Up#Id, .\\0, .x\\0z, #a#b, p..q { line-height: 1.2.3; line-height: 120% }\n"
                  b"-->\n.end { font: bold 2em; font: caption; font: italic 700 x-large/normal a , \"B  c\" ")

# What a selector asks of an attribute, in every form, and the element
# straight after another; then brackets that hold no such thing.
ATTRIBUTES_SHEET = rb"""@namespace epub "http://www.idpf.org/2007/ops";
h2 + p, h1+p.first, div > h2 + p i { text-indent: 0 }
section[epub|type~="chapter"] > p, [hidden], a[href^='#'], td[colspan="2"] p, li[class] + li { margin: 0 }
[lang|=en], [title$="say \"hi\""], [data-x*=q i], [ *|type = "a" S ], [|title], [a|b|=c] { margin: 0 }
[EPUB|Type~="Note"], [\65 pub|t\79pe], [a="1\
2"], [a='\41 b\0 c'], [a=\31 0], [a="]{,;"], a.x[href]#y[rel=""]:link { margin: 0 }
[], [a=], [a=1], [=b], [a b], [a=b ii], [a==b], [a~b], [a|], [a|b|c], [*|], [a='b'x], h|p { margin: 0 }
p[a][b][c][d][e][f][g][h][i], p[a] q[b] r[c] s[d] t[e] u[f] v[g] w[h], + p, p + + q, a ~ b { margin: 0 }
[a , b { margin: 0 }
[a="b
], b { margin: 0 }
.caf\e9, .\1F600 z, .-x, [a='\e9\1F600 '] { margin: 0 }
*, * > p, div *[x], 1a, p$, li:nth-child(2n+1) + li, p:not(.a, [b="c)"]) + q { margin: 0 }
p:, q::, li:lang("\)") { margin: 0 }
.end[x] { float: left }
"""

# Values that are cut short: by the end of the brackets' text after a
# backslash, by the end of the line, and with something after them.
CUT_SHORT_SHEET = (b'[a=b i i], [a="b\\ \n{ margin: 0 }\n[a="b\n{ margin: 0 }\n[a=b, c { margin: 0 }\n'
                   b'[a="b\\\n c"], [a=\'b { margin: 0 }\n.end { float: left }\n')


# Font families of every kind: named by a keyword, by a name that is known
# however it is written, by a short one that counts only as a whole word,
# by nothing; and names that hold a known one that must not be taken.
FAMILIES_SHEET = rb"""body { font-family: "AGaramondPro-Italic-0855", Georgia, serif }
h1 { font-family: 'Trade Gothic LT Std', Arial-R, sans-serif; font: italic bold 1.5em/1.2 "Helvetica Neue", Arial }
pre, code { font-family: "Letter Gothic", Ubuntu Mono, "Courier New", monospace }
.hand { font-family: Monotype Corsiva, "Bradley Hand", cursive }
.icon { font-family: "Material Icons", FontAwesome, fantasy }
.odd { font-family: Monotype Sorts, Handel, "Shift Light", "No Such Face" }
.free { font-family: "Hand of Handel", Georgia } .freer { font-family: Mono Monotype Monotype, serif }
.words { font-family: Aerial, Sometimes, Latona, Times_New_Roman, TIMES, lato-light }
.far { font-family: \65 b garamond, "caf\e9  sans", ui-monospace, system-ui, math, "", ,, }
.end { font-family: inherit; font: 12px x }
"""


# What stands before an item of a list, by its own property and by the
# one that says three things at once; and values that are not read.
LISTS_SHEET = rb"""ol.toc, nav ol { list-style-type: none } ul { list-style-type: square }
ol.a { list-style-type: lower-alpha } ol.b { list-style-type: UPPER-LATIN } ol.i { list-style-type: lower-roman }
ol.z { list-style-type: decimal-leading-zero; list-style-type: lower-greek }
ul.plain { list-style: none } ul.in { list-style: circle inside url("dot.png") } ul.pic { list-style: url(d.png) }
ul.two { list-style: disc square } ul.odd { list-style: none disc; list-style: outside none upper-roman }
li { list-style: inherit !important; list-style-position: inside; list-style-image: none; list-style: url(x }
.end { list-style: initial; list-style: ; list-style-type: disc inside }
"""


def css_seeds() -> None:
    clear("css")

    def seed(name: str, source: int, sheet: bytes) -> None:
        write("css", name, bytes([source]) + sheet)

    seed("a-sheet", 0, SHEET)
    seed("a-sheet-a-byte-at-a-time", 1, SHEET)
    seed("media-and-other-at-rules", 3, MEDIA_SHEET)
    seed("mistakes", 5, MISTAKES_SHEET)
    seed("what-is-asked-of-attributes", 0, ATTRIBUTES_SHEET)
    seed("what-is-asked-of-attributes-a-few-bytes-at-a-time", 3, ATTRIBUTES_SHEET)
    seed("values-of-attributes-cut-short", 2, CUT_SHORT_SHEET)
    seed("a-style-attribute", 0, b"margin-left: 2em; font-weight: 700 !important; color: red; font-family: \"a;b\", serif")
    seed("a-media-query", 0, b"only screen and (max-width: 420px), not amzn-mobi")
    seed("what-stands-before-an-item-of-a-list", 0, LISTS_SHEET)
    seed("what-stands-before-an-item-of-a-list-two-bytes-at-a-time", 2, LISTS_SHEET)
    seed("the-kinds-of-face", 0, FAMILIES_SHEET)
    seed("the-kinds-of-face-a-few-bytes-at-a-time", 3, FAMILIES_SHEET)
    seed("a-list-of-families", 0, b'"AGaramondPro-Italic-0855", Monotype Sorts, "Shift Light", Arial-R, sans-serif')
    seed("a-family-with-a-long-name", 0,
         b"x " * 70 + b"garamond, " + b"q" * 70 + b"courier, " + b"1 2 3 " * 30 + b"script")
    seed("longer-than-there-is-room-for", 6,
         b", ".join(b"p.class-number-%d" % n for n in range(80)) + b" { margin: 0 }\nq { font-family: " + b"x" * 600 +
         b"; float: left }\n" + b" > ".join(b"a" for _ in range(9)) + b" {}\np" + b".c" * 13 + b" {}\n")
    seed("nothing", 0, b"")


# ---- css_rules: byte 0 (how much memory, and how the sheets are given), then
# ---- sheets and a document, a byte of zero between them ----
#
# A document is a line for each thing a parser tells of: `<name|attribute|
# value` for an element that opens, `>` for one that closes, `"` and some
# text for text, `#` for a picture.

RULES_SHEET = rb"""body { margin: 0 5%; font-family: Georgia, "Times New Roman", serif }
h1, h2 { text-align: center; page-break-before: always; margin: 2em 0 1em; font-weight: bold }
p { text-indent: 1.5em; margin: 0 }
h2 + p, p.first, blockquote > p { text-indent: 0 }
p.first::first-letter, .drop::first-letter { float: left; font-size: 3em; line-height: 1 }
div.note p, div.note li > p { margin-left: 2em !important; font-size: 0.9em }
#end, p#end.first { text-align: right; margin-left: 0 }
.i { font-style: italic } .b.i { font-weight: bold } .I { text-decoration: underline }
section[epub|type~="dedication"] > p, [hidden], a[href^='#'] { text-align: center; display: none }
[lang|=en] i, [title$="s" i], [ *|type = "note" ] p { vertical-align: super }
hgroup > h2 + p + p, hgroup > h2 + p + p + p, tr + tr td, td + td { font-variant: small-caps }
ol > li + li > p, div div div > p, * + blockquote { margin-top: 1em; margin-top: 2em; padding: 1px 2px }
a > b c, a > b > c d { width: 50%; height: auto; width: 99999px }
"""

RULES_SECOND_SHEET = rb"""p { text-indent: 3em; text-align: left !important }
.first { text-indent: 2em; float: right }
h2 + p { line-height: 1.2 }
div.note p { margin-left: 3em !important }
* { break-inside: avoid }
"""

# A document's own rules, as the last sheet: names before and after the
# others in order, more selectors under old names, new nodes.
RULES_OWN_SHEET = rb"""p { float: right } a.b > p.a, zz + p.first { width: 2px } * { height: 1px }
i { font-family: A Face } div p + q, aa + aa + p { float: none } [k*=c i] > [k] + .first { width: 3px }
"""

# What is asked of attributes and is not so, in each way of asking; an id
# asked of an element outside; something imported and something passed
# over; and what is important written before what is not.
RULES_ASKING_SHEET = rb"""@import "more.css";
[title~='v w'] p, [title~=''] p, [title~=zz] i, [lang|=en-GB] i, [title*=zzz] p, [k^=''] p { width: 4px }
#start > i, #nope p, p#nope.first, div#nope > p { height: 4px }
[type] p, [TYPE=note] > p, li[type~=note] { text-indent: 5px }
q, p.first { color: red; width: 1px !important; width: 2px; height: 1px; height: 2px !important }
"""

# A document, a line to each element that opens or closes.
RULES_DOCUMENT = b"""<html|xmlns:epub|http://www.idpf.org/2007/ops|lang|en-GB
<body|class|b i
<section|epub:type|frontmatter dedication
<p
>
<P|CLASS|first
>
>
<hgroup
<h2
>
<p
>
<p
>
<p
>
<p
>
>
<h2|title|Headings
>
<p|style|margin-top: 1em; text-indent: 4em; margin-left: 9em
>
<p|class|first  drop\tI|id|start
<i|class|i
>
<a|href|#end|hidden|
>
>
<div|class|note|title|NOTES|k|abc
<p|k|
>
<p|class|first
>
<ol
<li
<p
>
>
<li|epub:type|note
<p
>
>
>
<div
<div
<p
>
>
>
>
<blockquote
<p
>
>
<blockquote|o:type|note
<p|class|first
>
>
<table
<tr
<td
>
<td
>
>
<tr
<td
>
<td
>
>
>
<a
<b
<x
<b
<c
<y
<c
<d
>
>
>
>
>
>
>
>
<p|class|first|id|end|style|float: none !important; text-align: justify
>
<p|class|FIRST|id|END|O:TYPE|note
>
>
>
"""


# ---- document_sheets: byte 0 (how much memory, and how a file is read),
# ---- byte 1 (which file fails, and where), byte 2 (where the first
# ---- document is given up), then the book's files, a byte of zero between
# ---- them: a path, a line end, and what the file holds.

def sheets_page(head: str, body: str = "<p>Text.</p>") -> str:
    return (f"<?xml version='1.0' encoding='utf-8'?>\n<html xmlns='http://www.w3.org/1999/xhtml'><head>"
            f"<title>A page</title>{head}</head><body>{body}</body></html>")


def sheets_link(href: str, more: str = "") -> str:
    return f"<link rel='stylesheet' type='text/css' href='{href}' {more}/>"


def document_sheets_seeds() -> None:
    clear("document_sheets")
    rule = "p { text-indent: 1em; margin: 0 } h1 + p, p.first { text-indent: 0 }\n"

    def seed(name: str, files: dict[str, str], choices: int = 0, failing: int = 7, given_up: int = 0) -> None:
        book = b"\0".join(path.encode() + b"\n" + contents.encode() for path, contents in files.items())
        write("document_sheets", name, bytes([choices, failing, given_up]) + book)

    two = {
        "text/one.xhtml": sheets_page(sheets_link("../css/a.css") + "<style>p { text-indent: 2em }</style>"),
        "text/two.xhtml": sheets_page(sheets_link("../css/b.css") + sheets_link("../css/a.css")),
        "css/a.css": "@import 'b.css'; " + rule, "css/b.css": rule,
    }
    seed("a-book-of-two-documents", two)
    seed("a-document-whose-path-is-too-long", {**two, "long/" + "x" * 600 + ".xhtml": two["text/one.xhtml"]})
    seed("a-book-read-a-byte-at-a-time", two, choices=3 << 6)
    seed("a-book-read-seven-bytes-at-a-time", two, choices=2 << 6)
    seed("a-document-given-up-part-way", two, given_up=5)
    seed("a-document-given-up-in-a-sheet", two, given_up=3, choices=2 << 6)
    seed("a-sheet-that-fails", two, failing=2 | (3 << 3))
    seed("a-document-that-fails", two, failing=0 | (20 << 3))
    seed("little-room-for-names", two, choices=2)
    seed("no-room-for-names", two, choices=3)
    seed("little-room-for-a-style", two, choices=2 << 2)
    seed("no-room-for-a-style", two, choices=3 << 2)
    seed("a-table-with-room-for-little", two, choices=2 << 4)
    seed("a-table-with-room-for-next-to-nothing", two, choices=3 << 4)
    seed("links-that-count-and-links-that-do-not", {
        "page.xhtml": sheets_page(
            "<link rel='StyleSheet' href='caps.css'/><link rel='alternate stylesheet' href='n.css'/>"
            "<link rel='stylesheet\talternate' href='n.css'/><link rel='icon' href='i.css'/><link href='norel.css'/>"
            "<link rel='stylesheet'/><link rel='stylesheet' type=' TEXT/CSS ' href='typed.css'/>"
            "<link rel='stylesheet' type='application/vnd.adobe-page-template+xml' href='t.xpgt'/>"
            "<link rel=' author  stylesheet ' href='words.css'/>"
            "<x:link xmlns:x='http://www.w3.org/1999/xhtml' rel='stylesheet' href='prefixed.css'/>"),
        "caps.css": rule, "n.css": rule, "typed.css": rule, "words.css": rule, "prefixed.css": rule,
    })
    seed("links-and-styles-for-other-media", {
        "page.xhtml": sheets_page(
            sheets_link("kindle.css", "media='amzn-mobi'") + sheets_link("all.css", "media='all'") +
            sheets_link("others.css", "media='not amzn-mobi'") + sheets_link("newer.css", "media='(min-device-width: 0px)'") +
            "<style media='print'>p { margin: 0 }</style><style media='screen'>p { margin: 1em }</style>"),
        "kindle.css": rule, "all.css": rule, "others.css": rule, "newer.css": rule,
    })
    seed("links-to-no-file-of-the-book", {
        "a/page.xhtml": sheets_page(sheets_link("https://example.org/a.css") + sheets_link("gone.css") + sheets_link("") +
                                    sheets_link("../../up.css") + sheets_link("/top.css#x") + sheets_link("a%20b.css?q")),
        "up.css": rule, "top.css": rule, "a/a b.css": rule,
    })
    seed("more-links-than-are-followed", {
        "page.xhtml": sheets_page("".join(sheets_link(f"s{i % 6}.css") for i in range(11))),
        **{f"s{i}.css": rule for i in range(6)},
    })
    seed("imports-too-deep-and-round-in-a-ring", {
        "page.xhtml": sheets_page(sheets_link("s0.css") + sheets_link("ring.css")),
        "deep.xhtml": sheets_page(sheets_link("s3.css")),
        **{f"s{i}.css": f"@import 's{i + 1}.css'; " + rule for i in range(5)},
        "ring.css": "@import 'ring.css'; @import url(s5.css); @import 'print.css' print; @import 'http://x.org/y.css'; " + rule,
    })
    seed("a-sheet-named-twice", {
        "links.xhtml": sheets_page(sheets_link("a.css") + sheets_link("b.css") + sheets_link("a.css")),
        "imports.xhtml": sheets_page(sheets_link("c.css") + sheets_link("d.css")),
        "a.css": rule, "b.css": rule, "shared.css": "p { text-indent: 3em }",
        "c.css": "@import 'shared.css'; p { text-indent: 1em }", "d.css": "@import 'shared.css'; h1 { margin: 0 }",
    })
    seed("styles-of-every-kind", {
        "page.xhtml": sheets_page(
            "<style>p { text-indent: 1em</style><style>h1 { font-weight: bold }</style>"
            "<style type='text/x-other'>p { margin: 9em }</style><style></style>"
            "<style>@import 'a.css'; p { text-indent: 1em }</style><style><b>p { margin: 0 }</b> h2 { margin: 0 }</style>"
            "<!--[if gte mso 9]><style>p { text-indent: 1em }</style><![endif]-->" + sheets_link("a.css")),
        "long.xhtml": sheets_page("<style>h1 { margin: 0 }</style><style>p.long { margin: 0 } " + " " * 80 +
                                  "</style><style>h2 { margin: 0 }</style>"),
        "a.css": rule,
    }, choices=1 << 2)
    seed("a-style-of-another-type-and-no-other", {
        "page.xhtml": sheets_page("<style type='text/x-other'>p { margin: 9em }</style>"),
        "typed.xhtml": sheets_page("<style type=' Text/CSS '>p { margin: 9em }</style>"),
    })
    # The room is 64 bytes: a style of 63 fits with the byte that ends it, and one of 64 does not.
    fits = "p { margin: 0 }".ljust(63)
    seed("a-style-that-just-fits-and-one-a-byte-longer", {
        "fits.xhtml": sheets_page(f"<style>{fits}</style>"),
        "long.xhtml": sheets_page(f"<style>{fits} </style>"),
        "two.xhtml": sheets_page(f"<style>{fits[:31]}</style><style>{fits[:31]}</style>"),
        "three.xhtml": sheets_page(f"<style>{fits[:31]}</style><style>{fits[:32]}</style><style>p{{}}</style>"),
    }, choices=1 << 2)
    seed("styles-that-would-read-otherwise-run-together", {
        "page.xhtml": sheets_page("<style>p { text-indent: 1em } h1 { margin: 0</style><style>} h2 { margin: 0 } "
                                  "h3 { margin: 0 }</style><style>h4 { margin: 0 }</style>"),
        "after.xhtml": sheets_page("<style>h5 { margin: 0 }</style>"),
    })
    seed("a-head-cut-short-inside-a-style", {
        "cut.xhtml": "<html><head><style>p { margin: 0 }</style><style>h1 { margin: 0 } h2 { margin: 0 }",
        "only.xhtml": "<html><head><style>p { margin: 0 } h1 { margin: 0 }",
    })
    # The target first asks for a document of this name, expecting none.
    seed("a-file-with-the-name-of-the-document-that-is-not-there", {
        "no such document": sheets_page(sheets_link("a.css") + sheets_link("gone.css")),
        "page.xhtml": sheets_page(sheets_link("a.css")),
        "a.css": rule,
    })
    seed("more-imports-than-are-remembered", {
        "page.xhtml": sheets_page(sheets_link("a.css")),
        "a.css": "".join(f"@import 'i{i % 5}.css';" for i in range(70)) + rule,
        **{f"i{i}.css": rule for i in range(5)},
    })
    seed("an-entity-in-the-head", {
        "page.xhtml": "<!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.1//EN' 'http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd'>"
                      "<html><head><title>A&nbsp;page</title>" + sheets_link("a.css") +
                      "<style>p { text-indent:&nbsp;1em } h1 { margin: 0 }</style></head><body/></html>",
        "a.css": rule,
    })
    seed("the-head-and-no-more", {
        "body.xhtml": sheets_page(sheets_link("a.css"), sheets_link("late.css") + "<style>p { margin: 0 }</style>" + "x" * 3000),
        "drawing.xhtml": "<svg xmlns='http://www.w3.org/2000/svg'><link rel='stylesheet' href='a.css'/></svg>",
        "broken.xhtml": "<html><head>" + sheets_link("a.css") + "<meta charset='utf-8'><style>p { margin: 0 }</style></head><body/></html>",
        "cut.xhtml": "<html><head>" + sheets_link("a.css") + "<style media='print'>p { margin: 0 }",
        "nothing.xhtml": "", "text.xhtml": "Not XML at all.", "a.css": rule, "late.css": rule,
    })


# What each element comes to: a sheet with every way a value is turned
# into an amount, and a document with every kind of element, the
# attributes that say how something looks or what it is, and what is
# nothing with something inside it.
STYLES_SHEET = rb"""html { font-size: 110%; margin: 1em } body { margin: 0 5% 2em; padding: 1px; line-height: 1.4 }
h1 { font-size: 2.5rem; margin: 2em 0 1em; text-align: center; page-break-before: always; page-break-after: auto }
p { text-indent: 1.5em; margin: 0; text-align: justify; line-height: 120% } .first::first-letter { float: left;
  font-size: 3em; font-weight: bold; font-family: Georgia } .small { font-size: small } .big { font-size: 18px }
.rem { font-size: 1.2rem; margin-left: 2rem; line-height: 1.5rem } .px { line-height: 20px; margin: 10px 2.5% -1em 1in }
.sans { font-family: "Trade Gothic", sans-serif } .unset { font-family: initial; font-weight: initial;
  text-align: initial; line-height: normal; text-indent: initial; list-style: initial }
.b6 { font-weight: 600 } .b5 { font-weight: 500 } .er { font-weight: bolder } .u { text-decoration: underline }
.s { text-decoration: line-through } .nou { text-decoration: none } .up { vertical-align: super }
.down { vertical-align: -0.2em } .base { vertical-align: baseline } .caps { font-variant: small-caps }
.same { font-weight: inherit; font-style: inherit; font-size: inherit; text-align: inherit; text-indent: inherit;
  line-height: inherit; list-style-type: inherit; vertical-align: inherit; font-family: inherit; font-variant: inherit }
.own { margin: inherit; padding: inherit; width: inherit; height: inherit; float: inherit; display: inherit;
  break-before: inherit; break-after: inherit; break-inside: inherit }
.block { display: block } .inline { display: inline } .item { display: list-item } .gone { display: none }
.auto { margin: 0 auto; width: 75% } .left { float: left; height: 2em } .keep { break-after: avoid;
  break-inside: avoid; break-before: avoid } .none { list-style-type: none } .roman { list-style: upper-roman }
.huge { font-size: 99999em; margin: 99999em -99999px; line-height: 99999; text-indent: -99999% }
"""

STYLES_DOCUMENT = b"""<html|xml:lang|en-GB|lang|fr
<head
<title
>
>
<body
<h1|class|keep
<small
>
<big|class|same
>
>
<h2
>
<h3
>
<h4
>
<h5|class|rem
>
<h6|align|RIGHT
>
<p|class|first small
<i
<b
<sup
<em|class|base
>
>
>
>
<span|class|big up
<span|class|down
>
>
>
<div|class|px sans
<p|class|rem unset
<code|class|same
>
<kbd
>
>
<pre
<span
>
>
>
<p|class|b6 u
<span|class|b5
<span|class|er
<s
<u|class|nou
<ins
>
>
>
>
>
<sub|class|up block
>
>
<blockquote|align|center
<address
>
>
<ul
<li
<ul
<li
<ul|type|Square
<li|type|a
>
>
>
>
>
<li|class|none same
>
>
<ol|type|A|class|roman
<li
>
<li|type|x|class|unset
>
>
<menu
<li
>
>
<dl
<dt
>
<dd
>
>
<figure
>
<hr|align|center
>
<table|align|center
<caption|align|left
>
<tr|align|right
<th
>
<td|align|justify|class|auto
>
>
>
<center
<p|class|own
>
>
<div|class|gone
<p|class|block
<b
>
>
>
<p|hidden|
<i
>
>
<span|class|left item
>
<div|class|inline|dir|rtl
<p|dir|LTR
<span|dir|up
>
>
>
<p|lang|de
<span|xml:lang|de-CH|lang|it
>
<span|lang|
>
<span|lang|DE
>
<span|lang|a-tag-that-is-longer-than-a-tag-may-be
>
>
<p|class|huge
<p|class|huge
<p|class|huge
>
>
>
<math
<mi
>
>
<ruby
<rt
>
>
<unknown|style|font-size: 2em; margin: 1em; display: block; list-style: square
>
>
>
"""

# Every word the stack of styles reads, each on an item of a list with an
# element inside it that says the same again, so that what is taken from
# outside has something to take; all of it inside text that is raised,
# slanted and underlined. In three parts, each short enough for the
# engine to keep whole.
STYLES_WORDS = [
    (b"font-size", b"xx-small x-small small medium large x-large xx-large smaller larger initial inherit 0 12px 2rem"),
    (b"font-weight", b"normal bold bolder lighter 100 600 900 initial inherit"),
    (b"font-style", b"normal italic oblique initial inherit"),
    (b"font-variant", b"normal small-caps initial inherit"),
    (b"text-decoration", b"none underline line-through overline initial inherit"),
    (b"vertical-align", b"baseline sub super top text-top middle bottom text-bottom 2px -2px 50% initial inherit"),
    (b"font-family", b"serif sans-serif monospace cursive Georgia initial inherit"),
    (b"text-align", b"left right center justify start end initial inherit"),
    (b"text-indent", b"0 1.5em -1em 5% 10px 1rem initial inherit"),
    (b"line-height", b"normal 1.2 1.2em 120% 20px 1.5rem 0 initial inherit"),
    (b"list-style-type", b"none disc circle square decimal decimal-leading-zero lower-alpha upper-alpha lower-roman "
     b"upper-roman lower-latin upper-latin initial inherit"),
    (b"display", b"block inline inline-block list-item none initial inherit"),
    (b"margin-left", b"0 auto 1em -1em 5% 10px 1rem initial inherit"),
    (b"padding-top", b"0 1em 5% 10px 1rem initial inherit"),
    (b"width", b"auto 0 10em 50% 100px initial inherit"),
    (b"height", b"auto 0 10em 50% 100px initial inherit"),
    (b"float", b"left right none initial inherit"),
    (b"break-before", b"auto always avoid left right page initial inherit"),
    (b"break-after", b"auto always avoid page initial inherit"),
    (b"break-inside", b"auto avoid initial inherit"),
]


def styles_words(part: int) -> tuple[bytes, bytes]:
    """The sheet and the document for a third of those properties."""
    words = [(name, n, word) for name, all_words in STYLES_WORDS[part::3] for n, word in enumerate(all_words.split(b" "))]
    sheet = b"".join(b".%s-%d { %s: %s }\n" % (name, n, name, word) for name, n, word in words)
    document = (b"<html|lang|en\n<body\n<ol|type|a\n<sup\n<i\n<u|dir|rtl\n" +
                b"".join(b"<li|class|%s-%d\n<b|class|%s-%d\n>\n>\n" % (name, n, name, n) for name, n, _ in words) +
                b">\n" * 6)
    return sheet, document


# Elements deeper than a stack keeps, each naming a language of its own,
# with one that is nothing far down and something inside it.
STYLES_DEEP_DOCUMENT = (b"".join(b"<div|lang|l%d|class|%s\n" % (n, b"gone" if n == 36 else b"big") for n in range(40)) +
                        b"<p\n>\n" + b">\n" * 40)


# A document's blocks: text straight in an element and beside blocks,
# white space alone, line ends and rules and pictures, headings, lists with
# their numbers, a table's cells, quotations, margins that meet and what
# keeps them apart, pages that end, a first letter's style, and text that
# begins floated.
BLOCKS_SHEET = rb"""body { margin: 0 5%; line-height: 1.4 } h1 { margin: 2em 0 1em; page-break-before: always }
h2 { page-break-after: avoid } h3 { break-after: always } p { margin: 0; text-indent: 1.5em }
p.first { text-indent: 0; margin-top: 1em } p.first::first-letter { float: left; font-size: 3em; font-weight: bold }
div.first::first-letter { font-variant: small-caps } .drop { float: left; font-size: 3em }
.verse span { display: block; margin-left: 2em } .note { margin: 1em 2em; padding: 0.5em 1em 7px; font-size: 0.9em }
.spacer { height: 2em; margin: 1em 0 } .pull { margin: -0.5em 0 1em -2em } .centre { width: 60%; margin: 2em auto }
.side { float: right; width: 30% } .keep { break-inside: avoid } .gone { display: none } .inl { display: inline }
blockquote { margin: 1em 40px 20px } ol.roman { list-style-type: upper-roman } .item { display: list-item }
td, th { padding: 2px 4px } hr { margin: 1em 25%; height: 1px } .mixed { margin: 4% 0 19px } br.b { display: block }
"""

BLOCKS_DOCUMENT = b"""<html|xml:lang|en
<body
"\r\x0c\t 
<h1
"The first
<i
"heading
>
>
<p|class|first
"It began,
<b
" slowly
>
"\t
<br
>
"a line of its own.
>
<p
<span|class|drop
"T
>
"he letter is dropped.
<span|class|drop
"Not this.
>
>
<p
<b
<span|class|drop
<span|class|drop
#
>
"A
>
>
"picture and a letter.
>
<div
"Text straight in a division.
<p
"Then a paragraph.
>
"And text after it.
<div
>
"And more.
>
<div|class|verse
<p
<span
"A line of verse,
>
"
<span
"and another.
>
>
>
<div|class|note
<p
"A note.
>
<p|class|pull
"Pulled.
>
>
<div|class|spacer
>
<div|class|spacer
<span
"
>
>
<div|class|spacer
<div|style|height: 3px
>
>
<p|class|centre
"Centred.
>
<div|class|side
<p|class|mixed
"To the side.
>
>
<h2
"Kept with what follows
>
<p|class|keep
"Together.
>
<h3
"A page ends
>
<blockquote
<p
"Quoted.
>
<blockquote
"In a quotation.
>
"And after.
>
<ul
<li
"One
>
<li
<p
"Two
>
<p
"Two again
>
>
<li
<ul
<li
"Inside
>
>
"Three
>
<li
<h3
"A heading in an item
>
>
<li
<hr
>
"After a rule.
>
<li|class|gone
"Gone
>
>
<ol|start|5|class|roman
<li
"Five
>
<li|value|9
"Nine
>
<li|value|x
"Ten
>
<ol|start|-2
<li
"Minus two
>
>
>
<ol|start|999999999
<li
>
<li|value|+12
"Twelve
>
>
<p|class|item|value|4
"An item by its display
>
<li
"An item in no list
>
<table
<caption
"A table
>
<tr
<th
"Head
>
<td
"Cell
<p
"and a paragraph
>
>
<td|class|inl
"In line
>
<td
>
>
>
<hr
>
<hr|class|gone
>
<pre
"  kept
"    as written
>
<p
<br
>
"After an empty line.
<br|class|gone
>
<br|class|b
>
<BR
>
>
<p
#
"A picture in a line.
>
<div|class|gone
"Nothing
<p
#
<br
>
>
>
<div|class|first
<div|class|first
>
"The inner one closed.
>
<div|class|first
<hr
>
"After a rule.
>
<i
"Text in no block of its own.
>
<H5
"Capitals
>
<h6|class|inl
"In line
>
>
>
"After everything.
"""

# The same things deeper than a few open elements, so that those too deep
# to keep open and close blocks.
BLOCKS_DEEP_DOCUMENT = (b"".join(b"<div|class|%s\n\"at %d\n" % (b"note" if n % 3 else b"spacer", n) for n in range(8)) +
                        b"<p|class|first\n\"text\n<br\n>\n>\n<hr\n>\n<ol\n<li\n\"item\n>\n>\n" + b">\n\"after\n" * 8)


def css_rules_seeds() -> None:
    clear("css_rules")

    def seed(name: str, choices: int, *parts: bytes) -> None:
        write("css_rules", name, bytes([choices]) + b"\0".join(parts))

    # The table's memory is the two low bits, the cascade's the next two,
    # how the sheets are given the two after.
    seed("two-sheets-and-a-document", 0, RULES_SHEET, RULES_SECOND_SHEET, RULES_DOCUMENT)
    seed("the-sheets-the-other-way", 1 << 4, RULES_SHEET, RULES_SECOND_SHEET, RULES_DOCUMENT)
    seed("a-sheet-given-twice", 2 << 4, RULES_SHEET, RULES_SECOND_SHEET, RULES_DOCUMENT)
    seed("a-sheet-left-out", 3 << 4, RULES_SHEET, RULES_SECOND_SHEET, RULES_DOCUMENT)
    seed("a-documents-own-rules-last", 0, RULES_SHEET, RULES_SECOND_SHEET, RULES_OWN_SHEET, RULES_DOCUMENT)
    seed("a-table-with-room-for-part", 1, RULES_SHEET, RULES_SECOND_SHEET, RULES_OWN_SHEET, RULES_DOCUMENT)
    seed("a-table-with-room-for-little", 2, RULES_SECOND_SHEET, RULES_OWN_SHEET, RULES_DOCUMENT)
    seed("a-table-with-room-for-next-to-nothing", 3, RULES_SECOND_SHEET, RULES_DOCUMENT)
    seed("room-for-a-few-open-elements", 1 << 2, RULES_SHEET, RULES_SECOND_SHEET, RULES_DOCUMENT)
    seed("room-for-one-open-element", 2 << 2, RULES_SHEET, RULES_OWN_SHEET, RULES_DOCUMENT)
    seed("room-for-no-open-element", 3 << 2, RULES_SHEET, RULES_OWN_SHEET, RULES_DOCUMENT)
    seed("more-nodes-than-a-table-takes", 0,
         b"".join(b"a%d p { margin-top: %dpx }\n" % (n, n) for n in range(256)) + b"one-more p, q { float: left }\n",
         b"a7 i, a8 > i { width: 1px }", b"<a254\n<a8\n<p\n>\n<i\n>\n<q\n>\n>\n>\n")
    seed("more-classes-than-are-looked-at", 0, b".c0 { float: left } .c15 { width: 1px } .late { height: 1px }",
         b"<p|class|" + b" ".join(b"c%d" % n for n in range(16)) + b" late\n>\n<p|class|late c0 c0\n")
    seed("what-is-asked-and-is-not-so", 0, RULES_SHEET, RULES_ASKING_SHEET, RULES_DOCUMENT)
    # One of a higher kind outranks any number of a lower: a class eight
    # element names, an id every class a selector can have.
    seed("one-of-a-higher-kind-over-many-of-a-lower", 0,
         b".a { float: left } a a a a a a a p { float: right } #x { width: 1px } "
         b"p.a.a.a.a.a.a.a.a.a.a.a.a[b][b][b][b][b][b][b][b]:link:link:link { width: 2px; height: 2px }",
         b"<a\n" * 7 + b"<p|class|a|id|x|b|\n")
    # A table of 40 bytes, and what does not fit in it: a name, a list of
    # families, what a rule sets, and the later selectors of a rule.
    small = b"<p|class|first\n<q\n>\n>\n"
    seed("no-room-for-a-name", 3, b"p { float: left } a-name-that-is-too-long-for-the-table { float: right }", small)
    seed("no-room-for-an-id", 3, b"p { float: left } #an-id-that-is-too-long-for-the-table { float: right }", small)
    # A table of 300 bytes that these rules fill to the last byte, so that
    # there is no room even to begin the one after them.
    seed("no-room-to-begin-a-rule", 2, b"p{float:left}" * 9 + b"p{width:1px}" * 6 + b"p{height:1px}q{float:left}", small)
    seed("no-room-for-a-list-of-families", 3,
         b"p { font-family: A Family With A Long Name, Another Of Them, serif } q { float: left }", small)
    seed("no-room-for-what-a-rule-sets", 3,
         b"p { margin: 1px 2px 3px 4px; padding: 1px 2px 3px 4px; width: 99999px; height: 99999px } q { float: left }",
         small)
    seed("no-room-for-the-later-selectors-of-a-rule", 3, b"p, q, r, s, t, u, v, w { float: left } q { width: 1px }",
         b"q { float: right }", small)
    # The stack of styles: how many open elements it keeps is the two high
    # bits.
    seed("what-each-element-comes-to", 0, STYLES_SHEET, STYLES_DOCUMENT)
    seed("what-each-element-comes-to-with-three-kept", 1 << 6, STYLES_SHEET, STYLES_DOCUMENT)
    seed("what-each-element-comes-to-with-one-kept", 2 << 6, STYLES_SHEET, STYLES_DOCUMENT)
    seed("what-each-element-comes-to-with-none-kept", 3 << 6, STYLES_SHEET, STYLES_DOCUMENT)
    seed("what-each-element-comes-to-with-no-sheet", 0, STYLES_DOCUMENT)
    seed("elements-deeper-than-are-kept-and-more-languages", 0, STYLES_SHEET, STYLES_DEEP_DOCUMENT)
    seed("elements-deeper-than-three-kept", 1 << 6, STYLES_SHEET, STYLES_DEEP_DOCUMENT)
    for part, kept in enumerate((0, 0, 2 << 6)):
        seed(f"the-words-the-stack-of-styles-reads-{part + 1}-of-3", kept, *styles_words(part))
    # The blocks of a document, found with as many open elements kept as
    # the stack of styles has.
    seed("the-blocks-of-a-document", 0, BLOCKS_SHEET, BLOCKS_DOCUMENT)
    seed("the-blocks-of-a-document-with-three-kept", 1 << 6, BLOCKS_SHEET, BLOCKS_DOCUMENT)
    seed("the-blocks-of-a-document-with-none-kept", 3 << 6, BLOCKS_SHEET, BLOCKS_DOCUMENT)
    seed("the-blocks-of-a-document-with-no-sheet", 0, BLOCKS_DOCUMENT)
    seed("blocks-deeper-than-three-kept", 1 << 6, BLOCKS_SHEET, BLOCKS_DEEP_DOCUMENT)
    seed("blocks-deeper-than-one-kept", 2 << 6, BLOCKS_SHEET, BLOCKS_DEEP_DOCUMENT)
    seed("a-document-and-no-sheet", 0, RULES_DOCUMENT)
    seed("a-sheet-and-no-document", 0, RULES_SHEET, b"")
    seed("nothing", 0)


# ---- svg_tags: byte 0 (how the file is cut up and handed back), then a file ----

DRAWING = b"""<?xml version="1.0" encoding="UTF-8"?>
<!-- A drawing with something of everything the renderer draws. -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 80 48" width="160" height="96">
  <title>Shapes</title>
  <rect width="100%" height="100%" fill="#eee"/>
  <g transform="translate(4 4)" fill="#444">
    <rect x="0" y="0" width="24" height="16" rx="3"/>
    <circle cx="40" cy="8" r="8" fill="black"/>
    <ellipse cx="60" cy="8" rx="10" ry="5" style="fill: rgb(50%, 50%, 50%); fill-opacity: .5"/>
    <g transform="rotate(15 16 30)">
      <polygon points="4,24 28,24 28,44 16,36 4,44" fill="#888" fill-rule="evenodd"/>
      <polyline points="8 28 24 28 16 34" fill="white"/>
    </g>
  </g>
  <path fill-rule="evenodd" d="M44 28h28v16h-28z M50 32v8h16v-8z" opacity="0.8"/>
  <path d="M46 30c2-4 5 4 8 0s4 5 1 6z" transform="translate(1)" fill="#222"/>
  <svg x="48" y="33" width="20" height="6" viewBox="0 0 100 100" preserveAspectRatio="none">
    <circle cx="50" cy="50" r="50" fill="&#35;222"/>
  </svg>
  <g display="none"><rect width="80" height="48"/></g>
</svg>
"""

# What the renderer leaves out, each of which it has to say.
LEFT_OUT = b"""<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="40" height="24">
<defs><linearGradient id="g"><stop offset="0" stop-color="#000"/></linearGradient>
<clipPath id="c"><rect width="9" height="9"/></clipPath><path id="p" d="M0 0h5v5z"/></defs>
<style>rect { fill: #333 } .a { stroke: red }</style>
<use xlink:href="#p" x="3" y="3"/><image href="picture.png" width="9" height="9"/>
<text x="2" y="20">Words <tspan>and more</tspan></text>
<rect width="8" height="8" stroke="#000" stroke-width="2" fill="url(#g) #666" clip-path="url(#c)"/>
<line x1="0" y1="0" x2="40" y2="24" stroke="black"/>
<g opacity=".5" mask="url(#m)" filter="url(#f)"><path d="M10 10h8v8z" marker-end="url(#k)"/></g>
<switch><foreignObject width="9" height="9"><p>Words</p></foreignObject><rect x="20" width="4" height="4"/></switch>
<rect x="30" width="4" height="4"><animate attributeName="x" to="9"/></rect><blink/><rect width="four" height="4"/>
</svg>
"""

DEFINED_BODY = (b"<defs><path id='MJMAIN-31' d='M6 2l3-1v10h3v1H5v-1h3V3L6 4z'/>"
                b"<path id=\"MJMATHI-78\" d='M2 6c1-2 3-2 4 0l1 2 2-2c1-1 2 0 1 1l-3 2 1 3c1 2 2 1 3 0l1 1c-2 2-4 2-5 0l-1-2"
                b"-2 2c-1 1-2 0-1-1l3-2-1-3c-1-1-2-1-3 0z'/>\n"
                b"<g id='frac' fill='#333'><use xlink:href='#MJMAIN-31' x='4'/><rect y='13' width='20' height='1'/>"
                b"<use xlink:href='#MJMATHI-78' x='3' y='13' fill='#777'/></g></defs>\n"
                b"<g transform='translate(1 0)'><use xlink:href='#frac'/></g>\n"
                b"<use href='#frac' transform='translate(22 2) scale(.75)' fill='#999' fill-opacity='.8'/>"
                b"<use href='#MJMATHI-78' x='26' y='12' style='fill:#222'></use>")
DEFINED = (b"<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' width='40' height='24'>"
           + DEFINED_BODY + b"</svg>")

LONG_DATA = b"M2 20" + b"l.3-.1 .05.2" * 120 + b"L38 22 2 22z"


def svg_tags_seeds() -> None:
    clear("svg_tags")

    def seed(name: str, piece: int, taken: int, file: bytes) -> None:
        write("svg_tags", name, bytes([piece | (taken << 3)]) + file)

    seed("a-drawing", 0, 0, DRAWING)
    seed("a-drawing-a-byte-at-a-time", 1, 0, DRAWING)
    seed("a-drawing-with-its-values-handed-back", 5, 1, DRAWING)
    seed("a-long-value-handed-back-three-bytes-at-a-time", 6, 2, b"<path d='" + LONG_DATA + b"' fill='#000'/>")
    seed("entities-and-characters-by-number", 3, 0,
         b"<a b='&lt;&gt;&amp;&quot;&apos;&#65;&#x20AC;&#x1F600;&ns_svg;&#0;&#xD800;'>x &amp; y &; R&D &amp z</a>")
    seed("comments-instructions-and-a-document-type", 2, 0,
         b"<?xml version='1.0'?><!DOCTYPE svg PUBLIC '-//W3C//DTD SVG 1.1//EN' \"svg11.dtd\" [\n"
         b"  <!ENTITY ns_svg \"http://www.w3.org/2000/svg\">\n  <!ENTITY more '>>'>\n]>\n"
         b"<!-- a comment -- with > and <tags/> in it ---><?php echo '?'; ?><svg/>")
    seed("text-marked-as-text", 4, 3, b"<style><![CDATA[ .a > .b { fill: red } ]] ]]]></style><a><![CDATA[x]y]]z]]]]></a>")
    seed("a-name-too-long", 0, 0, b"<" + b"n" * 60 + b" " + b"a" * 60 + b"='1'></" + b"n" * 60 + b">")
    seed("either-quote-and-space-anywhere", 4, 0, b"<a\n  b = \"it's\"\tc\r\n=\n'say \"x > y\"' d e=''  >\n</a  >")
    seed("not-closed", 0, 0, b"<svg><g><rect/>")
    seed("closed-twice", 0, 0, b"<svg></svg></svg>")
    seed("cut-short-inside-a-value", 7, 0, DRAWING[: DRAWING.index(b"rotate(15") + 6])
    seed("not-tags", 0, 0, b"<a>< b/><1/></><!-x-->")
    seed("nothing", 0, 0, b"")


# ---- svg_sheet: byte 0 (how the sheet is cut up), then a style sheet ----

INKSCAPE_SHEET = (b"\n\t.st0{fill:#1D1D1B;}\n\t.st1{fill:none;stroke:#1D1D1B;stroke-width:0.5;stroke-miterlimit:10;}\n"
                  b"\t.st2{font-family:'MyriadPro-Regular';}\n\t.st3{font-size:12px;letter-spacing:1;}\n"
                  b"\tpath , rect{fill-opacity:.5}\n")


def svg_sheet_seeds() -> None:
    clear("svg_sheet")

    def seed(name: str, piece: int, sheet: bytes) -> None:
        write("svg_sheet", name, bytes([piece]) + sheet)

    seed("a-sheet", 7, INKSCAPE_SHEET)
    seed("a-sheet-a-byte-at-a-time", 0, INKSCAPE_SHEET)
    seed("comments-and-marks", 2, b"<!-- /* a { fill: red } */ .a /**/ { fill : /* x */ blue } --> /")
    seed("strings-and-brackets", 3, b".a{font-family:\"x;}\\\"y\";fill:url(a;b);stroke:'}'}.b{fill:url(#g) #000}")
    seed("selectors-it-does-not-read", 4, b"g path{fill:red}#i{fill:red}.a.b,.c{fill:red}*{fill:red}[x]{}.d:hover{}.e{fill:blue}")
    seed("at-rules", 5, b"@import url(x.css);@media print { .a { fill: red } @page {} } .b{fill:blue}@font-face{src:'}'}")
    seed("blocks-and-mistakes-inside-a-rule", 1, b".a{fill;stroke:{x};:red;fill:blue;{q:1}opacity:1;;}}.b{FILL:RED}")
    seed("too-long", 6, b"." + b"n" * 130 + b"{fill:red}.a{fill:" + b"v" * 90 + b";stroke:red;" + b"p" * 40 + b":1}")
    seed("cut-short", 2, b".a{fill:red}.b{stroke:'none")
    seed("nothing", 0, b"")


# ---- svg_values: byte 0 (what the text is read as), eight bytes of a box and a space, then the text ----


def svg_values_seeds() -> None:
    clear("svg_values")
    # A box of 100 by 50 units beginning at (75, -37.5), into a space of 600 by 300 pixels beginning at (6, 9).
    numbers = struct.pack("<bbBBbbBB", 2, -1, 99, 49, 2, 3, 199, 99)

    # A box of another shape than its space, so that where it is put shows: 40 steps by 120 into 200 by 50.
    tall = struct.pack("<bbBBbbBB", -3, 4, 39, 119, -2, 1, 199, 49)

    def seed(name: str, reading: int, text: str, box: bytes = numbers) -> None:
        write("svg_values", name, bytes([reading]) + box + text.encode())

    for index, text in enumerate(["12", "1.5px", "50%", "2em", "1e2pt", "-4.25mm", "3PC", "9999999in"]):
        seed(f"a-length-{index}", 0, text)
    for index, text in enumerate(["5, 3", "1 2 3 4 5 6 7 8", "4 -2,1e2% 3mm", "1,,2", "1 2 3 4 5 6 7 8 9"]):
        seed(f"a-list-of-lengths-{index}", 0, text)
    for index, text in enumerate(["#abc", "#A1b2C3", "#ff000080", "rgb(255, 0, 0)", "rgba(10% 20% 30% / 50%)", "Black",
                                  "transparent", "rgb(300,-5,1e1)"]):
        seed(f"a-colour-{index}", 1, text)
    for index, text in enumerate(["none", "currentColor", "inherit", "url(#gradient) #fff", "url(#gradient)",
                                  "url( '#a' ) none", " grey ", "url(#gradient) bogus", "url(#gradient"]):
        seed(f"a-paint-{index}", 2, text)
    for index, text in enumerate(["0.5", "50%", "2", "-1"]):
        seed(f"an-opacity-{index}", 3, text)
    for index, text in enumerate(["0 0 100 50", "-1.5,2 , 3.25\n4e1", "0 0 10-5"]):
        seed(f"a-view-box-{index}", 4, text)
    for index, text in enumerate(["xMidYMid meet", "xMinYMax slice", "none", "defer xMaxYMin", "xMaxYMid"]):
        seed(f"a-fitting-{index}", 5, text)
        seed(f"a-fitting-of-a-tall-box-{index}", 5, text, tall)
    seed("a-fitting-of-a-tall-box-to-cover-its-space", 5, "xMidYMid slice", tall)
    for index, text in enumerate(["translate(10 20) scale(2) rotate(30 5 5)", "skewX(10),skewY(-5) matrix(1 0 0 1 3 4)",
                                  "scale(2047) translate(9e9)", "", "rotate(bad)"]):
        seed(f"a-transform-{index}", 6, text)
    for index, text in enumerate(["12", "none", "#fff", "xMidYMid", "0 0 1 1", "scale(2)", " "]):
        seed(f"read-every-way-{index}", 7, text)


# ---- svg_path: bytes 0 and 1 (how the data and the picture are cut up, and how it is drawn), then path data ----


def svg_path_seeds() -> None:
    clear("svg_path")

    def seed(name: str, data: bytes, piece: int = 0, commands: int = 0, matrix: int = 0, even_odd: bool = False,
             tolerance: int = 0, rows: int = 0, keep: bool = False) -> None:
        first = piece | (commands << 3) | (matrix << 5) | (0x80 if even_odd else 0)
        second = tolerance | (rows << 2) | (0x20 if keep else 0)
        write("svg_path", name, bytes([first, second]) + data)

    every = (b"M10 4h20v8H10zm4 2l3 3 3-3q2 4 4 0t4 0c1 2 3 2 4 0s3-2 4 0a5 3 30 1 0 3 3A2 2 0 0 1 20 22"
             b"Q8 26 6 18T12 10C14 2 30 2 34 12S20 30 8 20V8H4z")
    seed("every-command", every)
    seed("every-command-a-byte-and-a-command-at-a-time", every, piece=1, commands=1, rows=1, keep=True)
    seed("every-command-through-a-matrix", every, piece=4, commands=2, matrix=2, tolerance=1, rows=4)
    seed("every-command-as-large-as-can-be", every, piece=3, commands=3, matrix=3, rows=3, keep=True)
    seed("numbers-run-together", b"M1.5.5L10-5 .5.5e1,2E-1 1e1 9", piece=2)
    seed("an-arc's-flags-run-together", b"M4 4a6 6 0 0112 12 6 6 0 10-12-12z", rows=2, keep=True)
    seed("a-circle", b"M36 14A12 12 0 0 1 24 26A12 12 0 0 1 12 14A12 12 0 0 1 24 2A12 12 0 0 1 36 14Z", tolerance=3, rows=5)
    seed("a-star-that-crosses-itself", b"M20 2L31 26 4 10H36L9 26Z", even_odd=True, rows=6, keep=True)
    seed("one-outline-inside-another", b"M2 2h36v24h-36zM8 8v12h24v-12zM14 12h12v4h-12z", even_odd=True, matrix=1, rows=1)
    seed("drawn-on-after-it-was-closed", b"M5 5L15 15zl20 0 0 8zZ m3 3 h4", piece=5, commands=1)
    seed("an-arc-of-enormous-radius", b"M2 2a17394593339182234 17394593339182234 0 0 0 30 20L2 22z", rows=7)
    seed("arcs-that-cannot-be-as-asked", b"M5 5a1 1 0 0 0 30 0a0 5 0 0 1 0 10A-9-4 400 1 1 5 5z", tolerance=2, rows=2)
    seed("far-outside-the-picture", b"M-500000-500000L500000 500000 20 9e9C-9e9 0 9e9 0 20 14z", matrix=1, rows=3, keep=True)
    seed("many-small-steps", b"M2 2" + b"l.1.05" * 200 + b"H2z", piece=6, commands=3, rows=4, keep=True)
    seed("a-mistake-part-of-the-way", b"M2 2h20v20 L1 1 L2,,2 h9", piece=2, commands=2)
    seed("not-path-data", b"L1 2 K 3")
    seed("nothing", b"")


# ---- svg: bytes 0 and 1 (how the file and the picture are cut up, and how much room there is for what it
# defines), then a file ----


def svg_seeds() -> None:
    clear("svg")

    def seed(name: str, file: bytes, piece: int = 0, rows: int = 0, size: int = 0, tolerance: int = 0,
             room: int = 0, turn: int = 0, thinnest: int = 0) -> None:
        write("svg", name, bytes([piece | (rows << 3) | (size << 6),
                                  tolerance | (room << 2) | (turn << 5) | (thinnest << 7)]) + file)

    def drawing(body: bytes, attributes: bytes = b"width='40' height='24'") -> bytes:
        return b"<svg xmlns='http://www.w3.org/2000/svg' " + attributes + b">" + body + b"</svg>"

    seed("a-drawing", DRAWING)
    seed("a-drawing-a-byte-at-a-time-in-strips-of-one-row", DRAWING, piece=1, rows=1)
    seed("a-drawing-in-a-tall-picture", DRAWING, piece=4, rows=4, size=1, tolerance=1)
    seed("a-drawing-in-a-picture-of-seven-by-five", DRAWING, piece=5, rows=2, size=2, tolerance=2)
    seed("a-drawing-in-a-large-picture", DRAWING, piece=6, rows=6, size=3, tolerance=3)
    seed("what-is-left-out", LEFT_OUT, piece=3, rows=3)
    seed("long-path-data", drawing(b"<path fill='#333' d='" + LONG_DATA + b"'/>"), piece=5, rows=2)
    seed("long-path-data-with-its-transform-after-it",
         drawing(b"<path d='" + LONG_DATA + b"' transform='translate(2 -3)'/><rect width='4' height='4'/>"), piece=6, rows=5)
    seed("short-path-data-with-its-transform-after-it",
         drawing(b"<path d='M0 0h8v8h-8z' fill='#555' transform='translate(12 6) rotate(20)'/>"), piece=2, rows=1)
    seed("long-points", drawing(b"<polygon points='" + b"1,1 30,2 16,20 " * 60 + b"' fill-rule='evenodd'/>"), piece=4, rows=7)
    seed("groups-nested-too-deep", drawing(b"<g>" * 40 + b"<rect width='9' height='9'/>" + b"</g>" * 40 + b"<circle r='6'/>"), rows=4)
    seed("an-svg-inside-another",
         drawing(b"<svg x='4' y='3' width='30' height='16' viewBox='-5 0 20 10' preserveAspectRatio='xMaxYMin slice'>"
                 b"<rect width='50%' height='100%'/><svg width='5' height='5'><circle cx='50%' cy='50%' r='40%'/></svg></svg>"),
         piece=3, rows=2)
    seed("lengths-in-every-unit",
         drawing(b"<rect x='1mm' y='.1cm' width='.25in' height='9pt' rx='1pc' ry='.2em'/><circle cx='50%' cy='3ex' r='10%'/>",
                 b"width='1in' height='48pt' viewBox='0 0 96 64' preserveAspectRatio='xMinYMax meet'"), piece=4, size=3)
    seed("paint-every-way",
         drawing(b"<g fill='#888' color='#333' fill-opacity='.5' style='fill-rule:evenodd;opacity:.8'>"
                 b"<path d='M2 2h20v20h-20zM6 6h12v12h-12z' fill='currentColor'/>"
                 b"<rect x='20' width='16' height='16' style=' fill : rgba(0,0,0,.5) !important ; fill-opacity: 75% ;'/>"
                 b"<circle cx='30' cy='12' r='8' fill='inherit' visibility='hidden'/></g>"), piece=2, rows=5)
    seed("no-view-box-and-no-size", drawing(b"<rect width='150' height='75'/>", b""), rows=3)
    seed("a-view-box-of-no-size", drawing(b"<rect width='9' height='9'/>", b"viewBox='0 0 0 10'"))
    seed("numbers-too-large", drawing(b"<path d='M0 0h99999999v9e9H-9e9z' transform='scale(2047)'/>"
                                      b"<rect x='-9e9' y='9e9' width='9e9' height='9e9' rx='9e9'/><circle r='9e9'/>",
                                      b"viewBox='-9e9 9e9 9e9 9e9'"), piece=4, rows=1)
    seed("not-a-drawing", b"<html><body><svg><rect width='9' height='9'/></svg></body></html>")
    seed("cut-short-inside-an-outline", DRAWING[: DRAWING.index(b"h28v16") + 3], piece=3, rows=3)
    # Cut short where the renderer is in the middle of something: path data
    # too long to hold, which is in the filler by then, and elements it is
    # passing over.
    seed("cut-short-inside-long-path-data",
         drawing(b"<rect width='4' height='4'/><path fill='#333' d='" + LONG_DATA)[:900], piece=5, rows=2)
    seed("cut-short-inside-what-is-passed-over",
         drawing(b"<rect width='9' height='9'/><defs><g><symbol><path d='M0 0h9v9z'/>")[:-6], piece=2, rows=2)
    seed("tags-that-go-wrong", drawing(b"<rect width='9' height='9'/><<<"), piece=1)
    seed("more-after-the-drawing", drawing(b"<rect width='9' height='9'/>") + b"<rect width='40' height='24'/><svg/>")

    # What a file defines in one place and uses in another, and what an svg inside another cuts off.
    seed("things-defined-and-used", DEFINED)
    seed("things-defined-and-used-a-byte-at-a-time-in-strips-of-one-row", DEFINED, piece=1, rows=1)
    seed("things-defined-and-used-in-a-large-picture", DEFINED, piece=4, rows=5, size=3, tolerance=1)
    # With no memory to keep them in, with too little for any, for the first, and for all but the last.
    for room, name in ((1, "no"), (2, "24-bytes-of"), (3, "96-bytes-of"), (4, "300-bytes-of"), (5, "1024-bytes-of")):
        seed(f"things-defined-and-used-with-{name}-room", DEFINED, piece=3 + room % 3, rows=room, room=room)
    # With 24 bytes: the first thing is 22 of them, and there are not eight left to say where it is.
    seed("room-for-something-and-not-for-where-it-is",
         drawing(b"<defs><path id='first-one'/><rect id='b' width='4' height='4'/></defs><use href='#first-one'/><use href='#b'/>"),
         piece=2, rows=2, room=2)
    seed("long-path-data-defined-and-used-twice",
         drawing(b"<defs><path id='long' d='" + LONG_DATA + b"'/></defs><use href='#long' fill='#555'/>"
                 b"<use href='#long' transform='translate(40 24) rotate(180)' fill-opacity='.5'/>"), piece=6, rows=3)
    seed("something-that-uses-itself",
         drawing(b"<defs><g id='a'><rect width='3' height='3'/><use href='#a' x='4' y='2'/></g></defs><use href='#a'/>"),
         piece=2, rows=4)
    seed("things-that-use-things-that-use-things",
         drawing(b"<defs><rect id='u0' width='2' height='1'/>"
                 + b"".join(b"<g id='u%d'>" % level + b"".join(b"<use href='#u%d' x='%d' y='%d'/>" % (level - 1, 3 * step, step)
                                                               for step in range(6)) + b"</g>" for level in range(1, 6))
                 + b"</defs><use href='#u5'/><use href='#u3' y='12'/>"), piece=5, rows=6)
    seed("a-symbol-and-an-svg-used",
         drawing(b"<defs><symbol id='s' viewBox='0 0 10 10'><circle cx='5' cy='5' r='7'/></symbol>"
                 b"<svg id='v' width='6' height='6' viewBox='0 0 2 2'><rect width='3' height='1'/></svg></defs>"
                 b"<use href='#s' x='2' y='2' width='12' height='8'/><use href='#s' x='20'/>"
                 b"<use href='#v' x='18' y='14'/><use href='#v' y='14' width='16' height='9'/><symbol><rect width='40' height='24'/></symbol>"),
         piece=4, rows=2)
    seed("an-svg-that-cuts-off-what-is-in-it",
         drawing(b"<svg x='4.5' y='3.25' width='20' height='10'><circle cx='10' cy='5' r='11' fill='#444'/>"
                 b"<svg x='12' y='-3' width='20' height='9' overflow='hidden'><rect x='-9' y='-9' width='99' height='99' fill='#999'/></svg>"
                 b"<svg x='2' y='6' width='5' height='9' overflow='visible'><rect x='-1' width='9' height='9'/></svg></svg>"
                 b"<g transform='translate(30 14) rotate(30)'><svg width='6' height='6'><rect x='-3' y='-3' width='12' height='12'/></svg></g>"
                 b"<g transform='translate(40 0) scale(-1 1)'><svg x='1' y='18' width='7' height='5' style='overflow: auto'>"
                 b"<circle r='9'/></svg></g>"), piece=3, rows=3)
    seed("uses-that-find-nothing",
         drawing(b"<use href='#later'/><defs><rect id='later' width='4' height='4'/><rect id='&#97;' width='4' height='4'/>"
                 b"<rect id=\"a'b\" width='4' height='4'/><rect id='' width='4' height='4'/><rect id='aa' id='b' width='4' height='4'/>"
                 b"</defs><rect id='out' width='4' height='4'/><use/><use href=''/><use href='#'/><use href='later'/>"
                 b"<use href='#a'/><use href='#b'/><use href='#out'/><use href='" + b"#" + b"n" * 80 + b"'/>"
                 b"<use href='#later' x='10'><rect width='40' height='24'/><use href='#later' y='10'/></use>"
                 b"<use href='#aa' x='20' display='none'/><use href='#aa' x='20' y='10' opacity='.5' visibility='hidden'/>"),
         piece=2, rows=5)
    seed("cut-short-inside-what-is-defined",
         drawing(b"<rect width='9' height='9'/><defs><g id='a'><path id='b' d='M0 0h9v9z'/><rect id='c' width='5'")[:-6],
         piece=3, rows=2)
    seed("cut-short-as-something-is-used",
         drawing(b"<defs><g id='a'><path d='M0 0h9v9z'/><rect y='10' width='5' height='5'/></g></defs><use href='#a'/>")[:-6],
         piece=1, rows=3)
    seed("tags-that-go-wrong-inside-what-is-defined",
         drawing(b"<defs><rect id='a' width='9' height='9'/><g id='b'><<</g></defs><use href='#a'/>"), piece=4, rows=1)

    # Things the engine is a long time coming to by changing bytes, since each takes several tags that agree
    # with one another.
    dot = b"<defs><rect id='dot' width='3' height='2'/></defs>"
    seed("groups-inside-what-is-used",
         drawing(b"<defs><g id='a'><g fill='#555'><g><rect width='3' height='3'/></g><circle cx='8' cy='4' r='3'/></g>"
                 b"<g><title>Nothing</title></g><a><rect y='6' width='5' height='2'/></a></g></defs>"
                 b"<use href='#a'/><use href='#a' x='14' y='9' fill='#999'/>"), piece=2, rows=3)
    seed("a-use-that-names-a-use",
         drawing(b"<defs><rect id='dot' width='3' height='2'/><use id='right' href='#dot' x='10'/>"
                 b"<use id='down' href='#right' y='8'><desc>Down</desc></use></defs>"
                 b"<use href='#right'/><use href='#down' fill='#666'/><use href='#down' x='20' y='4'/>"), piece=3, rows=2)
    seed("something-used-at-every-depth-of-groups",
         drawing(dot + b"".join(b"<g><use href='#dot' x='%d' y='%d'/>" % (depth, depth % 20) for depth in range(36))
                 + b"</g>" * 36 + b"<use href='#dot' x='36' y='20'/>"), piece=4, rows=4)
    seed("names-inside-names-deeper-than-are-kept",
         drawing(b"<defs>" + b"".join(b"<g id='n%d'>" % depth for depth in range(11)) + b"<rect width='4' height='3'/>"
                 + b"</g>" * 11 + b"</defs><use href='#n0'/><use href='#n7' x='10'/><use href='#n10' x='20'/>"), piece=5, rows=1)
    seed("definitions-inside-what-is-used",
         drawing(b"<defs><g id='a'><defs><rect id='b' x='10' width='2' height='2'/></defs><rect width='4' height='4'/></g>"
                 b"</defs><use href='#a'/><use href='#b' y='6'/><use href='#a' y='12'/><use href='#b' x='20'/>"),
         piece=1, rows=2, room=4)
    seed("inherit-and-none-for-everything",
         drawing(b"<g fill='#777' fill-rule='evenodd' fill-opacity='.5' color='#333' stroke='#000' stroke-width='0' opacity='.9'>"
                 b"<path d='M2 2h20v20h-20zM6 6h12v12h-12z' fill='inherit' fill-rule='inherit' fill-opacity='inherit'"
                 b" color='inherit' visibility='inherit' stroke='inherit' stroke-width='inherit' opacity='inherit'/>"
                 b"<rect x='24' width='9' height='9' style='fill:inherit;fill-rule:inherit;fill-opacity:inherit;color:inherit;"
                 b"visibility:inherit;stroke:inherit;stroke-width:inherit;opacity:inherit;display:inherit;overflow:inherit'/>"
                 b"<g visibility='collapse'><rect y='12' width='9' height='9' visibility='inherit'/></g>"
                 b"<rect x='12' y='12' width='9' height='9' clip-path='none' mask='none' filter='none' marker-start='none'"
                 b" marker-mid='none' marker-end='none' stroke='none' display='inline'/>"
                 b"<rect x='24' y='12' width='9' height='9' style='clip-path:none;mask:none;filter:none;marker:none'/></g>"),
         piece=2, rows=5)
    seed("long-path-data-in-something-that-cannot-be-read",
         drawing(b"<path transform='rotate(' d='" + LONG_DATA + b"'/><path d='" + LONG_DATA + b"' transform='scale('/>"
                 b"<rect width='4' height='4'/>"), piece=6, rows=2)
    seed("an-svg-in-a-group-that-is-slanted",
         drawing(b"<g transform='skewX(30)'><svg x='4' y='4' width='8' height='8'><rect x='-4' y='-4' width='16' height='16'/>"
                 b"</svg></g><g transform='translate(24 0) skewY(20)'><svg width='8' height='8'><circle r='9'/></svg></g>"),
         piece=3, rows=3)
    # A style sheet, and gradients.
    gradients = (b"<defs><linearGradient id='base' x2='0' y2='1'><stop offset='0' stop-color='#000'/>"
                 b"<stop offset='.4' style='stop-color:#888;stop-opacity:.5'/><stop offset='1' stop-color='#fff'/>"
                 b"</linearGradient><linearGradient id='g' xlink:href='#base' x2='1' spreadMethod='reflect'"
                 b" gradientTransform='rotate(20 .5 .5)'/></defs>"
                 b"<linearGradient id='u' gradientUnits='userSpaceOnUse' x1='5' y1='2' x2='30%' y2='20'>"
                 b"<stop offset='0'/><stop offset='1' stop-color='#ccc' stop-opacity='0'/></linearGradient>")
    seed("a-style-sheet", drawing(b"<style><![CDATA[.st0{fill:#444}rect{fill:#999;fill-opacity:.5}.st1,.st2{display:none}]]>"
                                  b"</style><rect width='9' height='9'/><rect x='10' width='9' height='9' class='st0'/>"
                                  b"<g class='st1'><circle cx='30' cy='5' r='5'/></g>"
                                  b"<path class='x st0' d='M2 12h20v10z' style='fill:#000'/>"), piece=3, rows=3)
    seed("a-style-sheet-a-byte-at-a-time", drawing(b"<style>.a{fill:#888}path{fill-opacity:.5}</style>"
                                                    b"<path class='a' d='M2 2h30v20z'/>"), piece=1, rows=1)
    seed("a-style-sheet-after-what-it-styles", drawing(b"<rect class='a' width='9' height='9'/><style>.a{fill:#888}</style>"
                                                        b"<rect class='a' x='10' width='9' height='9'/>"), piece=2)
    seed("a-style-sheet-with-too-little-room", drawing(b"<style>.a{fill:#888}.b{fill:#444}.c{fill:url(#g)}</style>"
                                                        b"<rect class='a' width='9' height='9'/><rect class='b c' x='10' width='9' height='9'/>"),
         piece=4, rows=2, room=2)
    seed("gradients", drawing(gradients + b"<rect x='2' y='2' width='30' height='10' fill='url(#g)'/>"
                              b"<circle cx='20' cy='16' r='7' fill='url(#u)' fill-opacity='.8'/>"
                              b"<path d='M1 23c10-9 20 9 38 0' fill='url(#base)'/>"), piece=5, rows=3)
    seed("gradients-a-byte-at-a-time-in-strips-of-one-row",
         drawing(gradients + b"<g fill='url(#g)'><rect width='40' height='24'/></g>"), piece=1, rows=1)
    seed("gradients-in-a-large-picture", drawing(gradients + b"<style>.g{fill:url(#u)}</style>"
                                                 b"<g transform='skewX(20) scale(1.5 .7)'><rect class='g' width='30' height='30'/></g>"),
         piece=4, rows=5, size=3)
    seed("gradients-that-come-to-nothing",
         drawing(b"<rect width='9' height='9' fill='url(#later)'/><defs><linearGradient id='later'/>"
                 b"<linearGradient id='flat' x2='0'><stop stop-color='#888'/></linearGradient>"
                 b"<linearGradient id='squashed' gradientTransform='scale(0)'><stop/><stop offset='1'/></linearGradient>"
                 b"<radialGradient id='round'><stop/></radialGradient></defs>"
                 b"<rect x='10' width='9' height='9' fill='url(#flat)'/><rect x='20' width='9' height='9' fill='url(#squashed)'/>"
                 b"<rect x='30' width='9' height='9' fill='url(#round) #444'/><rect y='10' width='9' height='0' fill='url(#flat)'/>"
                 b"<path d='M0 20h40' fill='url(#flat)'/>"), piece=3, rows=2)
    seed("sheets-and-gradients-at-their-limits",
         drawing(b"<style>.a{fill:#888" + b";fill-opacity:0.99999999" * 25 + b"}" + b",".join(b".c%d" % n for n in range(40))
                 + b"{fill:#444}</style><linearGradient id='many' spreadMethod='pad' x2='9e9' gradientTransform='scale(2047)'>"
                 + b"".join(b"<stop offset='%d%%' stop-color='currentColor' stop-opacity='lots'/>" % (n * 10) for n in range(10))
                 + b"</linearGradient><rect class='a c3' width='30' height='20' fill='url(#many)'/>"), piece=3, rows=2)
    # Strokes: every corner and end, dashes, what is stroked and filled, and long data stroked as it arrives.
    strokes = (b"<g fill='none' stroke='#000' stroke-width='2'>"
               b"<path d='M2 2l8 6 8-6' stroke-linejoin='miter' stroke-miterlimit='2'/>"
               b"<path d='M2 10l8 6 8-6' stroke-linejoin='round' stroke-linecap='round'/>"
               b"<path d='M2 18l8 4 8-4' stroke-linejoin='bevel' stroke-linecap='square'/>"
               b"<polyline points='22,2 38,2 22,6 38,6' style='stroke-dasharray:3 1 0 1;stroke-dashoffset:-2'/>"
               b"<line x1='22' y1='10' x2='38' y2='22' stroke-dasharray='1' stroke-linecap='round' stroke-opacity='.5'/>"
               b"<circle cx='30' cy='16' r='4' stroke='#888' stroke-width='10%' fill='#ccc'/>"
               b"<path d='M30 9Z M33 9h0' stroke-linecap='round'/></g>")
    seed("strokes", drawing(strokes), piece=3, rows=3)
    seed("strokes-a-byte-at-a-time-in-strips-of-one-row", drawing(strokes), piece=1, rows=1)
    seed("strokes-stretched-and-turned",
         drawing(b"<g transform='scale(4 1) rotate(10)' stroke='#000' stroke-dasharray='2 1'>" + strokes + b"</g>"),
         piece=4, rows=2, turn=1)
    seed("long-path-data-stroked",
         drawing(b"<path fill='none' stroke='#000' stroke-width='3' stroke-linejoin='round' d='" + LONG_DATA + b"'/>"
                 b"<path stroke='#444' d='" + LONG_DATA + b"'/>"
                 b"<path fill='none' stroke='#000' d='" + LONG_DATA + b"' stroke-width='4'/>"), piece=5, rows=2)
    seed("hairlines-a-pixel-wide",
         drawing(b"<g stroke='#000' fill='none' stroke-width='.05'><path d='M2 2h36l-30 18'/>"
                 b"<g transform='scale(6 .5)'><line x1='1' y1='40' x2='5' y2='44'/></g>"
                 b"<circle cx='30' cy='14' r='6' stroke-width='3'/></g>"), piece=3, rows=2, thinnest=1)
    seed("hairlines-a-pixel-wide-turned", drawing(strokes), piece=2, rows=3, turn=2, thinnest=1)
    seed("dashes-more-than-an-outline-may-have",
         drawing(b"<path d='M0 12c10-20 30 20 40 0' fill='none' stroke='#000' stroke-dasharray='.001 .002'/>"),
         piece=4, rows=3)
    seed("a-gradient-in-an-svg-that-cuts-off-what-is-in-it",
         drawing(b"<linearGradient id='g' gradientUnits='userSpaceOnUse' x2='30'><stop/><stop offset='1' stop-color='#ccc'/>"
                 b"</linearGradient><svg x='6.5' y='3.25' width='20' height='10'>"
                 b"<rect x='-9' y='-9' width='99' height='99' fill=\"url( '#g' )\"/></svg>"), piece=3, rows=2)
    # Turned into the strips' rows, as a page is onto a panel that is turned in its device: each of the turns.
    seed("a-drawing-turned-a-quarter-clockwise", DRAWING, piece=3, rows=2, turn=1)
    seed("things-defined-and-used-turned-a-quarter-anticlockwise", DEFINED, piece=4, rows=3, size=1, turn=2)
    seed("an-svg-that-cuts-off-what-is-in-it-turned-a-half",
         drawing(b"<svg x='4.5' y='3.25' width='20' height='10'><circle cx='10' cy='5' r='11' fill='#444'/></svg>"
                 b"<linearGradient id='g' x2='0' y2='1'><stop/><stop offset='1' stop-color='#ccc'/></linearGradient>"
                 b"<rect x='22' y='2' width='15' height='20' fill='url(#g)'/>"), piece=2, rows=1, turn=3)
    seed("brackets-in-style-deeper-than-are-counted",
         drawing(b"<rect width='9' height='9' style='-x:" + b"(" * 256 + b";fill:#fff" + b")" * 256 + b";fill-opacity:0.5'/>"),
         piece=4)
    seed("nothing", b"")


# ---- paragraph: bytes 0 and 1 (the lines, the memory, the measure), then text ----


def paragraph_seeds() -> None:
    clear("paragraph")
    text = TEXT.encode()

    def seed(
        name: str,
        width: int,
        written: bytes,
        *,
        first: bool = False,
        at_a_time: int = 0,
        least: bool = False,
        hyphenator: bool = True,
        cost: int = 2,
        face: int = 1,
    ) -> None:
        """`width` is which of the target's eight widths the lines have; `at_a_time`, which of its four
        ways of offering text; `cost`, which of its four prices for a hyphen; `face`, which measure."""
        choices = width | (0x08 if first else 0) | (at_a_time << 4) | (0x40 if least else 0)
        choices |= 0x80 if hyphenator else 0
        write("paragraph", name, bytes([choices, cost | (face << 2)]) + written)

    seed("a-paragraph", 5, text)
    seed("narrow-lines", 3, text, cost=0)
    seed("no-hyphenator", 4, text, hyphenator=False)
    seed("first-lines-narrower", 4, text, first=True)
    seed("a-byte-at-a-time", 4, text[:300], at_a_time=1)
    seed("seven-bytes-at-a-time-and-little-memory", 3, text, at_a_time=2, least=True)
    seed("every-character-as-wide", 3, text[:300], face=0)
    # Lines that hold a letter or two, and lines that hold none.
    seed("lines-narrower-than-a-word", 2, text[:300])
    seed("lines-narrower-than-a-letter", 1, text[:200], least=True)
    seed("lines-of-no-width-to-speak-of", 0, text[:120], face=0, first=True)
    # A byte with no width, and then in another style a character wider than
    # the line: the two are on one line, which is too wide for the one.
    seed("a-byte-of-no-width-beside-a-character-too-wide", 0, b"\xb3\x06\xc3", face=0, first=True, cost=3)

    # A byte from 1 to 7 puts what follows in that style, and a byte of 0
    # asks for the line to end: here in the middle of words as well as
    # between them.
    seed(
        "styles",
        4,
        b"The \x01lamp\x02 was l\x03i\x01t bef\x02ore any\x03one had \x04asked\x01 for it, \x05and \x06the "
        b"\x07harbour\x01 went on with its evening.",
        face=2,
    )
    seed("a-word-in-many-styles", 3, b"".join(bytes([1 + n % 7]) + b"ab" for n in range(60)) + b" and on", face=2)
    seed("line-ends", 4, b"The lamp was lit\nbefore anyone  \n  had asked\n\nfor it,\x00and the har\x00bour went\n", cost=1)

    # Places a line may end that are written in the text: hyphens and
    # dashes (and where one is not such a place), soft hyphens, zero width
    # spaces.
    seed(
        "hyphens-and-dashes",
        3,
        "a well-known lamp—lit by ten-twelve– not -ish nor 10-12, a--b, non‑breaking, self-\n".encode(),
    )
    seed(
        "soft-hyphens-and-zero-width-spaces",
        3,
        "the lamp­lighter of the har­bour​side, ­ and ​ un­happy­ one​­two".encode(),
    )
    seed("words-the-hyphenator-breaks", 3, b"hyphenation unhappy lamp cabbage abba Unhappy HYPHENATION abab " * 3)

    # Runs of bytes with no space in them: longer than is taken as a word,
    # of characters of several bytes, and with marks set over letters.
    seed("a-word-too-long", 4, b"see " + b"harbour" * 100 + b" and on")
    seed("a-long-word-of-wide-characters", 3, "é\U00010428中".encode() * 90, at_a_time=3)
    seed("marks-over-letters", 2, "éàố ".encode() * 20)
    seed(
        "not-utf-8",
        3,
        b"lamp\xc0\xaflight ev\x80ening harb\xed\xa0\x80our \xff\xfe \xc2 \xe2\x80 half\xe2\x80\xc2\xad a\xcc b\xcd\xff",
    )

    # Spaces: in runs, of several kinds, and at either end.
    seed("spaces", 3, b"   The  lamp\t was \r\n lit   before \t\t anyone" + b" " * 300 + b"had asked.   ")
    # More spaces in a row than one call may look at bytes.
    seed("a-long-run-of-spaces", 4, b"lamp" + b" " * 2500 + b"lit" + b"\t" * 900)
    seed("nothing-has-any-width", 3, text, face=3, least=True)
    seed("no-words", 4, b"  \t \n \x00 \xc2\xad \xe2\x80\x8b ")
    write("paragraph", "nothing", b"")


# ---- png: bytes 0 and 1 (how the file is given and how its data is cut up), then a PNG file ----

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def png_chunk(name: bytes, body: bytes) -> bytes:
    return struct.pack(">I", len(body)) + name + body + struct.pack(">I", zlib.crc32(name + body))


def png_rows(width: int, height: int, colour: int, depth: int, filters: list[int], seed: int) -> bytes:
    """Rows of made-up samples, each with a filter byte in front of it. The
    samples are not filtered: with the data given plain, that makes no
    difference to what the reader is put through."""
    rng = random.Random(seed)
    samples = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    stride = (width * samples * depth + 7) // 8
    return b"".join(bytes([filters[y % len(filters)]]) + rng.randbytes(stride) for y in range(height))


def png_seeds() -> None:
    clear("png")

    def seed(name: str, file: bytes, *, piece: int = 6, room: int = 0, as_is: bool = False, plain: bool = True,
             data_piece: int = 7, empty: int = 0, level: int = 3) -> None:
        first = piece | (room << 3) | (int(as_is) << 5) | (int(plain) << 6)
        second = data_piece | (empty << 3) | (level << 5)
        write("png", name, bytes([first, second]) + file)

    def header(width: int, height: int, depth: int, colour: int, interlace: int = 0) -> bytes:
        return png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, colour, 0, 0, interlace))

    end = png_chunk(b"IEND", b"")
    every_filter = [0, 1, 2, 3, 4]
    kinds = [(0, 1), (0, 2), (0, 4), (0, 8), (0, 16), (2, 8), (2, 16), (3, 1), (3, 2), (3, 4), (3, 8), (4, 8), (4, 16),
             (6, 8), (6, 16)]
    for number, (colour, depth) in enumerate(kinds):
        extra = b""
        if colour == 3:
            entries = 1 << depth
            extra = png_chunk(b"PLTE", bytes((i * 37) & 255 for i in range(3 * entries)))
            # Alphas for the first half of the entries, from none to nearly solid.
            extra += png_chunk(b"tRNS", bytes(i * 255 // entries for i in range(max(1, entries // 2))))
        elif colour == 0:
            extra = png_chunk(b"tRNS", struct.pack(">H", 1))
        elif colour == 2:
            extra = png_chunk(b"tRNS", struct.pack(">3H", 1, 2, 3))
        data = png_chunk(b"IDAT", png_rows(13, 7, colour, depth, every_filter, number))
        seed(f"kind-{colour}-depth-{depth}", PNG_SIGNATURE + header(13, 7, depth, colour) + extra + data + end)

    # A file as an encoder writes it, taken as it is: compressed, with
    # chunks to pass over before the data and after.
    rows = png_rows(40, 30, 2, 8, [1, 4, 2], 100)
    stream = zlib.compress(rows, 9)
    sound = (PNG_SIGNATURE + header(40, 30, 8, 2) + png_chunk(b"gAMA", struct.pack(">I", 45455)) +
             png_chunk(b"tEXt", b"Comment\x00the harbour") + png_chunk(b"IDAT", stream[:200]) +
             png_chunk(b"IDAT", stream[200:]) + png_chunk(b"tIME", bytes(7)) + end)
    seed("a-file-as-it-is", sound, as_is=True, plain=False)
    seed("a-file-as-it-is-a-byte-at-a-time", sound, piece=0, as_is=True, plain=False)
    seed("a-file-as-it-is-cut-short", sound[: len(sound) // 2], as_is=True, plain=False)
    seed("a-file-with-a-wrong-checksum", sound[:60] + bytes([sound[60] ^ 1]) + sound[61:], as_is=True, plain=False)
    # A chunk of data after the first that says it is longer than a chunk may be.
    cut = sound.index(b"IDAT", sound.index(b"IDAT") + 4) - 4
    seed("a-later-chunk-of-data-too-long", sound[:cut] + b"\x80\x00\x00\x00IDAT" + sound[cut + 8:], as_is=True,
         plain=False)

    # The data in chunks of a byte, with empty ones between: as many as are
    # passed over, and one more.
    long_rows = png_rows(300, 4, 2, 8, [0], 101)
    seed("data-a-byte-to-a-chunk", PNG_SIGNATURE + header(300, 4, 8, 2) + png_chunk(b"IDAT", long_rows) + end,
         data_piece=0, empty=2, level=0)
    seed("too-many-empty-chunks", PNG_SIGNATURE + header(300, 4, 8, 2) + png_chunk(b"IDAT", long_rows) + end,
         data_piece=2, empty=3)
    # Room: a byte less than the picture needs, and 64 bytes.
    seed("a-byte-less-room", PNG_SIGNATURE + header(20, 3, 8, 6) + png_chunk(b"IDAT", png_rows(20, 3, 6, 8, [4], 102)),
         room=1)
    seed("little-room", PNG_SIGNATURE + header(5, 3, 1, 0) + png_chunk(b"IDAT", png_rows(5, 3, 0, 1, [1], 103)), room=2)
    # What is refused.
    seed("interlaced", PNG_SIGNATURE + header(8, 8, 8, 0, 1) + png_chunk(b"IDAT", png_rows(8, 8, 0, 8, [0], 104)) + end)
    seed("a-filter-there-is-not", PNG_SIGNATURE + header(8, 4, 8, 0) + png_chunk(b"IDAT", png_rows(8, 4, 0, 8, [0, 5], 105)))
    seed("a-palette-picture-without-a-palette",
         PNG_SIGNATURE + header(8, 4, 8, 3) + png_chunk(b"IDAT", png_rows(8, 4, 3, 8, [0], 106)))
    seed("a-chunk-it-must-know-and-does-not",
         PNG_SIGNATURE + header(8, 4, 8, 0) + png_chunk(b"ABCD", b"x") + png_chunk(b"IDAT", png_rows(8, 4, 0, 8, [0], 107)))
    seed("a-palette-of-the-wrong-length",
         PNG_SIGNATURE + header(8, 4, 8, 3) + png_chunk(b"PLTE", bytes(5)) + png_chunk(b"IDAT", png_rows(8, 4, 3, 8, [0], 108)))
    seed("two-headers", PNG_SIGNATURE + header(8, 4, 8, 0) + header(8, 4, 8, 0))
    seed("a-header-of-a-kind-there-is-not", PNG_SIGNATURE + header(8, 4, 3, 2))
    seed("a-wide-picture", PNG_SIGNATURE + header(0x7FFFFFFF, 1, 1, 0) + png_chunk(b"IDAT", b"\x00" * 64))
    seed("not-a-png", b"GIF89a" + bytes(20), as_is=True, plain=False)
    seed("nothing", b"")


# ---- jpeg: byte 0 (how many bytes the source gives at a call), byte 1 (the reduction asked, whether only the
# header is read, whether the room for rows is a byte short), then a JPEG file ----


def jpeg_seeds() -> None:
    """Pictures made by the encoder of tests/e2e/jpeg_tool.py, which the end-to-end test also makes its pictures
    with: small ones of every kind the reader reads, ones of the kinds it does not, and spoiled ones."""
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "e2e"))
    sys.dont_write_bytecode = True  # Or Python leaves a compiled copy of jpeg_tool beside it.
    import jpeg_tool  # noqa: E402  (found through the path set just above)

    clear("jpeg")
    reductions = {1: 0, 2: 1, 4: 2, 8: 3}

    def seed(name: str, file: bytes, *, piece: int = 0, reduction: int = 1, header: bool = False,
             short: bool = False) -> None:
        choices = reductions[reduction] | (int(header) << 2) | (int(short) << 3)
        write("jpeg", name, bytes([piece, choices]) + file)

    def plane(width: int, height: int, number: int) -> list[bytes]:
        """A gradient with a dark bar across it: some blocks of one gray, some not."""
        rows = []
        for y in range(height):
            row = bytearray((x * 7 + y * 3 + number * 40) & 255 for x in range(width))
            if height // 3 <= y < height // 2:
                row[width // 4:width // 2] = bytes(width // 2 - width // 4)
            rows.append(bytes(row))
        return rows

    def picture(width: int, height: int, marker: int, sampling: tuple[int, int] | None, *, restart: int = 0,
                made: bool = False, wide: bool = False, low: int = 0, together: bool = True, colour_first: bool = False,
                fill: int = 0, quality: int = 75, end: bool = True) -> bytes:
        """A picture: gray if `sampling` is None, and otherwise YCbCr with Y sampled so; in one scan with every
        component (`together`) or one for each, the colour's first if `colour_first`."""
        rng = random.Random(width * 1000 + height)
        scale = 5000 // quality if quality < 50 else 200 - 2 * quality
        most = 2000 if wide else 255
        steps = {which: tuple(min(most, max(1, (base * scale + 50) // 100)) for base in jpeg_tool.STANDARD_STEPS[which])
                 for which in (0, 1)}
        if sampling is None:
            components = [(1, 1, 1, 0)]
            planes = [plane(width, height, 0)]
        else:
            across, down = sampling
            components = [(1, across, down, 0), (2, 1, 1, 1), (3, 1, 1, 1)]
            planes = [plane(width, height, 0)] + [plane((width + across - 1) // across, (height + down - 1) // down,
                                                        number) for number in (1, 2)]
        every = list(range(len(components)))
        if marker == 0xC2:
            scans = [(every, 0, 0, 0, low)] if together else [([index], 0, 0, 0, low) for index in every]
            if colour_first and not together:
                scans = [scans[1], scans[0]] + scans[2:]
            scans += [([0], 1, 5, 0, 0)] + [([index], 1, 63, 0, 0) for index in every[1:]] + [([0], 6, 63, 0, 0)]
            if low:
                scans.append((every, 0, 0, 1, 0))
        elif together:
            scans = [(every, 0, 63, 0, 0)]
        else:
            order = every[1:] + every[:1] if colour_first else every
            scans = [([index], 0, 63, 0, 0) for index in order]
        plan = jpeg_tool.Plan(width, height, marker, components, planes, {k: steps[k] for k in range(len(components[:2]))},
                              wide_steps=wide, made_tables=made, restart=restart, scans=scans,
                              fill_before_restarts=fill, end=end)
        return jpeg_tool.encode(plan, rng, extras=False)

    def segment(marker: int, body: bytes) -> bytes:
        return bytes((0xFF, marker)) + struct.pack(">H", len(body) + 2) + body

    def ahead(file: bytes, more: bytes) -> bytes:
        """`more` just after the start of the picture."""
        return file[:2] + more + file[2:]

    # Every kind that is read, a few blocks of each.
    seed("gray", picture(24, 17, 0xC0, None))
    for across, down in ((1, 1), (2, 1), (2, 2), (1, 2), (4, 1)):
        seed(f"ycbcr-{across}x{down}", picture(35, 18, 0xC0, (across, down)), reduction=2)
    seed("extended-with-wide-steps", picture(20, 20, 0xC1, (2, 2), wide=True, quality=10), reduction=4)
    seed("tables-made-for-it", picture(30, 12, 0xC0, (2, 1), made=True))
    seed("a-scan-for-each-the-colour-first", picture(33, 16, 0xC0, (2, 2), together=False, colour_first=True))
    seed("restarts", picture(40, 24, 0xC0, (2, 2), restart=2), piece=7)
    seed("restarts-with-fill-bytes", picture(40, 8, 0xC0, None, restart=1, fill=16), reduction=8)
    seed("progressive", picture(40, 24, 0xC2, (2, 2)), reduction=8)
    seed("progressive-averages-a-bit-short", picture(40, 24, 0xC2, (2, 1), low=1, made=True), reduction=1)
    seed("progressive-the-colour-first", picture(32, 16, 0xC2, (1, 1), together=False, colour_first=True),
         reduction=8)
    seed("progressive-a-byte-at-a-time", picture(16, 16, 0xC2, None, restart=3), piece=1)
    seed("no-end", picture(17, 9, 0xC0, None, end=False), reduction=8)
    sound = picture(48, 32, 0xC0, (2, 2))
    seed("header-only", sound, header=True)
    seed("a-byte-short-of-room", sound, short=True)
    # What lies around the segments: segments to pass over, bytes that are not
    # markers, and fill bytes.
    seed("segments-to-pass-over", ahead(sound, segment(0xE0, b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00") +
                                        segment(0xFE, b"the harbour") + segment(0xE1, b"Exif\x00\x00" + bytes(30)) +
                                        segment(0xEE, b"Adobe\x00\x64\x00\x00\x00\x00\x01") + b"\xff\x01"))
    seed("bytes-that-are-not-markers", ahead(sound, b"\x00\x7f\xff\x00\x01" + b"\xff" * 20))
    # The kinds that are not read.
    places = jpeg_tool.segments_of(sound)
    at, marker, length = next(place for place in places if place[1] == 0xC0)
    frame = bytearray(sound[at + 4:at + 2 + length])
    before, after = sound[:at], sound[at + 2 + length:]

    def frame_seed(name: str, marker: int, body: bytes, more: bytes = b"") -> None:
        seed(name, before[:2] + more + before[2:] + segment(marker, body) + after)

    frame_seed("adobe-rgb", 0xC0, bytes(frame), segment(0xEE, b"Adobe\x00\x64\x00\x00\x00\x00\x00"))
    named = bytearray(frame)
    named[6], named[9], named[12] = ord("R"), ord("G"), ord("B")
    frame_seed("named-rgb", 0xC0, bytes(named))
    cmyk = bytearray(frame)
    cmyk[5] = 4
    frame_seed("cmyk", 0xC0, bytes(cmyk + bytes((4, 0x11, 1))))
    twelve = bytearray(frame)
    twelve[0] = 12
    frame_seed("twelve-bits", 0xC1, bytes(twelve))
    frame_seed("arithmetic", 0xC9, bytes(frame))
    frame_seed("lossless", 0xC3, bytes(frame))
    later = bytearray(frame)
    later[1:3] = b"\x00\x00"
    frame_seed("height-given-later", 0xC0, bytes(later))
    less = bytearray(frame)
    less[7], less[10] = 0x11, 0x22
    frame_seed("gray-sampled-less", 0xC0, bytes(less))
    # Spoiled.
    data_at = next(place[0] + 2 + place[2] for place in places if place[1] == 0xDA)
    seed("cut-short-in-its-data", sound[:data_at + (len(sound) - data_at) // 2])
    seed("cut-short-in-its-header", sound[:at + 6])
    restarted = picture(40, 24, 0xC0, (2, 2), restart=2)
    first = restarted.index(b"\xff\xd0")
    seed("a-wrong-restart-marker", restarted[:first + 1] + b"\xd3" + restarted[first + 2:])
    seed("a-restart-marker-left-out", restarted[:first] + restarted[first + 2:])
    huffman = next(place for place in places if place[1] == 0xC4)
    seed("a-table-left-out", sound[:huffman[0]] + sound[huffman[0] + 2 + huffman[2]:])
    seed("a-code-of-all-ones", ahead(sound, segment(0xC4, b"\x00" + bytes([2]) + bytes(15) + b"\x00\x01")))
    # The first byte from the middle of the data on that, changed, leaves
    # codes that no table has after some rows.
    for offset in range((data_at + len(sound)) // 2, len(sound)):
        bad_data = bytearray(sound)
        bad_data[offset] ^= 0xFF
        reading = jpeg_tool.read_jpeg(bytes(bad_data))
        if reading.status == "bad-data" and reading.rows:
            break
    seed("a-byte-of-data-changed", bytes(bad_data))
    seed("two-frames", before + segment(0xC0, bytes(frame)) + segment(0xC0, bytes(frame)) + after)
    seed("a-scan-before-its-frame", ahead(sound, segment(0xDA, b"\x01\x01\x00\x00\x3f\x00")))
    seed("not-a-jpeg", b"GIF89a" + bytes(20))
    seed("nothing", b"")


# ---- scale: the old and new sizes, two bytes of choices, then the old picture's pixels ----


def scale_seeds() -> None:
    clear("scale")

    def seed(name: str, old: tuple[int, int], new: tuple[int, int], pixels: bytes, *, out_of_turn: int | None = None,
             one_gray: bool = False) -> None:
        choices = (1 if out_of_turn is not None else 0) | (2 if one_gray else 0)
        header = struct.pack("<4H", *old, *new) + bytes([choices, out_of_turn or 0])
        write("scale", name, header + pixels)

    rng = random.Random(7)
    picture = bytes(rng.randrange(256) for _ in range(900))
    # Sizes are written as the target reads them: 2 to 299 come out one
    # more than given, 0 and 1 as they are, 65534 as the largest side and
    # 65535 one past it.
    seed("the-same-size", (29, 30), (29, 30), picture)
    seed("halved", (59, 39), (29, 19), picture)
    seed("made-larger", (9, 7), (63, 41), picture)
    seed("narrower-and-taller", (89, 9), (12, 79), picture)
    seed("three-into-two", (2, 2), (1, 1), bytes([0, 255, 30, 90, 200, 10, 0, 0, 0]))
    seed("a-page", (299, 199), (159, 239), picture)
    seed("one-gray", (40, 30), (17, 61), bytes([173]), one_gray=True)
    seed("black-and-white", (99, 59), (29, 17), bytes([0, 255, 255, 0, 255]))
    seed("the-widest-made-one-pixel", (65534, 1), (1, 1), bytes([255]))
    seed("the-tallest-made-one-pixel", (1, 65534), (1, 1), bytes([255]))
    seed("one-pixel-made-the-widest", (1, 1), (65534, 1), bytes([200]))
    seed("a-side-of-nothing", (0, 9), (9, 9), picture)
    seed("a-side-too-long", (9, 9), (65535, 9), picture)
    seed("a-row-out-of-turn-made-larger", (9, 7), (63, 41), picture, out_of_turn=3)
    seed("a-row-out-of-turn-made-smaller", (59, 39), (29, 19), picture, out_of_turn=1)
    seed("no-pixels", (19, 19), (7, 7), b"")


# ---- picture_cache: byte 0 (what is mended, how many rows a call), then a
# file of the cache ----


def picture_cache_file(width: int, height: int, levels: int, turn: int, rows: bytes) -> bytes:
    """A file as the cache writes it: the header's sector, then the rows."""
    header = bytearray(512)
    header[0:4] = b"BNPC"
    struct.pack_into("<HHHHB", header, 4, 3, levels, width, height, turn)
    struct.pack_into("<QIIII", header, 16, 0x0123456789ABCDEF, 4096, 77, zlib.crc32(rows), 5)
    struct.pack_into("<I", header, 40, zlib.crc32(bytes(header[:40])))
    return bytes(header) + rows


def picture_cache_seeds() -> None:
    clear("picture_cache")
    random.seed(64)
    for name, width, height, levels in (("two-levels", 20, 10, 2), ("four-levels", 13, 7, 4),
                                        ("sixteen-levels", 9, 4, 16), ("gray", 5, 3, 256), ("one-row", 300, 1, 2)):
        bits = 1 if levels <= 2 else 2 if levels <= 4 else 4 if levels <= 16 else 8
        row_bytes = (width * bits + 7) // 8
        rows = bytes(random.randrange(256) for _ in range(row_bytes * height))
        file = picture_cache_file(width, height, levels, 1, rows)
        write("picture_cache", name, bytes([0b00011]) + file)
        write("picture_cache", name + "-unmended", bytes([0b10100]) + file)
    rows = bytes(range(60))
    write("picture_cache", "cut-short", bytes([0b00111]) + picture_cache_file(20, 10, 2, 0, rows)[:512 + 26])
    write("picture_cache", "cut-short-in-the-header", bytes([0b00101]) + picture_cache_file(20, 10, 2, 0, rows)[:90])
    # Rows that fill the cache's memory several times over: 3,000 bytes of
    # them, gathered a sector at a time and three at a time.
    rows = bytes(random.randrange(256) for _ in range(50 * 60))
    file = picture_cache_file(200, 60, 4, 0, rows)
    write("picture_cache", "many-sectors-one-at-a-time", bytes([0b0001111]) + file)
    write("picture_cache", "many-sectors-three-at-a-time", bytes([0b1011011]) + file)
    write("picture_cache", "not-a-picture", bytes([0b00001]) + b"<svg/>" * 20)
    # A file of an odd length, so that the picture the target keeps beside
    # it has rows of 13 bytes, and of a length that gives it 348 of them:
    # given a row at a time to a cache with one sector of memory, the rows
    # come to end one byte short of a sector's end.
    write("picture_cache", "rows-that-end-a-byte-short-of-a-sector",
          bytes([0b0000011]) + picture_cache_file(20, 23, 2, 0, bytes((7 * n) % 256 for n in range(69))))


# ---- book_bundle: byte 0 (how the book is made), then the package file,
# the NCX and the nav of a book whose other files the target has ----

BUNDLE_PACKAGE = b"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uuid">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:title>A Book to Bundle</dc:title><dc:creator>Ren\xc3\xa9e Writer</dc:creator><dc:language>en-GB</dc:language>
<dc:identifier id="isbn">9780000000002</dc:identifier><dc:identifier id="uuid">urn:uuid:0aa1</dc:identifier>
<meta name="cover" content="cover-image"/>
</metadata>
<manifest>
<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
<item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
<item id="plate" href="img/plate.png" media-type="image/png"/>
<item id="ch2" href="text/ch2.xhtml" media-type="application/xhtml+xml"/>
<item id="cover-image" href="img/cover.jpg" media-type="image/jpeg"/>
<item id="ch3" href="ch%203.xhtml" media-type="application/xhtml+xml"/>
<item id="gone" href="img/gone.jpg" media-type="image/jpeg"/>
<item id="gif" href="img/a%20b.gif" media-type="image/gif"/>
<item id="again" href="img/plate.png" media-type="image/png"/>
<item id="svg" href="img/d.svg" media-type="image/svg+xml"/>
<item id="lost" href="lost.xhtml" media-type="application/xhtml+xml"/>
<item id="notes" href="notes.xhtml" media-type="application/xhtml+xml"/>
<item id="css" href="style.css" media-type="text/css"/>
</manifest>
<spine toc="ncx">
<itemref idref="ch1"/><itemref idref="ch2"/><itemref idref="lost"/><itemref idref="ch3"/>
<itemref idref="svg"/><itemref idref="notes" linear="no"/>
</spine>
</package>
"""

BUNDLE_NCX = b"""<?xml version="1.0"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/"><navMap>
<navPoint><navLabel><text>One</text></navLabel><content src="ch1.xhtml"/>
<navPoint><navLabel><text>A  part
 of it</text></navLabel><content src="ch1.xhtml#part-2"/></navPoint></navPoint>
<navPoint><navLabel><text>A heading</text></navLabel></navPoint>
<navPoint><navLabel><text>Two &amp; three</text></navLabel><content src="text/ch2.xhtml#a%20place"/></navPoint>
<navPoint><navLabel><text>Elsewhere</text></navLabel><content src="https://example.com/"/></navPoint>
</navMap></ncx>
"""


def book_bundle_seeds() -> None:
    clear("book_bundle")

    def made(name: str, choices: int, package: bytes, ncx: bytes, nav: bytes = b"") -> None:
        write("book_bundle", name, bytes([choices]) + struct.pack("<H", len(package)) + package
              + struct.pack("<H", len(ncx)) + ncx + nav)

    made("a-book-with-pictures-and-an-ncx", 0x00, BUNDLE_PACKAGE, BUNDLE_NCX)
    made("compressed-last-first-with-a-comment", 0x1C, BUNDLE_PACKAGE, BUNDLE_NCX)
    made("little-room-for-the-parser", 0x06, BUNDLE_PACKAGE, BUNDLE_NCX)
    # As EPUB 3 has it: a nav, which is the one read, and a cover named by
    # a property.
    package = BUNDLE_PACKAGE.replace(b'<item id="ncx"', b'<item id="nav" href="nav.xhtml" properties="nav" '
                                     b'media-type="application/xhtml+xml"/>\n<item id="ncx"') \
        .replace(b'<item id="plate" href="img/plate.png"', b'<item id="plate" properties="cover-image" href="img/plate.png"')
    made("a-nav-and-an-ncx", 0x04, package, BUNDLE_NCX, NAV)
    made("a-nav-that-is-no-table", 0x00, package, BUNDLE_NCX, b"<html><body><p>No list here.</p></body></html>")
    # Tables of contents that are not there, are no table, and go wrong
    # part way.
    made("no-table-of-contents", 0x00, BUNDLE_PACKAGE.replace(b'<item id="ncx" href="toc.ncx" '
                                                               b'media-type="application/x-dtbncx+xml"/>\n', b"")
         .replace(b' toc="ncx"', b""), b"")
    made("a-table-that-is-not-xml", 0x00, BUNDLE_PACKAGE, b"this is no table of contents")
    made("a-table-cut-short", 0x00, BUNDLE_PACKAGE, BUNDLE_NCX[:BUNDLE_NCX.index(b"<navPoint><navLabel><text>Two")] + b"<<<")
    made("a-table-of-nothing", 0x00, BUNDLE_PACKAGE, b"")
    made("a-package-cut-short", 0x00, BUNDLE_PACKAGE.replace(b"</package>", b"<guide><<<"), BUNDLE_NCX)
    made("no-identifier-and-no-author", 0x00,
         BUNDLE_PACKAGE.replace(b'<dc:identifier id="isbn">9780000000002</dc:identifier>', b"")
         .replace(b'<dc:identifier id="uuid">urn:uuid:0aa1</dc:identifier>', b"")
         .replace(b"<dc:creator>Ren\xc3\xa9e Writer</dc:creator>", b""), BUNDLE_NCX)
    made("an-identifier-that-is-not-the-one-named", 0x00, BUNDLE_PACKAGE.replace(b'id="uuid">', b'id="other">'), BUNDLE_NCX)
    # More pictures than a page of records holds: the directory is gone
    # through twice. Four of them have files.
    many = b"".join(b'<item id="q%d" href="img/q%d.png" media-type="image/png"/>\n' % (number, number)
                    for number in range(180))
    made("more-pictures-than-a-pass", 0x08, BUNDLE_PACKAGE.replace(b"</manifest>", many + b"</manifest>"), BUNDLE_NCX)
    made("no-pictures", 0x00, BUNDLE_PACKAGE.replace(b"image/", b"text/x-"), BUNDLE_NCX)
    # A long table of contents: entries enough to fill pages of the bundle.
    entries = b"".join(b'<navPoint><navLabel><text>Entry number %d of a long table</text></navLabel>'
                       b'<content src="ch1.xhtml#at-%d"/></navPoint>\n' % (number, number) for number in range(120))
    made("a-long-table", 0x04, BUNDLE_PACKAGE, b'<ncx><navMap>' + entries + b"</navMap></ncx>")
    made("not-a-package", 0x00, b"<html/>", BUNDLE_NCX)
    made("nothing", 0x00, b"", b"")

    # The same kind of book as a ZIP file of the bytes themselves.
    book = Zip()
    book.add("mimetype", b"application/epub+zip")
    book.add("META-INF/container.xml", CONTAINER % b"OEBPS/content.opf", deflated=True)
    book.add("OEBPS/img/plate.png", b"a plate")
    book.add("OEBPS/content.opf", BUNDLE_PACKAGE, deflated=True)
    book.add("OEBPS/toc.ncx", BUNDLE_NCX, deflated=True)
    book.add("OEBPS/img/cover.jpg", b"a cover")
    book.add("OEBPS/ch1.xhtml", b"<html/>", deflated=True)
    book.add("OEBPS/text/ch2.xhtml", b"<html/>")
    write("book_bundle", "a-whole-zip-file", bytes([0x01]) + book.finish())

    # And one whose directory has a picture's name twice, an entry too
    # large for the reader to take, and a name longer than any the package
    # could have kept.
    book = Zip()
    book.add("mimetype", b"application/epub+zip")
    book.add("META-INF/container.xml", CONTAINER % b"OEBPS/content.opf", deflated=True)
    book.add("OEBPS/img/plate.png", b"a plate")
    book.add("OEBPS/img/a b.gif", b"too large to be read", size=0xFFFFFFFF)
    book.add("OEBPS/content.opf", BUNDLE_PACKAGE, deflated=True)
    book.add("OEBPS/img/plate.png", b"a plate of the same name")
    book.add("OEBPS/" + "long/" * 110 + "d.svg", b"<svg/>")
    book.add("OEBPS/toc.ncx", BUNDLE_NCX, deflated=True)
    book.add("OEBPS/img/cover.jpg", b"a cover")
    book.add("OEBPS/ch1.xhtml", b"<html/>", deflated=True)
    write("book_bundle", "a-zip-file-with-a-name-twice-and-entries-not-to-be-read", bytes([0x01]) + book.finish())


# ---- bundle: byte 0 (what is mended, and the chapter that is committed
# afterwards), then a bundle, given as its length and the pieces of it
# that are not nothing ----


def bundle_input(choices: int, file: bytes) -> bytes:
    """What the target is given for a file: its length in fours of bytes,
    and then each run of it that is not zeros, as where it goes (in fours),
    how long it is, and its bytes."""
    assert len(file) % 4 == 0 and len(file) // 4 < 65536
    out = bytearray([choices]) + struct.pack("<H", len(file) // 4)
    at = 0
    while at < len(file):
        if not file[at:at + 4].strip(b"\0"):
            at += 4
            continue
        end = at + 4
        while end < len(file) and end - at < 2000 and file[end:end + 16].strip(b"\0"):
            end += 4
        out += struct.pack("<HH", at // 4, end - at) + file[at:end]
        at = end
    return bytes(out)


def bundle_file(documents: int, pictures: int, strings: bytes, contents: bytes,
                chapters: list[tuple[int, bytes | None]] = (), first_sequence: int = 2) -> bytes:
    """A bundle (epub/bundle.h) written the plain way: its header page, its
    four tables, its two slots and its segments. `chapters` are committed
    one after another, each a chapter's number and its segment, or None
    for a chapter that has none."""
    def file_record(number: int, kind: int) -> bytes:
        return struct.pack("<IIIIHHHBB", 1000 + number * 37, 300 + number, 900 + number, 0xC0000000 + number,
                           8 if number % 2 else 0, 0x0800, 20 + number % 30, kind, number % 3)

    tables = [strings, b"".join(file_record(number, 1) for number in range(documents)),
              b"".join(file_record(number, 4) for number in range(pictures)), contents]
    body = b"".join(tables)
    body += bytes(-len(body) % 4096)
    slot_offset = 4096 + len(body)
    used = 24 + 88 * documents
    slot_bytes = used + (-used % 4096)
    data_begin = slot_offset + 2 * slot_bytes

    header = bytearray(4096)
    header[0:4] = b"BNBK"
    struct.pack_into("<HHH", header, 4, 1, 2, 3)
    struct.pack_into("<IIII", header, 12, 123456, 0x5B7A1234, 0xD1EC7000, 0x9AC4A6E0)
    struct.pack_into("<HHH", header, 28, documents, pictures, 7)
    struct.pack_into("<III", header, 36, slot_offset, slot_bytes, data_begin)
    at = 4096
    for number, table in enumerate(tables):
        struct.pack_into("<III", header, 48 + 12 * number, at, len(table), zlib.crc32(table))
        at += len(table)
    header[96:101] = b"seeds"
    struct.pack_into("<I", header, 4092, zlib.crc32(bytes(header[:4092])))

    def slot(sequence: int, data_end: int, records: list[bytes]) -> bytes:
        out = bytearray(slot_bytes)
        out[0:4] = b"BNSL"
        struct.pack_into("<IIIHH", out, 4, sequence, used, data_end, documents, 88)
        for number, record in enumerate(records):
            out[20 + 88 * number:108 + 88 * number] = record
        struct.pack_into("<I", out, used - 4, zlib.crc32(bytes(out[:used - 4])))
        return bytes(out)

    records = [bytes(88)] * documents
    slots = [slot(first_sequence, data_begin, records), slot(first_sequence - 1, data_begin, records)]
    data = bytearray()
    sequence = first_sequence
    in_force = 0
    for number, (chapter, segment) in enumerate(chapters):
        if segment is None:
            record = struct.pack("<BBH", 3, 0, 0) + bytes(84)
        else:
            directory = min(8, len(segment))
            record = struct.pack("<BBHIIIIIII", 1 + number % 2, 0, number, data_begin + len(data), len(segment),
                                 len(segment) - directory, directory, zlib.crc32(segment[len(segment) - directory:]),
                                 3 + number, 1000 + 10 * number)
            record += struct.pack("<6I", *(100 * number + kind for kind in range(6))) + bytes(range(number, number + 32))
            data += segment + bytes(-len(segment) % 512)
        records[chapter] = record
        sequence += 1
        in_force = 1 - in_force
        slots[in_force] = slot(sequence, data_begin + len(data), records)
    return bytes(header) + body + slots[0] + slots[1] + bytes(data)


def bundle_seeds() -> None:
    clear("bundle")
    random.seed(81)

    def some(count: int) -> bytes:
        return bytes(random.randrange(256) for _ in range(count))

    def changed(file: bytes, at: int, value: bytes) -> bytes:
        return file[:at] + value + file[at + len(value):]

    def seed(name: str, choices: int, file: bytes) -> None:
        write("bundle", name, bundle_input(choices, file))

    # Byte 0: 1 mends the header page's checksum and 2 the slots'; 4, 8 and
    # 12 commit a segment of 1, 512 and 700 bytes, and none commits a
    # chapter with no segment; the top four bits are the chapter.
    new = bundle_file(1, 0, b"", b"")
    seed("a-new-bundle-of-one-document", 12, new)
    seed("a-bundle-of-no-documents", 4, bundle_file(0, 0, some(40), some(100)))
    small = bundle_file(5, 3, some(70), some(900), [(2, some(700)), (0, some(512)), (4, None), (2, some(100))])
    seed("five-documents-and-chapters-committed", 0x38, small)
    seed("a-chapter-with-no-segment-committed", 0x10, small)
    seed("a-chapter-committed-again", 0x24, small)
    # Slots of two pages, with a record across them and chapters either
    # side of it.
    larger = bundle_file(60, 2, some(30), some(200), [(46, some(600)), (45, some(1)), (47, some(1100)), (59, None)])
    seed("slots-of-two-pages", 0xF8, larger)
    # Tables that begin part way through a page of the file and run on over
    # the next.
    seed("tables-over-several-pages", 0x1C, bundle_file(5, 100, some(70), some(3000), [(3, some(200))]))

    # What is not one, or not a whole one.
    seed("nothing", 0, b"")
    seed("a-page-of-nothing", 0, bytes(4096))
    seed("a-header-page-and-no-more", 0, small[:4096])
    seed("cut-short-in-the-second-slot", 0, small[:14000])
    seed("another-version", 0x0C, changed(small, 4, b"\x02"))
    seed("another-version-with-its-checksum-mended", 0x0D, changed(small, 4, b"\x02"))
    seed("a-header-with-a-byte-changed", 0x0C, changed(small, 21, b"\x55"))
    seed("that-header-mended", 0x0D, changed(small, 21, b"\x55"))
    # The slot in force is the first again, after four commits.
    newer = 8192
    older = 12288
    seed("the-newer-slot-spoiled", 0x0C, changed(small, newer + 200, b"\x55"))
    seed("both-slots-spoiled", 0x0C, changed(changed(small, newer + 200, b"\x55"), older + 200, b"\x55"))

    # Headers and slots that pass their checksums, mended, and say what
    # none should.
    seed("more-documents-than-the-spine-has-records", 0x0F, changed(small, 28, b"\x06"))
    seed("a-table-that-does-not-follow-the-one-before", 0x0F, changed(small, 60, b"\x00\x20"))
    seed("slots-a-page-further-on", 0x0F, changed(small, 37, b"\x30"))
    record = newer + 20 + 2 * 88
    seed("a-chapter-in-a-state-there-is-none-of", 0x0F, changed(small, record, b"\x09"))
    seed("a-segment-among-the-slots", 0x0F, changed(small, record + 4, struct.pack("<I", 8192)))
    seed("a-segment-that-runs-out-of-the-file", 0x0F, changed(small, record + 8, struct.pack("<I", 900000)))
    seed("a-directory-out-of-its-segment", 0x0F, changed(small, record + 16, struct.pack("<I", 4000)))
    seed("segments-that-end-out-of-the-file", 0x0F, changed(small, newer + 12, struct.pack("<I", 0x7FFFFE00)))
    seed("a-slot-that-says-it-uses-more", 0x0F, changed(small, newer + 8, struct.pack("<I", 4000)))
    seed("sixty-five-thousand-documents-in-a-small-file", 0x0F, changed(small, 28, b"\xff\xff"))
    # A first slot of nothing, and the second with the sequence number a
    # slot of nothing has: the second is the one in force.
    two = bundle_file(2, 1, some(8), some(20))
    seed("a-blank-first-slot-and-the-second-numbered-nothing", 0x0F,
         changed(changed(two, 8192, bytes(4096)), 12288 + 4, bytes(4)))
    # The last sequence number there is: nothing more can be committed.
    seed("the-last-sequence-number", 0x0C,
         bundle_file(3, 0, some(10), some(10), [(1, some(300))], first_sequence=0xFFFFFFFE))
    seed("not-a-bundle", 0x0F, b"<html/>" * 700)


# ---- font: byte 0 (whether the image's length is mended, and how much room
# the map has), four code points to ask for, then a fonts image ----


def font_image(faces: list[dict]) -> bytes:
    """A fonts image (draw/font.h) written the plain way. A face is its
    role, style and size, and `glyphs`: code point to (advance, width,
    height, left, top), each drawn as a block of ink. It may have
    `kerning`: two code points to the quarters of a pixel the second is set
    further on after the first; each character that is the first of a pair
    is then a class of its own as a first, in the order of the code points,
    and each that is the second of one likewise as a second."""
    out = bytearray(16 + 48 * len(faces))
    out[0:5] = b"BNFN\x01"
    struct.pack_into("<H", out, 6, len(faces))
    for number, face in enumerate(faces):
        drawn = [(40, 6, 9, 0, 9)] + [face["glyphs"][code] for code in sorted(face["glyphs"])]
        ranges: list[list[int]] = []
        for glyph, code in enumerate(sorted(face["glyphs"]), start=1):
            if ranges and ranges[-1][0] + ranges[-1][1] == code:
                ranges[-1][1] += 1
            else:
                ranges.append([code, 1, glyph])
        ranges_at = len(out)
        for first, count, glyph in ranges:
            out += struct.pack("<III", first, count, glyph)
        kerning = face.get("kerning", {})
        firsts = {code: at for at, code in enumerate(sorted({first for first, _ in kerning}), start=1)}
        seconds = {code: at for at, code in enumerate(sorted({second for _, second in kerning}), start=1)}
        codes = [None] + sorted(face["glyphs"])
        glyphs_at = len(out)
        pictures = bytearray()
        for index, (advance, width, height, left, top) in enumerate(drawn):
            out += struct.pack("<HBBbbBBI", advance, width, height, left, top, firsts.get(codes[index], 0),
                               seconds.get(codes[index], 0), len(pictures))
            pixels = [(x + y + index) % 4 for y in range(height) for x in range(width)]
            packed = bytearray((len(pixels) * 2 + 7) // 8)
            for at, ink in enumerate(pixels):
                packed[at // 4] |= ink << (6 - (at % 4) * 2)
            pictures += packed
        kerning_at = len(out)
        table = bytearray(len(firsts) * len(seconds))
        for (first, second), quarters in kerning.items():
            table[(firsts[first] - 1) * len(seconds) + seconds[second] - 1] = quarters & 0xFF
        out += table
        pictures_at = len(out)
        out += pictures
        struct.pack_into("<BBHhhHHIIIIII", out, 16 + 48 * number, face["role"], face["style"], face["size"],
                         face["size"] - 4, 5, face["size"] + 4, face["size"] // 2, len(ranges), ranges_at,
                         len(drawn), glyphs_at, pictures_at, len(pictures))
        if table:
            struct.pack_into("<IBB", out, 16 + 48 * number + 36, kerning_at, len(firsts), len(seconds))
    struct.pack_into("<I", out, 8, len(out))
    return bytes(out)


def font_seeds() -> None:
    clear("font")

    def seed(name: str, image: bytes, mended: bool = False, asked: tuple[int, ...] = (0x61, 0x20, 0xE9, 0x2010),
             choices: int = 0) -> None:
        write("font", name, bytes([int(mended) | choices]) + struct.pack("<4H", *asked) + image)

    letters = {code: (36 + code % 5, 7, 9, 0, 9) for code in range(0x61, 0x67)}
    letters[0x20] = (20, 0, 0, 0, 0)
    letters[0xE9] = (38, 7, 12, -1, 12)
    one = [{"role": 0, "style": 0, "size": 20, "glyphs": letters}]
    seed("one-face", font_image(one))
    seed("four-styles-and-a-fallback", font_image(
        [{"role": 0, "style": style, "size": 20, "glyphs": letters} for style in range(4)]
        + [{"role": 3, "style": 0, "size": 20, "glyphs": {0x3B1: (40, 8, 9, 0, 9), 0x2010: (24, 5, 2, 1, 5)}}]))
    seed("two-sizes", font_image([{"role": 1, "style": 1, "size": size, "glyphs": letters} for size in (16, 30)]))
    apart = {code: (30 + code % 7, 3, 4, 0, 4) for code in range(0x100, 0x1C0, 3)}
    seed("many-ranges", font_image([{"role": 2, "style": 0, "size": 12, "glyphs": apart}]), asked=(0x100, 0x101, 0x1BD, 0xFF))
    seed("no-glyphs-but-the-first", font_image([{"role": 0, "style": 0, "size": 20, "glyphs": {}}]))
    seed("no-faces", font_image([]))

    whole = font_image(one)
    seed("with-more-after-it", whole + bytes(range(64)))
    seed("cut-short", whole[:-9])
    seed("cut-short-and-mended", whole[:-9], mended=True)
    seed("cut-in-its-faces", whole[:40], mended=True)
    # A glyph's picture begun too late to fit, and one too large to.
    spoiled = bytearray(whole)
    glyphs_at = struct.unpack_from("<I", whole, 16 + 24)[0]
    struct.pack_into("<I", spoiled, glyphs_at + 12 * 3 + 8, 0xFFFFFFF0)
    spoiled[glyphs_at + 12 * 4 + 2] = 255
    spoiled[glyphs_at + 12 * 4 + 3] = 255
    seed("pictures-outside-their-face", bytes(spoiled))
    # A range that points past the glyphs, and ranges out of order.
    spoiled = bytearray(whole)
    ranges_at = struct.unpack_from("<I", whole, 16 + 16)[0]
    struct.pack_into("<I", spoiled, ranges_at + 12 + 8, 0xFFFFFFFE)
    seed("a-range-past-the-glyphs", bytes(spoiled))
    spoiled = bytearray(whole)
    spoiled[ranges_at:ranges_at + 12], spoiled[ranges_at + 12:ranges_at + 24] = (
        whole[ranges_at + 12:ranges_at + 24], whole[ranges_at:ranges_at + 12])
    seed("ranges-out-of-order", bytes(spoiled))
    spoiled = bytearray(whole)
    struct.pack_into("<I", spoiled, 16 + 12, 0x7FFFFFFF)
    seed("more-ranges-than-fit", bytes(spoiled))
    spoiled = bytearray(whole)
    struct.pack_into("<H", spoiled, 6, 1025)
    seed("more-faces-than-may-be", bytes(spoiled))

    # Kerning: a face that kerns, asked for letters that do; two faces that
    # kern the same letters otherwise, and one beside them that does not.
    # Four classes of first glyphs and five of second, so that a row is not
    # as long as there are rows.
    pairs = {(0x61, 0x62): -4, (0x61, 0x63): 2, (0x64, 0x62): -8, (0x66, 0x66): 5, (0xE9, 0x61): -1,
             (0x64, 0x65): 7}
    kerned = [{"role": 0, "style": 0, "size": 20, "glyphs": letters, "kerning": pairs}]
    seed("kerned", font_image(kerned), asked=(0x61, 0x62, 0x64, 0x63))
    seed("kerned-through-a-small-map", font_image(kerned), asked=(0x61, 0x62, 0x64, 0xE9), choices=0x02 | (5 << 2))
    seed("faces-that-kern-and-one-that-does-not", font_image(
        [{"role": 0, "style": 0, "size": 20, "glyphs": letters, "kerning": pairs},
         {"role": 0, "style": 2, "size": 20, "glyphs": letters, "kerning": {(0x61, 0x62): 9, (0x62, 0x61): -9}},
         {"role": 2, "style": 0, "size": 20, "glyphs": letters}]), asked=(0x61, 0x62, 0x64, 0x63))
    whole = font_image(kerned)
    glyphs_at = struct.unpack_from("<I", whole, 16 + 24)[0]
    # A glyph of a class the table has no row for, and one it has no place for.
    spoiled = bytearray(whole)
    spoiled[glyphs_at + 12 * 2 + 6] = 200
    spoiled[glyphs_at + 12 * 3 + 7] = 255
    seed("classes-the-table-has-not", bytes(spoiled), asked=(0x61, 0x62, 0x64, 0x63))
    # And one of the class after the last there is, of each kind.
    firsts, seconds = struct.unpack_from("<BB", whole, 16 + 40)
    spoiled = bytearray(whole)
    spoiled[glyphs_at + 12 * 2 + 6] = firsts + 1
    spoiled[glyphs_at + 12 * 3 + 7] = seconds + 1
    seed("a-class-past-the-last", bytes(spoiled), asked=(0x61, 0x62, 0x64, 0x63))
    # A table that begins too near the end to fit, and one with more classes
    # than fit.
    spoiled = bytearray(whole)
    struct.pack_into("<I", spoiled, 16 + 36, len(whole) - 3)
    seed("kerning-outside-the-image", bytes(spoiled))
    # A table whose first row is inside the image and whose last is not.
    spoiled = bytearray(whole)
    struct.pack_into("<I", spoiled, 16 + 36, len(whole) - seconds - 2)
    seed("kerning-that-runs-off-the-end", bytes(spoiled), asked=(0x61, 0x62, 0x64, 0x63))
    spoiled = bytearray(whole)
    spoiled[16 + 40] = 255
    spoiled[16 + 41] = 255
    seed("more-classes-than-fit", bytes(spoiled))
    # A table that lies after everything else of its face: at the image's end.
    spoiled = bytearray(whole)
    kerning_at, firsts, seconds = struct.unpack_from("<IBB", whole, 16 + 36)
    struct.pack_into("<I", spoiled, 16 + 36, len(whole))
    spoiled += bytes(range(1, firsts * seconds + 1))
    seed("kerning-apart-from-its-face", bytes(spoiled), mended=True, asked=(0x61, 0x62, 0x64, 0x63))
    # And one that lies before everything else of its face: the second of
    # two faces is given the first's.
    two = bytearray(font_image(kerned + [{"role": 1, "style": 0, "size": 20, "glyphs": letters, "kerning": pairs}]))
    two[16 + 48 + 36:16 + 48 + 40] = two[16 + 36:16 + 40]
    seed("kerning-before-its-face", bytes(two), asked=(0x61, 0x62, 0x64, 0x63))
    whole = font_image(one)
    seed("another-version", whole[:4] + b"\x02" + whole[5:])
    seed("not-an-image", b"<svg/>" * 20)
    seed("nothing", b"")


def main() -> None:
    font_seeds()
    inflate_seeds()
    zip_seeds()
    xml_seeds()
    package_seeds()
    toc_seeds()
    region_heap_seeds()
    hyphenator_seeds()
    css_seeds()
    svg_sheet_seeds()
    css_rules_seeds()
    document_sheets_seeds()
    svg_tags_seeds()
    svg_values_seeds()
    svg_path_seeds()
    svg_seeds()
    paragraph_seeds()
    png_seeds()
    jpeg_seeds()
    scale_seeds()
    picture_cache_seeds()
    bundle_seeds()
    book_bundle_seeds()
    for directory in sorted(INPUTS.iterdir()):
        seeds = sorted(directory.glob("seed-*"))
        print(f"{directory.name}: {len(seeds)} seeds, {sum(seed.stat().st_size for seed in seeds)} bytes")


if __name__ == "__main__":
    main()
