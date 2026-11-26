#!/usr/bin/env python3
"""End-to-end check of bn-css cascade --blocks, and through it of the finding of a document's blocks, run by
CTest.

The unit tests say what the blocks of documents written for them should be. Here the tool is compared with a
second working-out, in Python, that goes another way round. The firmware's code goes through a document once,
keeping a little of each open element, and says of each block what it knows as the block begins. This keeps
the whole document, as the second working-out of what each element comes to left it (styles_tool.py), lays
it out as one list of places (each element's opening and closing, each piece of text, each line end, rule
and picture), and answers every question from that list: two things are in one block if no element that is a
block opens or closes between them; the space above a block is worked out from the margins that lie between it
and the block before; an item's first block is found by looking through all of them.

The two have to agree line for line, on a document written to bring up every rule and on documents and sheets
made at random.

usage: blocks_tool.py BN_CSS WORK_DIR
"""

from __future__ import annotations

import bisect
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import css_cascade_tool  # noqa: E402
import css_tool  # noqa: E402
import styles_tool  # noqa: E402
from css_tool import Failure, expect  # noqa: E402
from styles_tool import BITS, BULLETED, NUMBERED, as_part, book, length_text  # noqa: E402

BLANK = " \t\n\r\f"
KINDS = ("paragraphs", "headings", "list_items", "rules", "cells")
KIND_NAMES = ("paragraph", "heading", "list-item", "rule", "table-cell")
WHOLE_NUMBER = re.compile(r"^[+-]?[0-9]{1,9}$")
NOTHING = (0, 0, 0)


def name_of(element) -> str:
    return css_tool.lower(element.name)


def blockish(element) -> bool:
    """Whether an element begins and ends blocks: a `br` never does, and an `hr` always."""
    name = name_of(element)
    return name != "br" and (element.came["display"] != "inline" or name == "hr")


def plus(a: tuple, b: tuple) -> tuple:
    return tuple(as_part(x + y) for x, y in zip(a, b))


def measure(length: tuple) -> int:
    """What a length comes to with 16 pixels to the em and a page 30 em wide, in 320ths of a pixel."""
    return length[0] * 80 + length[1] * 40 + length[2] * 24


def style_key(text) -> tuple:
    return (text.size, text.size_from_book, frozenset(text.bits), frozenset(text.from_book), text.family,
            text.family_from_book, text.language)


def style_text(text, languages: list[str]) -> str:
    parts = []
    if text.size != 1000 or text.size_from_book:
        parts.append(f"size {text.size}{book(text.size_from_book)}")
    if text.bits:
        parts.append("text " + " ".join(bit for bit in BITS if bit in text.bits))
    if text.from_book:
        parts.append("text-book " + " ".join(bit for bit in BITS if bit in text.from_book))
    if text.family != "unset" or text.family_from_book:
        parts.append(f"family {text.family}{book(text.family_from_book)}")
    if text.language:
        parts.append("language " + languages[text.language - 1])
    return "; ".join(parts) or "plain"


class Space:
    """The space gathered between one block and the next: margins that meet, and what keeps them apart."""

    def __init__(self) -> None:
        self.total, self.from_book = NOTHING, False
        self.above = self.below = None  # (measure, the margin, whether it is the book's)
        self.nothing_from_book = False

    def join(self, margin: tuple, from_book: bool) -> None:
        size = measure(margin)
        if size == 0:
            self.nothing_from_book = self.nothing_from_book or from_book
        elif size > 0 and (self.above is None or size > self.above[0]):
            self.above = (size, margin, from_book)
        elif size < 0 and (self.below is None or size < self.below[0]):
            self.below = (size, margin, from_book)

    def settle(self) -> None:
        if self.above is None and self.below is None:
            self.from_book = self.from_book or self.nothing_from_book
        for winner in (self.above, self.below):
            if winner is not None:
                self.total = plus(self.total, winner[1])
                self.from_book = self.from_book or winner[2]
        self.above = self.below = None
        self.nothing_from_book = False

    def add(self, length: tuple, from_book: bool) -> None:
        self.settle()
        self.total = plus(self.total, length)
        self.from_book = self.from_book or from_book


class Blocks:
    """The blocks of a document, as the lines the tool prints."""

    def __init__(self, sheets: list[bytes], document: bytes, known: list) -> None:
        self.styles = styles_tool.Styles(sheets, document, known)
        # Every place of the document, in order: (what it is, the element or None, the elements open there from
        # the outermost in, the text or None). What is nothing, and all inside it, is not there.
        self.places: list[tuple] = []
        self.opens_at: dict[int, int] = {}
        self.closes_at: dict[int, int] = {}
        for element in self.styles.root:
            self.lay_out(element, ())

        places = self.places
        self.bounds = [at for at, place in enumerate(places) if place[0] in ("open", "close") and blockish(place[1])]
        leaves = [at for at, place in enumerate(places)
                  if place[0] in ("br", "rule", "picture") or (place[0] == "text" and place[3].strip(BLANK))]
        self.leaves = leaves
        # Blocks, each the places of its content: two are in one block if no bound lies between them.
        self.blocks: list[list[int]] = []
        self.first_of_stretch: dict[int, int] = {}
        for at in leaves:
            stretch = self.stretch(at)
            if stretch in self.first_of_stretch:
                self.blocks[-1].append(at)
            else:
                self.first_of_stretch[stretch] = at
                self.blocks.append([at])

        self.number_items()
        self.counts = {"blocks": len(self.blocks), **{kind: 0 for kind in KINDS},
                       "line_ends": sum(1 for place in places if place[0] == "br"),
                       "pictures": sum(1 for place in places if place[0] == "picture"), "markers": 0,
                       "first_letters": 0, "floated_letters": 0, "begin_floated": 0, "floated": 0, "centred": 0}
        self.lines = self.tell()

    def lay_out(self, element, around: tuple) -> None:
        if element.came["display"] == "none":
            return
        inside = around + (element,)
        self.opens_at[id(element)] = len(self.places)
        self.places.append(("open", element, around, None))
        name = name_of(element)
        if name == "br":
            self.places.append(("br", element, inside, None))
        elif name == "hr":
            self.places.append(("rule", element, inside, None))
        if element.name in ("img", "IMG"):
            self.places.append(("picture", element, inside, None))
        for part in element.content:
            if isinstance(part, str):
                self.places.append(("text", None, inside, part))
            else:
                self.lay_out(part, inside)
        self.closes_at[id(element)] = len(self.places)
        self.places.append(("close", element, around, None))

    def stretch(self, at: int) -> int:
        """Which stretch between bounds a place is in."""
        return bisect.bisect_left(self.bounds, at)

    def block_open_at(self, at: int) -> bool:
        """Whether a block has begun, and not ended, where this place is."""
        first = self.first_of_stretch.get(self.stretch(at))
        return first is not None and first < at

    def number_items(self) -> None:
        """Each item's number: the next of its list, or what its own `value` says."""
        self.numbers: dict[int, int] = {}
        next_of: dict[int, int] = {}
        for what, element, around, _ in self.places:
            if what != "open":
                continue

            def asked(attribute: str):
                text = element.attributes.get(attribute, "")
                return int(text) if WHOLE_NUMBER.match(text) else None

            if name_of(element) in BULLETED | NUMBERED:
                start = asked("start")
                next_of[id(element)] = 1 if start is None else start
            elif self.is_item(element):
                lists = [outer for outer in around if name_of(outer) in BULLETED | NUMBERED]
                value = asked("value")
                number = value if value is not None else next_of[id(lists[-1])] if lists else 1
                self.numbers[id(element)] = number
                if lists:
                    next_of[id(lists[-1])] = min(number + 1, 2**31 - 1)

    def is_item(self, element) -> bool:
        """An item of a list: by how it is shown, whatever it is called; but a list is not one, nor a `br`."""
        return element.came["display"] == "list-item" and name_of(element) not in BULLETED | NUMBERED | {"br"}

    def is_cell(self, element) -> bool:
        return name_of(element) in ("td", "th") and blockish(element)

    def has_something_in_it(self, element) -> bool:
        """Whether a block began inside an element, or a height was added there for an element with nothing."""
        opened, closed = self.opens_at[id(element)], self.closes_at[id(element)]
        at = bisect.bisect_right(self.leaves, opened)
        if at < len(self.leaves) and self.leaves[at] < closed:
            return True
        return any(place[0] == "close" and blockish(place[1]) and "height" in place[1].came["sizes"] and
                   not self.has_something_in_it(place[1]) for place in self.places[opened + 1:closed])

    def space_between(self, after: int, before: int) -> Space:
        space = Space()
        for what, element, _, _ in self.places[after + 1:before]:
            if what not in ("open", "close") or not blockish(element):
                continue
            came = element.came
            if what == "open":
                space.join(came["space"]["margin"][0], came["from_book"]["margin"][0])
                if any(came["space"]["padding"][0]):
                    space.add(came["space"]["padding"][0], came["from_book"]["padding"][0])
            else:
                if "height" in came["sizes"] and not self.has_something_in_it(element):
                    space.add(came["sizes"]["height"], True)
                if any(came["space"]["padding"][2]):
                    space.add(came["space"]["padding"][2], came["from_book"]["padding"][2])
                space.join(came["space"]["margin"][2], came["from_book"]["margin"][2])
        space.settle()
        return space

    def record(self, number: int, last_begun: int, last_of_text: int) -> tuple[list[str], object]:
        """The lines for a block as it begins, and the floated element its text begins in, if any. `last_begun`
        is where the block before it began, and `last_of_text` where the last one that is not a rule did."""
        places, counts = self.places, self.counts
        first = self.blocks[number][0]
        what, _, around, _ = places[first]
        rule = what == "rule"
        blocks_around = [element for element in around if blockish(element)]
        container = blocks_around[-1] if blocks_around else None

        # Its kind, and the item whose marker it has: every item and cell it is the first block of.
        kind, level, item = "paragraph", 0, None
        if rule:
            kind = "rule"
        else:
            named = False
            for element in reversed(around):
                name = name_of(element)
                awaits = self.first_blocks.get(id(element)) == number
                if not named and re.fullmatch("h[1-6]", name):
                    kind, level, named = "heading", int(name[1]), True
                if self.is_item(element) and awaits:
                    if not named:
                        kind, named = "list-item", True
                    item = item or element
                if self.is_cell(element) and awaits and not named:
                    kind, named = "table-cell", True
        counts[KINDS[KIND_NAMES.index(kind)]] += 1
        out = [f"B {kind}" + (f" {level}" if level else "")]

        if container is not None:
            lines, size = container.came["lines"], container.came["text"]
            if lines.align != "unset" or lines.align_from_book:
                out.append(f"R align: {lines.align}{book(lines.align_from_book)}")
            if lines.right_to_left:
                out.append("R right-to-left")
            if lines.spaces_as_written:
                out.append("R spaces-as-written")
            if size.size != 1000 or size.size_from_book:
                out.append(f"R size: {size.size}{book(size.size_from_book)}")
            if lines.line_height is not None:
                how, much = lines.line_height
                out.append("R line-height: " + (f"x{much}" if how == "x" else length_text(much)) +
                           book(lines.line_height_from_book))
            elif lines.line_height_from_book:
                out.append("R line-height: normal book")
            if lines.indent is not None:
                out.append(f"R indent: {length_text(lines.indent)}{book(lines.indent_from_book)}")

        # To its sides, the margins and the padding of every block it is inside.
        for side, word in ((3, "left"), (1, "right")):
            total, from_book = NOTHING, False
            for element in blocks_around:
                came = element.came
                total = plus(plus(total, came["space"]["margin"][side]), came["space"]["padding"][side])
                from_book = from_book or came["from_book"]["margin"][side] or came["from_book"]["padding"][side]
            if any(total) or from_book:
                out.append(f"R margin-{word}: {length_text(total)}{book(from_book)}")

        space = self.space_between(last_begun, first)
        if any(space.total) or space.from_book:
            out.append(f"R space-before: {length_text(space.total)}{book(space.from_book)}")
        if any(blockish(element) and element.came["break_before" if opening == "open" else "break_after"]
               for opening, element, _, _ in places[last_begun + 1:first] if opening in ("open", "close")):
            out.append("R break-before")
        if container is not None:
            came = container.came
            if came["keep"] or came["keep_from_book"]:
                out.append(("R keep-with-next" if came["keep"] else "R keep-with-next: no") +
                           book(came["keep_from_book"]))
            if came["keep_together"]:
                out.append("R keep-together")
        if any(element.came["floated"] for element in blocks_around):
            out.append("R floated")
            counts["floated"] += 1
        if container is not None and "width" in container.came["sizes"] and container.came["auto"][1] and \
                container.came["auto"][3]:
            out.append("R centred: " + length_text(container.came["sizes"]["width"]))
            counts["centred"] += 1
        quotes = min(255, sum(1 for element in blocks_around if name_of(element) == "blockquote"))
        if quotes:
            out.append(f"R quote-depth: {quotes}")
        if around and around[-1].came["lines"].lists:
            out.append(f"R list-depth: {around[-1].came['lines'].lists}")
        if item is not None:
            lines = item.came["lines"]
            out.append(f"R marker: {lines.marker} {self.numbers[id(item)]}{book(lines.marker_from_book)}")
            counts["markers"] += 1

        holder = None
        if not rule:
            # The first letter's style of the last element that has one and opened since a block of text began,
            # if this block is inside it.
            with_letter = [element for opening, element, _, _ in places[last_of_text + 1:first]
                           if opening == "open" and blockish(element) and element.came["letter"] is not None]
            if with_letter and self.closes_at[id(with_letter[-1])] > first:
                came = with_letter[-1].came
                out.append(("R first-letter floated: " if came["letter_floated"] else "R first-letter: ") +
                           style_text(came["letter"], self.styles.languages))
                counts["first_letters"] += 1
                counts["floated_letters"] += 1 if came["letter_floated"] else 0
            # It begins floated if it begins inside something that is part of a line, floated, and opened where
            # no block was open.
            floats = [element for element in around
                      if not blockish(element) and name_of(element) != "br" and element.came["floated"] and
                      not self.block_open_at(self.opens_at[id(element)])]
            if floats:
                out.append("R begins-floated")
                counts["begin_floated"] += 1
                holder = floats[0]
        return out, holder

    def tell(self) -> list[str]:
        places = self.places
        # For each item and cell, the first block of text that begins inside it.
        self.first_blocks: dict[int, int] = {}
        for number, block in enumerate(self.blocks):
            if places[block[0]][0] != "rule":
                for element in places[block[0]][2]:
                    self.first_blocks.setdefault(id(element), number)

        begins = {block[0]: number for number, block in enumerate(self.blocks)}
        ends = set()
        for block in self.blocks:
            at = bisect.bisect_right(self.bounds, block[-1])
            if at < len(self.bounds):
                ends.add(self.bounds[at])
        float_ends = set()
        out: list[str] = []
        last_begun = last_of_text = -1
        style = None
        for at, (what, element, around, text) in enumerate(places):
            if at in ends:
                out.append("E")
            if at in begins:
                lines, holder = self.record(begins[at], last_begun, last_of_text)
                out.extend(lines)
                last_begun = at
                if what != "rule":
                    last_of_text = at
                # The floated text ends where its element closes, if the block is still open there.
                if holder is not None and self.stretch(self.closes_at[id(holder)]) == self.stretch(at):
                    float_ends.add(self.closes_at[id(holder)])
                style = None
            if what == "br":
                out.append("L")
            elif what == "picture":
                out.append("P")
            elif what == "text" and (at in begins or self.block_open_at(at)):
                now = around[-1].came["text"]
                if style is None or style_key(now) != style:
                    out.append("S " + style_text(now, self.styles.languages))
                    style = style_key(now)
            elif what == "close" and at in float_ends:
                out.append("F")
        return out


def blocks_lines(sheets: list[bytes], document: bytes, known: list) -> tuple[list[bytes], int, dict, dict, int]:
    """What `bn-css cascade --blocks` prints for a document given these sheets; how many elements; what was
    counted of the blocks and what was left out, by the names the tool's one line has; and how many
    languages."""
    blocks = Blocks(sheets, document, known)
    lines = [line.translate(css_tool.ESCAPES).encode("latin-1") for line in blocks.lines]
    return lines, blocks.styles.elements, blocks.counts, blocks.styles.left_out, len(blocks.styles.languages)


# ---- What the test makes.

SHEET = b"""body { margin: 0 5%; font-family: Georgia, serif; line-height: 1.4 }
h1 { margin: 2em 0 1em; text-align: center; page-break-before: always }
h2 { page-break-after: avoid; margin: 1.5em 0 0.5em } h3 { break-after: always }
p { margin: 0; text-indent: 1.5em } p.first { text-indent: 0; margin-top: 1em }
p.first::first-letter { float: left; font-size: 3em; font-weight: bold }
.drop { float: left; font-size: 3em } .verse span { display: block; margin-left: 2em; text-indent: -1em }
.note { margin: 1em 2em; padding: 0.5em 1em; font-size: 0.9em } .spacer { height: 2em; margin: 1em 0 }
.pull { margin: -0.5em 0 1em } .centre { width: 60%; margin: 2em auto } .side { float: right; width: 30% }
.keep { break-inside: avoid } .gone { display: none } .break { page-break-after: always }
blockquote { margin: 1em 40px; font-style: italic } blockquote p { margin: 0.5em 0 }
ol.roman { list-style-type: upper-roman } li.plain { list-style-type: none } .item { display: list-item }
td, th { padding: 2px 4px } hr { margin: 1em 25%; height: 1px } .big { font-size: 150% }
.rtl { text-align: right } pre { margin: 1em 0 1em 2em } .inl { display: inline } .pad { padding: 1em 0 }
"""

DOCUMENT = b"""<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en"><head><title>A title</title></head><body>
<h1>The first <i>heading</i></h1>
<p class="first">It began, as these things do, <b>slowly</b>. And then<br/>a line of its own.</p>
<p>A second paragraph, <span class="big">louder <i>here</i></span> and <span xml:lang="fr">en fran&#231;ais</span>.</p>
<p><span class="drop">T</span>he letter is dropped by a floated element. <span class="drop">Not</span> this one.</p>
<p><b><span class="drop">&#8220;A</span></b> floated element inside another, at a paragraph's start.</p>
<div>Text straight in a division. <p>Then a paragraph.</p> And text after it. <div></div> And more.</div>
<div class="verse"><p><span>A line of verse,</span> <span>and another,</span><span>and a third.</span></p></div>
<div class="note"><p>A note.</p><p>Its second paragraph.</p></div>
<div class="spacer"></div><div class="spacer"><span> </span></div><div class="spacer">Not empty.</div>
<p class="pull">Pulled up.</p><div class="pad"><p class="pull">After padding.</p></div>
<p class="centre">Centred by its margins.</p><div class="side"><p>Floated to the side.</p></div>
<h2>A heading kept with what follows</h2><p class="keep">Kept together.</p>
<h3>After this a page ends</h3><p>On a new page.</p><div class="break"><p>One.</p></div><div><div><p>Two.</p></div></div>
<blockquote><p>Quoted.</p><blockquote><p>Quoted in a quotation.</p></blockquote>And after.</blockquote>
<ul><li>One</li><li><p>Two</p><p>Two again</p></li><li><ul><li>Inside, first</li></ul>Three, after its list</li>
<li><h3>A heading in an item</h3><p>And its text.</p></li><li><hr/>After a rule.</li><li class="gone">Gone</li></ul>
<ol start="5" class="roman"><li>Five</li><li value="9">Nine</li><li>Ten</li><li class="plain">Plain</li>
<li value="x">Twelve</li><ol><li>One of its own</li></ol></ol>
<ol start="-2"><li>Minus two</li><li>Minus one</li></ol><ol start="1000000000"><li>One</li></ol>
<p class="item">An item by its display</p><li>An item in no list</li><li value="7">With a value</li>
<table><caption>A table</caption><tr><th>Head</th><td>Cell <p>and a paragraph</p> and more</td></tr>
<tr><td><ul><li>An item in a cell</li></ul></td><td></td><td class="inl">In line</td></tr></table>
<hr/><hr class="gone"/><p>After the rules.</p>
<pre>  kept
    as written
</pre><p><br/>After an empty line.</p><p>One<br/><br/>Two</p><div><br/></div>
<p>A picture <img src="a.png" alt=""/> in a line.</p><div><img src="b.png" alt=""/></div>
<p><span class="drop"><img src="c.png" alt=""/></span>A picture as the first letter.</p>
<p class="rtl" dir="rtl">Right to left.</p><p align="center">Centred by its attribute.</p>
<p>   </p><p> <span> </span> </p><p class="gone">Nothing <b>here</b></p><p hidden="">Nor here</p>
<p>Before <span class="gone">hidden <br/></span>after.</p><div class="first">A division with a first letter.</div>
<div class="first"><div class="first"></div>The inner one closed.</div><p class="first"><b>B</b>old first.</p>
<section><p>In a section.</p></section><i>Text in no block of its own.</i> <b>More of it.</b>
<h4>A <small>small</small> heading</h4><H5>Capitals</H5><h6 class="inl">In line</h6> beside it.
</body></html>
"""


def random_document(rng: random.Random) -> bytes:
    names = ["div", "p", "span", "h1", "h3", "ul", "ol", "li", "blockquote", "i", "b", "sup", "small", "u", "code",
             "pre", "table", "tr", "td", "th", "center", "dl", "dd", "hr", "br", "img", "a", "DIV", "menu", "rt",
             "section", "nobody", "caption", "BR", "IMG", "H2"]
    words = ["", "", " ", "\n", " words ", "x", " and more\tof them ", "&#160;", "&amp;", "\n  \n"]

    def element(depth: int) -> str:
        name = rng.choice(names)
        text = "<" + name
        if rng.random() < 0.5:
            text += " class='" + rng.choice(["a", "b", "c", "a b", "c a", ""]) + "'"
        if rng.random() < 0.08:
            text += " lang='" + rng.choice(["en", "fr", "de", ""]) + "'"
        if rng.random() < 0.06:
            text += " align='" + rng.choice(["left", "right", "center", "justify"]) + "'"
        if rng.random() < 0.12:
            text += " " + rng.choice(["start", "value"]) + "='" + rng.choice(["3", "-1", "007", "+4", "x", "",
                                                                             "1234567890", "999999999"]) + "'"
        if rng.random() < 0.08:
            text += " type='" + rng.choice(["1", "a", "I", "disc", "square"]) + "'"
        if rng.random() < 0.03:
            text += " hidden=''"
        if rng.random() < 0.12:
            text += " style='" + rng.choice(["font-size: 2em; margin: 1em", "display: block", "float: left",
                                             "display: inline", "height: 1em", "margin: -1em 0 2px",
                                             "padding: 3px 0", "width: 10em; margin: 0 auto",
                                             "page-break-after: always", "display: list-item"]) + "'"
        text += ">" + rng.choice(words)
        for _ in range(rng.choice([0, 0, 1, 2, 3]) if depth < 7 else 0):
            text += element(depth + 1) + rng.choice(words)
        return text + "</" + name + ">"

    return ("<html" + rng.choice(["", " lang='en'"]) + "><head><title>t</title></head><body class='" +
            rng.choice(["", "a", "b"]) + "'>" + rng.choice(words) +
            "".join(element(2) + rng.choice(words) for _ in range(rng.randrange(1, 7))) + "</body></html>").encode()


def compare(tool: str, work: Path, sheets: list[bytes], document: bytes, what: str, known: list) -> tuple[int, dict]:
    """Runs `bn-css cascade --blocks` on the document and the sheets; returns how many lines agreed, and what
    was counted."""
    names = []
    for index, sheet in enumerate(sheets):
        names.append(str(work / f"sheet-{index}.css"))
        Path(names[-1]).write_bytes(sheet)
    (work / "document.xhtml").write_bytes(document)
    result = subprocess.run([tool, "cascade", "--blocks", str(work / "document.xhtml")] + names, capture_output=True,
                            check=False)
    expect(result.returncode == 0, f"{what}: bn-css ended with {result.returncode}: {result.stderr!r}")
    got = result.stdout.split(b"\n")
    expect(got[-1] == b"", f"{what}: the last line is not ended")
    got.pop()
    expected, elements, counts, left_out, languages = blocks_lines(sheets, document, known)
    for number, (mine, theirs) in enumerate(zip(got, expected)):
        if mine != theirs:
            raise Failure(f"{what}: line {number + 1} is {mine!r}, the second working-out has {theirs!r}; "
                          f"before that came {got[max(0, number - 12):number]!r}")
    expect(len(got) == len(expected), f"{what}: {len(got)} lines, the second working-out has {len(expected)}; "
           f"the tool's go on {got[len(expected):][:6]!r} and the other's {expected[len(got):][:6]!r}")

    # The one line that sums it up: the same checksum, the same counts, and no call that did too much work.
    result = subprocess.run([tool, "cascade", "--blocks", "--sum", str(work / "document.xhtml")] + names,
                            capture_output=True, check=False)
    said = result.stdout.decode().split()
    expect(result.returncode == 0 and said[:4] == ["blocks", f"{css_cascade_tool.checksum(expected):08x}",
                                                   f"elements={elements}", f"lines={len(expected)}"],
           f"{what}: --sum says {result.stdout!r}; the second working-out has "
           f"{css_cascade_tool.checksum(expected):08x}, {elements} elements, {len(expected)} lines")
    figures = {key: int(value) for key, value in (item.split("=") for item in said[4:])}
    theirs = dict(counts, **left_out, languages=languages, work_over=0)
    expect({key: figures.get(key) for key in theirs} == theirs,
           f"{what}: --sum says {figures}; the second working-out has {theirs}")
    return len(got), counts


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tool = str(Path(sys.argv[1]).resolve())
    work = Path(sys.argv[2]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    try:
        known = css_tool.known_families(tool)
        lines, counts = compare(tool, work, [SHEET], DOCUMENT, "the document written for it", known)
        expect(lines > 500, f"only {lines} lines: not much of a test")
        expect(all(counts.values()), f"the document written for it has none of something: {counts}")
        lines += compare(tool, work, [], DOCUMENT, "the document with no sheet", known)[0]

        rng = random.Random(20261126)
        rounds = 0
        seen = {key: 0 for key in counts}
        while rounds < 200:
            sheets = [styles_tool.random_sheet(rng) for _ in range(rng.randrange(0, 3))]
            count, counted = compare(tool, work, sheets, random_document(rng), f"made at random (round {rounds})", known)
            lines += count
            rounds += 1
            for key, value in counted.items():
                seen[key] += value
        expect(all(seen.values()), f"the documents made at random have none of something: {seen}")

        expect(subprocess.run([tool, "cascade", "--blocks", str(work / "no-such-file.xhtml")], capture_output=True,
                              check=False).returncode == 1, "a missing file should be refused")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"bn-css cascade --blocks agrees with the second working-out: {lines} lines, of a document written for "
          f"it and {rounds} made at random")
    return 0


if __name__ == "__main__":
    sys.exit(main())
