#!/usr/bin/env python3
"""End-to-end check of `bn-pages`, run by CTest.

`bn-pages` sets a chapter of a bundle in pages: it reads the chapter with
the reader of its segment, sets each block in lines with the firmware's
paragraph setter in the made-up face, and chooses pages among the lines
with the firmware's page chooser. Whether a paragraph's lines are the
best there are is for lines_tool.py; here the rest is set beside a second
working-out that shares none of the code, for chapters this makes up and
`bn-bundle write-chapter` writes:

  - the lines of every block: each is characters of the block as this
    script has it, in order and with nothing but spaces left out between
    two; as wide as its characters add up to in the made-up face, in the
    style each is in; no wider than the block's margins and indent leave,
    and for lines of words all one width, as full as that; as high as
    the block's record says, with the space above it that the block and
    the one before ask for; and kept with the line before, or the start
    of a page, where the records say;
  - every page of a chapter going down from its first line, against the
    pages this chooses from those lines by adding their heights up over
    the whole chapter;
  - the page above a line, and the page a chapter entered at a place is
    shown, for the place given as a block, a count of characters, a place
    in the source and an id, against the same;
  - with a page of another size;
  - the page an entry of the book's table of contents leads to, the last
    page of the chapter before for the page above a chapter's first line,
    and a saved place found again after its chapter is written anew.

usage: pages_tool.py BN_PAGES BN_BUNDLE WORK_DIR
"""

from __future__ import annotations

import random
import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True  # Or Python leaves compiled copies of the tests' scripts beside them.

import epub_tool  # noqa: E402  (found through the path set just above)
import segment_tool  # noqa: E402
from epub_tool import Failure, expect  # noqa: E402

PICTURE = 0x1A  # The byte a picture is among a block's characters.
TEXT_SIZE = 16
NARROW = b"iljtf.,;:'!|"
WIDE = b"mwMW"


# ---- A chapter's blocks, as this has them ----

def whole(value: int, by: int) -> int:
    """A division with what is left over dropped, towards nothing."""
    return abs(value) // by * (1 if value >= 0 else -1)


def held(value: int, least: int, most: int) -> int:
    return max(least, min(most, value))


def quarters(length: tuple, size: int, width: int) -> int:
    """A length in quarters of a pixel: its three amounts, each taken whole, added."""
    return whole(length[0] * size, 1000) + whole(length[1], 2) + whole(length[2] * width, 6400)


def pixels(length: tuple, size: int, width: int) -> int:
    return whole(quarters(length, size, width), 4)


def character_width(first: int, style: int) -> int:
    """How wide the made-up face has the character that begins with this byte."""
    if (first & 0xC0) == 0x80 or first in (0xCC, 0xCD):
        return 0
    if first in NARROW:
        width = 18
    elif first in WIDE:
        width = 54
    elif 0x41 <= first <= 0x5A:
        width = 44
    elif first == 0x2D:
        width = 22
    elif first >= 0xC0:
        width = 40
    else:
        width = 36
    return width + 4 * style


def blocks_of(chapter: dict, width: int, height: int) -> list[dict]:
    """Each block of a chapter: its characters, the style each byte is in, and what its record comes to on a
    page whose text is so wide and so high."""
    blocks: list[dict] = []
    block = None
    style = 0
    before = 0
    waiting: list[tuple] = []  # The ids met between two blocks: they are at the start of the next.
    for thing in chapter["things"]:
        what = thing[0]
        if what == "block":
            block = {"kind": thing[1], "level": thing[2], "source": thing[3], "style": thing[4], "characters": b"",
                     "styles": [], "pictures": [], "ids": waiting, "before": before}
            blocks.append(block)
            waiting = []
            style = 0
        elif what == "text":
            block["characters"] += thing[1]
            block["styles"] += [style] * len(thing[1])
        elif what == "style":
            style = (1 if thing[1] & 1 else 0) | (2 if thing[1] & 2 else 0)
        elif what == "picture":
            block["pictures"].append((thing[1], thing[4], len(block["characters"])))
            block["characters"] += bytes([PICTURE])
            block["styles"].append(style)
        elif what == "id":
            (waiting if block is None else block["ids"]).append((thing[1], 0 if block is None else
                                                                 len(block["characters"])))
        elif what == "end":
            block["count"] = max(len(block["characters"]), 1)
            before += block["count"]
            block = None
    after_before = 0
    kept = False
    for number, made in enumerate(blocks):
        said = made["style"]
        size = said["size"][0][0] if "size" in said else 1000
        length = lambda name: quarters(said[name][0], size, width) if name in said else 0  # noqa: E731
        made["rest"] = held(width - length("left") - length("right"), width // 10, width)
        made["first"] = held(made["rest"] - length("indent"), width // 10, width)
        line = whole(TEXT_SIZE * size * 1250, 1_000_000)
        if "line" in said:
            multiple, *amounts = said["line"][0]
            line = whole(TEXT_SIZE * size * multiple, 1_000_000) if multiple else pixels(tuple(amounts), size, width)
        # A block that is a picture, where the picture comes before any text.
        if made["kind"] == 4 and made["pictures"] and made["pictures"][0][2] == 0:
            picture, declared, _ = made["pictures"][0]
            line = pixels(declared, size, width)
            line = line if line > 0 else 100
            if picture in chapter["sizes"]:
                wide, high = chapter["sizes"][picture]
                line = whole(high * (width // 4), wide) if wide > width // 4 else high
            line = min(line, height)
        made["line"] = held(line, 1, 65535)
        space = held(pixels(said["before"][0], size, width), 0, 65535) if "before" in said else 0
        made["gap"] = held(space + after_before, 0, 65535)
        breaking = said["breaking"][0][0] if "breaking" in said else 0
        made["kept"] = kept
        made["together"] = bool(breaking & 4)
        made["starts"] = number == 0 or bool(breaking & 1)
        after_before = held(pixels(said["after"][0], size, width), 0, 65535) if "after" in said else 0
        kept = bool(breaking & 2)
    return blocks


# ---- What bn-pages prints ----

def run(tool: str, *arguments) -> subprocess.CompletedProcess:
    return subprocess.run([tool, *[str(argument) for argument in arguments]], capture_output=True, check=False)


def unescaped(text: bytes) -> bytes:
    out = bytearray()
    at = 0
    while at < len(text):
        if text[at] == 0x5C:
            number = text[at + 2:at + 4]
            expect(text[at + 1:at + 2] == b"x" and len(number) == 2 and all(digit in b"0123456789ABCDEF"
                                                                         for digit in number),
                   f"a backslash that begins no \\xNN in {text[:60]!r}")
            out.append(int(number, 16))
            at += 4
        else:
            out.append(text[at])
            at += 1
    return bytes(out)


def line_of(printed: bytes) -> dict:
    """A line as bn-pages prints one."""
    fields, _, text = printed.partition(b" |")
    block, begin, end, width, height, gap, flags = fields.split(b" ")
    return {"block": int(block), "begin": int(begin), "end": int(end), "width": int(width), "height": int(height),
            "gap": int(gap), "keep": b"k" in flags, "starts": b"s" in flags, "hyphen": b"h" in flags,
            "last": b"l" in flags, "text": unescaped(text)}


def lines_printed(tool: str, bundle: Path, chapter: int, geometry: list) -> list[tuple[dict, list[dict]]]:
    """Every block of a chapter with its lines, as bn-pages lines prints them."""
    ran = run(tool, "lines", bundle, chapter, *geometry)
    expect(ran.returncode == 0, f"bn-pages lines {chapter} ended with {ran.returncode}: {ran.stderr.decode()!r}")
    blocks: list[tuple[dict, list[dict]]] = []
    for printed in ran.stdout.split(b"\n")[:-1]:
        if printed.startswith(b"block "):
            words = printed.split(b" ")
            blocks.append(({"block": int(words[1]), "kind": int(words[3]), "level": int(words[5]),
                            "characters": int(words[7]), "before": int(words[9]), "lines": int(words[11])}, []))
        else:
            blocks[-1][1].append(line_of(printed))
    return blocks


def page_printed(ran: subprocess.CompletedProcess, what: str) -> dict | None:
    """A page as bn-pages prints one; nothing for "no page"."""
    expect(ran.returncode == 0, f"{what} ended with {ran.returncode}: {ran.stderr.decode()!r}")
    printed = [line for line in ran.stdout.split(b"\n")[:-1] if not line.startswith(b"#")]
    if printed == [b"no page"]:
        return None
    words = printed[0].decode().split(" ")
    expect(words[0] == "page" and words[1] == "chapter" and len(words) in (12, 13), f"{what} printed {printed[0]!r}")
    return {"chapter": int(words[2]), "from": words[4], "to": words[6], "lines": int(words[8]),
            "height": int(words[10]), "way": words[11], "text": [line_of(line) for line in printed[1:]]}


# ---- The lines, beside the chapter ----

def check_lines(what: str, printed: list, blocks: list[dict]) -> list[dict]:
    """Sets what bn-pages says of each block's lines beside the block. Returns every line of the chapter, in
    order."""
    expect(len(printed) == len(blocks), f"{what}: {len(printed)} blocks printed, and the chapter has {len(blocks)}")
    every: list[dict] = []
    for number, ((head, lines), block) in enumerate(zip(printed, blocks)):
        where = f"{what}, block {number}"
        expect((head["block"], head["kind"], head["level"], head["characters"], head["before"], head["lines"]) ==
               (number, block["kind"], block["level"], block["count"], block["before"], len(lines)),
               f"{where}: said to be {head}")
        expect(lines, f"{where}: no line")
        characters = block["characters"]
        at = 0
        for index, line in enumerate(lines):
            here = f"{where}, line {index}"
            expect(line["block"] == number and at <= line["begin"] <= line["end"] <= len(characters),
                   f"{here}: it is {line['begin']} to {line['end']}, after {at}")
            expect(not characters[at:line["begin"]].strip(b" \t\r\n"), f"{here}: something was left out before it")
            expect(line["text"] == characters[line["begin"]:line["end"]], f"{here}: its text is {line['text']!r}")
            at = line["end"]
            # As wide as its characters add up to, each in its own style; a
            # space or a tab is a space.
            width = 0
            for offset in range(line["begin"], line["end"]):
                style = block["styles"][offset]
                width += 18 + 4 * style if characters[offset] in b" \t\r" else character_width(characters[offset], style)
            expect(line["width"] == width and not line["hyphen"], f"{here}: it is {line['width']} wide, and adds up "
                   f"to {width}")
            most = block["first"] if index == 0 and "indent" in block["style"] else block["rest"]
            several = len(line["text"].decode(errors="replace")) > 1
            expect(width <= most or not several, f"{here}: it is {width} wide in a block whose lines may be {most}")
            expect(line["height"] == block["line"], f"{here}: it is {line['height']} high, and the block's lines "
                   f"are {block['line']}")
            expect(line["gap"] == (block["gap"] if index == 0 else 0), f"{here}: the space above it is {line['gap']}")
            expect(line["keep"] == (block["kept"] if index == 0 else block["together"]), f"{here}: kept, or not")
            expect(line["starts"] == (index == 0 and block["starts"]), f"{here}: starts a page, or not")
            # The last of its block, or one the text asked to end.
            asked = index + 1 < len(lines) and b"\n" in characters[line["end"]:lines[index + 1]["begin"]]
            expect(line["last"] == (index == len(lines) - 1 or asked), f"{here}: the last of its paragraph, or not")
        expect(not characters[at:].strip(b" \t\r\n"), f"{where}: something was left out after its last line")
        every += lines
    return every


# ---- Pages, chosen by adding heights up over the whole chapter ----

def height_of(lines: list[dict], first: int, end: int) -> int:
    """How high the lines from `first` up to `end` stand on a page that begins with `first`."""
    height = lines[first]["height"] + (lines[first]["gap"] if lines[first]["starts"] else 0)
    return height + sum(line["gap"] + line["height"] for line in lines[first + 1:end])


def may_begin(line: dict) -> bool:
    return line["starts"] or not line["keep"]


def down(lines: list[dict], first: int, page: int) -> int:
    """Where a page that begins with `first` ends."""
    end = first + 1
    while end < len(lines) and not lines[end]["starts"] and height_of(lines, first, end + 1) <= page:
        end += 1
    if end < len(lines) and not lines[end]["starts"] and lines[end]["keep"]:
        for place in range(end - 1, first, -1):
            if not lines[place]["keep"]:
                return place
    return end


def up(lines: list[dict], end: int, page: int) -> int:
    """Where a page that ends before `end` begins."""
    highest = end - 1
    while highest > 0 and not lines[highest]["starts"] and height_of(lines, highest - 1, end) <= page:
        highest -= 1
    for place in range(highest, end):
        if may_begin(lines[place]):
            return place
    return highest


def holding(lines: list[dict], asked: int, page: int) -> int:
    """Where the page that is to show the line `asked` begins."""
    place = asked
    while True:
        if place != asked and height_of(lines, place, asked + 1) > page:
            return asked
        if may_begin(lines[place]) or place == 0:
            return place
        place -= 1


def named(lines: list[dict], index: int) -> str:
    return "end" if index == len(lines) else f"{lines[index]['block']}+{lines[index]['begin']}"


def same_page(what: str, got: dict | None, lines: list[dict], first: int, end: int, way: str, chapter: int) -> None:
    expect(got is not None, f"{what}: no page")
    want = (chapter, named(lines, first), named(lines, end), end - first, height_of(lines, first, end), way)
    expect((got["chapter"], got["from"], got["to"], got["lines"], got["height"], got["way"]) == want,
           f"{what}: the page is {got['from']} to {got['to']}, {got['lines']} lines, {got['height']} high, filled "
           f"{got['way']}; and by adding up it is {want[1:]}")
    expect(got["text"] == lines[first:end], f"{what}: the page's lines are not the chapter's")


def check_pages(tool: str, bundle: Path, number: int, chapter: dict, lines: list[dict], blocks: list[dict],
                geometry: list, page: int, seed: int, many: int) -> int:
    """Every page going down, and `many` pages come to each of the other ways. Returns how many pages were set
    beside this script's."""
    what = f"chapter {number}" + (f" with {' '.join(str(word) for word in geometry)}" if geometry else "")
    rng = random.Random(seed)

    # Down from the first line.
    ran = run(tool, "walk", bundle, number, "--pages", *geometry)
    expect(ran.returncode == 0, f"{what}: bn-pages walk ended with {ran.returncode}: {ran.stderr.decode()!r}")
    printed = [line.decode().split(" ") for line in ran.stdout.split(b"\n") if line.startswith(b"page ")]
    first = 0
    pages = []
    while first < len(lines):
        end = down(lines, first, page)
        pages.append((first, end))
        first = end
    expect(len(printed) == len(pages), f"{what}: walk has {len(printed)} pages, and by adding up there are "
           f"{len(pages)}")
    for words, (first, end) in zip(printed, pages):
        expect(words[2:] == [str(number), "from", named(lines, first), "lines", str(end - first), "height",
                             str(height_of(lines, first, end))],
               f"{what}: walk has a page {' '.join(words)}, and by adding up it is from {named(lines, first)}, "
               f"{end - first} lines, {height_of(lines, first, end)} high")
    said = ran.stdout.decode()
    expect(f"chapters 1, pages {len(pages)}, lines {len(lines)}, blocks {len(blocks)}, " in said,
           f"{what}: what walk said: {said[-300:]!r}")
    checked = len(pages)

    # The page that begins with a line, the page above one, and the page a
    # chapter entered at a place is shown.
    some = sorted({0, len(lines) - 1} | {rng.randrange(len(lines)) for _ in range(many)})
    for asked in some:
        at = (lines[asked]["block"], lines[asked]["begin"])
        got = page_printed(run(tool, "forward", bundle, number, *at, *geometry), f"{what}: forward {at}")
        same_page(f"{what}: forward from {at}", got, lines, asked, down(lines, asked, page), "down", number)
        got = page_printed(run(tool, "back", bundle, number, *at, *geometry), f"{what}: back {at}")
        if asked == 0:
            expect(got is None or got["chapter"] < number, f"{what}: a page above the first line")
        else:
            began = up(lines, asked, page)
            if lines[began]["starts"]:
                same_page(f"{what}: back from {at}", got, lines, began, down(lines, began, page), "down", number)
            else:
                same_page(f"{what}: back from {at}", got, lines, began, asked, "up", number)
        # Entered some way into the line: by the block, and by the
        # chapter's characters.
        into = lines[asked]["begin"] + rng.randrange(max(lines[asked]["end"] - lines[asked]["begin"], 1))
        began = holding(lines, asked, page)
        got = page_printed(run(tool, "page", bundle, number, "block", at[0], into, *geometry), f"{what}: page")
        same_page(f"{what}: entered at block {at[0]}, {into}", got, lines, began, down(lines, began, page), "down",
                  number)
        got = page_printed(run(tool, "page", bundle, number, "characters", blocks[at[0]]["before"] + into, *geometry),
                           f"{what}: page")
        same_page(f"{what}: entered at character {blocks[at[0]]['before'] + into}", got, lines, began,
                  down(lines, began, page), "down", number)
        checked += 4
    began = up(lines, len(lines), page)
    got = page_printed(run(tool, "back", bundle, number, "end", *geometry), f"{what}: back end")
    same_page(f"{what}: back from the end", got, lines, began,
              down(lines, began, page) if lines[began]["starts"] else len(lines),
              "down" if lines[began]["starts"] else "up", number)
    # A place at or past the chapter's last character is its end: the same page.
    total = blocks[-1]["before"] + blocks[-1]["count"]
    got = page_printed(run(tool, "page", bundle, number, "characters", total, *geometry), f"{what}: page at the end")
    same_page(f"{what}: entered at its end", got, lines, began,
              down(lines, began, page) if lines[began]["starts"] else len(lines),
              "down" if lines[began]["starts"] else "up", number)
    got = page_printed(run(tool, "page", bundle, number, "start", *geometry), f"{what}: page start")
    same_page(f"{what}: entered at its start", got, lines, 0, down(lines, 0, page), "down", number)

    # A saved place: the last block that began at or before a place in the
    # source, and so far into it. And an id.
    line_holding = lambda block, into: max(index for index, line in enumerate(lines)  # noqa: E731
                                           if line["block"] == block and line["begin"] <= into)
    for _ in range(min(many, 6)):
        block = rng.randrange(len(blocks))
        source = blocks[block]["source"]
        last = max(index for index, other in enumerate(blocks) if other["source"] <= source)
        into = rng.randrange(blocks[last]["count"])
        began = holding(lines, line_holding(last, into), page)
        got = page_printed(run(tool, "page", bundle, number, "source", source, into, *geometry), f"{what}: page source")
        same_page(f"{what}: entered at {into} of the block that began at {source} in the source", got, lines, began,
                  down(lines, began, page), "down", number)
        checked += 1
    ids = [(name, block, into) for block, made in enumerate(blocks) for name, into in made["ids"]]
    for name, block, into in rng.sample(ids, min(len(ids), min(many, 6))):
        began = holding(lines, line_holding(block, into), page)
        got = page_printed(run(tool, "page", bundle, number, "id", name.decode(), *geometry), f"{what}: page id")
        same_page(f"{what}: entered at the id {name[:30]!r}", got, lines, began, down(lines, began, page), "down",
                  number)
        checked += 1
    return checked + 2


# ---- Chapters written for particular things ----

def plain(kind: int, source: int, text: bytes, style: dict | None = None) -> list[tuple]:
    return [("block", kind, 2 if kind == 2 else 0, source, style or {}), ("text", text), ("end",)]


def particular_chapter() -> dict:
    """Blocks whose lines and pages are known before they are set."""
    things: list[tuple] = []
    # Words all one width: a small letter is 36 and a space 18, so a word of
    # four letters and the space before it are 162, and as many go on a
    # line as its width has room for.
    even = b" ".join([b"abcd"] * 400)
    things += plain(1, 10, even)
    things += plain(1, 20, even, {"left": ((0, 320, 0), True), "right": ((64, 0, 640), False)})
    things += plain(1, 30, even, {"indent": ((200, 0, 0), True)})
    # A heading kept with what follows it, at the foot of a page; a block
    # kept together; a page that ends where the book says.
    things += plain(2, 40, b"A heading that is kept with the next", {"breaking": ((2,), True)})
    things += plain(1, 50, even)
    things += plain(1, 60, b" ".join([b"kept"] * 300), {"breaking": ((4,), False), "before": ((0, 320, 0), True),
                                                        "after": ((0, 160, 0), True)})
    things += plain(1, 70, b"After a space that both blocks asked for.", {"before": ((64, 0, 0), False)})
    things += plain(1, 80, b"A new page begins here.", {"breaking": ((1,), True), "before": ((0, 800, 0), True)})
    # A rule, a picture standing by itself whose size is known and wider
    # than the page, one whose size is only declared, and one of which
    # nothing is known; and a picture among words.
    things += [("block", 5, 0, 90, {}), ("end",)]
    things += [("block", 4, 0, 100, {}), ("picture", 7, 0, (0, 0, 0), (0, 0, 0), b"a wide picture"), ("end",)]
    things += [("block", 4, 0, 110, {}), ("picture", 8, 0, (0, 0, 0), (0, 2400, 0), b"declared"), ("end",)]
    things += [("block", 4, 0, 120, {}), ("picture", 65535, 0, (0, 0, 0), (0, 0, 0), b""), ("end",)]
    things += [("block", 4, 0, 125, {}), ("picture", 9, 0, (0, 0, 0), (0, 0, 0), b"too tall"), ("end",)]
    things += [("block", 1, 0, 130, {}), ("text", b"Words, then "), ("picture", 7, 1, (0, 0, 0), (0, 0, 0), b""),
               ("text", b" and words again, "), ("style", 1, 0, 0, False, 0, 1000), ("text", b"bold, "),
               ("style", 3, 0, 0, False, 0, 1000), ("text", b"bold and italic, "),
               ("style", 2, 0, 0, False, 0, 1000), ("text", b"italic."), ("end",)]
    # Lines the text asks to end; and a block of nothing but spaces.
    things += plain(1, 140, b"one line\nand another\n\nafter an empty one")
    things += plain(1, 150, b"   ")
    # Text larger and smaller, with lines set further apart.
    things += plain(1, 160, even[:900], {"size": ((1500,), True)})
    things += plain(1, 170, even[:900], {"size": ((750,), False), "line": ((2000, 0, 0, 0), True)})
    things += plain(1, 180, even[:900], {"line": ((0, 0, 256, 0), True)})
    # A block that is a picture, whose picture comes after text: its lines
    # are as high as text.
    things += [("block", 4, 0, 185, {}), ("text", b"A caption first: "),
               ("picture", 7, 0, (0, 0, 0), (0, 0, 0), b"then the picture"), ("end",)]
    things += [("id", b"the-last-block")]
    things += plain(1, 190, b"The last block.")
    return {"state": "whole", "reason": 0, "source_bytes": 500, "languages": [], "sizes": {7: (2000, 1000),
                                                                                           9: (100, 5000)},
            "things": things}


def check_particular(lines: list[dict], blocks: list[dict]) -> None:
    by_block = lambda number: [line for line in lines if line["block"] == number]  # noqa: E731
    # 1,760 quarters hold ten words and nine spaces (1,602) and not eleven.
    expect([len(line["text"].split()) for line in by_block(0)[:-1]] == [10] * 39, "words all one width, ten a line")
    # Margins of 40 pixels and of an em and a tenth of the page: 160 and 240
    # quarters less.
    expect(blocks[1]["rest"] == 1360 and all(len(line["text"].split()) == 8 for line in by_block(1)[:-1]),
           f"a block with margins has lines of {blocks[1]['rest']}")
    # An indent of 200 quarters leaves the first line room for nine.
    expect(len(by_block(2)[0]["text"].split()) == 9 and blocks[2]["first"] == 1560 and
           all(len(line["text"].split()) == 10 for line in by_block(2)[1:-1]), "a first line that is indented")
    expect(by_block(3)[0]["keep"] is False and by_block(4)[0]["keep"] is True, "the block after a heading is kept "
           "with it")
    expect(all(line["keep"] for line in by_block(5)[1:]) and by_block(5)[0]["gap"] == 40, "a block kept together")
    expect(by_block(6)[0]["gap"] == 16 + 20, "the space above a block is its own and the one before's")
    expect(by_block(7)[0]["starts"] and by_block(7)[0]["gap"] == 100, "a block that starts a page")
    expect([(line["begin"], line["end"], line["text"]) for line in by_block(8)] == [(0, 0, b"")], "a rule")
    # 2,000 by 1,000 on a page 440 wide is 220 high; 300 as declared; 100
    # where nothing is known; and no higher than the page.
    expect([by_block(number)[0]["height"] for number in (9, 10, 11, 12)] == [220, 300, 100, 760], "pictures' heights: "
           f"{[by_block(number)[0]['height'] for number in (9, 10, 11, 12)]}")
    expect(by_block(9)[0]["text"] == bytes([PICTURE]), "a picture standing by itself is one character")
    # A picture among words is one character of its line; and the runs after
    # it are in the made-up face's second, fourth and third styles.
    expect(bytes([PICTURE]) in by_block(13)[0]["text"] and sorted(set(blocks[13]["styles"])) == [0, 1, 2, 3],
           "a picture among words")
    expect([line["text"] for line in by_block(14)] == [b"one line", b"and another", b"", b"after an empty one"] and
           all(line["last"] for line in by_block(14)), "lines the text asks to end")
    expect([(line["begin"], line["end"]) for line in by_block(15)] == [(0, 0)], "a block of nothing but spaces")
    expect([by_block(number)[0]["height"] for number in (16, 17, 18)] == [30, 24, 32], "lines' heights: "
           f"{[by_block(number)[0]['height'] for number in (16, 17, 18)]}")
    expect(by_block(19)[0]["height"] == 20 and bytes([PICTURE]) in by_block(19)[0]["text"],
           "a block that is a picture has lines as high as the picture only where the picture comes first")


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    tool, bundle_tool = sys.argv[1], sys.argv[2]
    work = Path(sys.argv[3])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        book = epub_tool.books(work)[0]
        bundle = work / "book.bundle"
        ran = run(bundle_tool, "make", book, bundle, "--written", 1_795_000_000)
        expect(ran.returncode == 0, f"making the bundle ended with {ran.returncode}")

        def write(number: int, chapter: dict) -> None:
            described = work / f"chapter-{number}.txt"
            described.write_bytes(segment_tool.to_text(chapter))
            wrote = run(bundle_tool, "write-chapter", bundle, number, described)
            expect(wrote.returncode == 0, f"write-chapter {number} ended with {wrote.returncode}: "
                   f"{wrote.stderr.decode()!r}")

        # The table of contents of the book: an entry that names a place.
        data = bundle.read_bytes()
        offset, length = struct.unpack_from("<II", data, 48 + 36)
        contents = data[offset:offset + length]
        entries = []
        at = 0
        while at < len(contents):
            _, fragment, document, label = struct.unpack_from("<BBHH", contents, at)
            entries.append((document, contents[at + 6:at + 6 + fragment]))
            at += 6 + fragment + label
        entry, (document, fragment) = next((number, found) for number, found in enumerate(entries)
                                           if found[1] and found[0] not in (0, 1, 2, 3, 0xFFFF))

        # The last has one block more than bn-pages keeps set at a time, so
        # that a walk of it lets blocks go while their lines are in use.
        chapters = {0: segment_tool.make_chapter(11, 40, [b"fr"], True), 1: particular_chapter(),
                    3: segment_tool.make_chapter(12, 257, [], False)}
        # The chapter the entry leads to has a block with the id it names.
        led_to = segment_tool.make_chapter(13, 30, [], False)
        ends = [index for index, thing in enumerate(led_to["things"]) if thing[0] == "end"]
        led_to["things"][ends[20] + 1:ends[20] + 1] = [("id", fragment)]
        chapters[document] = led_to
        for number, chapter in chapters.items():
            write(number, chapter)

        checked = set_lines = 0
        for number, chapter in chapters.items():
            for geometry, (width, height) in (([], (1760, 760)), (["--width", 1200, "--height", 400], (1200, 400))):
                if geometry and number != 0:
                    continue
                blocks = blocks_of(chapter, width, height)
                what = f"chapter {number}" + (" on a smaller page" if geometry else "")
                lines = check_lines(what, lines_printed(tool, bundle, number, geometry), blocks)
                if number == 1:
                    check_particular(lines, blocks)
                set_lines += len(lines)
                checked += check_pages(tool, bundle, number, chapter, lines, blocks, geometry, height, number,
                                       12 if number != 1 else 30)

        # The whole book: every chapter that was imported, and none other.
        ran = run(tool, "walk", bundle)
        expect(ran.returncode == 0 and ran.stdout.startswith(b"chapters 4, pages "),
               f"walking the book: {ran.returncode} {ran.stdout[:80]!r} {ran.stderr!r}")

        # The entry of the table of contents: the page entered at its id.
        blocks = blocks_of(led_to, 1760, 760)
        lines = check_lines("the chapter an entry leads to", lines_printed(tool, bundle, document, []), blocks)
        block = next(number for number, made in enumerate(blocks) if any(name == fragment for name, _ in made["ids"]))
        asked = next(index for index, line in enumerate(lines) if line["block"] == block)
        began = holding(lines, asked, 760)
        got = page_printed(run(tool, "contents", bundle, entry), "contents")
        same_page(f"entry {entry} of the table of contents", got, lines, began, down(lines, began, 760), "down",
                  document)
        # An entry whose chapter has no such place leads to the chapter's start.
        other = next((number for number, found in enumerate(entries)
                      if found[0] == document and found[1] and found[1] != fragment), None)
        if other is not None:
            got = page_printed(run(tool, "contents", bundle, other), "contents")
            same_page("an entry that names a place the chapter has not", got, lines, 0, down(lines, 0, 760), "down",
                      document)

        # Above a chapter's first line is the last page of the chapter
        # before that was imported: chapter 2 was not.
        before = check_lines("chapter 1", lines_printed(tool, bundle, 1, []), blocks_of(chapters[1], 1760, 760))
        began = up(before, len(before), 760)
        got = page_printed(run(tool, "back", bundle, 3, 0, 0), "back from a chapter's first line")
        same_page("the page above chapter 3's first line", got, before, began,
                  down(before, began, 760) if before[began]["starts"] else len(before),
                  "down" if before[began]["starts"] else "up", 1)
        expect(page_printed(run(tool, "back", bundle, 0, 0, 0), "back from the book's first line") is None,
               "there is a page above the book's first line")

        # A saved place is found again after its chapter is written anew:
        # the page is the same, line for line.
        place = ["page", bundle, 3, "source", blocks_of(chapters[3], 1760, 760)[100]["source"], 7]
        first = run(tool, *place).stdout
        write(3, chapters[3])
        expect(first and run(tool, *place).stdout == first, "a place was not found again after its chapter was written "
               "anew")

        # What cannot be done is said.
        ran = run(tool, "page", bundle, 2, "start")
        expect(ran.returncode == 1 and b"not imported" in ran.stderr, f"a chapter not imported: {ran.stderr!r}")
        ran = run(tool, "page", bundle, 0, "id", "no-such-id")
        expect(ran.returncode == 1 and b"no such place" in ran.stderr, f"an id the chapter has not: {ran.stderr!r}")
        ran = run(tool, "page", bundle, 0, "somewhere")
        expect(ran.returncode == 2 and b"usage: " in ran.stderr, f"a place that is none: {ran.returncode}")
        ran = run(tool, "walk", bundle, 0, "--height", 0)
        expect(ran.returncode == 2, f"a page of no height: {ran.returncode}")
        ran = run(tool, "lines", book, 0)
        expect(ran.returncode == 1 and b"not a bundle" in ran.stderr, f"a file that is no bundle: {ran.stderr!r}")
        spoiled = bytearray(bundle.read_bytes())
        record = segment_tool.record_of(bytes(spoiled), 0)
        spoiled[record["segment_offset"] + 2000] ^= 0x40
        (work / "spoiled.bundle").write_bytes(spoiled)
        ran = run(tool, "walk", work / "spoiled.bundle", 0)
        expect(ran.returncode == 1 and b"damaged" in ran.stderr, f"a chapter with a byte changed: {ran.returncode} "
               f"{ran.stderr!r}")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print(f"bn-pages and Python agree about {set_lines} lines of {len(chapters)} chapters, and about {checked} pages "
          "chosen among them: down from a line, up to one, and about a place.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
