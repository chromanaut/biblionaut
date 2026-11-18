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
    seed("longer-than-there-is-room-for", 6,
         b", ".join(b"p.class-number-%d" % n for n in range(80)) + b" { margin: 0 }\nq { font-family: " + b"x" * 600 +
         b"; float: left }\n" + b" > ".join(b"a" for _ in range(9)) + b" {}\np" + b".c" * 13 + b" {}\n")
    seed("nothing", 0, b"")


# ---- css_rules: byte 0 (how much memory, and how the sheets are given), then
# ---- sheets and a document, a byte of zero between them ----

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
    """A file as the cache writes it: the header, then the rows."""
    header = bytearray(64)
    header[0:4] = b"BNPC"
    struct.pack_into("<HHHHB", header, 4, 2, levels, width, height, turn)
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
    write("picture_cache", "cut-short", bytes([0b00111]) + picture_cache_file(20, 10, 2, 0, rows)[:90])
    write("picture_cache", "not-a-picture", bytes([0b00001]) + b"<svg/>" * 20)


def main() -> None:
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
    svg_tags_seeds()
    svg_values_seeds()
    svg_path_seeds()
    svg_seeds()
    paragraph_seeds()
    png_seeds()
    scale_seeds()
    picture_cache_seeds()
    for directory in sorted(INPUTS.iterdir()):
        seeds = sorted(directory.glob("seed-*"))
        print(f"{directory.name}: {len(seeds)} seeds, {sum(seed.stat().st_size for seed in seeds)} bytes")


if __name__ == "__main__":
    main()
