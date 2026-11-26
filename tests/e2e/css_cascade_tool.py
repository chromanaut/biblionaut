#!/usr/bin/env python3
"""End-to-end check of `bn-css cascade`, and through it of the table of rules and the cascade, run by CTest.

The unit tests say what the cascade should make of sheets and documents
written for them. Here what it makes of a document is compared, line for
line, with a second working-out of the same: `cascade_lines` below. It
takes the rules from the second reading of CSS (css_tool.py), keeps the
whole document as a tree, tries every selector of every rule against
every element by going back over each choice a selector leaves, and sorts
what the selectors that hold set into the order CSS gives. The tool keeps
no tree and tries a selector only where it could hold, so the two share
an account of what CSS means and nothing of how to get there.

They are compared on sheets and documents written here, on ones made at
random, and on a document read out of an EPUB. tools/check-books.py
compares them on real books.

usage: css_cascade_tool.py BN_CSS WORK_DIR
"""

from __future__ import annotations

import random
import shutil
import subprocess
import sys
import xml.parsers.expat
import zipfile
import zlib
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import css_tool  # noqa: E402

SPACE = " \t\n\r\f"
MAX_CLASSES = 16

# The properties, in the order the tool writes them.
PROPERTIES = ["font-style", "font-weight", "font-variant", "font-family", "font-size", "line-height", "text-align",
              "text-indent", "text-decoration", "vertical-align", "margin-top", "margin-right", "margin-bottom",
              "margin-left", "padding-top", "padding-right", "padding-bottom", "padding-left", "display", "float",
              "break-before", "break-after", "break-inside", "width", "height", "list-style-type"]


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


class Element:
    """One element of a document, with what a selector can ask of it."""

    def __init__(self, name: str, attributes: list[tuple[str, str]], parent: "Element | None") -> None:
        self.name = name
        self.attributes = attributes
        self.parent = parent
        self.before = parent.last if parent is not None else None  # The element before it in the same parent.
        self.last: Element | None = None                           # The last element to have closed inside it.

    def attribute(self, name: str) -> str | None:
        for written, value in self.attributes:
            if written == name:
                return value
        return None


def answers(value: str, sign: str, asked: str, any_case: bool) -> bool:
    """Whether an attribute that is `value` is what `sign` and `asked` ask of it."""
    if any_case:
        value = css_tool.lower(value)
        asked = css_tool.lower(asked)
    if sign == "":
        return True
    if sign == "=":
        return value == asked
    if sign == "~=":
        words = [word for word in split_words(value)]
        return bool(asked) and not any(ch in SPACE for ch in asked) and asked in words
    if sign == "|=":
        return value == asked or value.startswith(asked + "-")
    if sign == "^=":
        return bool(asked) and value.startswith(asked)
    if sign == "$=":
        return bool(asked) and value.endswith(asked)
    if sign == "*=":
        return bool(asked) and asked in value
    raise Failure(f"a sign that there is not: {sign!r}")


def split_words(text: str) -> list[str]:
    words = []
    word = ""
    for ch in text:
        if ch in SPACE:
            if word:
                words.append(word)
            word = ""
        else:
            word += ch
    if word:
        words.append(word)
    return words


def is_part(part: tuple, element: Element) -> bool:
    """Whether one part of a selector is true of an element, by itself."""
    _, name, part_id, classes, asked = part
    if name and name != css_tool.lower(element.name):
        return False
    if part_id and element.attribute("id") != part_id:
        return False
    if classes:
        written = element.attribute("class")
        has = split_words(written)[:MAX_CLASSES] if written is not None else []
        if any(wanted not in has for wanted in classes):
            return False
    for attribute, sign, value, any_prefix, any_case in asked:
        for written, is_value in element.attributes:
            lowered = css_tool.lower(written)
            if (lowered == attribute or (any_prefix and lowered.rpartition(":")[2] == attribute)) and \
                    answers(is_value, sign, value, any_case):
                break
        else:
            return False
    return True


def holds(parts: list[tuple], index: int, element: Element) -> bool:
    """Whether the selector's parts up to `index` hold, with that part being `element`."""
    if not is_part(parts[index], element):
        return False
    if index == 0:
        return True
    joined = parts[index][0]
    if joined == ">":
        return element.parent is not None and holds(parts, index - 1, element.parent)
    if joined == "+":
        return element.before is not None and holds(parts, index - 1, element.before)
    outer = element.parent
    while outer is not None:
        if holds(parts, index - 1, outer):
            return True
        outer = outer.parent
    return False


def bytes_as_text(text: str) -> str:
    """Text from a document as the second reading of CSS has text: a character to each byte of its UTF-8."""
    return text.encode("utf-8").decode("latin-1")


def cascade_lines(sheets: list[bytes], document: bytes, known: list) -> tuple[list[bytes], int]:
    """What `bn-css cascade` prints for a document given these sheets, a line to an item; and how many elements.

    `known` is the names of faces that are known (css_tool.known_families): a list of font families is given as
    the kind of face it comes to, worked out by the second reading.
    """
    rules: list[tuple[int, int, dict]] = []
    for order, sheet in enumerate(sheets):
        of_sheet: list[dict] = []
        css_tool.read_css(sheet, of_sheet, known)
        rules += [(order, index, rule) for index, rule in enumerate(of_sheet)]

    out: list[str] = []
    open_elements: list[Element] = []
    count = 0

    def settle(settings: list[tuple], mark: str) -> None:
        # In the order that decides: the last to set a property wins it.
        won: dict[str, str] = {}
        for _, name, value in sorted(settings, key=lambda setting: setting[0]):
            won[name] = value
        out.extend(f"{mark} {name}: {won[name]}" for name in PROPERTIES if name in won)

    def start(name: str, attributes: list[str]) -> None:
        nonlocal count
        count += 1
        pairs = [(bytes_as_text(attributes[i]), bytes_as_text(attributes[i + 1])) for i in range(0, len(attributes), 2)]
        element = Element(bytes_as_text(name), pairs, open_elements[-1] if open_elements else None)
        open_elements.append(element)
        own: list[tuple] = []
        first_letter: list[tuple] = []
        for order, index, rule in rules:
            for selector in rule["selectors"]:
                if not holds(selector["parts"], len(selector["parts"]) - 1, element):
                    continue
                into = first_letter if selector["first_letter"] else own
                for position, (name_set, value, important) in enumerate(rule["declarations"]):
                    into.append(((important, False, selector["rank"], order, index, position), name_set, value))
        style = element.attribute("style")
        if style is not None:
            position = 0
            for line in css_tool.read_style(style.encode("latin-1"), known):
                text = line.decode("latin-1")
                if text.startswith("D "):
                    name_set, _, value = text[2:].partition(": ")
                    important = value.endswith(" !important")
                    # The line is as the tool prints it; `settle` wants it as it was before that.
                    own.append(((important, True, (0, 0, 0), 0, 0, position), name_set,
                                unescaped(value[:-len(" !important")] if important else value)))
                    position += 1
        out.append("< " + element.name)
        settle(own, "P")
        settle(first_letter, "F")

    def end(_name: str) -> None:
        closed = open_elements.pop()
        if open_elements:
            open_elements[-1].last = closed
        out.append(">")

    parser = xml.parsers.expat.ParserCreate()
    parser.ordered_attributes = True
    parser.StartElementHandler = start
    parser.EndElementHandler = end
    parser.Parse(document, True)
    return [line.translate(css_tool.ESCAPES).encode("latin-1") for line in out], count


def unescaped(text: str) -> str:
    """A value as it was before the tool's way of printing a backslash and a line's end was put on it."""
    out = ""
    at = 0
    while at < len(text):
        if text[at] == "\\" and at + 1 < len(text):
            out += {"\\": "\\", "n": "\n", "t": "\t", "r": "\r"}.get(text[at + 1], text[at + 1])
            at += 2
        else:
            out += text[at]
            at += 1
    return out


def checksum(lines: list[bytes]) -> int:
    crc = 0
    for line in lines:
        crc = zlib.crc32(line + b"\n", crc)
    return crc


# ---- What the test makes.

SHEET = rb"""body { margin: 0 5%; font-family: Georgia, "Times New Roman", serif }
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
a > b c, a > b > c d { width: 50%; height: auto }
"""

SECOND_SHEET = rb"""p { text-indent: 3em; text-align: left !important }
.first { text-indent: 2em; float: right }
h2 + p { line-height: 1.2 }
div.note p { margin-left: 3em !important }
* { break-inside: avoid }
"""

DOCUMENT = """<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="en-GB" lang="en-GB">
<head><title>A document</title><style>p { text-indent: 9em }</style></head>
<body class="b i">
<section epub:type="frontmatter dedication"><p>For A.</p><P CLASS="first">Not a class.</P></section>
<hgroup><h2>One</h2><p>and</p><p>a half</p><p>more</p><p>and more</p><p>and yet more</p></hgroup>
<h2 title="Headings">One</h2>
text between
<p style="margin-top: 1em; text-indent: 4em; margin-left: 9em">It began.</p>
<p class="first  drop\tI" id="start">And went <i class="i">on</i>, <a href="#end" hidden="">there</a>&nbsp;.</p>
<div class="note" title="NOTES"><p>One.</p><ol><li><p>Two.</p></li><li epub:type="note"><p>Three.</p></li></ol>
<div><div><p>Deep.</p></div></div></div>
<blockquote><p>Said.</p></blockquote><blockquote o:type="note" xmlns:o="o"><p class="first">Again.</p></blockquote>
<table><tr><td>a</td><td>b</td></tr><tr><td>c</td><td>d</td></tr></table>
<a><b><x><b><c><y><c><d/></c></y></c></b></x></b></a>
<p class="first" id="end" style="float: none !important; text-align: justify">The end. café</p>
</body>
</html>
""".encode()


def random_sheet(rng: random.Random) -> bytes:
    def part() -> str:
        text = rng.choice(["div", "p", "span", "h2", "i", "*", "", "", ""])
        text += "".join("." + rng.choice("abc") for _ in range(rng.choice([0, 0, 1, 1, 2])))
        if rng.random() < 0.15:
            text += "#" + rng.choice("xy")
        if rng.random() < 0.2:
            text += rng.choice(["[lang]", "[lang|=en]", "[title~=w]", "[k^=a]", "[k$=b i]", "[*|k*=c]", "[title='']",
                                "[o|k]", "[k=AB i]"])
        return text or "*"

    lines = []
    for _ in range(rng.randrange(1, 15)):
        selectors = []
        for _ in range(rng.randrange(1, 4)):
            text = part()
            for _ in range(rng.choice([0, 0, 1, 1, 2, 3])):
                text += rng.choice([" ", " > ", " + ", " ", " + "]) + part()
            selectors.append(text + ("::first-letter" if rng.random() < 0.08 else ""))
        declarations = "; ".join(rng.choice(["float: left", "float: right", "width: 1px", "width: 2em", "margin: 0 1px",
                                             "text-indent: 0", "font-family: A, \"b c\"", "font-family: serif",
                                             "font-family: \"Trade Gothic LT Std\", Georgia",
                                             "list-style-type: none", "list-style: square inside",
                                             "list-style-type: lower-latin",
                                             "font: italic 1em/1.2 Foo, 'Courier New'", "font-family: Monotype Corsiva",
                                             "height: 50%", "font: italic 1em/1.5 x", "color: red"]) +
                                 (" !important" if rng.random() < 0.15 else "") for _ in range(rng.randrange(1, 5)))
        lines.append(", ".join(selectors) + " { " + declarations + " }")
    return "\n".join(lines).encode()


def random_document(rng: random.Random) -> bytes:
    def element(depth: int) -> str:
        name = rng.choice(["div", "p", "span", "h2", "i", "DIV", "q"])
        text = "<" + name
        if rng.random() < 0.5:
            text += " class='" + rng.choice(["a", "b", "c", "a b", "c  a", "b c a", ""]) + "'"
        if rng.random() < 0.2:
            text += " id='" + rng.choice("xyz") + "'"
        if rng.random() < 0.2:
            text += " " + rng.choice(["lang='en'", "lang='en-GB'", "title='v w'", "k='ab'", "o:k='xcB'", "title=''"])
        if rng.random() < 0.15:
            text += " style='" + rng.choice(["float: none", "width: 3px !important", "margin-top: 5px; height: 1px",
                                             "font-family: \"q\"; color: red", "font-family: Arial-R, serif",
                                             "font-family: &quot;a, lekton&quot;, Times"]) + "'"
        text += ">"
        for _ in range(rng.choice([0, 0, 1, 2, 3, 4]) if depth < 6 else 0):
            text += element(depth + 1) + rng.choice(["", "", " words "])
        return text + "</" + name + ">"

    return ("<r xmlns:o='o'>" + "".join(element(1) for _ in range(rng.randrange(1, 5))) + "</r>").encode()


def compare(tool: str, work: Path, sheets: list[bytes], document: bytes, what: str, known: list) -> int:
    """Runs `bn-css cascade` on the document and the sheets; returns how many lines agreed."""
    names = []
    for index, sheet in enumerate(sheets):
        names.append(str(work / f"sheet-{index}.css"))
        Path(names[-1]).write_bytes(sheet)
    (work / "document.xhtml").write_bytes(document)
    result = subprocess.run([tool, "cascade", str(work / "document.xhtml")] + names, capture_output=True, check=False)
    expect(result.returncode == 0, f"{what}: bn-css ended with {result.returncode}: {result.stderr!r}")
    got = result.stdout.split(b"\n")
    expect(got[-1] == b"", f"{what}: the last line is not ended")
    got.pop()
    expected, _ = cascade_lines(sheets, document, known)
    for number, (mine, theirs) in enumerate(zip(got, expected)):
        if mine != theirs:
            raise Failure(f"{what}: line {number + 1} is {mine!r}, the second working-out has {theirs!r}; "
                          f"the element is {[line for line in got[:number + 1] if line.startswith(b'<')][-1:]!r}")
    expect(len(got) == len(expected), f"{what}: {len(got)} lines, the second working-out has {len(expected)}")
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
        known = css_tool.known_families(tool)
        # The document with one sheet, with two in either order, with the same one twice, and with none.
        lines = 0
        for what, sheets in (("one sheet", [SHEET]), ("two sheets", [SHEET, SECOND_SHEET]),
                             ("two sheets, the other way", [SECOND_SHEET, SHEET]),
                             ("a sheet given twice", [SHEET, SECOND_SHEET, SHEET]), ("no sheet", [])):
            lines += compare(tool, work, sheets, DOCUMENT, what, known)
        expected, elements = cascade_lines([SHEET, SECOND_SHEET], DOCUMENT, known)
        styled = sum(1 for line in expected if line[:2] in (b"P ", b"F "))
        expect(elements >= 50 and styled > 200, f"only {elements} elements and {styled} things set: not much of a test")
        # The test's own working-out is not saying yes to everything: what it has to say of a few elements is known.
        text = b"\n".join(expected).decode("latin-1")
        expect("< p\nP line-height: 1.2\nP text-align: left\nP text-indent: 4em\nP margin-top: 1em\nP margin-right: 0px\n"
               "P margin-bottom: 0px\nP margin-left: 9em\nP break-inside: avoid\n>" in text,
               "the paragraph after the heading is not given what it should be")
        expect("< d\nP break-inside: avoid\nP width: 50%\nP height: auto\n>" not in text and
               text.count("P width: 50%") == 2, "`a > b c` and `a > b > c d` are not true of what they should be")

        # Sheets and documents made at random.
        rng = random.Random(20261116)
        rounds = 0
        for _ in range(120):
            sheets = [random_sheet(rng) for _ in range(rng.randrange(1, 4))]
            for _ in range(2):
                lines += compare(tool, work, sheets, random_document(rng), f"made at random (round {rounds})", known)
                rounds += 1

        # One line in place of all of them.
        result = subprocess.run([tool, "cascade", "--sum", str(work / "document.xhtml"), str(work / "sheet-0.css")],
                                capture_output=True, check=False)
        last_sheets = [(work / "sheet-0.css").read_bytes()]
        last_document = (work / "document.xhtml").read_bytes()
        expected, elements = cascade_lines(last_sheets, last_document, known)
        said = result.stdout.decode().split()
        expect(result.returncode == 0 and said[:4] == ["cascade", f"{checksum(expected):08x}", f"elements={elements}",
                                                       f"lines={len(expected)}"],
               f"--sum: bn-css says {result.stdout!r}; the second working-out has {checksum(expected):08x}, "
               f"{elements} elements, {len(expected)} lines")
        figures = dict(item.split("=") for item in said[4:])
        expect(set(figures) == {"table_bytes", "nodes", "level_bytes", "left_out", "too_deep", "too_many_classes",
                                "most_work"} and figures["left_out"] == "0" and figures["too_deep"] == "0",
               f"--sum: the figures are {figures}")

        # In an EPUB: the sheets and the document read out of the ZIP file.
        book = work / "book.epub"
        with zipfile.ZipFile(book, "w") as archive:
            archive.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
            archive.writestr("OEBPS/css/book.css", SHEET, zipfile.ZIP_DEFLATED)
            archive.writestr("OEBPS/css/more.css", SECOND_SHEET, zipfile.ZIP_STORED)
            archive.writestr("OEBPS/text/one.xhtml", DOCUMENT, zipfile.ZIP_DEFLATED)
        result = subprocess.run([tool, "cascade", "--in", str(book), "OEBPS/text/one.xhtml", "OEBPS/css/book.css",
                                 "OEBPS/css/more.css"], capture_output=True, check=False)
        expected, _ = cascade_lines([SHEET, SECOND_SHEET], DOCUMENT, known)
        expect(result.returncode == 0 and result.stdout.split(b"\n")[:-1] == expected,
               f"book.epub: what bn-css prints is not what the second working-out has: it ended with "
               f"{result.returncode}, {result.stderr!r}")

        # What cannot be read is said to be so.
        for what, arguments in (("a missing document", ["cascade", str(work / "none.xhtml")]),
                                ("a missing sheet", ["cascade", str(work / "document.xhtml"), str(work / "none.css")]),
                                ("a missing book", ["cascade", "--in", str(work / "none.epub"), "a"]),
                                ("an entry the book has not got", ["cascade", "--in", str(book), "OEBPS/none.xhtml"]),
                                ("a document that is not XML", ["cascade", str(work / "sheet-0.css")])):
            result = subprocess.run([tool] + arguments, capture_output=True, check=False)
            expect(result.returncode == 1 and result.stderr.startswith(b"error: "), f"{what} should be refused")
        expect(subprocess.run([tool, "cascade"], capture_output=True, check=False).returncode == 2,
               "no document should be refused")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"bn-css cascade agrees with the second working-out: {lines} lines of 5 pairings and {rounds} made at random, "
          "a sum and a book")
    return 0


if __name__ == "__main__":
    sys.exit(main())
