#!/usr/bin/env python3
"""End-to-end check of `bn-bundle import`, and through it of the importing of a chapter, run by CTest.

The tool imports the chapters of a book into its bundle with the code the firmware imports with
(epub/chapter_import.h), and `bn-bundle chapter` prints what a chapter's segment then holds. Here each chapter is
worked out a second way, in Python, and the two have to agree line for line.

The firmware's code goes through a document once, giving a writer each piece of text as it comes and holding
back only a space, a few bytes of white space and a block's first characters. This keeps the whole document, as
the second workings-out of what each element comes to and of where its blocks are left it (styles_tool.py,
blocks_tool.py), gathers everything that lies in a block into one list, and answers from the list: which white
space becomes a space is settled by what lies to either side of it in its line, whether a block begins with a
dropped capital by what its first things are, and where an id stands by what was written before it.

The book is one the test makes: a document written to bring up every rule of importing, the one the test of
blocks was written for, documents and sheets made at random, a document that goes wrong part way, one that is
not XML at all, one with nothing in it, a file the book has not got, and a picture standing in the spine.

usage: import_tool.py BN_BUNDLE BN_CSS WORK_DIR
"""

from __future__ import annotations

import html.entities
import random
import shutil
import subprocess
import sys
import xml.parsers.expat
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import blocks_tool  # noqa: E402
import css_cascade_tool  # noqa: E402
import css_tool  # noqa: E402
import sheets_tool  # noqa: E402
import styles_tool  # noqa: E402
from blocks_tool import blockish, name_of  # noqa: E402
from css_tool import Failure, expect  # noqa: E402
from styles_tool import ALIGNS, BITS, MARKERS  # noqa: E402

BLANK = " \t\n\r\f"
FAMILIES = ("unset", "serif", "sans", "mono", "script", "other")
MOST_HELD = 64       # White space as written that is kept from before a block's first text.
MOST_FIRST = 64      # The longest text standing for a picture that may be a dropped capital.
MOST_NAME = 255      # The longest id, and the longest text standing for a picture.
NO_PICTURE = 65535
PLAIN = (0, 0, 0, False, 0, 1000)  # How a block's text is set before any style item.
MARKS = {0x22, 0x27, 0xA1, 0xAB, 0xBB, 0xBF, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2039, 0x203A}
REASONS = ("none", "no-file", "unreadable", "not-XML", "no-memory")


def is_letter(ch: str) -> bool:
    """A letter of the Latin, Greek or Cyrillic alphabets, as importing counts them."""
    code = ord(ch)
    return ("A" <= ch <= "Z" or "a" <= ch <= "z" or (0xC0 <= code <= 0x24F and code not in (0xD7, 0xF7)) or
            (0x370 <= code <= 0x3FF and code not in (0x37E, 0x387)) or
            (0x400 <= code <= 0x52F and not 0x482 <= code <= 0x489) or 0x1E00 <= code <= 0x1FFF)


def printable(data: bytes) -> str:
    return "".join(f"\\x{byte:02X}" if byte < 0x20 or byte == 0x5C else chr(byte) for byte in data)


def cut(data: bytes, most: int) -> bytes:
    """As much of some text as is kept, ending where a character does."""
    if len(data) <= most:
        return data
    at = most
    while at > 0 and data[at] & 0xC0 == 0x80:
        at -= 1
    return data[:at]


def numbers(length: tuple) -> str:
    return ",".join(str(amount) for amount in length)


def star(from_book: bool) -> str:
    return "*" if from_book else ""


class Chapter:
    """A document imported: the lines `bn-bundle chapter` prints for it, and what `bn-bundle import` counts."""

    def __init__(self, sheets: list[bytes], document: bytes, known: list, language: bytes) -> None:
        self.entities_left_out = 0

        def entity(name: str) -> str:
            # One of HTML's names is its character; any other is left out.
            code = 0x27 if name == "apos" else html.entities.name2codepoint.get(name)
            if code is None:
                self.entities_left_out += 1
                return ""
            return chr(code)

        self.blocks = blocks_tool.Blocks(sheets, document, known, entity)
        self.styles = self.blocks.styles
        self.own = css_tool.lower(css_cascade_tool.bytes_as_text(language.decode("utf-8")))
        self.held_left_out = self.ids_left_out = self.dropped = self.characters = 0
        # How many times text was set of which the book said what it is not.
        self.said_not = 0

        # Everything of the document in its order, what is nothing among it for its ids and its tags.
        self.events: list[tuple] = []
        for element in self.styles.root:
            self.go(element, False)

        # Each block as what lies in it; then what is written of each.
        self.made: list[dict] = []
        self.gather()
        written = [self.write(block) for block in self.made]

        # The languages, in the order a style item first names each.
        named: list[int] = []
        for _, items, _ in written:
            for item in items:
                if item[0] == "style" and item[1][4] and item[1][4] not in named:
                    named.append(item[1][4])
        self.lines = [f"chapter whole 0 {len(document)}"]
        self.lines += ["language " + printable(self.styles.languages[number - 1].encode("latin-1")) for number in named]
        for block, (head, items, ids) in zip(self.made, written):
            self.lines += self.tell(block, head, items, ids, named)
        self.lines += ["id " + printable(name) for name in self.after]

        left = self.styles.left_out
        self.figures = {"blocks": len(self.made), "characters": self.characters, "dropped": self.dropped,
                        "entities_left_out": self.entities_left_out, "held_left_out": self.held_left_out,
                        "too_deep": left["styles_too_deep"], "languages": len(self.styles.languages),
                        "left_out": (self.entities_left_out + self.held_left_out + self.ids_left_out +
                                     left["styles_too_deep"] + left["languages_left_out"])}

    def go(self, element, hidden: bool) -> None:
        """One element and all inside it, as events. The places are those the second working-out of blocks laid
        out, counted the same way."""
        events, blocks = self.events, self.blocks
        hidden = hidden or element.came["display"] == "none"
        events.append(("tag", element.at))
        at = None if hidden else blocks.opens_at[id(element)]
        if not hidden:
            if at in blocks.ends:
                events.append(("end",))
            if blockish(element):
                events.append(("bound",))
        attributes = element.attributes
        name = attributes["id"] if "id" in attributes else attributes.get("name", "") if name_of(element) == "a" else ""
        if name:
            events.append(("id", name.encode("utf-8")))
        if hidden:
            for child in element.children:
                self.go(child, True)
            events.append(("tag", element.end_at))
            return

        at += 1
        if name_of(element) in ("br", "hr"):
            if at in blocks.begins:
                events.append(("begin", blocks.begins[at]))
            if name_of(element) == "br":
                events.append(("br",))
            at += 1
        if element.name in ("img", "IMG"):
            if at in blocks.begins:
                events.append(("begin", blocks.begins[at]))
            events.append(("picture", cut(attributes.get("alt", "").encode("utf-8"), MOST_NAME), element))
            at += 1
        for part in element.content:
            if isinstance(part, str):
                if at in blocks.begins:
                    events.append(("begin", blocks.begins[at]))
                events.append(("text", part, element))
                at += 1
            else:
                self.go(part, False)
                if part.came["display"] != "none":
                    at = blocks.closes_at[id(part)] + 1
        expect(at == blocks.closes_at[id(element)], "the places of the two workings-out do not agree")
        events.append(("tag", element.end_at))
        if at in blocks.float_ends:
            events.append(("float ends",))
        if at in blocks.ends:
            events.append(("end",))
        if blockish(element):
            events.append(("bound",))

    def gather(self) -> None:
        """Puts what lies in each block into its list: its characters, each with whether white space is as
        written where it stands and the element whose text it is; line ends, pictures, ids, and the end of the
        floated text it began with."""
        block = None
        tag = 0
        before: list[str] = []   # White space as written since the last block or boundary.
        waiting: list[bytes] = []  # Ids met between blocks: they are at the start of the next.
        for event in self.events:
            what = event[0]
            if what == "tag":
                tag = event[1]
            elif what == "bound":
                before = []
            elif what == "end":
                block = None
                before = []
            elif what == "begin":
                block = {"fact": self.blocks.facts[event[1]], "source": tag, "before": before, "ids": waiting,
                         "things": []}
                self.made.append(block)
                before, waiting = [], []
            elif what == "id":
                if len(event[1]) > MOST_NAME:
                    self.ids_left_out += 1
                elif block is None:
                    waiting.append(event[1])
                else:
                    block["things"].append(event)
            elif block is not None and block["fact"]["kind"] == "rule":
                continue  # A rule has nothing in it.
            elif what == "text":
                _, text, element = event
                as_written = element.came["lines"].spaces_as_written
                if block is not None:
                    block["things"] += [("character", ch, as_written, element) for ch in text]
                elif as_written:
                    before += list(text)
            elif block is not None:
                block["things"].append(event)
        self.after = waiting

    def run_of(self, text, size: int) -> tuple:
        """How text is set, as a style item says it: its size as so much of its block's, and its language as
        none if it is the book's own."""
        language = text.language
        if language and css_tool.lower(self.styles.languages[language - 1]) == self.own:
            language = 0
        # A bit is the book's only where it is set: of what the book said text is not, nothing is kept.
        self.said_not += 1 if set(text.from_book) - set(text.bits) else 0
        return (sum(1 << BITS.index(bit) for bit in text.bits),
                sum(1 << BITS.index(bit) for bit in text.from_book if bit in text.bits),
                FAMILIES.index(text.family), text.family_from_book, language,
                min(65535, (text.size * 1000 + size // 2) // size) if size else 1000)

    @staticmethod
    def held(blanks: list[str]) -> tuple[list[str], int]:
        """What is kept of the white space as written before a block's first text: what follows the last line
        end, and of that so much; and how much more there was."""
        if "\n" in blanks:
            blanks = blanks[len(blanks) - blanks[::-1].index("\n"):]
        return blanks[:MOST_HELD], max(0, len(blanks) - MOST_HELD)

    def write(self, block: dict) -> tuple[str, list[tuple], list[tuple]]:
        """What is written of a block: the line it begins with, its items, and its ids with how many characters
        into it each is."""
        fact = block["fact"]
        container = fact["container"]
        size = container.came["text"].size if container is not None else 1000
        things = block["things"]

        # Its lines: what lies between the line ends.
        solid = [at for at, thing in enumerate(things)
                 if thing[0] in ("picture", "br") or (thing[0] == "character" and thing[1] not in BLANK)]
        first = solid[0] if solid else len(things)
        kept, over = self.held(block["before"] + [thing[1] for thing in things[:first]
                                                  if thing[0] == "character" and thing[2]])

        # What is written, in the order it is: ("text", how it is set, its bytes), ("picture", the text that
        # stands for it), ("id", its name), ("float ends",). White space that is combined becomes a space where
        # something is written before it in its line and something after, and is written when that is.
        out: list[tuple] = []
        space = None
        begun = False  # Something has been written in this line.
        for at, thing in enumerate(things):
            what = thing[0]
            if what in ("id", "float ends"):
                out.append(thing)
                continue
            if what == "br":
                out.append(("text", None, b"\n"))
                space, begun, kept, over = None, False, [], 0
                continue
            if at < first:
                continue  # White space before the block's first text: what is kept of it has been worked out.
            if what == "character" and thing[1] in BLANK and not thing[2]:
                if begun and space is None:
                    space = self.run_of(thing[3].came["text"], size)
                continue
            run = self.run_of(thing[2 if what == "picture" else 3].came["text"], size)
            if space is not None:
                out.append(("text", space, b" "))
                space = None
            # What was held is written with the first thing that is, and what there was no room to hold is
            # then left out.
            out += [("text", run, blank.encode()) for blank in kept]
            self.held_left_out += over
            kept, over = [], 0
            out.append(("picture", thing[1]) if what == "picture" else ("text", run, thing[1].encode("utf-8")))
            begun = True

        # A capital that is dropped: the block's first characters, by the first things written.
        settled, dropped = 0, 0
        written = [at for at, thing in enumerate(out) if thing[0] != "id"]
        marks: list[int] = []
        if fact["kind"] != "rule" and (fact["holder"] is not None or fact["letter"] is not None):
            floated = fact["holder"] is not None
            settled = len(out)
            picture = letter = False
            for at in written:
                thing = out[at]
                if thing[0] == "float ends":
                    if floated:
                        dropped = 1 if picture else len(marks) if letter else 0
                        settled = at
                        break
                    continue
                fits = False
                if thing[0] == "text" and thing[1] is not None and not picture and not letter:
                    ch = thing[2].decode("utf-8")
                    is_one = is_letter(ch) or (not floated and "0" <= ch <= "9")
                    if (is_one or (ord(ch) in MARKS and len(marks) < 2)) and \
                            (not marks or out[marks[0]][1] == thing[1]):
                        fits = True
                        marks.append(at)
                        letter = is_one
                elif thing[0] == "picture" and floated and not marks and not picture and len(thing[1]) <= MOST_FIRST:
                    fits = picture = True
                if not fits:
                    settled = at
                    break
                if letter and not floated:
                    # The first letter, with the marks before it, in the style its rule gives.
                    own, rule_floats = fact["letter"]
                    run = self.run_of(own, size)
                    for mark in marks:
                        out[mark] = ("text", run, out[mark][2])
                    dropped = len(marks) if rule_floats else 0
                    settled = at + 1
                    break
        self.dropped += 1 if dropped else 0

        # The items, and where each id stands: at the block's start until its first characters are settled.
        items: list[tuple] = []
        ids: list[tuple] = [(0, name) for name in block["ids"]]
        into = 0
        run = PLAIN
        for at, thing in enumerate(out):
            if thing[0] == "id":
                ids.append((into if at >= settled else 0, thing[1]))
            elif thing[0] == "picture":
                items.append(("picture", thing[1]))
                into += 1
            elif thing[0] == "text":
                if thing[1] is not None and thing[1] != run:
                    run = thing[1]
                    items.append(("style", run))
                if items and items[-1][0] == "text":
                    items[-1] = ("text", items[-1][1] + thing[2])
                else:
                    items.append(("text", thing[2]))
                into += len(thing[2])
        self.characters += into or 1
        return self.head(block, dropped), items, ids

    def head(self, block: dict, dropped: int) -> str:
        """The line a block begins with: its kind, a heading's level, where it began, and its style's fields."""
        fact = block["fact"]
        container = fact["container"]
        fields = []
        breaking = (1 if fact["breaks"] else 0) | (16 if fact["floated"] else 0) | (64 if fact["centred"] else 0) | \
            (min(dropped, 3) << 8)
        own = keep_from_book = False
        if container is not None:
            came = container.came
            lines, size = came["lines"], came["text"]
            if lines.align != "unset" or lines.align_from_book:
                align = ALIGNS.index(lines.align) if lines.align != "unset" else 4
                fields.append(f"align={align}{star(lines.align_from_book)}")
            if lines.right_to_left:
                fields.append("direction=1")
            if size.size != 1000 or size.size_from_book:
                fields.append(f"size={size.size}{star(size.size_from_book)}")
            if lines.line_height is not None:
                how, much = lines.line_height
                fields.append("line=" + (f"{much},0,0,0" if how == "x" else "0," + numbers(much)) +
                              star(lines.line_height_from_book))
            elif lines.line_height_from_book:
                fields.append("line=0,0,0,0*")
            if lines.indent is not None:
                fields.append(f"indent={numbers(lines.indent)}{star(lines.indent_from_book)}")
            breaking |= (2 if came["keep"] else 0) | (4 if came["keep_together"] else 0) | \
                (8 if lines.spaces_as_written else 0)
            keep_from_book = came["keep_from_book"]
            # The bits are the book's unless one of them is the element's own.
            own = (came["keep"] and not keep_from_book) or lines.spaces_as_written
        for side in ("left", "right"):
            total, from_book = fact["sides"][side]
            if any(total) or from_book:
                fields.append(f"{side}={numbers(total)}{star(from_book)}")
        space = fact["space"]
        if any(space.total) or space.from_book:
            fields.append(f"before={numbers(space.total)}{star(space.from_book)}")
        if breaking or keep_from_book:
            fields.append(f"breaking={breaking}{star(not own)}")
        if fact["quotes"] or fact["lists"]:
            fields.append(f"depths={fact['quotes']},{fact['lists']}")
        if fact["item"] is not None:
            lines = fact["item"].came["lines"]
            fields.append(f"marker={MARKERS.index(lines.marker)},{fact['number'] & 0xFFFFFFFF}"
                          f"{star(lines.marker_from_book)}")
        if fact["centred"]:
            fields.append(f"width={numbers(container.came['sizes']['width'])}*")
        return " ".join([f"block {fact['kind']} {fact['level']} {block['source']}"] + fields)

    def tell(self, block: dict, head: str, items: list[tuple], ids: list[tuple], named: list[int]) -> list[str]:
        """A block as `bn-bundle chapter` prints it: an id before whatever stands where it does."""
        out: list[str] = []
        next_id = 0

        def tell_ids(as_far_as: int) -> None:
            nonlocal next_id
            while next_id < len(ids) and ids[next_id][0] <= as_far_as:
                out.append("id " + printable(ids[next_id][1]))
                next_id += 1

        tell_ids(0)
        out.append(head)
        into = 0
        for item in items:
            tell_ids(into)
            if item[0] == "text":
                text = item[1]
                while text:
                    piece = len(text)
                    if next_id < len(ids) and ids[next_id][0] - into < piece:
                        piece = ids[next_id][0] - into
                    out.append("text " + printable(text[:piece]))
                    text = text[piece:]
                    into += piece
                    tell_ids(into)
            elif item[0] == "style":
                bits, from_book, family, family_from_book, language, size = item[1]
                number = named.index(language) + 1 if language else 0
                out.append(f"style {bits} {from_book} {family}{star(family_from_book)} {number} {size}")
            else:
                out.append(f"picture {NO_PICTURE} 1 0 0 0 0 0 0" + (" " + printable(item[1]) if item[1] else ""))
                into += 1
        tell_ids(1 << 32)
        out.append("end")
        return out


# ---- What the test makes.

SHEET = b"""p { margin: 0; text-indent: 1em } .first::first-letter { float: left; font-size: 3em; font-weight: bold }
.raised::first-letter { font-size: 2em; font-style: italic } .drop { float: left; font-size: 3em }
.gone { display: none } .inl { display: inline } .sc { font-variant: small-caps } .big { font-size: 1.5em }
code { font-family: monospace } h2 { font-size: 2em } .u { text-decoration: underline }
.not { font-weight: normal; font-style: normal; font-variant: normal } .level { vertical-align: baseline }
"""

LONG_ID = "i" * 255
TOO_LONG_ID = "j" * 256
LONG_ALT = "ab" + ("é" * 127)   # 256 bytes: cut to 254, where a character ends.

DOCUMENT = (
    """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="EN-gb"><head><title id="title">A title</title>
<link rel="stylesheet" type="text/css" href="import.css"/></head><body id="body">
<h2 id="one">White   space</h2>
<p id="p1">  Runs  of \t white
   space   are <b>  one </b> space,<i> </i>set as <u>the first </u> <b>of them</b> is.  </p>
<p>None<span> </span>at a line's end <br/>  or its start, <b> </b> nor at the block's <i>end </i> </p>
<p><br/>After an empty line.<br/><br/>Two line ends.<br/></p><div><br/></div><div> <br/> </div>
<p>A soft hy&#173;phen stays, a no-break&#160;space and&nbsp;a named one are not white space,
and&#32;a numbered space is, a&#13;return &#13;&#13; and&#9;a tab.</p><p>&nbsp;</p><p>&#160; &#160;</p>
<h2>Names</h2>
<p>caf&eacute; &mdash; &ldquo;quoted&rdquo; &hellip; &amp; &lt;tag&gt; &nosuchname; &Eacute;&eacute;</p>
<p>&nosuchname;</p><div>&nbsp;<p>&copy; 1900</p>&euro;</div>
<h2>As written</h2>
<pre>
  first line, indented
    second\tline

  after an empty one
</pre>
<pre>one line</pre><pre>  </pre><pre>

</pre><pre>   <b>bold after held white space</b>  <i>and</i>
</pre><div><pre class="inl">  inline, as written  </pre> then  combined  <pre class="inl">  and  again</pre></div>
<p>Before <pre class="inl">   x   </pre>   after, <pre class="inl">y</pre>   <pre class="inl">  z</pre></p>
<pre>"""
    + " " * 70
    + """more white space than is held</pre><pre><br/>  after a line end</pre><pre>  <img src="a.png" alt="p"/>  </pre>
<pre>a<span id="in-pre">  </span>b</pre>
<h2>Ids</h2>
<p id="p2">An id <a id="a1"/>in a line, <span id="s1">on a span</span>, <a name="named">by a name</a>, <A name="capital">in capitals</A>,
<a id="both" name="not-this">both</a>, <a id="">none</a>, at the end<a id="at-end"/></p>
<a id="between"/><p class="gone" id="hidden">Not shown <span id="in-hidden">at all</span></p>
<p>After white space <b id="after-space">here</b> and <span id="x1"/><span id="x2"/>two together.</p>
<p id=\""""
    + LONG_ID
    + """\">A long id.</p><p id=\""""
    + TOO_LONG_ID
    + """\">One too long.</p>
<h2>Pictures</h2>
<p>A picture <img src="a.png" alt="a small picture"/> in a line, <img src="b.png"/> one with nothing said,
<img src="c.png" alt=""/> and <img class="gone" src="d.png" alt="hidden"/> one hidden.</p>
<div><img src="e.png" alt=\""""
    + LONG_ALT
    + """\"/></div><div> <img src="f.png" alt="alone, with white space round it"/> </div>
<p>Text <img src="g.png" alt="g"/> <img src="h.png" alt="h"/> text.</p>
<h2>Languages</h2>
<p>English, <span xml:lang="fr">fran&ccedil;ais, <i xml:lang="la">lingua latina</i>, encore</span>,
<span lang="en-GB">the book's own</span>, <span xml:lang="de"> </span><span xml:lang="it">  </span>and
<span xml:lang="el">&epsilon;&lambda;&lambda;&eta;&nu;&iota;&kappa;&#940;</span>.</p>
<p xml:lang="fr">Tout en fran&ccedil;ais.</p>
<h2>How text is set</h2>
<p>Plain, <b>bold, <i>bold italic,</i></b> <span class="sc">small capitals</span>, x<sup>2</sup>, H<sub>2</sub>O,
<span class="big">bigger</span>, <code>code</code>, <span class="u">under<b>lined</b></span>.</p>
<h2 class="big">A <small>heading</small></h2>
<p class="not">What a sheet says text is not: <b class="not">bold</b>, <i class="not">italic</i>,
x<sup class="level">2</sup>, <b>and bold all the same</b>.</p><h2 class="not">Nor a heading</h2>
<h2>Capitals</h2>
<p class="first">The first letter is dropped by its rule.</p>
<p class="first">&ldquo;Quoted,&rdquo; with the mark before it.</p>
<p class="first">&ldquo;&lsquo;Two marks.</p><p class="first">&ldquo;&lsquo;&rsquo;Three marks are too many.</p>
<p class="first">1900 began with a digit.</p><p class="first">&hellip;and this with neither.</p>
<p class="first"> <b> W</b>hite space before it, and bold.</p><p class="first">&ldquo;<b>M</b>ark and letter set apart.</p>
<p class="first"><img src="i.png" alt="I"/><a id="after-picture"/>A picture first.</p><p class="first"><br/>A line end first.</p>
<p class="first" id="lettered"><a id="in-letter"/>&ldquo;<a id="after-mark"/>An id before the letter.</p>
<p class="first">&Eacute;cole, <span xml:lang="el">&Omega;</span>.</p><p class="first">&Omega;&mu;&#941;&gamma;&alpha;.</p>
<p class="first">&#1046;ivago.</p><p class="first">&times; is no letter.</p><p class="first">I</p><p class="first">&ldquo;</p>
<p class="first">&laquo;Guillemets&raquo;.</p><p class="first">&iquest;Qu&eacute;?</p><p class="first">'Plain' marks.</p>
<p class="first">&mdash;A dash is no mark.</p><p class="first"><span class="drop">B</span>oth ways at once.</p>
<p class="raised">Raised: a first letter that is not floated.</p>
<div class="first"><p>The first block inside what has the rule.</p><p>Not the second.</p></div>
<p><span class="drop">T</span>he letter is in a floated element.</p>
<p><span class="drop">&ldquo;T</span>he mark is with it.</p><p><span class="drop">T </span>he white space is not.</p>
<p><span class="drop">Th</span>e two letters are not a capital.</p><p><span class="drop">7</span> is no letter.</p>
<p><span class="drop">&ldquo;</span>A mark alone.</p><p><span class="drop"></span>Nothing floated.</p>
<p><span class="drop"><img src="j.png" alt="J"/></span>ust a picture.</p>
<p><img class="drop" src="k.png" alt="K"/>A picture floated itself.</p>
<p><span class="drop"><img src="l.png" alt="L"/>x</span>A picture and more.</p>
<p><span class="drop"><img src="m.png" alt="a text of more than sixty-four bytes standing for a picture, which is not held"/>
</span>Too long.</p><p><span class="drop"><img src="n.png" alt="N"/><img src="o.png" alt="O"/></span>Two pictures.</p>
<p><span class="drop" id="on-drop">D<a id="in-drop"/></span>ropped, with ids.</p>
<p><span class="drop">E<br/></span>A line end in it.</p><p><span class="drop">F</span></p>
<p><span class="drop"><b>&ldquo;</b>G</span>Set apart.</p><div><span class="drop">H<p>A block inside.</p></span></div>
<p><span class="drop gone">X</span><span class="drop">Y</span>After something hidden.</p>
<p><span class="drop">&Auml;</span>hnlich, <span class="drop">Z</span> not at the start.</p>
<p><span class="drop">&#7842;</span> and <span class="drop">&#7944;</span>: further alphabets.</p>
<h2>What is nothing</h2>
<p class="gone">Nothing <b>at all</b> &nosuchname; <img src="q.png" alt="q"/><br/></p><p hidden="">Nor this.</p>
<p>Before <span class="gone"> hidden </span> after.</p>
<hr id="rule"/><hr/><hr>Inside <b id="in-rule">a rule</b><br/><img src="r.png" alt="r"/> </hr><p>After the rules.</p><i>Text in no block of its own</i> <b>and more.</b><a id="last"/>
</body></html>
"""
).encode("utf-8")

CONTAINER = (b'<container><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/>'
             b'</rootfiles></container>')


def random_document(rng: random.Random) -> bytes:
    """A document for the sheets styles_tool.random_sheet() makes, with text that brings up importing's rules."""
    names = ["div", "p", "span", "h1", "h3", "ul", "ol", "li", "blockquote", "i", "b", "sup", "small", "u", "code",
             "pre", "pre", "td", "center", "dd", "hr", "br", "br", "img", "img", "a", "DIV", "section", "BR", "IMG"]
    words = ["", "", " ", "\n", " words ", "x", " and more\tof them ", "&#160;", "&amp;", "\n  \n", "&nbsp;",
             "&ldquo;", "&ldquo;T", "A", " B", "&Eacute;", "9", "&nosuch;", "  indented\n   lines\n", "&mdash;",
             "&#937;", " \t ", "The quick brown fox", "caf&eacute;", "x" * 40, " " * 70 + "y"]
    count = 0

    def element(depth: int) -> str:
        nonlocal count
        name = rng.choice(names)
        text = "<" + name
        if rng.random() < 0.5:
            text += " class='" + rng.choice(["a", "b", "c", "a b", "c a", ""]) + "'"
        if rng.random() < 0.15:
            text += " xml:lang='" + rng.choice(["en", "fr", "de", "EN", "en-gb", ""]) + "'"
        if rng.random() < 0.25:
            count += 1
            text += f" id='{rng.choice(['n', 'id', '', 'a b'])}{count}'"
        if rng.random() < 0.1:
            text += " name='named'"
        if name in ("img", "IMG") and rng.random() < 0.7:
            text += " alt='" + rng.choice(["", "A", "a picture", "&#201;", "x" * 70]) + "'"
        if rng.random() < 0.2:
            text += " style='" + rng.choice(["font-size: 2em; margin: 1em", "display: block", "float: left",
                                             "float: left", "display: inline", "display: none", "font-weight: bold",
                                             "page-break-after: always", "display: list-item"]) + "'"
        text += ">" + rng.choice(words)
        for _ in range(rng.choice([0, 0, 1, 2, 3]) if depth < 7 else 0):
            text += element(depth + 1) + rng.choice(words)
        return text + "</" + name + ">"

    return ('<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "x.dtd">\n<html' +
            rng.choice(["", " lang='en'", " xml:lang='fr'"]) + "><head><title>t</title>" +
            "<link rel='stylesheet' href='random.css'/></head><body class='" + rng.choice(["", "a", "b"]) + "'>" +
            rng.choice(words) + "".join(element(2) + rng.choice(words) for _ in range(rng.randrange(1, 7))) +
            "</body></html>").encode()


def cut_short(document: bytes, rng: random.Random) -> tuple[bytes, bytes, int]:
    """A document that goes wrong where one of the elements standing straight in its body began, in its second
    half; the same with the two elements open there closed instead; and where a parser finds that the first
    went wrong. With nothing but the body open, what ends there ends the same way in both."""
    open_elements: list[str] = []
    places: list[int] = []
    parser = xml.parsers.expat.ParserCreate()

    def start(name: str, _attributes: dict) -> None:
        if len(open_elements) == 2 and open_elements[1] == "body":
            places.append(parser.CurrentByteIndex)
        open_elements.append(name)

    parser.StartElementHandler = start
    parser.EndElementHandler = lambda _name: open_elements.pop()
    parser.Parse(document, True)
    expect(len(places) > 0, "a document with nothing in its body cannot be cut short")
    at = places[rng.randrange(len(places) // 2, len(places))]

    wrong = document[:at] + b"<<"
    parser = xml.parsers.expat.ParserCreate()
    try:
        parser.Parse(wrong, True)
    except xml.parsers.expat.ExpatError:
        pass
    else:
        raise Failure("a document cut short was read to its end")
    return wrong, document[:at] + b"</body></html>", parser.ErrorByteIndex


def run(*command: str) -> list[str]:
    result = subprocess.run(command, capture_output=True, check=False)
    expect(result.returncode == 0, f"{command[1:]} ended with {result.returncode}: {result.stderr[-300:]!r}")
    expect(result.stdout.endswith(b"\n"), f"{command[1:]}: the last line is not ended")
    return result.stdout.decode("latin-1").split("\n")[:-1]


def figures_of(line: str) -> tuple[int, str, dict]:
    """A line of `bn-bundle import`: the chapter's number, what became of it, and the rest by name."""
    head, _, path = line.partition(" path=")
    words = head.split(" ")
    expect(len(words) > 3 and words[0] == "chapter", f"not a line for a chapter: {line!r}")
    said = dict(word.split("=", 1) for word in words[3:])
    said["path"] = path
    return int(words[1]), words[2], said


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    tool, css = (str(Path(argument).resolve()) for argument in sys.argv[1:3])
    work = Path(sys.argv[3]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    try:
        known = css_tool.known_families(css)
        rng = random.Random(20261127)

        # The book: its documents, each with what the second working-out is to be given for it.
        files = {b"META-INF/container.xml": CONTAINER, b"text/import.css": SHEET,
                 b"text/blocks.css": blocks_tool.SHEET, b"pictures/in-spine.png": b"not looked at"}
        blocks_document = blocks_tool.DOCUMENT.replace(
            b"<head><title>A title</title>", b'<head><title>A title</title><link rel="stylesheet" href="blocks.css"/>')
        expect(blocks_document != blocks_tool.DOCUMENT, "the document written for the blocks has no such head")
        whole = {b"text/written.xhtml": DOCUMENT, b"text/blocks.xhtml": blocks_document,
                 b"text/empty.xhtml": b"<html><head><title>Nothing</title></head><body> <div> </div> </body></html>",
                 b"text/own-style.xhtml": (b"<html><head><style>p { text-align: center } b { display: block }</style>"
                                           b"</head><body><p>One <b>two</b> three</p></body></html>")}
        randoms = 60
        for number in range(randoms):
            whole[f"text/random-{number}.xhtml".encode()] = random_document(rng)
            files[f"text/random-{number}.css".encode()] = styles_tool.random_sheet(rng)
        for name in list(whole):
            if name.startswith(b"text/random-"):
                whole[name] = whole[name].replace(b"random.css", name[5:-6] + b".css")
        files.update(whole)

        # Documents that go wrong part way: the written one, and some of those made at random.
        wrong: dict[bytes, tuple[bytes, int]] = {}
        for number, name in enumerate([b"text/written.xhtml"] + [f"text/random-{at}.xhtml".encode() for at in
                                                                 range(0, randoms, 5)]):
            files[f"text/cut-{number}.xhtml".encode()], closed, where = cut_short(whole[name], rng)
            wrong[f"text/cut-{number}.xhtml".encode()] = (closed, where)
        files[b"text/not-xml.xhtml"] = b"This is not XML at all. <"
        files[b"text/wrong-at-once.xhtml"] = b"<html><head><title>T</title></head><body><<</body></html>"

        documents = [name for name in files if name.endswith(b".xhtml")]
        rng.shuffle(documents)
        spine = documents[:]
        spine.insert(rng.randrange(len(spine)), b"text/no-such-chapter.xhtml")
        spine.insert(rng.randrange(len(spine)), b"pictures/in-spine.png")
        manifest = "".join(
            f'<item id="i{number}" href="{name.decode()}" media-type="'
            f'{"image/png" if name.endswith(b".png") else "application/xhtml+xml"}"/>'
            for number, name in enumerate(spine))
        files[b"content.opf"] = (
            '<package xmlns="http://www.idpf.org/2007/opf" version="3.0"><metadata '
            'xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>A book to import</dc:title>'
            '<dc:language>en-GB</dc:language></metadata><manifest>' + manifest + '</manifest><spine>' +
            "".join(f'<itemref idref="i{number}"/>' for number in range(len(spine))) + '</spine></package>').encode()
        book = work / "book.epub"
        sheets_tool.write_book(book, files, rng)

        bundle = work / "book.bundle"
        run(tool, "make", str(book), str(bundle), "--written", "1795000000")
        said = run(tool, "import", str(book), str(bundle), "all")
        expect(len(said) == len(spine) + 1 and said[-1].startswith("memory "),
               f"import said {len(said)} lines for {len(spine)} chapters")

        finder = sheets_tool.Book(lambda name: name in files, lambda name: files[name])
        lines = dropped = left_out = 0
        states: dict[str, int] = {}
        for number, name in enumerate(spine):
            which, state, figures = figures_of(said[number])
            expect(which == number, f"chapter {number}: its line is for {which}")
            states[state] = states.get(state, 0) + 1
            what = f"chapter {number} ({name.decode()})"
            if name == b"text/no-such-chapter.xhtml":
                expect((state, figures["reason"]) == ("refused", "no-file"), f"{what}: {said[number]!r}")
                continue
            if name.endswith(b".png"):
                expect((state, figures["reason"]) == ("not-a-document", "none"), f"{what}: {said[number]!r}")
                continue
            if name in (b"text/not-xml.xhtml", b"text/wrong-at-once.xhtml"):
                expect((state, figures["reason"]) == ("refused", "not-XML"), f"{what}: {said[number]!r}")
                continue
            expect(figures["path"] == name.decode(), f"{what}: its path is said to be {figures['path']!r}")

            document, where = wrong.get(name, (files[name], None))
            listed, _, own = finder.sheets_of(name)
            sheets = [files[sheet] for sheet in listed if sheet != sheets_tool.OWN_STYLE] + own
            chapter = Chapter(sheets, document, known, b"en-GB")
            expected = chapter.lines
            if where is not None and not chapter.made:
                # Nothing was read of it before it went wrong.
                expect((state, figures["reason"]) == ("refused", "not-XML"), f"{what}: {said[number]!r}")
                continue
            if where is not None:
                expected[0] = f"chapter cut-short {REASONS.index('not-XML')} {where}"
            expect((state, figures["reason"]) == (("whole", "none") if where is None else ("cut-short", "not-XML")),
                   f"{what}: {said[number]!r}")

            got = [line for line in run(tool, "chapter", str(bundle), str(number)) if not line.startswith("#")]
            for at, (mine, theirs) in enumerate(zip(got, expected)):
                if mine != theirs:
                    raise Failure(f"{what}: line {at + 1} is {mine!r}, the second working-out has {theirs!r}; "
                                  f"before that came {got[max(0, at - 8):at]!r}")
            expect(len(got) == len(expected), f"{what}: {len(got)} lines, the second working-out has {len(expected)}; "
                   f"the tool's go on {got[len(expected):][:6]!r} and the other's {expected[len(got):][:6]!r}")
            theirs = {key: str(value) for key, value in chapter.figures.items()}
            expect({key: figures.get(key) for key in theirs} == theirs,
                   f"{what}: import says {figures}; the second working-out has {theirs}")
            expect(figures["cascade_too_deep"] == "0", f"{what}: the cascade left elements out")
            lines += len(got)
            dropped += chapter.figures["dropped"]
            left_out += chapter.figures["left_out"]
            if name == b"text/written.xhtml":
                expect(chapter.figures["dropped"] >= 24 and chapter.figures["entities_left_out"] == 3 and
                       chapter.figures["held_left_out"] == 6 and chapter.figures["languages"] >= 6 and
                       chapter.said_not >= 6,
                       f"the document written for it has too little of something: {chapter.figures}, "
                       f"{chapter.said_not} times text of which a sheet said what it is not")
        expect(states.get("whole") == len(whole) and states.get("cut-short", 0) > len(wrong) // 2 and
               states.get("not-a-document") == 1 and sum(states.values()) == len(spine),
               f"what became of the chapters: {states}")
        expect(dropped > 20 and left_out > 20, f"too little of something: {dropped} dropped, {left_out} left out")

        # A chapter imported by itself is the chapter it was among the others; and imported again, the same.
        alone = work / "alone.bundle"
        run(tool, "make", str(book), str(alone), "--written", "1795000000")
        number = spine.index(b"text/written.xhtml")
        for _ in range(2):
            again = run(tool, "import", str(book), str(alone), str(number))
            expect(again[0] == said[number], f"imported alone: {again[0]!r}; with the others: {said[number]!r}")
            expect(run(tool, "chapter", str(alone), str(number)) == run(tool, "chapter", str(bundle), str(number)),
                   "a chapter imported alone is not the chapter it was among the others")
        for bad in ("9999", "x", "-1", ""):
            expect(subprocess.run([tool, "import", str(book), str(alone), bad], capture_output=True,
                                  check=False).returncode == 1, f"chapter {bad!r} should be refused")
        expect(subprocess.run([tool, "import", str(work / "no-such.epub"), str(alone), "all"], capture_output=True,
                              check=False).returncode == 1, "a missing book should be refused")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"bn-bundle import agrees with the second working-out: {lines} lines of {len(whole) + len(wrong)} "
          f"chapters, {randoms} of them made at random and {len(wrong)} cut short")
    return 0


if __name__ == "__main__":
    sys.exit(main())
