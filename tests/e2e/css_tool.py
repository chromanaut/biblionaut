#!/usr/bin/env python3
"""End-to-end check of bn-css, and through it of the CSS reader, run by CTest.

The unit tests say what the reader should make of sheets written for them.
Here what it makes of a sheet is compared, line for line, with what a
second reading of the same sheet makes of it. Python has no reader of CSS
of its own, so the second reading is written here: `read_css` below, which
shares no code with the reader and goes about it another way (the whole
sheet at once, with its comments taken out first, where the reader takes
a byte at a time). The two were written from the same account of what is
read and what is passed over, which is at the top of epub/css.h, so what
they can catch in one another is a slip, not a misunderstanding.

They are compared on sheets written here, on those sheets with bytes
changed, taken out and put in at random, and on the sheets of an EPUB read
out of the ZIP file. tools/check-books.py compares them on real books.

usage: css_tool.py BN_CSS WORK_DIR
"""

from __future__ import annotations

import random
import shutil
import subprocess
import sys
import zipfile
import zlib
from pathlib import Path

SPACE = " \t\n\r\f"
MAX_PRELUDE = 1024
MAX_DECLARATION = 512
MAX_COMPOUNDS = 8
MAX_CLASSES = 12
MAX_NUMBER = 100_000_000
MOST_DROPPED_TEXT = 80


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


# ---- The second reading. Text is handled as str with one character to a
# ---- byte (latin-1), so that what is cut and counted is bytes.


def lower(text: str) -> str:
    """Capitals of English made small, and nothing else changed."""
    return "".join(chr(ord(ch) + 32) if "A" <= ch <= "Z" else ch for ch in text)


def is_name_char(ch: str) -> bool:
    return ch.isascii() and (ch.isalnum() or ch in "-_") or ord(ch) >= 0x80


def number_text(thousandths: int) -> str:
    size = abs(thousandths)
    text = ("-" if thousandths < 0 else "") + str(size // 1000)
    if size % 1000:
        text += (".%03d" % (size % 1000)).rstrip("0")
    return text


EVERYWHERE = {"inherit": "inherit", "unset": "inherit", "initial": "initial"}
FONT_STYLES = {"normal": "normal", "italic": "italic", "oblique": "oblique"}
FONT_WEIGHTS = {"normal": "normal", "bold": "bold", "bolder": "bolder", "lighter": "lighter"}
FONT_VARIANTS = {"normal": "normal", "small-caps": "small-caps"}
FONT_SIZES = {name: name for name in ("xx-small", "x-small", "small", "medium", "large", "x-large", "xx-large",
                                      "smaller", "larger")}
NORMAL = {"normal": "normal"}
AUTO = {"auto": "auto"}
TEXT_ALIGNS = {name: name for name in ("left", "right", "center", "justify", "start", "end")}
DECORATIONS = {name: name for name in ("none", "underline", "line-through", "overline")}
VERTICAL_ALIGNS = {name: name for name in ("baseline", "sub", "super", "top", "bottom", "middle", "text-top",
                                           "text-bottom")}
DISPLAYS = {name: name for name in ("block", "inline", "inline-block", "list-item", "none")}
FLOATS = {name: name for name in ("left", "right", "none")}
BREAKS_AROUND = {"auto": "auto", "always": "always", "avoid": "avoid", "page": "always", "left": "always",
                 "right": "always", "recto": "always", "verso": "always", "avoid-page": "avoid"}
BREAKS_INSIDE = {"auto": "auto", "avoid": "avoid", "avoid-page": "avoid"}

# A unit: what it is kept as, and how many of that to one of it, as a fraction.
UNITS = {"px": ("px", 1, 1), "em": ("em", 1, 1), "rem": ("rem", 1, 1), "%": ("%", 1, 1), "pt": ("px", 4, 3),
         "pc": ("px", 16, 1), "in": ("px", 96, 1), "cm": ("px", 9600, 254), "mm": ("px", 960, 254),
         "q": ("px", 240, 254), "ex": ("em", 1, 2), "ch": ("em", 1, 2)}


def length(text: str, *, negative: bool = False, number: bool = False, percent: bool = True) -> str | None:
    """A length as the tool writes it, or None if `text` is not one that may be here."""
    at = 0
    below = False
    if at < len(text) and text[at] in "+-":
        below = text[at] == "-"
        at += 1
    whole = 0
    thousandths = 0
    digits = False
    while at < len(text) and text[at] in "0123456789":
        whole = min(whole * 10 + int(text[at]), MAX_NUMBER)
        digits = True
        at += 1
    if at < len(text) and text[at] == ".":
        at += 1
        scale = 100
        fraction = False
        while at < len(text) and text[at] in "0123456789":
            thousandths += scale * int(text[at])
            scale //= 10
            fraction = True
            at += 1
        if not fraction:
            return None
        digits = True
    if not digits:
        return None
    unit = text[at:]
    if len(unit) > 1 and unit[0] in "eE" and unit[1] in "0123456789+-":
        return None
    value = whole * 1000 + thousandths
    if not unit:
        if value != 0 and not number:
            return None
        kept = "" if number else "px"
    else:
        known = UNITS.get(lower(unit))
        if known is None or (known[0] == "%" and not percent):
            return None
        kept, times, over = known
        value = (min(value, MAX_NUMBER) * times + over // 2) // over
    value = min(value, MAX_NUMBER)
    if below:
        if not negative and value != 0:
            return None
        value = -value
    return number_text(value) + kept


def next_piece(rest: str) -> tuple[str, str]:
    """The next piece of a value, and what is left after it."""
    rest = rest.strip(SPACE)
    at = 0
    depth = 0
    quote = ""
    while at < len(rest):
        ch = rest[at]
        if quote:
            if ch == "\\" and at + 1 < len(rest):
                at += 1
            elif ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1 if depth else 0
        elif depth == 0 and ch in SPACE:
            break
        at += 1
    return rest[:at], rest[at:]


def families(text: str) -> str:
    """The names of a font-family value, as the tool writes them."""
    names = []
    rest = text
    while rest:
        name = ""
        quote = ""
        space = False
        at = 0
        while at < len(rest):
            ch = rest[at]
            if quote:
                if ch == quote:
                    quote = ""
                    at += 1
                    continue
                if ch == "\\" and at + 1 < len(rest):
                    at += 1
            elif ch in "\"'":
                quote = ch
                at += 1
                continue
            elif ch == ",":
                break
            if rest[at] in SPACE:
                space = bool(name)
            else:
                if space:
                    name += " "
                    space = False
                name += lower(rest[at])
            at += 1
        rest = rest[at + 1:]
        if name:
            names.append(name[:256])
    return ", ".join(names)


def is_weight(text: str) -> str | None:
    value = length(text, number=True, percent=False)
    if value is None or not value.isdigit() or not 1 <= int(value) <= 1000:
        return None
    return value


def declare(text: str, out: list[str]) -> None:
    """One declaration: what it sets, or why it was dropped."""
    text = text.strip(SPACE)
    if not text:
        return
    colon = text.find(":")
    name = (text if colon < 0 else text[:colon]).strip(SPACE)
    if colon < 0 or not name or not all(is_name_char(ch) for ch in name):
        out.append("X declaration: " + text[:MOST_DROPPED_TEXT])
        return
    value = text[colon + 1:].strip(SPACE)
    important = ""
    trimmed = value
    if len(trimmed) >= 10 and lower(trimmed[-9:]) == "important":
        before = trimmed[:-9].strip(SPACE)
        if before.endswith("!"):
            value = before[:-1].strip(SPACE)
            important = " !important"
    prop = lower(name)
    told: list[str] = []

    def say(which: str, what: str) -> None:
        told.append(f"D {which}: {what}{important}")

    def words(which: str, table: dict[str, str], extra=None) -> bool:
        what = EVERYWHERE.get(lower(value)) or table.get(lower(value)) or (extra(value) if extra else None)
        if what is None:
            return False
        say(which, what)
        return True

    def sides(first: str, table: dict[str, str], negative: bool) -> bool:
        values = []
        rest = value
        while True:
            piece, rest = next_piece(rest)
            if not piece:
                break
            if len(values) == 4:
                return False
            everywhere = EVERYWHERE.get(lower(piece))
            if everywhere and (values or rest.strip(SPACE)):
                return False
            what = everywhere or table.get(lower(piece)) or length(piece, negative=negative)
            if what is None:
                return False
            values.append(what)
        if not values:
            return False
        which = [[0, 0, 0, 0], [0, 1, 0, 1], [0, 1, 2, 1], [0, 1, 2, 3]][len(values) - 1]
        for side, index in zip(("top", "right", "bottom", "left"), which):
            say(f"{first}-{side}", values[index])
        return True

    def font() -> bool:
        everywhere = EVERYWHERE.get(lower(value))
        names = ("font-style", "font-variant", "font-weight", "font-size", "line-height", "font-family")
        if everywhere:
            for which in names:
                say(which, everywhere)
            return True
        style = variant = weight = height = "normal"
        piece, rest = next_piece(value)
        for _ in range(4):
            if not piece:
                break
            small = lower(piece)
            if small == "normal":
                pass
            elif small in FONT_STYLES:
                style = small
            elif small in FONT_VARIANTS:
                variant = small
            elif small in FONT_WEIGHTS:
                weight = small
            elif is_weight(piece):
                weight = is_weight(piece)
            else:
                break
            piece, rest = next_piece(rest)
        slash = piece.find("/")
        size_text = piece if slash < 0 else piece[:slash]
        size = FONT_SIZES.get(lower(size_text)) or length(size_text)
        if size is None:
            return False
        if slash >= 0:
            height = NORMAL.get(lower(piece[slash + 1:])) or length(piece[slash + 1:], number=True)
            if height is None:
                return False
        family = rest.strip(SPACE)
        if not family:
            return False
        for which, what in zip(names, (style, variant, weight, size, height, families(family))):
            say(which, what)
        return True

    def decoration() -> bool:
        what = EVERYWHERE.get(lower(value))
        rest = value
        while what is None:
            piece, rest = next_piece(rest)
            if not piece:
                return False
            what = DECORATIONS.get(lower(piece))
        say("text-decoration", what)
        return True

    any_length = lambda v: length(v, negative=True)  # noqa: E731
    not_below = lambda v: length(v)  # noqa: E731
    understood = None
    if prop == "font-style":
        understood = words("font-style", FONT_STYLES)
    elif prop == "font-weight":
        understood = words("font-weight", FONT_WEIGHTS, is_weight)
    elif prop == "font-variant":
        understood = words("font-variant", FONT_VARIANTS)
    elif prop == "font-family":
        understood = bool(value) and words("font-family", {}, families)
    elif prop == "font-size":
        understood = words("font-size", FONT_SIZES, not_below)
    elif prop == "font":
        understood = font()
    elif prop == "line-height":
        understood = words("line-height", NORMAL, lambda v: length(v, number=True))
    elif prop == "text-align":
        understood = words("text-align", TEXT_ALIGNS)
    elif prop == "text-indent":
        understood = words("text-indent", {}, any_length)
    elif prop in ("text-decoration", "text-decoration-line"):
        understood = decoration()
    elif prop == "vertical-align":
        understood = words("vertical-align", VERTICAL_ALIGNS, any_length)
    elif prop == "margin":
        understood = sides("margin", AUTO, True)
    elif prop in ("margin-top", "margin-right", "margin-bottom", "margin-left"):
        understood = words(prop, AUTO, any_length)
    elif prop == "padding":
        understood = sides("padding", {}, False)
    elif prop in ("padding-top", "padding-right", "padding-bottom", "padding-left"):
        understood = words(prop, {}, not_below)
    elif prop == "display":
        understood = words("display", DISPLAYS)
    elif prop == "float":
        understood = words("float", FLOATS)
    elif prop in ("page-break-before", "break-before"):
        understood = words("break-before", BREAKS_AROUND)
    elif prop in ("page-break-after", "break-after"):
        understood = words("break-after", BREAKS_AROUND)
    elif prop in ("page-break-inside", "break-inside"):
        understood = words("break-inside", BREAKS_INSIDE)
    elif prop in ("width", "height"):
        understood = words(prop, AUTO, not_below)
    if understood is None:
        out.append("X property: " + name[:MOST_DROPPED_TEXT])
    elif not understood:
        out.append("X value: " + text[:MOST_DROPPED_TEXT])
    else:
        out.extend(told)


def read_style(text: bytes) -> list[bytes]:
    """What `bn-css style` prints for a `style` attribute."""
    out: list[str] = []
    source = text.decode("latin-1")
    start = 0
    depth = 0
    quote = ""
    at = 0
    while at <= len(source):
        ch = source[at] if at < len(source) else ";"
        if at < len(source) and quote:
            if ch == "\\":
                at += 1
            elif ch == quote:
                quote = ""
        elif at < len(source) and ch in "\"'":
            quote = ch
        elif at < len(source) and ch == "(":
            depth += 1
        elif at < len(source) and ch == ")":
            depth -= 1 if depth else 0
        elif ch == ";" and (depth == 0 or at >= len(source)):
            declare(source[start:min(at, len(source))], out)
            start = at + 1
        at += 1
    return [line.translate(ESCAPES).encode("latin-1") for line in out]


def take_name(text: list[str], at: int) -> tuple[str, int]:
    """A name at `at`, its backslashes taken out where it stands; and where it ends."""
    start = at
    out = at
    first = at
    if first < len(text) and text[first] == "-":
        first += 1
    if first >= len(text) or text[first] in "0123456789" or not (is_name_char(text[first]) or text[first] == "\\"):
        return "", at
    while at < len(text):
        ch = text[at]
        if is_name_char(ch):
            text[out] = ch
            out += 1
            at += 1
        elif ch == "\\" and at + 1 < len(text) and text[at + 1] != "\n":
            at += 1
            if text[at] in "0123456789abcdefABCDEF":
                digits = ""
                while len(digits) < 6 and at < len(text) and text[at] in "0123456789abcdefABCDEF":
                    digits += text[at]
                    at += 1
                if at < len(text) and text[at] in SPACE:
                    at += 1
                code = int(digits, 16)
                if code == 0 or code > 0x10FFFF or 0xD800 <= code <= 0xDFFF:
                    code = 0xFFFD if at - out >= 3 else ord("?")
                for byte in chr(code).encode("utf-8"):
                    text[out] = chr(byte)
                    out += 1
            else:
                text[out] = text[at]
                out += 1
                at += 1
        else:
            break
    return "".join(text[start:out]), at


def skip_brackets(text: list[str], at: int) -> int:
    depth = 0
    quote = ""
    while at < len(text):
        ch = text[at]
        if quote:
            if ch == "\\":
                at += 1
            elif ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
        elif ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
            if depth == 0:
                return at + 1
        at += 1
    return at


def read_selector(piece: str, out: list[str]) -> bool:
    """One selector: its line, or why it was dropped. False if it was."""
    text = list(piece.strip(SPACE))

    def drop(kind: str, what: str | None = None) -> bool:
        out.append(f"X {kind}: " + ("".join(text) if what is None else what)[:MOST_DROPPED_TEXT])
        return False

    parts: list[str] = []
    class_total = 0
    ids = classes = elements = 0
    first_letter = False
    combinator = ""
    want_part = True
    at = 0
    while at < len(text):
        space = False
        while at < len(text) and text[at] in SPACE:
            at += 1
            space = True
        if at >= len(text):
            break
        ch = text[at]
        if ch == ">":
            if want_part:
                return drop("selector")
            combinator = " > "
            want_part = True
            at += 1
            continue
        if ch in "+~":
            return drop("sibling selector")
        if not want_part:
            if not space:
                return drop("selector")
            combinator = " "
        if first_letter or len(parts) == MAX_COMPOUNDS:
            return drop("selector")
        element = ""
        part_id = ""
        part_classes: list[str] = []
        anything = False
        if ch == "*":
            anything = True
            at += 1
        elif ch not in ".#:[":
            start = at
            element, at = take_name(text, at)
            if not element:
                return drop("selector")
            element = lower(element)
            text[start:start + len(element)] = list(element)
            elements += 1
            anything = True
        if at < len(text) and text[at] == "|":
            return drop("selector")
        while at < len(text) and text[at] not in SPACE and text[at] not in ">+~":
            mark = text[at]
            if first_letter:
                return drop("selector")
            if mark == "[":
                return drop("attribute selector")
            if mark in ".#":
                at += 1
                name, at = take_name(text, at)
                if not name or (mark == "#" and part_id) or (mark == "." and class_total == MAX_CLASSES):
                    return drop("selector")
                if mark == "#":
                    part_id = name
                    ids += 1
                else:
                    part_classes.append(name)
                    class_total += 1
                    classes += 1
                anything = True
                continue
            if mark != ":":
                return drop("selector")
            colon = at
            at += 1
            is_element = False
            if at < len(text) and text[at] == ":":
                is_element = True
                at += 1
            name_start = at
            name, at = take_name(text, at)
            if not name:
                return drop("selector")
            name = lower(name)
            text[name_start:name_start + len(name)] = list(name)
            if at < len(text) and text[at] == "(":
                at = skip_brackets(text, at)
            is_element = is_element or name in ("first-letter", "first-line", "before", "after")
            if is_element and name == "first-letter":
                first_letter = True
                elements += 1
            elif not is_element and name == "link":
                classes += 1
            else:
                return drop("pseudo-element" if is_element else "pseudo-class",
                            "".join(text[colon:name_start + len(name)]))
            anything = True
        if not anything:
            return drop("selector")
        shown = element + ("#" + part_id if part_id else "") + "".join("." + name for name in part_classes)
        parts.append(combinator + (shown or "*"))
        want_part = False
    if not parts or want_part:
        return drop("selector")
    out.append(f"S {min(ids, 255)},{min(classes, 255)},{min(elements, 255)} " + "".join(parts) +
               (" ::first-letter" if first_letter else ""))
    return True


def selector_end(text: str) -> int:
    depth = 0
    quote = ""
    at = 0
    while at < len(text):
        ch = text[at]
        if quote:
            if ch == "\\":
                at += 1
            elif ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
        elif ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1 if depth else 0
        elif ch == "," and depth == 0:
            return at
        at += 1
    return len(text)


def media_applies(query: str) -> bool:
    query = query.strip(SPACE)
    if not query:
        return True
    for one in query.split(","):
        one = one.strip(SPACE)
        if not one or "(" in one:
            continue
        turned = False
        word, rest = next_piece(one)
        if lower(word) == "not":
            turned = True
            word, rest = next_piece(rest)
        elif lower(word) == "only":
            word, rest = next_piece(rest)
        if not word or rest.strip(SPACE):
            continue
        if (lower(word) in ("all", "screen")) != turned:
            return True
    return False


def take_url(text: str) -> tuple[str, str]:
    text = text.strip(SPACE)
    if lower(text[:4]) == "url(":
        close = text.find(")")
        if close < 0:
            return "", ""
        inside = text[4:close].strip(SPACE)
        if len(inside) >= 2 and inside[0] in "\"'" and inside[-1] == inside[0]:
            inside = inside[1:-1]
        return inside, text[close + 1:]
    if text[:1] in ("\"", "'"):
        close = text.find(text[0], 1)
        if close < 0:
            return "", ""
        return text[1:close], text[close + 1:]
    return "", ""


def unwrapped(text: str) -> str:
    text = text.strip(SPACE)
    for wrapper in ("\xef\xbb\xbf", "<!--", "-->"):
        if text.startswith(wrapper):
            text = text[len(wrapper):].strip(SPACE)
    return text


ESCAPES = str.maketrans({"\\": "\\\\", "\n": "\\n", "\t": "\\t", "\r": "\\r"})


def read_css(data: bytes) -> list[bytes]:
    """What `bn-css rules` prints for a sheet, a line to an item."""
    source = data.decode("latin-1")
    # First, the sheet without its comments, each character marked with
    # whether it is only itself: inside quotes, or after a backslash.
    plain: list[tuple[str, bool]] = []
    quote = ""
    escaped = False
    at = 0
    while at < len(source):
        ch = source[at]
        if not quote and not escaped and ch == "/" and at + 1 < len(source) and source[at + 1] == "*":
            close = source.find("*/", at + 2)
            at = len(source) if close < 0 else close + 2
            continue
        at += 1
        if quote or escaped:
            plain.append((ch, True))
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch in (quote, "\n"):
                quote = ""
            continue
        if ch in "\"'":
            quote = ch
        elif ch == "\\":
            escaped = True
        plain.append((ch, False))

    out: list[str] = []
    at = 0
    end = len(plain)

    def skip_block() -> None:
        nonlocal at
        depth = 1
        while at < end:
            ch, itself = plain[at]
            at += 1
            if not itself and ch == "{":
                depth += 1
            elif not itself and ch == "}":
                depth -= 1
                if depth == 0:
                    return

    def read_block() -> None:
        """The declarations of a rule, up to the brace that closes it."""
        nonlocal at
        text: list[str] = []
        too_long = False
        parens = braces = 0

        def end_declaration() -> None:
            nonlocal text, too_long, parens, braces
            what = "".join(text).strip(SPACE)
            if too_long:
                out.append("X too long: " + what[:MOST_DROPPED_TEXT])
            else:
                declare(what, out)
            text = []
            too_long = False
            parens = braces = 0

        while at < end:
            ch, itself = plain[at]
            at += 1
            if not itself:
                if ch == "{":
                    braces += 1
                elif ch == "}":
                    if braces == 0:
                        break
                    braces -= 1
                elif ch in "([":
                    parens += 1
                elif ch in ")]" and parens > 0:
                    parens -= 1
                elif ch == ";" and parens == 0 and braces == 0:
                    end_declaration()
                    continue
            if len(text) < MAX_DECLARATION:
                text.append(ch)
            else:
                too_long = True
        end_declaration()
        out.append("}")

    while at < end:
        # What comes before a block, or an at-rule up to its semicolon.
        text: list[str] = []
        too_long = False
        parens = 0
        block = None
        while at < end:
            ch, itself = plain[at]
            at += 1
            if not itself:
                if not text and ch in SPACE:
                    continue
                if ch == "{":
                    block = True
                    break
                if ch in "([":
                    parens += 1
                elif ch in ")]" and parens > 0:
                    parens -= 1
                elif parens == 0:
                    if ch == ";" and unwrapped("".join(text)).startswith("@"):
                        block = False
                        break
                    if ch == "}":
                        text = []
                        too_long = False
                        parens = 0
                        continue
            if len(text) < MAX_PRELUDE:
                text.append(ch)
            else:
                too_long = True
        prelude = unwrapped("".join(text))
        if block is None:
            # The sheet ended. An at-rule with nothing after it is whole.
            if not prelude.startswith("@"):
                break
            block = False
        if too_long:
            out.append("X too long: " + prelude[:MOST_DROPPED_TEXT])
            if block:
                skip_block()
            continue
        if prelude.startswith("@"):
            letters = list(prelude)
            name, after = take_name(letters, 1)
            name = lower(name)
            rest = prelude[after:].strip(SPACE)
            if name == "media" and block:
                if not media_applies(rest):
                    out.append("X media block: " + rest[:MOST_DROPPED_TEXT])
                    skip_block()
                continue
            if name == "import" and not block:
                url, media = take_url(rest)
                if url and media_applies(media):
                    out.append("I " + url)
                    continue
            out.append("X at-rule: " + name[:MOST_DROPPED_TEXT])
            if block:
                skip_block()
            continue
        if not block:
            continue
        selectors: list[str] = []
        rest = prelude
        while rest:
            cut = selector_end(rest)
            read_selector(rest[:cut], selectors)
            rest = rest[cut + 1:]
        told = any(line.startswith("S ") for line in selectors)
        opened = False
        for line in selectors:
            if line.startswith("S ") and not opened:
                out.append("{")
                opened = True
            out.append(line)
        if not told:
            out.append("X rule: ")
            skip_block()
            continue
        read_block()
    return [line.translate(ESCAPES).encode("latin-1") for line in out]


def checksum(lines: list[bytes]) -> int:
    crc = 0
    for line in lines:
        crc = zlib.crc32(line + b"\n", crc)
    return crc


# ---- The sheets the test makes.


def sheets() -> dict[str, bytes]:
    rng = random.Random(20261115)
    made: dict[str, bytes] = {}
    made["plain.css"] = b"""@charset "utf-8";
@import url("more.css");
@import 'screen.css' screen;
@import "print.css" print;
/* The body, and what is in it. */
body { margin: 0 5%; font: 1em/1.4 Georgia, "Times New Roman", serif; text-align: justify }
h1, h2, h3.sub { font-weight: bold; text-align: center; page-break-before: always; margin: 2em 0 1em }
p { text-indent: 1.5em; margin: 0; widows: 2; orphans: 2 }
p.first, h1 + p, blockquote > p:first-child { text-indent: 0 }
p.first::first-letter { float: left; font-size: 3em; line-height: 1; padding: 0 0.1em 0 0 }
div#cover img, .full { width: 100%; height: auto; max-width: 100% }
a:link, a:visited { text-decoration: none; color: #036 }
.sc { font-variant: small-caps } .i { font-style: italic } sup { vertical-align: super; font-size: 75% }
table td[colspan], li::before, q:lang(fr) { display: none }
"""
    made["media.css"] = b"""@media not amzn-mobi { .k { margin-left: 12pt } }
@media amzn-kf8 { .k { margin-left: 0 } }
@media screen and (max-width: 420px) { body { margin: 0 } }
@media all { @media print { p { display: none } } i { font-style: normal !important } }
@font-face { font-family: "A Face"; src: url(data:font/ttf;base64,AAAA;BBBB) }
@page { margin: 1em; @bottom-center { content: counter(page) } }
@supports (display: grid) { div { display: grid } }
.after { float: right; margin: 1px 2px 3px 4px }
"""
    made["mistakes.css"] = (b"\xef\xbb\xbf<!--\n} p { margin-top: 1em margin-bottom: 2em; text-indent: 0 }\n"
                            b"p { b { margin: 0 } float: left } i { width: calc(1em; float: left }\n"
                            b"@import url(a.css; q { } u { font-family: \"open;\n margin: 1 em; margin-top: 1em }\n"
                            b"s, , t,, { padding: 1px -1px; padding: 1px 2px 3px 4px 5px; height: 5vh }\n"
                            b".\\31 0, .a\\:b, DIV.Up#Id, .\\0, .x\\0z, #a#b, p..q { line-height: 1.2.3; line-height: 120% }\n"
                            b"p::first-letter::first-letter, p::first-letter.x, p:first-letter, :first-letter { float: left }\n"
                            b"-->\n.end { font: bold 2em; font: caption; font: italic 700 x-large/normal a , \"B  c\" ")
    long_selectors = ", ".join(f"p.class-number-{n}" for n in range(80))
    made["long.css"] = (long_selectors + " { margin: 0 }\nq { font-family: " + "x" * 600 + "; float: left }\n" +
                        "p" + " " * 1022 + "{ float: none }\n" + " > ".join("a" * 3 for _ in range(9)) + " {}\n" +
                        "p" + ".c" * 13 + ", p" + ".c" * 12 + " { margin: 99999999999999999999px -0.0005em }\n").encode()
    properties = ["margin", "margin-top", "padding", "padding-left", "font-size", "line-height", "text-indent", "width",
                  "height", "vertical-align", "font-weight", "font-style", "font-variant", "font-family", "font",
                  "text-align", "text-decoration", "display", "float", "page-break-before", "break-inside", "color",
                  "text-transform"]
    values = ["0", "1em", "-1.5em", "12pt", "2.54cm", "150%", "1.2", "auto", "inherit", "normal", "bold", "italic",
              "small-caps", "none", "left", "always", "avoid", "center", "underline", "700", "x-large", "1e3px",
              "3 em", "5vh", "calc(1px + 2px)", "Georgia, serif", "\"Gill Sans\"", "+.5in", "10mm", "1q", "2ex",
              "red", "1px 2px", "1px 2px 3px", "bold 1em/1.5 serif", "super", "0.0004px", "999999999999px"]
    lines = []
    for _ in range(400):
        selector = rng.choice(["p", "div.a", ".b > i", "h1, h2", "a:link", "p::first-letter", "td[x]", "u + s", "*"])
        declarations = "; ".join(f"{rng.choice(properties)}: {rng.choice(values)}" +
                                 (" !important" if rng.random() < 0.1 else "") for _ in range(rng.randrange(1, 5)))
        lines.append(f"{selector} {{ {declarations} }}")
    made["every-value.css"] = "\n".join(lines).encode()
    made["empty.css"] = b""
    made["no-rules.css"] = b"/* nothing but a comment */\n\n"
    return made


def damaged(rng: random.Random, sheet: bytes) -> bytes:
    """`sheet` with a few bytes changed, taken out or put in."""
    marks = b"{}()[];:,\"'\\/*@!.#> \n-"
    data = bytearray(sheet)
    for _ in range(rng.randrange(1, 9)):
        if not data:
            break
        at = rng.randrange(len(data))
        choice = rng.randrange(4)
        if choice == 0:
            data[at] = rng.randrange(256)
        elif choice == 1:
            data[at] = rng.choice(marks)
        elif choice == 2:
            del data[at:at + rng.randrange(1, 30)]
        else:
            data[at:at] = bytes(rng.choice(marks) for _ in range(rng.randrange(1, 4)))
    return bytes(data)


def compare(tool: str, path: Path, data: bytes, what: str) -> int:
    """Runs bn-css over `data` written to `path`; returns how many lines agreed."""
    path.write_bytes(data)
    result = subprocess.run([tool, "rules", str(path)], capture_output=True, check=False)
    expect(result.returncode == 0, f"{what}: bn-css ended with {result.returncode}: {result.stderr!r}")
    got = result.stdout.split(b"\n")
    expect(got[-1] == b"", f"{what}: the last line is not ended")
    got.pop()
    expected = read_css(data)
    for number, (mine, theirs) in enumerate(zip(got, expected)):
        expect(mine == theirs, f"{what}: line {number + 1} is {mine!r}, the second reading has {theirs!r}")
    if len(got) != len(expected):
        longer = got if len(got) > len(expected) else expected
        raise Failure(f"{what}: {len(got)} lines, the second reading has {len(expected)}: "
                      f"{longer[min(len(got), len(expected))]!r} is the first of the rest")
    return len(got)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tool = str(Path(sys.argv[1]).resolve())
    work = Path(sys.argv[2]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    try:
        made = sheets()
        lines = 0
        for name, data in made.items():
            lines += compare(tool, work / name, data, name)
        expect(lines > 2000, f"only {lines} lines came of the sheets: the test is not testing much")

        # The same sheets, damaged: whatever is done to a sheet, the two
        # readings make the same of what is left.
        rng = random.Random(1115)
        rounds = 0
        for name in ("plain.css", "media.css", "mistakes.css"):
            for _ in range(150):
                compare(tool, work / "damaged.css", damaged(rng, made[name]), f"{name}, damaged (round {rounds})")
                rounds += 1

        # A `style` attribute.
        style = b"margin-left: 2em; font-weight: 700 !important; color: red; font-family: \"a;b\"; width: calc(1px; x"
        result = subprocess.run([tool, "style", style.decode()], capture_output=True, check=False)
        expect(result.returncode == 0 and result.stdout.split(b"\n")[:-1] == read_style(style),
               f"a style attribute: bn-css says {result.stdout!r}, the second reading {read_style(style)!r}")

        # In an EPUB: read out of the ZIP file, inflated and read in one go.
        book = work / "book.epub"
        with zipfile.ZipFile(book, "w") as archive:
            archive.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
            for name, data in made.items():
                archive.writestr("OEBPS/css/" + name, data, zipfile.ZIP_DEFLATED if len(name) % 2 else zipfile.ZIP_STORED)
            archive.writestr("OEBPS/Capitals.CSS", b"P { MARGIN: 0 }", zipfile.ZIP_DEFLATED)
            archive.writestr("OEBPS/chapter.xhtml", "<html><body><p style='margin: 0'>Not a sheet.</p></body></html>")
        result = subprocess.run([tool, "book", str(book), "--dropped"], capture_output=True, check=False)
        expect(result.returncode == 0, f"book.epub: bn-css ended with {result.returncode}: {result.stderr!r}")
        said = {}
        totals = b""
        dropped: dict[bytes, int] = {}
        for line in result.stdout.split(b"\n")[:-1]:
            if line.startswith(b"css_sheets="):
                totals = line
            elif line.startswith(b"dropped "):
                _, count, what = line.split(b" ", 2)
                dropped[what] = int(count)
            else:
                how, crc, rules, declarations, passed, name = line.split(None, 5)
                said[name.decode()] = (how, int(crc, 16), int(rules), int(declarations), int(passed))
        expect(set(said) == {"OEBPS/css/" + name for name in made} | {"OEBPS/Capitals.CSS"},
               f"book.epub: the sheets read were {sorted(said)}")
        all_rules = all_declarations = all_dropped = 0
        expected_dropped: dict[bytes, int] = {}
        for name, data in list(made.items()) + [("../Capitals.CSS", b"P { MARGIN: 0 }")]:
            expected = read_css(data)
            rules = sum(1 for line in expected if line == b"}")
            declarations = sum(1 for line in expected if line.startswith(b"D "))
            passed = sum(1 for line in expected if line.startswith(b"X "))
            for line in expected:
                if line.startswith(b"X "):
                    expected_dropped[line[2:]] = expected_dropped.get(line[2:], 0) + 1
            entry = "OEBPS/Capitals.CSS" if name.startswith("..") else "OEBPS/css/" + name
            expect(said[entry] == ("ok".encode(), checksum(expected), rules, declarations, passed),
                   f"book.epub: {entry}: bn-css says {said[entry]}; the second reading has checksum "
                   f"{checksum(expected):08x}, {rules} rules, {declarations} declarations, {passed} passed over")
            all_rules += rules
            all_declarations += declarations
            all_dropped += passed
        expect(totals.startswith(f"css_sheets={len(made) + 1} css_failed=0 ".encode()) and
               totals.endswith(f" css_rules={all_rules} css_declarations={all_declarations} "
                               f"css_dropped={all_dropped}".encode()),
               f"book.epub: the totals are {totals!r}")
        expect(dropped == expected_dropped, "book.epub: what was passed over, by kind and text, is not what the second "
                                            f"reading passed over: {sorted(set(dropped.items()) ^ set(expected_dropped.items()))[:6]}")

        # What cannot be read is said to be so.
        expect(subprocess.run([tool, "rules", str(work / "no-such-file.css")], capture_output=True,
                              check=False).returncode == 1, "a missing file should be refused")
        expect(subprocess.run([tool], capture_output=True, check=False).returncode == 2, "no arguments should be refused")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"bn-css agrees with the second reading: {lines} lines of {len(made)} sheets, {rounds} damaged sheets, "
          "a style attribute and a book")
    return 0


if __name__ == "__main__":
    sys.exit(main())
