#!/usr/bin/env python3
"""Writes the seed inputs of the fuzz targets, into inputs/<target>/seed-*.

A seed is an input that is known to take its target somewhere worth
starting from: a sound stream, a sound book, a table of contents with
entries in it, a language's hyphenation table and words for it. The fuzzing
engine makes its new inputs by changing the ones it has, so what the seeds
reach is where it begins. The program that goes through the kept inputs
(see tests/fuzz/CMakeLists.txt) takes them too.

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


def css_seeds() -> None:
    clear("css")

    def seed(name: str, source: int, sheet: bytes) -> None:
        write("css", name, bytes([source]) + sheet)

    seed("a-sheet", 0, SHEET)
    seed("a-sheet-a-byte-at-a-time", 1, SHEET)
    seed("media-and-other-at-rules", 3, MEDIA_SHEET)
    seed("mistakes", 5, MISTAKES_SHEET)
    seed("a-style-attribute", 0, b"margin-left: 2em; font-weight: 700 !important; color: red; font-family: \"a;b\", serif")
    seed("a-media-query", 0, b"only screen and (max-width: 420px), not amzn-mobi")
    seed("longer-than-there-is-room-for", 6,
         b", ".join(b"p.class-number-%d" % n for n in range(80)) + b" { margin: 0 }\nq { font-family: " + b"x" * 600 +
         b"; float: left }\n" + b" > ".join(b"a" for _ in range(9)) + b" {}\np" + b".c" * 13 + b" {}\n")
    seed("nothing", 0, b"")


def main() -> None:
    inflate_seeds()
    zip_seeds()
    xml_seeds()
    package_seeds()
    toc_seeds()
    region_heap_seeds()
    hyphenator_seeds()
    css_seeds()
    for directory in sorted(INPUTS.iterdir()):
        seeds = sorted(directory.glob("seed-*"))
        print(f"{directory.name}: {len(seeds)} seeds, {sum(seed.stat().st_size for seed in seeds)} bytes")


if __name__ == "__main__":
    main()
