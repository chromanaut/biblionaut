#!/usr/bin/env python3
"""End-to-end check of the chapters `bn-bundle` writes and reads, run by CTest.

A chapter of a book is kept in the book's bundle as a segment
(epub/segment.h). `bn-bundle write-chapter` writes one, from text, with
the code the firmware writes it with; `bn-bundle chapter` and `bn-bundle
find` read one with the code the firmware reads it with. This has a
writer and a reader of its own, made from the format and with none of
that code, and sets each beside the other's work on chapters it makes
up:

  - what the tool writes, this reads: every block, with its style, its
    items and its characters; every id, in the block and at the place it
    was given; the index's record of each block; the pictures' sizes and
    the languages; and every checksum, of each block, each page and the
    directory. What it reads is the chapter that was described, and the
    chapter's record in the bundle says of it what this works out: its
    blocks, its characters, its text by class of font family, and the
    rows of characters it uses;
  - what this writes, the tool reads: `bn-bundle chapter` prints the
    chapter that was described, line for line, and `bn-bundle find` says
    where each block, count of characters, place in the source and id
    is, as this works it out;
  - what the tool prints of a chapter, it writes again as the same
    chapter;
  - a segment with a byte changed is said to be damaged.

usage: segment_tool.py BN_BUNDLE WORK_DIR
"""

from __future__ import annotations

import random
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True  # Or Python leaves compiled copies of the tests' scripts beside them.

import epub_tool  # noqa: E402  (found through the path set just above)
from epub_tool import Failure, expect  # noqa: E402

PAGE = 4096
SECTOR = 512
MOST_ITEM = 4088
KINDS = ["paragraph", "heading", "list-item", "picture", "rule", "table-cell"]
LINKS = ["inside", "note", "outside"]
TEXT, STYLE, LINK, ENDLINK, PICTURE, END = 1, 2, 3, 4, 5, 6
INDEX_PAGE, ANCHOR_PAGE = 0xF0, 0xF1
NO_PICTURE = 0xFFFF
# A block's fields, in the order of their bits: the name bn-bundle gives
# each, and how its values are packed.
FIELDS = [("align", "<B"), ("direction", "<B"), ("size", "<H"), ("line", "<Hhhh"), ("indent", "<hhh"),
          ("left", "<hhh"), ("right", "<hhh"), ("before", "<hhh"), ("after", "<hhh"), ("breaking", "<H"),
          ("depths", "<BB"), ("marker", "<BI"), ("table", "<HHHBBBB"), ("width", "<hhh"), ("height", "<hhh")]
RECORD = "<BBHIIIIIII6I32s"  # A chapter's record in a slot.
WORDS = ["the ", "quick ", "brown ", "fox ", "café ", "naïve ", "— ", "漢字 ", "\U0001f600 ",
         "jumps ", "αβγ ", "over ", "back\\slash ", "a\ttab "]


# ---- A chapter, made up ----
#
# A chapter is what it says of itself (its state, the reason, the length of
# its source, its languages, the sizes of its pictures) and a list of
# things in the order a document has them:
#
#   ("block", kind, level, source, {field: (values, from_book)})
#   ("text", bytes)
#   ("style", bits, book_bits, family, family_from_book, language, size)
#   ("link", kind, chapter, fragment)        ("endlink",)
#   ("picture", number, bits, width, height, alt)
#   ("id", name)                             ("end",)

def words(rng: random.Random, size: int) -> bytes:
    text = ""
    while len(text.encode()) < size:
        text += rng.choice(WORDS)
    return text.encode()


def length(rng: random.Random) -> tuple:
    return (rng.randrange(-100, 300), rng.randrange(0, 300), rng.randrange(0, 6400))


def style_of(rng: random.Random) -> dict:
    """Some of the fields a block may have, each with values of its own."""
    style = {}
    if rng.random() < 0.4:
        return style
    for name, packing in FIELDS:
        if rng.random() < 0.25:
            if name == "align":
                values = (rng.randrange(6),)
            elif name == "direction":
                values = (rng.randrange(2),)
            elif name == "line":
                values = (rng.choice([0, 1200, 1500]),) + length(rng)
            elif name == "marker":
                values = (rng.randrange(10), rng.randrange(1 << 32))
            elif name == "table":
                values = (rng.randrange(9), rng.randrange(700), rng.randrange(30), 1 + rng.randrange(3),
                          1 + rng.randrange(4), rng.randrange(2), 1 + rng.randrange(3))
            elif packing == "<hhh":
                values = length(rng)
            elif packing == "<H":
                values = (rng.randrange(65536),)
            else:
                values = (rng.randrange(4), rng.randrange(5))
            style[name] = (values, rng.random() < 0.5)
    return style


def make_chapter(seed: int, blocks: int, languages: list[bytes], long_text: bool) -> dict:
    """A chapter of so many blocks, the same for the same seed, as bn-bundle prints one: an id between two
    blocks or after text or a picture, and no text straight after text."""
    rng = random.Random(seed)
    things: list[tuple] = []
    shown: set[int] = set()
    source = 40
    ids = 0

    def an_id() -> None:
        nonlocal ids
        name = b"id-%d" % ids + (b"x" * rng.randrange(150) if rng.random() < 0.3 else b"")
        ids += 1
        things.append(("id", name))

    def some_text(size: int) -> None:
        if things[-1][0] == "text":
            things[-1] = ("text", things[-1][1] + words(rng, size))
        else:
            things.append(("text", words(rng, size)))

    for _ in range(blocks):
        for _ in range(rng.choice([0, 0, 0, 0, 0, 1, 2])):
            an_id()
        kind = rng.choice([1] * 12 + [2, 2, 3, 3, 4, 5, 6, 6])
        source += rng.randrange(900)
        things.append(("block", kind, 1 + rng.randrange(6) if kind == 2 else 0, source, style_of(rng)))
        if kind == 4:
            number = rng.randrange(40)
            shown.add(number)
            things.append(("picture", number, 0, length(rng), length(rng), words(rng, rng.randrange(100))))
        elif kind != 5:
            for _ in range(1 + rng.randrange(4)):
                if rng.random() < 0.5:
                    things.append(("style", rng.randrange(128), rng.randrange(128), rng.randrange(6),
                                   rng.random() < 0.5, rng.randrange(len(languages) + 1), 600 + rng.randrange(900)))
                link = rng.random() < 0.2
                if link:
                    things.append(("link", rng.randrange(3), rng.randrange(60),
                                   b"" if rng.random() < 0.3 else b"to-%d" % rng.randrange(900)))
                size = rng.randrange(30) if long_text else 9
                size = 9000 + rng.randrange(9000) if size == 0 else 1500 + rng.randrange(3000) if size < 3 else \
                    1 + rng.randrange(300)
                some_text(size)
                if rng.random() < 0.3:
                    an_id()
                    if rng.random() < 0.5:
                        some_text(1 + rng.randrange(40))
                if rng.random() < 0.15:
                    number = NO_PICTURE if rng.random() < 0.2 else rng.randrange(40)
                    if number != NO_PICTURE:
                        shown.add(number)
                    things.append(("picture", number, 1, length(rng), length(rng), b"a picture"))
                    if rng.random() < 0.3:
                        an_id()
                if link:
                    things.append(("endlink",))
        things.append(("end",))
    if rng.random() < 0.6:
        an_id()
    sizes = {number: (100 + number, 200 + number) for number in sorted(shown) if number % 3 != 0}
    return {"state": rng.choice(["whole", "cut-short"]), "reason": rng.randrange(256),
            "source_bytes": source + rng.randrange(5000), "languages": list(languages), "sizes": sizes,
            "things": things}


def printable(text: bytes) -> bytes:
    """As bn-bundle prints text: control characters and the backslash by their numbers."""
    return b"".join(b"\\x%02X" % byte if byte < 0x20 or byte == 0x5C else bytes([byte]) for byte in text)


def to_text(chapter: dict) -> bytes:
    """The chapter as bn-bundle prints it and reads it, without the lines that begin with #."""
    lines = [b"chapter %s %d %d" % (chapter["state"].encode(), chapter["reason"], chapter["source_bytes"])]
    lines += [b"language " + tag for tag in chapter["languages"]]
    lines += [b"size %d %d %d" % (number, *size) for number, size in sorted(chapter["sizes"].items())]
    for thing in chapter["things"]:
        what = thing[0]
        if what == "block":
            _, kind, level, source, style = thing
            line = b"block %s %d %s" % (KINDS[kind - 1].encode() if kind <= len(KINDS) else b"%d" % kind, level,
                                         b"-" if source is None else b"%d" % source)
            for name, _ in FIELDS:
                if name in style:
                    values, from_book = style[name]
                    line += b" %s=%s%s" % (name.encode(), b",".join(b"%d" % value for value in values),
                                           b"*" if from_book else b"")
            lines.append(line)
        elif what == "text":
            lines.append(b"text " + printable(thing[1]))
        elif what == "style":
            _, bits, book, family, family_book, language, size = thing
            lines.append(b"style %d %d %d%s %d %d" % (bits, book, family, b"*" if family_book else b"", language, size))
        elif what == "link":
            lines.append(b"link %s %d" % (LINKS[thing[1]].encode(), thing[2])
                         + (b" " + printable(thing[3]) if thing[3] else b""))
        elif what == "picture":
            _, number, bits, width, height, alt = thing
            lines.append(b"picture %d %d %d %d %d %d %d %d" % (number, bits, *width, *height)
                         + (b" " + printable(alt) if alt else b""))
        elif what == "id":
            lines.append(b"id " + printable(thing[1]))
        else:
            lines.append(what.encode())
    return b"\n".join(lines) + b"\n"


def facts_of(chapter: dict) -> dict:
    """What a chapter comes to, worked out from the things it is made of: for each block where it began, its
    kind, its bits for breaking, the characters before it and in it; every id's block and place; the
    characters by class of family and the rows of characters used."""
    blocks: list[dict] = []
    ids: list[tuple] = []
    classes = [0] * 6
    rows = bytearray(32)
    characters = 0
    block = None
    family = 0
    for thing in chapter["things"]:
        what = thing[0]
        if what == "block":
            breaking = thing[4]["breaking"][0][0] & 0xFF if "breaking" in thing[4] else 0
            block = {"kind": thing[1], "source": thing[3], "breaking": breaking, "before": characters, "characters": 0}
            blocks.append(block)
            family = 0
        elif what == "text":
            block["characters"] += len(thing[1])
            if block["kind"] != 2:
                classes[family] += len(thing[1])
            for letter in thing[1].decode():
                row = ord(letter) >> 8 if ord(letter) < 65536 else 255
                rows[row // 8] |= 1 << (row % 8)
        elif what == "style":
            family = thing[3]
        elif what == "picture":
            block["characters"] += 1
        elif what == "id":
            ids.append((thing[1], len(blocks) - 1 if block is not None else len(blocks),
                        block["characters"] if block is not None else 0))
        elif what == "end":
            block["characters"] = max(block["characters"], 1)
            characters += block["characters"]
            block = None
    return {"blocks": blocks, "ids": ids, "characters": characters, "classes": classes, "rows": bytes(rows)}


# ---- A segment, written from the format ----

def pack_style(style: dict) -> bytes:
    if not style:
        return b""
    has = book = 0
    fields = b""
    for bit, (name, packing) in enumerate(FIELDS):
        if name in style:
            values, from_book = style[name]
            has |= 1 << bit
            book |= (1 << bit) if from_book else 0
            fields += struct.pack(packing, *values)
    return struct.pack("<HH", has, book) + fields


def cut_text(text: bytes, rng: random.Random) -> list[bytes]:
    """Text as items of whatever lengths, each ending where a character does."""
    pieces = []
    at = 0
    while at < len(text):
        end = min(len(text), at + rng.choice([1, 7, 200, 1020, MOST_ITEM, MOST_ITEM]))
        while end > at and end < len(text) and (text[end] & 0xC0) == 0x80:
            end -= 1
        if end == at:  # Less than a character was asked for: the whole of it.
            end = at + 1
            while end < len(text) and (text[end] & 0xC0) == 0x80:
                end += 1
        pieces.append(text[at:end])
        at = end
    return pieces


def write_segment(chapter: dict, seed: int) -> tuple[bytes, dict]:
    """The chapter's segment, and what its record in a slot is to say but for where the segment is."""
    rng = random.Random(seed)
    facts = facts_of(chapter)
    out = bytearray()
    index = bytearray()  # The index page being filled; and its entries in the directory.
    index_first = 0
    index_entries = []
    anchors = bytearray()  # The same for the anchor page.
    anchor_ids = 0
    anchor_entries = []
    block_at = block_crc = blocks = ids = 0
    shown: set[int] = set()

    def record(kind: int, byte: int, data: bytes) -> None:
        nonlocal block_crc
        whole = struct.pack("<BBH", kind, byte, len(data)) + data
        block_crc = zlib.crc32(whole, block_crc)
        out.extend(whole)

    def page(kind: int, data: bytes) -> int:
        """Writes a page, which is no part of a block, and says where it is."""
        out.extend(struct.pack("<BBH", kind, 0, len(data)))
        out.extend(data)
        return len(out) - len(data)

    for thing in chapter["things"]:
        what = thing[0]
        if what == "block":
            _, kind, level, _, style = thing
            block_at, block_crc = len(out), 0
            record(kind, level, pack_style(style))
        elif what == "text":
            for piece in cut_text(thing[1], rng):
                record(TEXT, 0, piece)
        elif what == "style":
            _, bits, book, family, family_book, language, size = thing
            record(STYLE, 0, struct.pack("<HHBBH", bits, book, family | (8 if family_book else 0), language, size))
        elif what == "link":
            record(LINK, thing[1], struct.pack("<HB", thing[2], len(thing[3])) + thing[3])
        elif what == "endlink":
            record(ENDLINK, 0, b"")
        elif what == "picture":
            _, number, bits, width, height, alt = thing
            if number != NO_PICTURE:
                shown.add(number)
            record(PICTURE, 0, struct.pack("<HHhhhhhh", number, bits, *width, *height) + alt)
        elif what == "id":
            name, block, into = facts["ids"][ids]
            ids += 1
            entry = bytes([len(name)]) + name + struct.pack("<HI", block, into)
            if len(anchors) + len(entry) > PAGE:
                where = page(ANCHOR_PAGE, anchors)  # Between two items, or two blocks: wherever this id is.
                anchor_entries.append(struct.pack("<IHHI", where, len(anchors), anchor_ids, zlib.crc32(anchors)))
                anchors, anchor_ids = bytearray(), 0
            anchors.extend(entry)
            anchor_ids += 1
        else:
            said = facts["blocks"][blocks]
            end = struct.pack("<BBHI", END, 0, 8, said["characters"])
            block_crc = zlib.crc32(end, block_crc)
            out.extend(end + struct.pack("<I", block_crc))
            if not index:
                index_first = said["before"]
            index.extend(struct.pack("<IIIBBH", block_at, said["before"], said["source"], said["kind"],
                                     said["breaking"], 0))
            blocks += 1
            if len(index) == PAGE:
                index_entries.append(struct.pack("<III", page(INDEX_PAGE, index), index_first, zlib.crc32(index)))
                index = bytearray()
    if index:
        index_entries.append(struct.pack("<III", page(INDEX_PAGE, index), index_first, zlib.crc32(index)))
    if anchors:
        where = page(ANCHOR_PAGE, anchors)
        anchor_entries.append(struct.pack("<IHHI", where, len(anchors), anchor_ids, zlib.crc32(anchors)))
    pictures = b"".join(struct.pack("<HHHBB", number, *chapter["sizes"].get(number, (0, 0)),
                                    1 if number in chapter["sizes"] else 0, 0) for number in sorted(shown))
    directory = struct.pack("<HHHHIII", len(index_entries), len(anchor_entries), len(shown),
                            len(chapter["languages"]), blocks, facts["characters"], chapter["source_bytes"])
    directory += b"".join(index_entries) + b"".join(anchor_entries) + pictures
    directory += b"".join(bytes([len(tag)]) + tag for tag in chapter["languages"])
    said = {"state": 1 if chapter["state"] == "whole" else 2, "reason": chapter["reason"], "left_out": 0,
            "segment_bytes": len(out) + len(directory), "directory_offset": len(out),
            "directory_bytes": len(directory), "directory_checksum": zlib.crc32(directory), "blocks": blocks,
            "characters": facts["characters"], "classes": facts["classes"], "rows": facts["rows"]}
    return bytes(out) + directory, said


# ---- A bundle's slots, by the places the format gives ----

def slots_of(data: bytes) -> dict:
    """Where a bundle's slots are, which is in force, and that one's bytes."""
    expect(data[:4] == b"BNBK" and struct.unpack_from("<I", data, 4092)[0] == zlib.crc32(data[:4092]),
           "the file's first page is not a bundle's")
    documents = struct.unpack_from("<H", data, 28)[0]
    slot_at, slot_bytes, data_begin = struct.unpack_from("<III", data, 36)
    used = 24 + 88 * documents
    best = None
    for slot in (0, 1):
        begin = slot_at + slot * slot_bytes
        whole = data[begin:begin + used]
        if whole[:4] == b"BNSL" and struct.unpack_from("<I", whole, used - 4)[0] == zlib.crc32(whole[:used - 4]):
            sequence = struct.unpack_from("<I", whole, 4)[0]
            if best is None or sequence > best[1]:
                best = (slot, sequence, whole)
    expect(best is not None, "neither slot of the bundle is whole")
    return {"documents": documents, "slot_at": slot_at, "slot_bytes": slot_bytes, "data_begin": data_begin,
            "used": used, "slot": best[0], "sequence": best[1], "bytes": best[2],
            "data_end": struct.unpack_from("<I", best[2], 12)[0]}


def record_of(data: bytes, number: int) -> dict:
    """A chapter's record, from the slot in force."""
    fields = struct.unpack_from(RECORD, slots_of(data)["bytes"], 20 + 88 * number)
    return {"state": fields[0], "reason": fields[1], "left_out": fields[2], "segment_offset": fields[3],
            "segment_bytes": fields[4], "directory_offset": fields[5], "directory_bytes": fields[6],
            "directory_checksum": fields[7], "blocks": fields[8], "characters": fields[9],
            "classes": list(fields[10:16]), "rows": fields[16]}


def commit(data: bytes, number: int, segment: bytes, said: dict) -> bytes:
    """The bundle with a segment added where its segments end, and the chapter's record in the other slot."""
    slots = slots_of(data)
    begin = slots["data_end"]
    expect(begin % SECTOR == 0 and begin <= len(data), f"the segments end at {begin}")
    grown = bytearray(data[:begin]) + segment
    grown += bytes(-len(grown) % SECTOR)
    slot = bytearray(slots["bytes"])
    struct.pack_into("<I", slot, 4, slots["sequence"] + 1)
    struct.pack_into("<I", slot, 12, len(grown))
    struct.pack_into(RECORD, slot, 20 + 88 * number, said["state"], said["reason"], said["left_out"], begin,
                     said["segment_bytes"], said["directory_offset"], said["directory_bytes"],
                     said["directory_checksum"], said["blocks"], said["characters"], *said["classes"], said["rows"])
    struct.pack_into("<I", slot, slots["used"] - 4, zlib.crc32(bytes(slot[:slots["used"] - 4])))
    other = slots["slot_at"] + (1 - slots["slot"]) * slots["slot_bytes"]
    grown[other:other + slots["used"]] = slot
    return bytes(grown)


# ---- A segment, read from the format ----

def unpack_style(data: bytes) -> dict:
    if not data:
        return {}
    has, book = struct.unpack_from("<HH", data, 0)
    expect(has != 0 and has < 0x8000 and book | has == has, f"a style record's masks are {has:#x} and {book:#x}")
    style = {}
    at = 4
    for bit, (name, packing) in enumerate(FIELDS):
        if has & (1 << bit):
            style[name] = (struct.unpack_from(packing, data, at), bool(book & (1 << bit)))
            at += struct.calcsize(packing)
    expect(at == len(data), f"a style record of {len(data)} bytes whose fields come to {at}")
    return style


def read_segment(data: bytes, record: dict) -> dict:
    """The chapter a segment holds, as the things it is made of, with where each block was found. Raises
    Failure where the bytes are not as the format has them, or a checksum does not hold."""
    expect(record["state"] in (1, 2), f"the chapter's state is {record['state']}")
    segment = data[record["segment_offset"]:record["segment_offset"] + record["segment_bytes"]]
    expect(len(segment) == record["segment_bytes"], "the segment runs out of the file")
    end = record["directory_offset"]
    directory = segment[end:end + record["directory_bytes"]]
    expect(end + record["directory_bytes"] == len(segment), "the directory does not end where the segment does")
    expect(zlib.crc32(directory) == record["directory_checksum"], "the directory fails its checksum")
    index_pages, anchor_pages, pictures, languages, blocks, characters, source_bytes = \
        struct.unpack_from("<HHHHIII", directory, 0)
    expect((blocks, characters) == (record["blocks"], record["characters"]), "the record and the directory differ")
    at = 20
    index: list[tuple] = []
    page_places = set()
    for number in range(index_pages):
        where, first, checksum = struct.unpack_from("<III", directory, at)
        at += 12
        count = min(256, min(blocks, 65536) - 256 * number)
        expect(count > 0, "an index page with no block")
        page = segment[where:where + 16 * count]
        expect(zlib.crc32(page) == checksum, f"index page {number} fails its checksum")
        expect(segment[where - 4:where] == struct.pack("<BBH", INDEX_PAGE, 0, len(page)), "an index page's head")
        page_places.add(where)
        records = [struct.unpack_from("<IIIBBH", page, 16 * i) for i in range(count)]
        expect(records[0][1] == first and not any(entry[5] for entry in records), "an index page's entry or spares")
        index += records
    ids: list[tuple] = []
    for number in range(anchor_pages):
        where, size, count, checksum = struct.unpack_from("<IHHI", directory, at)
        at += 12
        page = segment[where:where + size]
        expect(0 < size <= PAGE and zlib.crc32(page) == checksum, f"anchor page {number} fails its checksum")
        expect(segment[where - 4:where] == struct.pack("<BBH", ANCHOR_PAGE, 0, size), "an anchor page's head")
        page_places.add(where)
        inside = 0
        found = 0
        while inside < size:
            name = page[inside + 1:inside + 1 + page[inside]]
            ids.append((name, *struct.unpack_from("<HI", page, inside + 1 + page[inside])))
            inside += len(name) + 7
            found += 1
        expect(inside == size and found == count, f"anchor page {number} has {found} ids and says {count}")
    sizes = {}
    shown = []
    for _ in range(pictures):
        number, width, height, sized, spare = struct.unpack_from("<HHHBB", directory, at)
        at += 8
        expect(spare == 0 and sized in (0, 1), "a picture's entry")
        shown.append(number)
        if sized:
            sizes[number] = (width, height)
    expect(shown == sorted(set(shown)), f"the pictures are not in the order of their numbers: {shown}")
    tags = []
    for _ in range(languages):
        tags.append(directory[at + 1:at + 1 + directory[at]])
        at += 1 + directory[at]
    expect(at == len(directory), "the directory is not as long as what it holds")

    # The records, from the first to the directory.
    blocks_read: list[dict] = []
    block = None
    crc = 0
    before = 0
    pages = 0
    at = 0
    while at < end:
        kind, byte, size = struct.unpack_from("<BBH", segment, at)
        body = segment[at + 4:at + 4 + size]
        expect(at + 4 + size <= end, "a record runs into the directory")
        if kind in (INDEX_PAGE, ANCHOR_PAGE):
            expect(at + 4 in page_places and byte == 0, "a page that the directory does not have")
            pages += 1
            at += 4 + size
            continue
        if block is None:
            block = {"kind": kind, "level": byte, "style": unpack_style(body), "offset": at, "before": before,
                     "characters": 0, "items": []}
            crc = zlib.crc32(segment[at:at + 4 + size])
            at += 4 + size
            continue
        if kind == END:
            expect(size == 8, "a block's end is not 8 bytes")
            said, checksum = struct.unpack("<II", body)
            crc = zlib.crc32(segment[at:at + 8], crc)
            expect(checksum == crc, f"block {len(blocks_read)} fails its checksum")
            expect(said == max(block["characters"], 1), f"block {len(blocks_read)} says it is {said} characters")
            block["characters"] = said
            before += said
            blocks_read.append(block)
            block = None
            at += 12
            continue
        crc = zlib.crc32(segment[at:at + 4 + size], crc)
        expect(size <= MOST_ITEM, "an item longer than an item may be")
        if kind == TEXT:
            expect(size > 0 and (body[0] & 0xC0) != 0x80, "a text item that begins inside a character")
            body.decode()  # Whole characters, or this raises.
            if block["items"] and block["items"][-1][0] == "text":
                block["items"][-1] = ("text", block["items"][-1][1] + body)
            else:
                block["items"].append(("text", body))
            block["characters"] += size
        elif kind == STYLE:
            bits, book, family, language, run_size = struct.unpack("<HHBBH", body)
            expect(family < 16 and language <= languages, "a style item")
            block["items"].append(("style", bits, book, family & 7, bool(family & 8), language, run_size))
        elif kind == LINK:
            chapter, name_size = struct.unpack_from("<HB", body, 0)
            expect(size == 3 + name_size and byte < 3, "a link")
            block["items"].append(("link", byte, chapter, body[3:]))
        elif kind == ENDLINK:
            expect(size == 0, "a link's end")
            block["items"].append(("endlink",))
        elif kind == PICTURE:
            number, bits, *amounts = struct.unpack_from("<HHhhhhhh", body, 0)
            expect(size <= 16 + 255, "a picture's text")
            block["items"].append(("picture", number, bits, tuple(amounts[:3]), tuple(amounts[3:]), body[16:]))
            block["characters"] += 1
        else:
            raise Failure(f"an item of the kind {kind}")
        at += 4 + size
    expect(block is None and at == end, "the records do not end at the directory, between two blocks")
    expect(len(blocks_read) == blocks and before == characters, "not the blocks or characters the directory says")
    expect(pages == index_pages + anchor_pages, "pages among the records that the directory does not have")
    expect(len(index) == min(blocks, 65536), "the index has not a record for each block")
    for number, entry in enumerate(index):
        found = blocks_read[number]
        breaking = found["style"]["breaking"][0][0] & 0xFF if "breaking" in found["style"] else 0
        expect(entry[:2] == (found["offset"], found["before"]) and entry[3:5] == (found["kind"], breaking),
               f"the index's record of block {number} is {entry}")
        found["source"] = entry[2]

    # The things the chapter is made of, with each id where it is.
    things: list[tuple] = []
    waiting = list(ids)

    def ids_as_far_as(number: int, into: int) -> None:
        while waiting and (waiting[0][1] < number or (waiting[0][1] == number and waiting[0][2] <= into)):
            things.append(("id", waiting.pop(0)[0]))

    for number, found in enumerate(blocks_read):
        ids_as_far_as(number, 0)
        things.append(("block", found["kind"], found["level"], found.get("source"), found["style"]))
        into = 0
        for item in found["items"]:
            ids_as_far_as(number, into)
            if item[0] != "text":
                things.append(item)
                into += 1 if item[0] == "picture" else 0
                continue
            text = item[1]
            while text:
                piece = len(text)
                if waiting and waiting[0][1] == number and waiting[0][2] - into < piece:
                    piece = waiting[0][2] - into
                things.append(("text", text[:piece]))
                text = text[piece:]
                into += piece
                ids_as_far_as(number, into)
        ids_as_far_as(number, 1 << 40)
        things.append(("end",))
    things += [("id", name) for name, _, _ in waiting]
    return {"state": "whole" if record["state"] == 1 else "cut-short", "reason": record["reason"],
            "source_bytes": source_bytes, "languages": tags, "sizes": sizes, "things": things,
            "blocks": blocks_read, "ids": ids, "pages": sorted(page_places)}


# ---- The comparisons ----

def run(tool: str, *arguments) -> subprocess.CompletedProcess:
    return subprocess.run([tool, *[str(argument) for argument in arguments]], capture_output=True, check=False)


def printed(tool: str, bundle: Path, number: int) -> bytes:
    """What bn-bundle chapter prints, without the lines that say something of the chapter and are not it."""
    ran = run(tool, "chapter", bundle, number)
    expect(ran.returncode == 0, f"bn-bundle chapter {number} ended with {ran.returncode}: {ran.stderr.decode()!r}")
    return b"".join(line + b"\n" for line in ran.stdout.split(b"\n")[:-1] if not line.startswith(b"#"))


def same_text(got: bytes, want: bytes, what: str) -> None:
    if got == want:
        return
    got_lines, want_lines = got.split(b"\n"), want.split(b"\n")
    for number, (one, other) in enumerate(zip(got_lines, want_lines)):
        if one != other:
            raise Failure(f"{what}: line {number + 1} is {one[:200]!r}, and the chapter has {other[:200]!r}")
    raise Failure(f"{what}: {len(got_lines)} lines, and the chapter has {len(want_lines)}")


def check_finds(tool: str, bundle: Path, number: int, chapter: dict, offsets: list[int], seed: int,
                many: bool) -> int:
    """bn-bundle find, beside where each place is as the chapter's things say: at a few places, or at `many`.
    Returns how many were looked for."""
    facts = facts_of(chapter)
    blocks = facts["blocks"]
    rng = random.Random(seed)

    def said(what: str, value) -> str:
        ran = run(tool, "find", bundle, number, what, value)
        expect(ran.returncode == 0, f"find {what} {value!r} ended with {ran.returncode}: {ran.stderr.decode()!r}")
        return ran.stdout.decode().strip()

    def place(block: int, into: int) -> str:
        if block == len(blocks):
            return f"end block={block} before={facts['characters']}"
        found = blocks[block]
        return (f"block={block} offset={offsets[block]} before={found['before']} source={found['source']} "
                f"kind={found['kind']} breaking={found['breaking']} into={into}")

    looked = 0
    some = {0, len(blocks) - 1}
    if many:
        some |= {255, 256, 257} | {rng.randrange(len(blocks)) for _ in range(6)}
    for block in sorted(block for block in some if 0 <= block < len(blocks)):
        found = blocks[block]
        expect(said("block", block) == place(block, 0), f"find block {block}: {said('block', block)}")
        for into in {0, found["characters"] // 2, found["characters"] - 1}:
            got = said("characters", found["before"] + into)
            expect(got == place(block, into), f"find characters {found['before'] + into}: {got}")
        # The last block that began at or before the place.
        last = max(index for index, other in enumerate(blocks) if other["source"] <= found["source"])
        got = said("source", found["source"])
        expect(got == place(last, 0), f"find source {found['source']}: {got}, and block {last} is the last there")
        looked += 5
    expect(said("block", len(blocks)) == "none", "a block the chapter does not have")
    expect(said("characters", facts["characters"]) == place(len(blocks), 0), "the place after the last character")
    expect(said("source", 0) == place(max([0] + [index for index, other in enumerate(blocks) if other["source"] == 0]),
                                      0), "a place before every block")
    seen = set()
    for name, block, into in facts["ids"]:
        if name in seen or rng.random() < (0.9 if many else 0.98):
            continue
        seen.add(name)
        got = said("id", printable(name).decode())
        expect(got == place(block, into), f"find id {name[:40]!r}: {got}")
        looked += 1
    expect(said("id", "no-such-id") == "none", "an id the chapter does not have")
    return looked + 4


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tool = sys.argv[1]
    work = Path(sys.argv[2])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        book = epub_tool.books(work)[0]
        bundle = work / "book.bundle"
        ran = run(tool, "make", book, bundle, "--written", 1_795_000_000)
        expect(ran.returncode == 0, f"making the bundle ended with {ran.returncode}")
        documents = slots_of(bundle.read_bytes())["documents"]
        expect(documents >= 12, f"the book has {documents} documents")

        chapters = [make_chapter(1, 40, [], True), make_chapter(2, 300, [b"fr", b"grc", b"en-GB"], False),
                    make_chapter(3, 0, [], False), make_chapter(4, 120, [b"de"], True),
                    make_chapter(5, 600, [], False)]
        blocks = looked = 0
        for number, chapter in enumerate(chapters):
            text = to_text(chapter)
            facts = facts_of(chapter)
            blocks += len(facts["blocks"])

            # What the tool writes, this reads.
            described = work / f"chapter-{number}.txt"
            described.write_bytes(text)
            ran = run(tool, "write-chapter", bundle, 2 * number, described)
            expect(ran.returncode == 0, f"write-chapter {number} ended with {ran.returncode}: {ran.stderr.decode()!r}")
            data = bundle.read_bytes()
            record = record_of(data, 2 * number)
            read = read_segment(data, record)
            same_text(to_text(read), text, f"chapter {number} as the tool wrote it")
            expect((record["blocks"], record["characters"], record["left_out"]) ==
                   (len(facts["blocks"]), facts["characters"], 0), f"chapter {number}'s record: {record}")
            expect(record["classes"] == facts["classes"], f"chapter {number}'s text by class: {record['classes']}, "
                   f"and it is {facts['classes']}")
            expect(record["rows"] == facts["rows"], f"chapter {number}'s rows of characters")
            expect(ran.stdout.decode().startswith(f"blocks={len(facts['blocks'])} characters={facts['characters']} "
                                                  f"bytes={record['segment_bytes']} "),
                   f"what write-chapter said: {ran.stdout!r}")
            # Its text items are of a kilobyte at the most, whatever was given.
            same_text(printed(tool, bundle, 2 * number), text, f"chapter {number}, written and printed by the tool")
            if facts["blocks"]:
                looked += check_finds(tool, bundle, 2 * number, chapter, [found["offset"] for found in read["blocks"]],
                                      number, number in (1, 4))

            # What this writes, the tool reads.
            segment, said = write_segment(chapter, number)
            data = commit(bundle.read_bytes(), 2 * number + 1, segment, said)
            bundle.write_bytes(data)
            mine = read_segment(data, record_of(data, 2 * number + 1))
            same_text(to_text(mine), text, f"chapter {number} as this wrote it, read here")
            same_text(printed(tool, bundle, 2 * number + 1), text, f"chapter {number} as this wrote it")
            if facts["blocks"]:
                looked += check_finds(tool, bundle, 2 * number + 1, chapter,
                                      [found["offset"] for found in mine["blocks"]], number + 100, number == 1)
            info = run(tool, "info", bundle).stdout.decode()
            line = (f"chapter {2 * number + 1}: {'whole' if said['state'] == 1 else 'cut short'}, {said['blocks']} "
                    f"blocks, {said['characters']} characters, 0 left out, {len(segment)} bytes at ")
            expect(line in info, f"bn-bundle info has no line beginning {line!r}")

        # A chapter that is printed is written again as the same chapter.
        again = work / "again.txt"
        again.write_bytes(run(tool, "chapter", bundle, 7).stdout)
        ran = run(tool, "write-chapter", bundle, 11, again)
        expect(ran.returncode == 0, f"writing a chapter from what was printed ended with {ran.returncode}")
        same_text(printed(tool, bundle, 11), to_text(chapters[3]), "a chapter written from what was printed")

        # The chapters written first are still what they were, after ten more.
        data = bundle.read_bytes()
        same_text(to_text(read_segment(data, record_of(data, 0))), to_text(chapters[0]), "the first chapter, at the end")

        # A segment with a byte changed is damaged, wherever the byte is.
        record = record_of(data, 3)
        heads = {place - back for place in read_segment(data, record)["pages"] for back in (1, 2, 3, 4)}
        changed = 0
        for at in list(range(0, record["directory_offset"], 2999)) + [record["directory_offset"] + 9,
                                                                     record["segment_bytes"] - 1]:
            if at in heads:
                continue  # The four bytes before a page are of no checksum.
            spoiled = bytearray(data)
            spoiled[record["segment_offset"] + at] ^= 0x10
            (work / "spoiled.bundle").write_bytes(spoiled)
            ran = run(tool, "chapter", work / "spoiled.bundle", 3)
            expect(ran.returncode == 1 and b"damaged" in ran.stderr,
                   f"byte {at} of a segment changed: bn-bundle chapter ended with {ran.returncode}, {ran.stderr!r}")
            changed += 1

        # What cannot be done is said.
        ran = run(tool, "chapter", bundle, 10)
        expect(ran.returncode == 1 and b"not imported" in ran.stderr, f"a chapter not imported: {ran.stderr!r}")
        ran = run(tool, "chapter", bundle, documents)
        expect(ran.returncode == 1 and b"no chapter" in ran.stderr, f"a chapter the book has not: {ran.stderr!r}")
        (work / "bad.txt").write_bytes(b"chapter whole 0 0\nblock paragraph 0 0\ntext fine\nsomething else\nend\n")
        ran = run(tool, "write-chapter", bundle, 10, work / "bad.txt")
        expect(ran.returncode == 1 and b"bad.txt:4: not a line of a chapter" in ran.stderr,
               f"text that is no chapter: {ran.returncode} {ran.stderr!r}")
        (work / "bad.txt").write_bytes(b"chapter whole 0 0\ntext outside a block\n")
        ran = run(tool, "write-chapter", bundle, 10, work / "bad.txt")
        expect(ran.returncode == 1 and b"bad.txt:2: misused" in ran.stderr, f"text out of turn: {ran.stderr!r}")
        expect(record_of(bundle.read_bytes(), 10)["state"] == 0, "a chapter that could not be written was committed")
        # A block whose place in the source is not known begins where the one before it did.
        (work / "unplaced.txt").write_bytes(b"chapter whole 0 900\nblock paragraph 0 500\ntext a\nend\n"
                                            b"block paragraph 0 -\ntext b\nend\n")
        ran = run(tool, "write-chapter", bundle, 10, work / "unplaced.txt")
        expect(ran.returncode == 0, f"a block with no place in the source: {ran.returncode} {ran.stderr!r}")
        expect(printed(tool, bundle, 10).count(b"block paragraph 0 500\n") == 2,
               f"a block with no place in the source is printed {printed(tool, bundle, 10)!r}")
        ran = run(tool, "find", bundle, 0, "nothing", 3)
        expect(ran.returncode == 2, f"find of what is no kind of place ended with {ran.returncode}")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print(f"bn-bundle and Python agree about {2 * len(chapters)} chapters of {blocks} blocks, written by each and read "
          f"by the other; about {looked} places looked for in them; and that {changed} segments with a byte changed "
          "are damaged.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
