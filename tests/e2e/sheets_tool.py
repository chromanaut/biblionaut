#!/usr/bin/env python3
"""End-to-end check of `bn-css sheets` and `bn-css document`, run by CTest.

The tool finds, for each document of a book, the style sheets it asks
for, with the code the firmware finds them with (epub/document_sheets.h).
This works the same out a second way, in Python, for books it makes
itself: it reads a document's head with Python's expat, judges its links
and its `<style>` elements by the rules that header gives, goes down from
each link by calling itself, reading a sheet for its imports with the
second reader of CSS (css_tool.py), and keeps every name whole. The two
are set side by side: each document's list of sheets, line for line, and
how many things of each kind were left out. For some documents the rules
are then applied as well: `bn-css document` against the second
working-out of the cascade (css_cascade_tool.py), given the sheets found
here.

The tool's checksum of what the documents were given (epub/sheets_digest.h)
is worked out here too, from this finding and not from what the tool
printed. And each book is given a package, with a spine that has its
documents in another order than the ZIP file has them, leaves some out and
names one that is not there: `bn-css sheets --spine` has to go through
those documents in that order, whether it opens each from where the
package says it is or finds it by its name.

What is not worked out here is what the tool leaves out for want of room
(a sheet or an import past what it keeps): the books made here stay
inside that, and tools/check-books.py, which uses this for real books,
passes over a book where the tool says it ran out.

usage: sheets_tool.py BN_CSS WORK_DIR
"""

from __future__ import annotations

import random
import subprocess
import sys
import zipfile
import zlib
from collections import Counter
from collections.abc import Callable
from pathlib import Path
from urllib.parse import quote, unquote_to_bytes
from xml.parsers import expat

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True  # Or Python leaves compiled copies of the tests' scripts beside them.

import css_cascade_tool  # noqa: E402  (found through the path set just above)
import css_tool  # noqa: E402

# The limits epub/document_sheets.h gives.
MAX_LINKS = 8
MAX_DEPTH = 4
STYLE_BYTES = 4096
MAX_PATH = 512
MAX_PART = 256
OWN_STYLE = b"<style>"

# What is left out, by the names the tool gives the kinds, in its order.
KINDS = ("link for another medium", "link to no file of the book", "link after the most followed",
         "import of no file of the book", "import too deep", "import of a sheet on the way to it",
         "import after the most remembered", "sheet with no room", "sheet not read to its end",
         "style for another medium", "style too long", "import in a style", "head not read through")
# The kinds that say the tool ran out of room, or that a file was damaged:
# not worked out here.
NOT_WORKED_OUT = ("import after the most remembered", "sheet with no room", "sheet not read to its end")


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


class Stop(Exception):
    """The head is over: nothing more of the document is wanted."""


def resolved(base: bytes, reference: str) -> bytes | None:
    """The file of the book a reference means, from the folder `base`; None if it means none."""
    for mark in "#?":
        reference = reference.split(mark, 1)[0]
    if not reference or reference.startswith("//"):
        return None
    for at, ch in enumerate(reference):
        letter = "a" <= ch <= "z" or "A" <= ch <= "Z"
        other = "0" <= ch <= "9" or ch in "+-."
        if ch == ":":
            if at > 0:
                return None
            break
        if not letter and (at == 0 or not other):
            break
    parts = [] if reference.startswith("/") else [part for part in base.split(b"/") if part]
    for raw in reference.split("/"):
        part = unquote_to_bytes(raw)
        if "%" in raw and len(part) > MAX_PART:
            return None
        if part in (b"", b"."):
            continue
        if part == b"..":
            if parts:
                parts.pop()
            continue
        parts.append(part)
    path = b"/".join(parts)
    return path if path and len(path) <= MAX_PATH else None


def folder_of(name: bytes) -> bytes:
    return name.rpartition(b"/")[0]


def words(text: str) -> list[str]:
    """The words of an attribute, which HTML's spaces part, in small letters."""
    for space in css_tool.SPACE[1:]:
        text = text.replace(space, " ")
    return [css_tool.lower(word) for word in text.split(" ") if word]


def is_css(kind: str) -> bool:
    """Whether a `type` says CSS, or says nothing."""
    return css_tool.lower(kind.strip(css_tool.SPACE)) in ("", "text/css")


def head_of(document: bytes) -> tuple[list[dict[str, str]], list[dict], bool]:
    """A document's `link` elements (their attributes) and its `<style>` elements, as far as `<body>`, and
    whether the head could be read through."""
    links: list[dict[str, str]] = []
    styles: list[dict] = []
    state = {"root": False, "style": 0}

    def start(name: str, attributes: dict[str, str]) -> None:
        if state["style"]:
            state["style"] += 1
            return
        local = css_tool.lower(name.rpartition(":")[2])
        if not state["root"]:
            state["root"] = True
            if local != "html":
                raise Stop  # Not HTML: it has no head.
            return
        if local == "body":
            raise Stop
        if local == "link":
            links.append(attributes)
        elif local == "style":
            state["style"] = 1
            styles.append({"type": attributes.get("type", ""), "media": attributes.get("media", ""), "text": [],
                           "closed": False})

    def end(name: str) -> None:
        if state["style"]:
            state["style"] -= 1
            if not state["style"]:
                styles[-1]["closed"] = True

    def text(piece: str) -> None:
        if state["style"]:
            styles[-1]["text"].append(piece)

    parser = expat.ParserCreate()
    parser.StartElementHandler = start
    parser.EndElementHandler = end
    parser.CharacterDataHandler = text
    through = True
    try:
        parser.Parse(document, True)
    except Stop:
        pass
    except expat.ExpatError:
        through = False
    return links, styles, through


def imports_of(sheet: bytes) -> list[str]:
    """The sheets a sheet imports, as it writes them."""
    return [css_cascade_tool.unescaped(line[2:].decode("utf-8", "replace")) for line in css_tool.read_css(sheet)
            if line.startswith(b"I ")]


class Book:
    """What a book's documents are given, worked out from its files: `has` says whether it has a file of a name,
    and `read` gives one's bytes."""

    def __init__(self, has: Callable[[bytes], bool], read: Callable[[bytes], bytes], style_bytes: int = STYLE_BYTES):
        self.has = has
        self.read = read
        self.style_bytes = style_bytes
        self.imports: dict[bytes, list[str]] = {}

    def imports_from(self, sheet: bytes) -> list[str]:
        # A sheet's imports are the same however often it is come to.
        if sheet not in self.imports:
            self.imports[sheet] = imports_of(self.read(sheet))
        return self.imports[sheet]

    def sheets_of(self, name: bytes) -> tuple[list[bytes], Counter, list[bytes]]:
        """The document's list (OWN_STYLE for its own rules), what was left out by kind, and the text of each
        `<style>` that is read."""
        left: Counter = Counter()
        links, styles, through = head_of(self.read(name))
        if not through:
            left["head not read through"] += 1

        followed: list[bytes] = []
        for attributes in links:
            if "href" not in attributes:
                continue
            rel = words(attributes.get("rel", ""))
            if "stylesheet" not in rel or "alternate" in rel or not is_css(attributes.get("type", "")):
                continue
            if not css_tool.media_applies(attributes.get("media", "")):
                left["link for another medium"] += 1
                continue
            path = resolved(folder_of(name), attributes["href"])
            if path is None:
                left["link to no file of the book"] += 1
            elif len(followed) == MAX_LINKS:
                left["link after the most followed"] += 1
            else:
                followed.append(path)

        listed: list[bytes] = []

        def give(sheet: bytes | None, is_import: bool, within: tuple[bytes, ...]) -> None:
            if is_import and len(within) > MAX_DEPTH:
                left["import too deep"] += 1
                return
            if is_import and sheet is not None and sheet in within:
                left["import of a sheet on the way to it"] += 1
                return
            if sheet is None or not self.has(sheet):
                left["import of no file of the book" if is_import else "link to no file of the book"] += 1
                return
            for url in self.imports_from(sheet):
                give(resolved(folder_of(sheet), url), True, within + (sheet,))
            # Named before, it counts where it is named last.
            if sheet in listed:
                listed.remove(sheet)
            listed.append(sheet)

        for path in followed:
            give(path, False, ())

        own: list[bytes] = []
        used = 0
        for style in styles:
            if not is_css(style["type"]):
                continue
            if not css_tool.media_applies(style["media"]):
                left["style for another medium"] += 1
                continue
            text = "".join(style["text"]).encode("utf-8")
            if not style["closed"] or not text:
                continue
            if used + len(text) + 1 > self.style_bytes:
                left["style too long"] += 1
                continue
            used += len(text) + 1
            own.append(text)
            left["import in a style"] += len(imports_of(text))
        if own:
            listed.append(OWN_STYLE)
        return listed, +left, own


def digest_of(found: list[tuple[bytes, list[bytes] | None, Counter]]) -> int:
    """The checksum epub/sheets_digest.h describes, of documents in their order: each with its list (or None for
    one that could not be opened) and what was left out for it."""
    lines = b""
    for name, listed, left in found:
        lines += b"D " + name + b"\n"
        if listed is None:
            lines += b"N\n"
            continue
        for sheet in listed:
            lines += b"S " + sheet + b"\n"
        for kind in KINDS:
            if left[kind]:
                lines += f"X {kind} {left[kind]}\n".encode()
    return zlib.crc32(lines)


def said_by(tool: str, book: Path, *options: str) -> tuple[dict[bytes, list[bytes]], Counter, dict[str, int]] | str:
    """What `bn-css sheets --dropped` says of a book: each document's list, how many things of each kind were
    left out, and its other figures; or, as text, why that could not be had. `options` go to the tool as well."""
    result = subprocess.run([tool, "sheets", str(book), "--dropped", *options], capture_output=True, check=False)
    lines = result.stdout.split(b"\n")[:-1]
    if result.returncode != 0 or len(lines) < 4 or not lines[-4].startswith(b"sheets_documents="):
        return f"ended with {result.returncode} and its last lines are {lines[-4:]!r}: {result.stderr[-120:]!r}"
    lists: dict[bytes, list[bytes]] = {}
    told: Counter = Counter()
    document = b""
    for line in lines[:-4]:
        mark, _, rest = line.partition(b" ")
        if mark == b"D":
            document = css_cascade_tool.unescaped(rest.decode("utf-8", "surrogateescape")).encode("utf-8", "surrogateescape")
            lists[document] = []
        elif mark == b"S":
            lists[document].append(css_cascade_tool.unescaped(rest.decode("utf-8", "surrogateescape"))
                                   .encode("utf-8", "surrogateescape"))
        elif mark == b"X":
            told[rest.partition(b": ")[0].decode("utf-8", "replace")] += 1
    # Its figures: numbers, but for the checksum, which is written in hexadecimal.
    figures = {key: int(value, 16 if key == "sheets_checksum" else 10)
               for line in (lines[-4], lines[-3], lines[-1])
               for key, value in (item.split("=") for item in line.decode().split())}
    kinds = {key.replace("_", " "): int(value)
             for key, value in (item.split("=") for item in lines[-2].decode().split()[1:])}
    left = Counter({kind: count for kind, count in kinds.items() if count})
    if left != told:
        return f"its counts are {dict(left)} and its lines of what was left out come to {dict(told)}"
    return lists, left, figures


# ---- What the test makes.

def page(head: str, body: str = "<p>Text.</p>") -> bytes:
    return (f"<?xml version='1.0' encoding='utf-8'?>\n<html xmlns='http://www.w3.org/1999/xhtml'><head>"
            f"<title>A page</title>{head}</head><body>{body}</body></html>").encode()


def link(href: str, more: str = "") -> str:
    return f"<link rel='stylesheet' type='text/css' href='{href}' {more}/>"


RULE = b"p { text-indent: 1em; margin: 0 } h1 + p, p.first { text-indent: 0 }\n"


def written_books() -> dict[str, dict[bytes, bytes]]:
    """Books that each try one thing, by name: a book is its files."""
    books: dict[str, dict[bytes, bytes]] = {}
    books["links in order, from the document's folder"] = {
        b"OEBPS/text/one.xhtml": page(link("../css/b.css") + link("a.css") + link("/top.css#x")),
        b"OEBPS/text/two.xhtml": page(link("a.css") + link("../css/b.css")),
        b"OEBPS/css/b.css": RULE, b"OEBPS/text/a.css": b"p.first { margin-left: 2em }", b"top.css": RULE,
    }
    books["which links count"] = {
        b"page.xhtml": page("<link rel='StyleSheet' href='caps.css'/><link rel='alternate stylesheet' href='n.css'/>"
                            "<link rel='stylesheet\talternate' href='n.css'/><link rel='icon' href='i.css'/>"
                            "<link href='norel.css'/><link rel='stylesheet'/>"
                            "<link rel='stylesheet' type=' TEXT/CSS ' href='typed.css'/>"
                            "<link rel='stylesheet' type='application/vnd.adobe-page-template+xml' href='t.xpgt'/>"
                            "<link rel=' author  stylesheet ' href='words.css'/>"
                            "<x:link xmlns:x='http://www.w3.org/1999/xhtml' rel='stylesheet' href='prefixed.css'/>"),
        **{name: RULE for name in (b"caps.css", b"n.css", b"i.css", b"norel.css", b"typed.css", b"t.xpgt",
                                   b"words.css", b"prefixed.css")},
    }
    books["media"] = {
        b"page.xhtml": page(link("kindle.css", "media='amzn-mobi'") + link("all.css", "media='all'") +
                            link("screen.css", "media='screen'") + link("others.css", "media='not amzn-mobi'") +
                            link("newer.css", "media='(min-device-width: 0px)'") + link("speech.css", "media='speech'") +
                            "<style media='print'>p { margin: 0 }</style><style media='screen'>p { margin: 1em }</style>"),
        **{name: RULE for name in (b"kindle.css", b"all.css", b"screen.css", b"others.css", b"newer.css", b"speech.css")},
    }
    books["links to no file, and too many links"] = {
        b"page.xhtml": page(link("https://example.org/a.css") + link("gone.css") + link("") +
                            "".join(link(f"s{i}.css") for i in range(11))),
        b"again.xhtml": page(link("gone.css") + link("s0.css")),
        **{f"s{i}.css".encode(): RULE for i in range(11)},
    }
    books["imports, their order, their folders and their media"] = {
        b"page.xhtml": page(link("css/a.css") + link("z.css")),
        b"css/a.css": b"@import 'b.css'; @import url(more/c.css); @import 'print.css' print; " + RULE,
        b"css/b.css": b"@import \"../d.css\"; @import 'gone.css'; @import url(http://example.org/f.css); " + RULE,
        b"css/more/c.css": RULE, b"d.css": RULE, b"z.css": RULE, b"css/print.css": RULE,
    }
    books["imports too deep, and round in a ring"] = {
        b"page.xhtml": page(link("s0.css") + link("ring.css") + link("self.css")),
        b"deep.xhtml": page(link("s4.css")),
        **{f"s{i}.css".encode(): f"@import 's{i + 1}.css'; ".encode() + RULE for i in range(7)},
        b"ring.css": b"@import 'gnir.css'; " + RULE, b"gnir.css": b"@import 'ring.css'; " + RULE,
        b"self.css": b"@import 'self.css'; " + RULE,
    }
    books["a sheet named twice"] = {
        b"links.xhtml": page(link("a.css") + link("b.css") + link("a.css")),
        b"imports.xhtml": page(link("c.css") + link("d.css")),
        b"a.css": RULE, b"b.css": RULE, b"shared.css": b"p { text-indent: 3em }",
        b"c.css": b"@import 'shared.css'; p { text-indent: 1em }", b"d.css": b"@import 'shared.css'; h1 { margin: 0 }",
    }
    long_style = "p.long { margin: 0 } " + " " * STYLE_BYTES
    books["a document's own style"] = {
        b"before.xhtml": page("<style type='text/css'>p { text-indent: 2em } h1 { font-weight: bold }</style>" + link("a.css")),
        b"plain.xhtml": page(link("a.css")),
        b"several.xhtml": page("<style>p { text-indent: 1em</style><style>h1 { font-weight: bold }</style>"
                               "<style type='text/x-other'>p { margin: 9em }</style><style></style>"),
        b"long.xhtml": page(f"<style>h1 {{ margin: 0 }}</style><style>{long_style}</style><style>h2 {{ margin: 0 }}</style>"),
        b"fits.xhtml": page("<style>" + " " * (STYLE_BYTES - 1) + "</style>"),
        b"over.xhtml": page("<style>" + " " * STYLE_BYTES + "</style>"),
        b"import.xhtml": page("<style>@import 'a.css'; p { text-indent: 1em }</style>"),
        b"comment.xhtml": page("<!--[if gte mso 9]><style>p { text-indent: 1em }</style><![endif]-->"),
        b"a.css": b"p { text-indent: 1em }",
    }
    books["the head and no more"] = {
        b"body.xhtml": page(link("a.css"), link("late.css") + "<style>p { margin: 0 }</style>" + "x" * 200000),
        b"page.svg.xhtml": (b"<svg xmlns='http://www.w3.org/2000/svg'><link rel='stylesheet' href='a.css'/>"
                            b"<style>p { margin: 0 }</style></svg>"),
        b"broken.xhtml": ("<html><head>" + link("a.css") + "<meta charset='utf-8'><style>p { margin: 0 }</style>"
                          "</head><body/></html>").encode(),
        b"cut.xhtml": ("<html><head>" + link("a.css") + "<style media='print'>p { margin: 0 }").encode(),
        b"nothing.xhtml": b"",
        b"text.xhtml": b"Not XML at all.",
        b"a.css": RULE, b"late.css": RULE,
    }
    return books


def random_book(rng: random.Random) -> dict[bytes, bytes]:
    files: dict[bytes, bytes] = {}
    sheets = rng.randrange(1, 10)
    for sheet in range(sheets):
        text = ""
        for _ in range(rng.randrange(1, 4) if rng.randrange(4) == 0 else 0):
            if rng.randrange(10) == 0:
                text += "@import 'http://example.org/y.css';"
            else:
                text += f"@import 's{rng.randrange(sheets + 1)}.css'{' print' if rng.randrange(6) == 0 else ''};"
        text += f"p.s{sheet} {{ text-indent: {sheet}em }} h{1 + sheet % 3} {{ margin: {sheet}px }}"
        files[f"css/s{sheet}.css".encode()] = text.encode()
    for document in range(rng.randrange(1, 6)):
        head = ""
        for _ in range(rng.randrange(7)):
            kind = rng.randrange(10)
            if kind < 6:
                rel = rng.choice(["stylesheet"] * 3 + ["Stylesheet", "alternate stylesheet", "icon"])
                more = rng.choice([""] * 3 + ["type='text/css'", "media='screen'", "media='print'", "type='text/plain'",
                                              "media='(min-width: 1px)'"])
                href = "http://example.org/x.css" if rng.randrange(8) == 0 else f"../css/s{rng.randrange(sheets + 2)}.css"
                head += f"<link rel='{rel}' href='{href}' {more}/>"
            elif kind < 8:
                text = rng.choice(["p { text-indent: 1em }", "", "@import 's0.css'; h1 { font-weight: bold }",
                                   "p { margin: 0", "h2 { font-style: italic } p { text-indent: 0 }"])
                pad = " " * (rng.randrange(3000) if rng.randrange(3) == 0 else 0)
                head += f"<style{' media=' + chr(39) + 'print' + chr(39) if rng.randrange(5) == 0 else ''}>{text}{pad}</style>"
            elif kind == 8:
                head += "<meta name='x' content='y'/>"
            else:
                head += "<!-- a comment <style>p { margin: 0 }</style> -->"
        body = "".join(f"<p class='s{rng.randrange(sheets)}'>Text.</p>" for _ in range(rng.randrange(1, 6)))
        data = page(head, "<h1>A page</h1>" + body)
        if rng.randrange(10) == 0:
            data = data[:rng.randrange(len(data))]
        files[f"text/d{document}.xhtml".encode()] = data
    return files


def write_book(path: Path, files: dict[bytes, bytes], rng: random.Random) -> None:
    names = list(files)
    rng.shuffle(names)
    with zipfile.ZipFile(path, "w") as archive:
        for name in names:
            archive.writestr(zipfile.ZipInfo(name.decode()), files[name],
                             zipfile.ZIP_DEFLATED if rng.randrange(2) else zipfile.ZIP_STORED)


def documents_of(files: dict[bytes, bytes], path: Path) -> list[bytes]:
    """The book's documents, in the order the ZIP file has them: those the tool goes through."""
    with zipfile.ZipFile(path) as archive:
        names = [info.filename.encode() for info in archive.infolist()]
    return [name for name in names if name.lower().endswith((b".xhtml", b".html", b".htm"))]


def with_a_package(files: dict[bytes, bytes], rng: random.Random) -> tuple[dict[bytes, bytes], list[bytes]]:
    """The book with a package: its documents in the manifest, and a spine that has most of them, in an order of
    its own, and one document more that the book has not got. Returns the book and the spine's documents that
    are in it, in the spine's order."""
    documents = [name for name in files if name.lower().endswith((b".xhtml", b".html", b".htm"))]
    rng.shuffle(documents)
    spine = [name for name in documents if rng.randrange(5)] or documents[:1]
    manifest = "".join(f'<item id="d{number}" href="{quote(name)}" media-type="application/xhtml+xml"/>'
                       for number, name in enumerate(documents))
    manifest += '<item id="gone" href="no-such-chapter.xhtml" media-type="application/xhtml+xml"/>'
    refs = [f'<itemref idref="d{documents.index(name)}"/>' for name in spine]
    refs.insert(rng.randrange(len(refs) + 1), '<itemref idref="gone"/>')
    made = dict(files)
    made[b"META-INF/container.xml"] = (b'<container><rootfiles><rootfile full-path="content.opf" '
                                       b'media-type="application/oebps-package+xml"/></rootfiles></container>')
    made[b"content.opf"] = (f'<package xmlns="http://www.idpf.org/2007/opf"><metadata/><manifest>{manifest}</manifest>'
                            f'<spine>{"".join(refs)}</spine></package>').encode()
    return made, spine


def check_spine(tool: str, path: Path, files: dict[bytes, bytes], spine: list[bytes], what: str) -> int:
    """Sets what the tool finds going through a book's spine beside the second working-out, in the spine's order.
    Returns how many documents that was."""
    book = Book(lambda name: name in files, lambda name: files[name])
    found = [(name, *book.sheets_of(name)[:2]) for name in spine]
    by_place = said_by(tool, path, "--spine")
    by_name = said_by(tool, path, "--spine", "--by-name")
    for how, said in (("opened from their places", by_place), ("found by their names", by_name)):
        expect(not isinstance(said, str), f"{what}: bn-css sheets --spine, its documents {how}, {said}")
        lists, left, figures = said
        expect(list(lists) == spine,
               f"{what}: {how}, the tool went through {list(lists)}, and the spine's documents are {spine}")
        for name, listed, _ in found:
            expect(lists[name] == listed,
                   f"{what}: {how}, {name!r} is given {lists[name]}; the second working-out has {listed}")
        expect(left == sum((document_left for _, _, document_left in found), Counter()),
               f"{what}: {how}, left out, by kind: {dict(left)}")
        expect(figures["sheets_checksum"] == digest_of(found),
               f"{what}: {how}, the tool's checksum is {figures['sheets_checksum']:08x}; the second working-out's "
               f"lines come to {digest_of(found):08x}")
        expect(figures["sheets_documents"] == len(spine) and figures["sheets_absent"] == 1 and
               figures["sheets_unopened"] == 0, f"{what}: {how}, its figures are {figures}")
    return len(spine)


def check_book(tool: str, path: Path, files: dict[bytes, bytes], what: str) -> tuple[int, int, int]:
    """Sets the tool's finding beside the second working-out for one book. Returns how many documents, sheets given
    and things left out there were."""
    said = said_by(tool, path)
    expect(not isinstance(said, str), f"{what}: bn-css sheets {said}")
    lists, left, figures = said
    book = Book(lambda name: name in files, lambda name: files[name])
    expected_left: Counter = Counter()
    given = 0
    names = documents_of(files, path)
    expect(list(lists) == names, f"{what}: the tool went through {list(lists)}, and the book's documents are {names}")
    found = []
    for name in names:
        listed, document_left, own = book.sheets_of(name)
        expect(lists[name] == listed, f"{what}: {name!r} is given {lists[name]}; the second working-out has {listed}")
        expected_left += document_left
        given += len(listed)
        found.append((name, listed, document_left))

        # The rules applied, where the whole document is XML: the same
        # sheets in the same order, by the second working-out of the cascade.
        try:
            expat.ParserCreate().Parse(files[name], True)
        except expat.ExpatError:
            continue
        sheets = [files[sheet] for sheet in listed if sheet != OWN_STYLE] + own
        lines, elements = css_cascade_tool.cascade_lines(sheets, files[name], css_tool.known_families(tool))
        result = subprocess.run([tool, "document", "--sum", str(path), name.decode()], capture_output=True, check=False)
        words_said = result.stdout.decode("utf-8", "replace").split()
        expect(result.returncode == 0 and len(words_said) > 3 and words_said[0] == "document",
               f"{what}: bn-css document {name!r} ended with {result.returncode}: {result.stderr[-200:]!r}")
        mine = (words_said[1], words_said[2], words_said[3])
        theirs = (f"{css_cascade_tool.checksum(lines):08x}", f"elements={elements}", f"lines={len(lines)}")
        expect(mine == theirs, f"{what}: the rules applied to {name!r} come to {mine}; the second working-out has {theirs}")
    expect(left == expected_left, f"{what}: left out, by kind: {dict(left)}; the second working-out has {dict(expected_left)}")
    expect(not any(left[kind] for kind in NOT_WORKED_OUT), f"{what}: the tool ran out of room: {dict(left)}")
    expect(figures["sheets_documents"] == len(names) and figures["sheets_given"] == given and
           figures["sheets_unopened"] == 0 and figures["sheets_absent"] == 0 and
           figures["sheets_own"] == sum(1 for _, listed, _ in found if OWN_STYLE in listed),
           f"{what}: its figures are {figures}")
    expect(figures["sheets_checksum"] == digest_of(found),
           f"{what}: the tool's checksum is {figures['sheets_checksum']:08x}; the second working-out's lines come "
           f"to {digest_of(found):08x}")
    return len(names), given, sum(left.values())


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tool = sys.argv[1]
    work = Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    rng = random.Random(20261124)
    documents = given = left = books = in_spines = 0
    try:
        made = list(written_books().items()) + [(f"book {seed} made at random", random_book(rng)) for seed in range(100)]
        for number, (what, files) in enumerate(made):
            path = work / f"book-{number}.zip"
            write_book(path, files, rng)
            counts = check_book(tool, path, files, what)
            documents += counts[0]
            given += counts[1]
            left += counts[2]
            books += 1
            # The same book with a package, gone through by its spine.
            with_package, spine = with_a_package(files, rng)
            write_book(path, with_package, rng)
            in_spines += check_spine(tool, path, files, spine, what)
            path.unlink()
        expect(given > 150 and left > 100, f"the books were too plain: {given} sheets given and {left} things left out")
        expect(in_spines > 200, f"the spines were too short: {in_spines} documents in all")

        # A book with no package has no spine to go through.
        (work / "bare.zip").unlink(missing_ok=True)
        write_book(work / "bare.zip", {b"a.xhtml": page("")}, rng)
        bare = subprocess.run([tool, "sheets", str(work / "bare.zip"), "--spine"], capture_output=True, check=False)
        expect(bare.returncode == 1 and b"no container" in bare.stderr, f"a book with no package: {bare}")
        (work / "bare.zip").unlink()
        # And finding by name is only a way of going through a spine.
        odd = subprocess.run([tool, "sheets", str(work / "bare.zip"), "--by-name"], capture_output=True, check=False)
        expect(odd.returncode == 2, f"--by-name without --spine: {odd}")

        # What is asked of a book that is not one.
        missing = subprocess.run([tool, "sheets", str(work / "none.zip")], capture_output=True, check=False)
        expect(missing.returncode == 1 and b"cannot read" in missing.stderr, f"a book that is not there: {missing}")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"{books} books, {documents} documents: each given the sheets the second working-out gives it, {given} in "
          f"all, with {left} things left out, kind for kind; the rules applied come to the same; and {in_spines} "
          f"documents of their spines, in reading order, are given the same again")
    return 0


if __name__ == "__main__":
    sys.exit(main())
