#!/usr/bin/env python3
"""End-to-end check of `bn-css cascade --styles`, and through it of what each element of a document comes to
(epub/style_stack.h), run by CTest.

The unit tests say what an element should come to for sheets and documents written for them. Here what the tool
says of every element of a document is compared, line for line, with a second working-out of the same:
`styles_lines` below. It takes what the rules set for each element from the second working-out of the cascade
(css_cascade_tool.py), which shares nothing with the tool; keeps the whole document as a tree; and works each
element out from the one it is inside by going down the tree, with its own list of what HTML's elements are
given. The tool keeps no tree. The two were written from the same account of the rules, which is at the top of
epub/style_stack.h, so what they can catch in one another is a slip in doing what it says.

They are compared on a sheet and a document written here for every rule, and on ones made at random.
tools/check-books.py compares them on real books.

usage: styles_tool.py BN_CSS WORK_DIR
"""

from __future__ import annotations

import random
import re
import shutil
import subprocess
import sys
import xml.parsers.expat
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import css_cascade_tool  # noqa: E402
import css_tool  # noqa: E402
from css_tool import Failure, expect  # noqa: E402

LEVELS = 32          # How many open elements the tool keeps.
MAX_LANGUAGES = 15
MAX_LANGUAGE_BYTES = 23

# ---- What HTML's elements are given, as its own account of how they are shown has it.

BLOCKS = set("""address article aside blockquote body caption center dd details dir div dl dt fieldset figcaption
figure footer form h1 h2 h3 h4 h5 h6 header hgroup hr html legend listing main menu nav ol p plaintext pre section
summary table tbody td tfoot th thead tr ul xmp""".split())
NOTHING = set("""area base basefont col colgroup datalist head link math meta noembed noframes param rp rt script
source style template title track""".split())
NOT_SHOWN = {"math", "rt"}
HEADINGS = {"h1": (2000, 670), "h2": (1500, 830), "h3": (1170, 1000), "h4": (1000, 1330), "h5": (830, 1670),
            "h6": (670, 2330)}
BOLD = {"b", "strong", "th"} | set(HEADINGS)
ITALIC = {"address", "cite", "dfn", "em", "i", "var"}
UNDERLINED = {"ins", "u"}
STRUCK = {"del", "s", "strike"}
VERBATIM = {"listing", "plaintext", "pre", "xmp"}
MONO = {"code", "kbd", "samp", "tt"} | VERBATIM
CENTRED = {"caption", "center", "th"}
ALIGN_IS_NOT_OF_LINES = {"caption", "hr", "table"}
BULLETED = {"dir", "menu", "ul"}
NUMBERED = {"ol"}
LISTS = BULLETED | NUMBERED | {"dl"}
MARGIN_EM = {"blockquote": 1000, "dl": 1000, "figure": 1000, "hr": 500, "p": 1000,
             **{name: 1000 for name in BULLETED | NUMBERED | VERBATIM}, **{h: m for h, (_, m) in HEADINGS.items()}}
SIDE_PX = {"blockquote": 40, "figure": 40}
PAGE = {"html", "body"}

NAMED_SIZES = {"xx-small": 563, "x-small": 625, "small": 813, "medium": 1000, "large": 1125, "x-large": 1500,
               "xx-large": 2000}
ALIGNS = ("left", "right", "center", "justify", "start", "end")
MARKERS = ("none", "disc", "circle", "square", "decimal", "decimal-leading-zero", "lower-alpha", "upper-alpha",
           "lower-roman", "upper-roman")
BITS = ("bold", "italic", "underline", "line-through", "small-caps", "raised", "lowered")
SIDES = ("top", "right", "bottom", "left")
LENGTH = re.compile(r"^(-?)(\d+)(?:\.(\d+))?(px|em|rem|%)?$")


def over(a: int, b: int) -> int:
    """a over b to the nearest whole number, a half going away from nothing."""
    return (a + b // 2) // b if a >= 0 else -((-a + b // 2) // b)


def as_size(size: int) -> int:
    return max(0, min(65535, size))


def as_part(part: int) -> int:
    return max(-32768, min(32767, part))


def amount(text: str):
    """A length as the tool prints one: (thousandths, unit), the unit `number` for none; None for a word."""
    match = LENGTH.match(text)
    if not match:
        return None
    thousandths = int(match.group(2)) * 1000 + int((match.group(3) or "").ljust(3, "0")[:3])
    return (-thousandths if match.group(1) else thousandths), match.group(4) or "number"


def as_length(text: str, size: int, root: int) -> tuple[int, int, int]:
    """(64ths of the reader's size, 8ths of a pixel, 64ths of a percent of the page's width)."""
    number, unit = amount(text)
    if unit == "em":
        return as_part(over(number * size * 64, 1_000_000)), 0, 0
    if unit == "rem":
        return as_part(over(number * root * 64, 1_000_000)), 0, 0
    if unit == "%":
        return 0, 0, as_part(over(number * 64, 1000))
    return 0, as_part(over(number * 8, 1000)), 0


def part_text(value: int, of: int) -> str:
    size = abs(value)
    text = ("-" if value < 0 else "") + str(size // of)
    if size % of:
        text += (".%06d" % (size % of * 1_000_000 // of)).rstrip("0")
    return text


def length_text(length: tuple[int, int, int]) -> str:
    parts = [part_text(value, of) + unit for value, of, unit in zip(length, (64, 8, 64), ("em", "px", "%")) if value]
    return "+".join(parts) or "0"


def book(from_book: bool) -> str:
    return " book" if from_book else ""


class Text:
    """How text is set."""

    def __init__(self, other: "Text | None" = None) -> None:
        self.bits = set(other.bits) if other else set()
        self.from_book = set(other.from_book) if other else set()
        self.family = other.family if other else "unset"
        self.family_from_book = other.family_from_book if other else False
        self.language = other.language if other else 0
        self.size = other.size if other else 1000
        self.size_from_book = other.size_from_book if other else False

    def set_bit(self, bit: str, on: bool, by_book: bool) -> None:
        (self.bits.add if on else self.bits.discard)(bit)
        (self.from_book.add if by_book else self.from_book.discard)(bit)

    def lines(self, mark: str, languages: list[str]) -> list[str]:
        out = []
        if self.size != 1000 or self.size_from_book:
            out.append(f"{mark} size: {self.size}{book(self.size_from_book)}")
        if self.bits:
            out.append(f"{mark} text: " + " ".join(bit for bit in BITS if bit in self.bits))
        if self.from_book:
            out.append(f"{mark} text-book: " + " ".join(bit for bit in BITS if bit in self.from_book))
        if self.family != "unset" or self.family_from_book:
            out.append(f"{mark} family: {self.family}{book(self.family_from_book)}")
        if self.language:
            out.append(f"{mark} language: {languages[self.language - 1]}")
        return out


def set_text(said: dict[str, str], outside: Text, in_line: bool, root: int, text: Text) -> None:
    """What a sheet said of how text is set, put on `text`; `outside` is what `inherit` takes."""
    size = said.get("font-size")
    if size is not None:
        found = amount(size)
        if found:
            number, unit = found
            text.size = as_size({"em": over(outside.size * number, 1000), "%": over(outside.size * number, 100_000),
                                 "rem": over(root * number, 1000)}.get(unit, over(number, 16)))
        elif size == "smaller":
            text.size = as_size(over(outside.size * 5, 6))
        elif size == "larger":
            text.size = as_size(over(outside.size * 6, 5))
        elif size == "inherit":
            text.size = outside.size
        else:
            text.size = NAMED_SIZES.get(size, 1000)
        text.size_from_book = True
    weight = said.get("font-weight")
    if weight is not None:
        found = amount(weight)
        text.set_bit("bold", found[0] >= 600_000 if found else
                     "bold" in outside.bits if weight == "inherit" else weight in ("bold", "bolder"), True)
    slant = said.get("font-style")
    if slant is not None:
        text.set_bit("italic", "italic" in outside.bits if slant == "inherit" else slant in ("italic", "oblique"), True)
    variant = said.get("font-variant")
    if variant is not None:
        text.set_bit("small-caps", "small-caps" in outside.bits if variant == "inherit" else variant == "small-caps",
                     True)
    if said.get("text-decoration") in ("underline", "line-through"):
        text.set_bit(said["text-decoration"], True, True)
    where = said.get("vertical-align")
    if in_line and where is not None:
        found = amount(where)
        if found:
            raised, lowered = found[0] > 0, found[0] < 0
        elif where == "inherit":
            raised, lowered = "raised" in outside.bits, "lowered" in outside.bits
        else:
            raised, lowered = where in ("super", "top", "text-top"), where in ("sub", "bottom", "text-bottom")
        text.set_bit("raised", raised, True)
        text.set_bit("lowered", lowered, True)
    family = said.get("font-family")
    if family is not None:
        text.family = outside.family if family == "inherit" else "unset" if family == "initial" else family
        text.family_from_book = True


class Inside:
    """What the elements inside an element take from it."""

    def __init__(self) -> None:
        self.hidden = False
        self.text = Text()
        self.align = "unset"
        self.align_from_book = False
        self.indent = None  # A length, once something has said one.
        self.indent_from_book = False
        self.line_height = None  # ("x", thousandths) or ("length", a length); None for the reader's.
        self.line_height_from_book = False
        self.marker = "disc"
        self.marker_from_book = False
        self.right_to_left = False
        self.spaces_as_written = False
        self.lists = 0
        self.in_list = False


class Element:
    def __init__(self, name: str, attributes: dict[str, str]) -> None:
        self.name = name
        self.attributes = attributes
        self.children: list[Element] = []
        # The elements and the pieces of text inside it, in their order; a reference to an entity is "&".
        self.content: list[Element | str] = []
        self.said: dict[str, str] = {}
        self.first_letter: dict[str, str] = {}
        # What it came to, by name, for whoever works on from it; "display" alone for what is nothing.
        self.came: dict = {}
        # Where its opening tag and its closing tag began in the document, in bytes.
        self.at = self.end_at = 0


class Styles:
    """What every element of a document comes to, as the lines the tool prints."""

    def __init__(self, sheets: list[bytes], document: bytes, known: list, entity=None) -> None:
        # `entity` says what text a reference to an entity the document does not declare is, by its name: "&"
        # for any, unless it is given.
        # What the rules set for each element, by the second working-out of the cascade, in the elements' order.
        cascade, _ = css_cascade_tool.cascade_lines(sheets, document, known)
        said: list[tuple[dict, dict]] = []
        for line in cascade:
            text = line.decode("latin-1")
            if text.startswith("< "):
                said.append(({}, {}))
            elif text[:2] in ("P ", "F "):
                name, _, value = text[2:].partition(": ")
                said[-1][text[0] == "F"][name] = value

        # The document, whole, as a tree.
        root: list[Element] = []
        open_elements: list[Element] = []
        count = 0

        def start(name: str, attributes: dict[str, str]) -> None:
            nonlocal count
            element = Element(name, attributes)
            element.said, element.first_letter = said[count]
            element.at = parser.CurrentByteIndex
            count += 1
            (open_elements[-1].children if open_elements else root).append(element)
            if open_elements:
                open_elements[-1].content.append(element)
            open_elements.append(element)

        def end(_name: str) -> None:
            open_elements.pop().end_at = parser.CurrentByteIndex

        def text(piece: str) -> None:
            if open_elements and piece:
                open_elements[-1].content.append(piece)

        def skipped(name: str, is_parameter: bool) -> None:
            if not is_parameter:
                text("&" if entity is None else entity(name))

        parser = xml.parsers.expat.ParserCreate()
        parser.StartElementHandler = start
        parser.EndElementHandler = end
        parser.CharacterDataHandler = text
        parser.SkippedEntityHandler = skipped
        parser.Parse(document, True)
        self.root = root

        self.elements = count
        self.lines: list[str] = []
        self.languages: list[str] = []
        self.root_size = 1000
        self.left_out = {"styles_too_deep": 0, "page_margins": 0, "inherits_of_own": 0, "languages_left_out": 0,
                         "breaks_avoided_before": 0, "not_shown": 0}
        for element in root:
            self.go(element, [], 0, False)

    def language(self, tag: str, outside: int) -> int:
        as_bytes = css_cascade_tool.bytes_as_text(tag)
        for number, known in enumerate(self.languages, 1):
            if css_tool.lower(known) == css_tool.lower(as_bytes):
                return number
        if len(self.languages) == MAX_LANGUAGES or len(as_bytes) > MAX_LANGUAGE_BYTES:
            self.left_out["languages_left_out"] += 1
            return outside
        self.languages.append(as_bytes)
        return len(self.languages)

    def go(self, element: Element, kept: list[Inside], depth: int, hidden: bool) -> None:
        """One element and all inside it. `kept` is what the elements it is inside left, the first LEVELS of them;
        `hidden` is whether one of them, however deep, is nothing."""
        self.lines.append("< " + css_cascade_tool.bytes_as_text(element.name))
        inside = self.comes_to(element, kept[-1] if kept else Inside(), depth, hidden)
        below = kept + [inside] if depth < LEVELS else kept
        for child in element.children:
            self.go(child, below, depth + 1, inside.hidden)
        self.lines.append(">")

    def comes_to(self, element: Element, outside: Inside, depth: int, hidden: bool) -> Inside:
        said, attributes, out = element.said, element.attributes, self.lines
        name = css_tool.lower(element.name)
        if depth >= LEVELS:
            self.left_out["styles_too_deep"] += 1

        # What it is to the lines of a page.
        display = "none" if name in NOTHING else "list-item" if name == "li" else "block" if name in BLOCKS else "inline"
        asked = said.get("display")
        if asked in ("block", "inline", "list-item", "none"):
            display = asked
        elif asked == "initial":
            display = "inline"
        elif asked == "inherit":
            self.left_out["inherits_of_own"] += 1
        if hidden or "hidden" in attributes:
            display = "none"
        now = Inside()
        if display == "none":
            if not hidden and name in NOT_SHOWN:
                self.left_out["not_shown"] += 1
            out.append("C display: none")
            now.hidden = True
            element.came = {"display": "none"}
            return now
        if display != "inline":
            out.append("C display: " + display)
        in_line = display == "inline"

        # How its text is set.
        text = Text(outside.text)
        if not in_line:
            text.set_bit("raised", False, False)
            text.set_bit("lowered", False, False)
        if name in HEADINGS:
            text.size = as_size(over(outside.text.size * HEADINGS[name][0], 1000))
        elif name in ("small", "sub", "sup"):
            text.size = as_size(over(outside.text.size * 5, 6))
        elif name == "big":
            text.size = as_size(over(outside.text.size * 6, 5))
        given = {"bold": name in BOLD, "italic": name in ITALIC,
                 "underline": name in UNDERLINED and "text-decoration" not in said,
                 "line-through": name in STRUCK and "text-decoration" not in said}
        for bit, on in given.items():
            if on and bit not in text.bits:
                text.set_bit(bit, True, False)
        if in_line and name in ("sup", "sub"):
            text.set_bit("raised", name == "sup", False)
            text.set_bit("lowered", name == "sub", False)
        if name in MONO:
            text.family, text.family_from_book = "mono", False
        set_text(said, outside.text, in_line, 1000 if depth == 0 else self.root_size, text)
        tag = attributes.get("xml:lang", attributes.get("lang"))
        if tag is not None:
            text.language = self.language(tag, outside.text.language) if tag else 0
        if depth == 0:
            self.root_size = text.size
        root = self.root_size
        out.extend(text.lines("C", self.languages))
        now.text = text

        # What is set of a block's lines.
        now.align, now.align_from_book = outside.align, outside.align_from_book
        if name in CENTRED:
            now.align, now.align_from_book = "center", False
        if not in_line and name not in ALIGN_IS_NOT_OF_LINES and \
                css_tool.lower(attributes.get("align", "")) in ("left", "right", "center", "justify"):
            now.align, now.align_from_book = css_tool.lower(attributes["align"]), True
        if "text-align" in said:
            asked = said["text-align"]
            now.align = outside.align if asked == "inherit" else asked if asked in ALIGNS else "unset"
            now.align_from_book = True
        if now.align != "unset" or now.align_from_book:
            out.append(f"C align: {now.align}{book(now.align_from_book)}")

        now.indent, now.indent_from_book = outside.indent, outside.indent_from_book
        if "text-indent" in said:
            asked = said["text-indent"]
            if amount(asked):
                now.indent = as_length(asked, text.size, root)
            elif asked != "inherit":
                now.indent = (0, 0, 0)
            elif now.indent is None:
                now.indent = (0, 0, 0)
            now.indent_from_book = True
        if now.indent is not None:
            out.append(f"C indent: {length_text(now.indent)}{book(now.indent_from_book)}")

        now.line_height, now.line_height_from_book = outside.line_height, outside.line_height_from_book
        if "line-height" in said:
            asked = said["line-height"]
            found = amount(asked)
            if found:
                number, unit = found
                if unit in ("number", "em"):
                    now.line_height = ("x", as_size(number))
                elif unit == "%":
                    now.line_height = ("x", as_size(over(number, 100)))
                else:
                    now.line_height = ("length", as_length(asked, text.size, root))
            elif asked != "inherit":
                now.line_height = None
            now.line_height_from_book = True
        if now.line_height is not None:
            kind, value = now.line_height
            out.append("C line-height: " + (f"x{value}" if kind == "x" else length_text(value)) +
                       book(now.line_height_from_book))
        elif now.line_height_from_book:
            out.append("C line-height: normal book")

        # What stands before the items of a list.
        now.lists = outside.lists
        now.marker, now.marker_from_book = outside.marker, outside.marker_from_book
        is_list = name in BULLETED or name in NUMBERED
        if is_list:
            now.lists = min(now.lists + 1, 255)
            now.marker = "decimal" if name in NUMBERED else ("disc", "circle")[now.lists - 1] if now.lists < 3 else "square"
            now.marker_from_book = False
        if (is_list or display == "list-item") and "type" in attributes:
            kind = attributes["type"]
            numbered = {"1": "decimal", "a": "lower-alpha", "A": "upper-alpha", "i": "lower-roman", "I": "upper-roman"}
            if kind in numbered:
                now.marker, now.marker_from_book = numbered[kind], True
            if css_tool.lower(kind) in ("disc", "circle", "square"):
                now.marker, now.marker_from_book = css_tool.lower(kind), True
        if "list-style-type" in said:
            asked = said["list-style-type"]
            now.marker = outside.marker if asked == "inherit" else asked if asked in MARKERS else "disc"
            now.marker_from_book = True
        if display == "list-item":
            out.append(f"C marker: {now.marker}{book(now.marker_from_book)}")

        now.right_to_left = outside.right_to_left
        direction = css_tool.lower(attributes.get("dir", ""))
        if direction in ("rtl", "ltr"):
            now.right_to_left = direction == "rtl"
        if now.right_to_left:
            out.append("C right-to-left")
        now.spaces_as_written = outside.spaces_as_written or name in VERBATIM
        if now.spaces_as_written:
            out.append("C spaces-as-written")

        # What is its own: the space around it.
        now.in_list = outside.in_list or name in LISTS
        em = 0 if name in LISTS and outside.in_list else MARGIN_EM.get(name, 0)
        own = (as_part(over(em * text.size * 64, 1_000_000)), 0, 0)
        side = SIDE_PX.get(name, 0)
        space = {"margin": [own, (0, side * 8, 0), own, (0, (side + (40 if name == "dd" else 0)) * 8, 0)],
                 "padding": [(0, 0, 0), (0, 0, 0), (0, 0, 0), (0, 320 if is_list else 0, 0)]}
        from_book = {"margin": [False] * 4, "padding": [False] * 4}
        is_auto = [False] * 4
        for index, which in enumerate(SIDES):
            for kind in ("margin", "padding"):
                asked = said.get(f"{kind}-{which}")
                if asked is None:
                    continue
                if asked == "inherit":
                    self.left_out["inherits_of_own"] += 1
                    continue
                length = as_length(asked, text.size, root) if amount(asked) else (0, 0, 0)
                if name in PAGE:
                    self.left_out["page_margins"] += 1 if any(length) else 0
                    continue
                space[kind][index] = length
                from_book[kind][index] = True
                if kind == "margin":
                    is_auto[index] = asked == "auto" and which in ("left", "right")
        for kind in ("margin", "padding"):
            for index, which in enumerate(SIDES):
                if any(space[kind][index]) or from_book[kind][index]:
                    shown = "auto" if kind == "margin" and is_auto[index] else length_text(space[kind][index])
                    out.append(f"C {kind}-{which}: {shown}{book(from_book[kind][index])}")

        # Its width and height, the side it is floated to, and where a page may end.
        sizes = {}
        for which in ("width", "height"):
            asked = said.get(which)
            if asked == "inherit":
                self.left_out["inherits_of_own"] += 1
            elif asked is not None and amount(asked):
                sizes[which] = as_length(asked, text.size, root)
                out.append(f"C {which}: {length_text(sizes[which])}")
        if said.get("float") in ("left", "right"):
            out.append("C float: " + said["float"])
        elif said.get("float") == "inherit":
            self.left_out["inherits_of_own"] += 1
        keep, keep_from_book = name in HEADINGS, False
        before, after = said.get("break-before"), said.get("break-after")
        if before == "always":
            out.append("C break-before")
        elif before == "avoid":
            self.left_out["breaks_avoided_before"] += 1
        elif before == "inherit":
            self.left_out["inherits_of_own"] += 1
        if after == "inherit":
            self.left_out["inherits_of_own"] += 1
        elif after is not None:
            if after == "always":
                out.append("C break-after")
            keep, keep_from_book = after == "avoid", True
        if keep or keep_from_book:
            out.append(("C keep-with-next" if keep else "C keep-with-next: no") + book(keep_from_book))
        if said.get("break-inside") == "avoid":
            out.append("C keep-together")
        elif said.get("break-inside") == "inherit":
            self.left_out["inherits_of_own"] += 1

        # Its first letter.
        letter, floated = None, False
        if element.first_letter:
            letter = Text(text)
            set_text(element.first_letter, text, True, root, letter)
            floated = element.first_letter.get("float") in ("left", "right")
            out.append("F first-letter: floated" if floated else "F first-letter")
            out.extend(letter.lines("F", self.languages))
        element.came = {"display": display, "text": text, "lines": now, "space": space, "from_book": from_book,
                        "auto": is_auto, "sizes": sizes, "floated": said.get("float") in ("left", "right"),
                        "break_before": before == "always", "break_after": after == "always", "keep": keep,
                        "keep_from_book": keep_from_book, "keep_together": said.get("break-inside") == "avoid",
                        "letter": letter, "letter_floated": floated}
        return now


def styles_lines(sheets: list[bytes], document: bytes, known: list) -> tuple[list[bytes], int, dict, int]:
    """What `bn-css cascade --styles` prints for a document given these sheets; how many elements; what was left
    out, by the names the tool's one line has; and how many languages."""
    styles = Styles(sheets, document, known)
    lines = [line.translate(css_tool.ESCAPES).encode("latin-1") for line in styles.lines]
    return lines, styles.elements, styles.left_out, len(styles.languages)


# ---- What the test makes.

SHEET = rb"""html { font-size: 110%; margin: 1em; padding: 0 } body { margin: 0 5% 2em; padding: 1px; line-height: 1.4 }
h1 { font-size: 2.5rem; margin: 2em 0 1em; text-align: center; page-break-before: always; page-break-after: auto }
h2 { font-weight: normal; font-style: italic; break-after: avoid } h3 { font-size: larger; break-after: always }
p { text-indent: 1.5em; margin: 0; text-align: justify; line-height: 120% } p.first { text-indent: 0 }
p.first::first-letter { float: left; font-size: 3em; font-weight: bold; font-family: Georgia; line-height: 1 }
p.plain::first-letter { font-variant: small-caps }
.small { font-size: small } .big { font-size: 18px } .tiny { font-size: 50% } .none { font-size: 0 }
.rem { font-size: 1.2rem; margin-left: 2rem; line-height: 1.5rem } .px { line-height: 20px; margin: 10px 2.5% -0.5em 1in }
.sans { font-family: "Trade Gothic", sans-serif } .mono { font-family: monospace } .face { font-family: inherit }
.unset { font-family: initial; font-weight: initial; font-style: initial; text-align: initial; line-height: normal }
.b6 { font-weight: 600 } .b5 { font-weight: 500 } .bolder { font-weight: bolder } .lighter { font-weight: lighter }
.upright { font-style: normal } .oblique { font-style: oblique } .caps { font-variant: small-caps }
.u { text-decoration: underline } .s { text-decoration: line-through } .o { text-decoration: overline }
.nou { text-decoration: none } .same { font-weight: inherit; font-style: inherit; font-variant: inherit;
  font-size: inherit; text-align: inherit; text-indent: inherit; line-height: inherit; list-style-type: inherit;
  vertical-align: inherit; text-decoration: inherit }
.up { vertical-align: super } .top { vertical-align: text-top } .down { vertical-align: -0.2em }
.raise { vertical-align: 30% } .base { vertical-align: baseline } .mid { vertical-align: middle }
.block { display: block } .inline { display: inline } .item { display: list-item } .gone { display: none }
.ib { display: inline-block } .own { margin: inherit; padding-left: inherit; width: inherit; float: inherit;
  display: inherit; break-before: inherit; break-after: inherit; break-inside: inherit; height: inherit }
.auto { margin: 0 auto; width: 75% } .autoleft { margin-left: auto; width: 20em; height: 2em } .w { width: auto }
.left { float: left } .right { float: right; break-inside: avoid; break-before: avoid }
.center { text-align: center } .end { text-align: end } .hang { text-indent: -2em; padding-left: 2em }
ol.toc { list-style-type: none } ul.sq { list-style: square inside } .roman { list-style-type: upper-roman }
.reset { list-style: initial; margin: initial; text-indent: initial } .keep { break-after: avoid }
"""

DOCUMENT = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en-GB" lang="fr">
<head><title>Styles</title><style>p { color: red }</style></head>
<body>
<h1>One <small>small</small> <big class="same">big</big></h1><h2>Two <b>bold</b></h2><h3 class="keep">Three</h3>
<h4>Four</h4><h5 class="rem">Five</h5><h6 align="RIGHT">Six</h6>
<p class="first">A <i>slanted <b>and bold <sup>raised <em class="upright">not</em></sup></b></i> start.</p>
<p class="plain small">Small <span class="big">big <span class="tiny">tiny <span class="none">nothing</span></span></span></p>
<div class="px sans"><p class="rem face">In. <code>code <span class="unset">unset</span></code> <kbd class="sans">k</kbd></p>
<pre class="mono">  as
written <span>inside</span></pre><p><tt>tt</tt><samp>s</samp><var>v</var><cite>c</cite><dfn>d</dfn></p></div>
<p class="b6">six <span class="b5">five <span class="bolder">er <span class="lighter">light <strong>strong</strong>
</span></span></span><span class="oblique caps">oc</span></p>
<p class="u">under <s>struck <u class="nou">both <ins>still</ins></u></s><del class="o">del</del><strike>x</strike>
<span class="s">through</span><span class="nou">none takes nothing away</span></p>
<p>e=mc<sup>2</sup> H<sub>2</sub>O <span class="up">up <span>still <i class="base">base</i></span></span>
<span class="top">top</span><span class="down">down</span><span class="raise">raise</span><span class="mid">mid</span>
<sup class="base">not raised</sup><sub class="up">raised sub</sub><span class="up block">a block is not raised</span></p>
<blockquote class="center"><p>Quoted.</p><p class="end same">End.</p><address>An address</address></blockquote>
<ul><li>disc<ul><li>circle<ul><li>square<ul><li>square still</li></ul></li></ul></li></ul></li>
<li type="CIRCLE">typed</li></ul>
<ol class="toc"><li>none</li><li class="roman">roman</li><li class="reset">disc</li></ol>
<ol type="a" start="3"><li>a</li><li type="I" value="7">I</li><li type="x">unknown</li></ol>
<ol type="A"><li>A<ol><li>decimal<ul class="sq"><li>square</li></ul></li></ol></li></ol>
<menu><li>menu</li></menu><dir type="square"><li>dir</li></dir><dl><dt>term</dt><dd>said</dd></dl>
<figure class="hang"><figcaption>Caption</figcaption></figure><hr/><hr align="center"/>
<table align="center"><caption align="bottom">Cap</caption><thead><tr><th>h</th></tr></thead>
<tbody><tr align="right"><td align="center">c</td><td class="left">l</td></tr></tbody></table>
<center>centred <p>and inside</p></center>
<div class="auto">auto</div><div class="autoleft">auto left</div><div class="w right">right</div>
<span class="left">floated span</span><span class="block">a span that is a block</span>
<div class="inline">a div in a line <p class="inline">and a p</p></div><span class="item">an item</span>
<div class="gone">gone <p>and all inside</p></div><p hidden="hidden">hidden <b>too</b></p>
<div class="ib">inline-block is as it was</div><span class="ib">here too</span><p class="own">its own</p>
<div dir="rtl">right to left <p>inside <span dir="LTR">left again</span><span dir="up">as it was</span></p></div>
<p lang="de">German <span xml:lang="de-CH" lang="it">Swiss</span><span lang="">none</span><span lang="DE">de again</span>
<span lang="a-tag-that-is-longer-than-a-tag-may-be">too long</span></p>
<p style="font-size: 2em; margin-top: 1em; text-indent: 1em; line-height: 2">Twice the size, and its ems.</p>
<math><mi>x</mi></math><ruby>base<rt>note</rt><rp>(</rp></ruby><unknown>an element nothing knows</unknown>
<DIV CLASS="center"><P>In capitals.</P></DIV><svg><title>t</title></svg>
</body>
</html>
""".encode()


def random_sheet(rng: random.Random) -> bytes:
    values = {
        "font-size": ["1.5em", "80%", "12px", "0.9rem", "small", "x-large", "smaller", "larger", "inherit", "0",
                      "initial", "99999em"],
        "font-weight": ["bold", "normal", "bolder", "lighter", "600", "500", "inherit", "initial"],
        "font-style": ["italic", "normal", "oblique", "inherit"], "font-variant": ["small-caps", "normal", "inherit"],
        "font-family": ["Georgia", "sans-serif", "monospace", "\"No Such Face\"", "inherit", "initial", "Zapfino"],
        "line-height": ["1.2", "150%", "1.4em", "18px", "2rem", "normal", "inherit", "0", "initial"],
        "text-align": ["left", "right", "center", "justify", "start", "end", "inherit", "initial"],
        "text-indent": ["1.5em", "-2em", "5%", "10px", "0", "inherit", "initial", "1rem"],
        "text-decoration": ["underline", "line-through", "none", "overline", "inherit"],
        "vertical-align": ["super", "sub", "top", "bottom", "text-top", "text-bottom", "baseline", "middle", "0.3em",
                           "-2px", "50%", "inherit", "0"],
        "margin": ["0", "1em", "1em 2px", "0 auto", "5% 1em 2px", "-1em 0 0 3%", "inherit", "initial", "auto"],
        "margin-left": ["auto", "2em", "-10px", "inherit"], "margin-top": ["3rem", "0", "inherit"],
        "padding": ["0", "1em", "2px 5%", "inherit", "initial"], "padding-left": ["40px", "1.5em", "inherit"],
        "display": ["block", "inline", "list-item", "none", "inline-block", "inherit", "initial"],
        "float": ["left", "right", "none", "inherit"], "width": ["50%", "20em", "auto", "inherit", "100px"],
        "height": ["2em", "auto", "inherit", "10px"],
        "page-break-before": ["always", "avoid", "auto", "inherit"],
        "page-break-after": ["always", "avoid", "auto", "inherit"], "break-inside": ["avoid", "auto", "inherit"],
        "list-style-type": ["none", "disc", "circle", "square", "decimal", "lower-roman", "upper-alpha", "inherit",
                            "initial", "decimal-leading-zero"],
        "list-style": ["none", "square inside", "inherit"],
    }
    selectors = ["html", "body", "p", "div", "span", "h1", "h3", "li", "ul", "ol", "i", "b", "sup", "td", "pre", "u",
                 ".a", ".b", ".c", "p.a", "div .b", "li > .c", "*", "p::first-letter", ".a::first-letter", "blockquote"]
    lines = []
    for _ in range(rng.randrange(2, 16)):
        chosen = ", ".join(rng.sample(selectors, rng.randrange(1, 3)))
        declarations = []
        for _ in range(rng.randrange(1, 5)):
            name = rng.choice(sorted(values))
            declarations.append(f"{name}: {rng.choice(values[name])}" + (" !important" if rng.random() < 0.1 else ""))
        lines.append(f"{chosen} {{ {'; '.join(declarations)} }}")
    return "\n".join(lines).encode()


def random_document(rng: random.Random, deep: bool = False) -> bytes:
    names = ["div", "p", "span", "h1", "h3", "h6", "ul", "ol", "li", "blockquote", "i", "b", "em", "strong", "sup",
             "sub", "small", "big", "u", "s", "code", "pre", "table", "tr", "td", "th", "center", "dl", "dd", "figure",
             "hr", "a", "q", "DIV", "menu", "head", "rt", "nobody", "address", "caption", "tt", "ins", "del"]
    languages = ["en", "en-GB", "fr", "de", "", "EN", "x-" + "y" * 30] + [f"l{n}" for n in range(20)]

    def element(depth: int) -> str:
        name = rng.choice(names)
        text = "<" + name
        if rng.random() < 0.5:
            text += " class='" + rng.choice(["a", "b", "c", "a b", "c a", ""]) + "'"
        which = rng.random()
        if which < 0.12:
            text += " " + rng.choice(["lang", "xml:lang"]) + "='" + rng.choice(languages) + "'"
        elif which < 0.2:
            text += " lang='" + rng.choice(languages[:4]) + "' xml:lang='" + rng.choice(languages[:4]) + "'"
        if rng.random() < 0.08:
            text += " dir='" + rng.choice(["rtl", "ltr", "RTL", "auto"]) + "'"
        if rng.random() < 0.1:
            text += " align='" + rng.choice(["left", "right", "center", "justify", "CENTER", "middle", ""]) + "'"
        if rng.random() < 0.1:
            text += " type='" + rng.choice(["1", "a", "A", "i", "I", "disc", "Circle", "square", "x"]) + "'"
        if rng.random() < 0.04:
            text += " hidden=''"
        if rng.random() < 0.12:
            text += " style='" + rng.choice(["font-size: 2em; margin: 1em", "display: block", "float: left",
                                             "text-indent: inherit", "font-weight: 700 !important",
                                             "list-style: none", "vertical-align: super; font-size: smaller"]) + "'"
        text += ">"
        for _ in range((1 if deep else rng.choice([0, 0, 1, 2, 3])) if depth < (40 if deep else 6) else 0):
            text += element(depth + 1) + rng.choice(["", "", " words "])
        return text + "</" + name + ">"

    return ("<html" + rng.choice(["", " lang='en'", " xml:lang='sv' dir='rtl'"]) + "><head><title>t</title></head>"
            "<body class='" + rng.choice(["", "a", "b"]) + "'>" +
            "".join(element(2) for _ in range(rng.randrange(1, 6))) + "</body></html>").encode()


def compare(tool: str, work: Path, sheets: list[bytes], document: bytes, what: str, known: list) -> tuple[int, dict]:
    """Runs `bn-css cascade --styles` on the document and the sheets; returns how many lines agreed, and what
    was left out."""
    names = []
    for index, sheet in enumerate(sheets):
        names.append(str(work / f"sheet-{index}.css"))
        Path(names[-1]).write_bytes(sheet)
    (work / "document.xhtml").write_bytes(document)
    result = subprocess.run([tool, "cascade", "--styles", str(work / "document.xhtml")] + names, capture_output=True,
                            check=False)
    expect(result.returncode == 0, f"{what}: bn-css ended with {result.returncode}: {result.stderr!r}")
    got = result.stdout.split(b"\n")
    expect(got[-1] == b"", f"{what}: the last line is not ended")
    got.pop()
    expected, elements, left_out, languages = styles_lines(sheets, document, known)
    for number, (mine, theirs) in enumerate(zip(got, expected)):
        if mine != theirs:
            raise Failure(f"{what}: line {number + 1} is {mine!r}, the second working-out has {theirs!r}; "
                          f"the element is {[line for line in got[:number + 1] if line.startswith(b'<')][-1:]!r}, "
                          f"and before that came {got[max(0, number - 6):number]!r}")
    expect(len(got) == len(expected), f"{what}: {len(got)} lines, the second working-out has {len(expected)}")

    # The one line that sums it up: the same checksum, and the same things left out.
    result = subprocess.run([tool, "cascade", "--styles", "--sum", str(work / "document.xhtml")] + names,
                            capture_output=True, check=False)
    said = result.stdout.decode().split()
    expect(result.returncode == 0 and said[:4] == ["styles", f"{css_cascade_tool.checksum(expected):08x}",
                                                   f"elements={elements}", f"lines={len(expected)}"],
           f"{what}: --sum says {result.stdout!r}; the second working-out has "
           f"{css_cascade_tool.checksum(expected):08x}, {elements} elements, {len(expected)} lines")
    figures = {key: int(value) for key, value in (item.split("=") for item in said[4:])}
    theirs = dict(left_out, languages=languages)
    expect({key: figures.get(key) for key in theirs} == theirs,
           f"{what}: --sum says {figures} was left out; the second working-out has {theirs}")
    return len(got), left_out


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
        lines, left_out = compare(tool, work, [SHEET], DOCUMENT, "the document written for it", known)
        expected, elements, _, languages = styles_lines([SHEET], DOCUMENT, known)
        text = b"\n".join(expected).decode("latin-1")
        expect(elements > 140 and lines > 900, f"only {elements} elements and {lines} lines: not much of a test")
        # The test's own working-out is not saying yes to everything: what a few elements come to is known.
        expect("< h1\nC display: block\nC size: 2750 book\nC text: bold\nC language: en-GB\n"
               "C align: center book\nC line-height: x1400 book\nC margin-top: 5.5em book\nC margin-right: 0 book\n"
               "C margin-bottom: 2.75em book\nC margin-left: 0 book\nC break-before\nC keep-with-next: no book\n" in text,
               "the first heading does not come to what it should")
        expect("< small\nC size: 2292 book\nC text: bold\n" in text and
               "< dd\nC display: block\nC size: 1100 book\nC language: en-GB\nC line-height: x1400 book\n"
               "C margin-left: 40px\n>" in text, "what an element gives, and what it takes, are not what they should be")
        expect(left_out == {"styles_too_deep": 0, "page_margins": 11, "inherits_of_own": 12, "languages_left_out": 1,
                            "breaks_avoided_before": 1, "not_shown": 2} and languages == 3,
               f"what was left out of the document written for it is {left_out}, with {languages} languages")
        lines += compare(tool, work, [], DOCUMENT, "the document with no sheet", known)[0]

        # Sheets and documents made at random; and some deeper than the tool keeps, with more languages than it
        # has room for.
        rng = random.Random(20261125)
        rounds = 0
        seen = {key: 0 for key in left_out}
        for round_ in range(160):
            sheets = [random_sheet(rng) for _ in range(rng.randrange(0, 3))]
            count, left = compare(tool, work, sheets, random_document(rng, deep=round_ % 8 == 7),
                                  f"made at random (round {rounds})", known)
            lines += count
            rounds += 1
            for key, value in left.items():
                seen[key] += value
        expect(all(seen.values()), f"the documents made at random left out {seen}: something never came up")

        # What cannot be read is said to be so.
        expect(subprocess.run([tool, "cascade", "--styles", str(work / "no-such-file.xhtml")], capture_output=True,
                              check=False).returncode == 1, "a missing file should be refused")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"bn-css cascade --styles agrees with the second working-out: {lines} lines, of a document written for "
          f"it and {rounds} made at random")
    return 0


if __name__ == "__main__":
    sys.exit(main())
