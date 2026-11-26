#!/usr/bin/env python3
"""Checks what the book tools make of real books against a reading of each by Python.

bn-zip, bn-xml, bn-epub, bn-css, bn-png, bn-jpeg and bn-bundle run the firmware's own
code on one file. This runs them over every EPUB under the folders it is given and
compares, book by book:

  - bn-zip with Python's zipfile: every entry of the directory, every
    entry's contents by its checksum, and a few entries byte for byte;
  - bn-xml with Python's expat: every XML document, event for event;
  - bn-epub info with a reading of the package by ElementTree;
  - bn-epub toc with a reading of each table of contents the book has;
  - bn-css with a second reading of every style sheet, line for line;
  - bn-css sheets with a second finding of the style sheets each document
    asks for: those its head links, each after the sheets it imports, and
    its own `<style>` rules, in their order, and what was left out by its
    kind;
  - bn-css sheets --spine with the same finding for the documents of the
    book's spine, in reading order, as a reader goes through a book: by
    the checksum of what every document is given, which is worked out
    here from the second finding; and that the tool finds the same
    whether it opens each document from where the package says it is or
    finds it by its name;
  - bn-css document with a second working-out of what the rules come to
    for each element, on the first few documents of the book, each given
    the sheets the second finding gives it; and bn-css document --styles
    with a second working-out of what each element then comes to, with
    what it takes from the element it is inside and what its element
    gives it, on the same documents; and bn-css document --blocks with a
    second working-out of the blocks of text each of those documents is
    made into, with each block's record and how its text is set;
  - bn-png book with a second reading of every PNG picture: its header, how
    the reading ended, and the checksum of every row of gray;
  - bn-jpeg book with a second reading of every JPEG picture, both read at
    an eighth of their size: its header, how the reading ended, what was
    passed over, and the checksum of every row of gray, which at an eighth
    is worked out in whole numbers and so the same byte for byte;
  - bn-bundle make with a second reading of the bundle it makes of the
    book, by the places the format gives things: what its header page
    says of the EPUB, its strings, a record for every document of the
    spine and every picture, and every entry of its contents, each set
    beside the book as zipfile and ElementTree read it; and bn-bundle
    info with what that reading comes to.

It also counts, and compares with nothing, since no tool reads a
document's text yet: the paragraphs and the ids of every document of each
book's spine. A paragraph is where tools/layout-text.py has one, the text
between the starts and ends of block elements, and its size is that text
in UTF-8 with white space in a row made one space and a br made a line
end; what is in head, script, style, svg or math is no part of it. An id
is the value of any element's id, or of an a's name where it has no id,
counted once in a document. A line says what the largest paragraph, the
most paragraphs and the most ids of a document are, book by book.

The readings of the ZIP file, the documents, the package, the style sheets,
the pictures and the bundle, the finding of a document's sheets and the
working-out of the cascade, are the ones the end-to-end tests use on the
books they make (tests/e2e). The reading of a table of contents is written here,
since real tables do things the tests' own do not.

With --css-report it also says what the style sheets of the books lose to
what the reader of CSS does not read: each kind of thing that was passed
over, and the properties, pseudo-classes and at-rules passed over most,
each with how many times and in how many books. It says what keeping a
book's rules takes: the size of a table of all of them, book by book. And
it says what the documents ask for: how many sheets, how many have rules
of their own, and each kind of thing that was left out in finding them,
with how many times and in how many books; and how many reads of the file
finding the sheets of a spine's documents takes, with each document
opened from its place and with each found by its name.

With --document-report it says more of the documents of the spines: how
large they are, how many paragraphs and ids each has, how long the names
of its ids are, how many have a paragraph over each of a few sizes, and
which documents of which books have the most of each.

A folder with a file named SHA256SUMS in it is also checked against that
file: every file it lists has to be there and unchanged, and every EPUB
beside or below it has to be listed.

usage: tools/check-books.py [--tools BUILD] [--zip BN_ZIP] [--xml BN_XML] [--epub BN_EPUB] [--css BN_CSS]
                            [--png BN_PNG] [--jpeg BN_JPEG] [--bundle BN_BUNDLE] [--css-report]
                            [--document-report] [--jobs N]
                            FOLDER_OR_FILE...

BUILD is a build folder to take the tools from: build/host-release if it
has them, or else build/host. Each of the seven can be named by itself.

It ends with 0 if everything agreed, 1 if anything did not, 2 if it could
not start.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import importlib.util
import os
import posixpath
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile
import zlib
from collections import Counter
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote_to_bytes

REPOSITORY = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPOSITORY / "tests" / "e2e"))
sys.dont_write_bytecode = True  # Or Python leaves compiled copies of the tests' scripts beside them.

import bundle_tool  # noqa: E402  (found through the path set just above)
import css_cascade_tool  # noqa: E402
import css_tool  # noqa: E402
import blocks_tool  # noqa: E402
import styles_tool  # noqa: E402
import epub_tool  # noqa: E402
import jpeg_tool  # noqa: E402
import png_tool  # noqa: E402
import sheets_tool  # noqa: E402
import xml_tool  # noqa: E402
import zip_tool  # noqa: E402

XML_SUFFIXES = (".xhtml", ".html", ".htm", ".xml", ".opf", ".ncx")
SPACE = re.compile(r"[ \t\r\n]+")
MAX_LABEL = 256
MAX_FRAGMENT = 128
# The second working-out of the cascade tries everything, and is slow: of
# each book it is given so many documents, with so many elements between
# them, from the first of the book's files.
MAX_CASCADE_DOCUMENTS = 6
MAX_CASCADE_ELEMENTS = 1500
MAX_CASCADE_LOOKED_AT = 40
# The report names so many documents for each thing it has the most of, and
# counts the documents with a paragraph over each of these sizes.
MOST_NAMED = 5
PARAGRAPH_SIZES = (4096, 8192, 16384, 32768, 65536)


def script_beside(name: str):
    """One of the scripts beside this one, as a module. Their names have a hyphen, which `import` does not take."""
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), Path(__file__).resolve().parent / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


layout_text = script_beside("layout-text")


def run(*command: str) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, check=False)


def local(tag: object) -> str:
    return tag.rsplit("}", 1)[-1] if isinstance(tag, str) else ""


def parsed(data: bytes) -> ET.Element:
    """A document as ElementTree reads it, with the one name of HTML's that books use outside their chapters."""
    parser = ET.XMLParser()
    parser.entity["nbsp"] = " "
    parser.feed(data)
    return parser.close()


def resolved(base: bytes, reference: str) -> bytes | None:
    """The file of the book a reference means, from the folder `base`; None if it means none."""
    reference = re.split(r"[#?]", reference, maxsplit=1)[0]
    if not reference or reference.startswith("//") or re.match(r"[A-Za-z][A-Za-z0-9+.\-]*:", reference):
        return None
    parts = [] if reference.startswith("/") else [part for part in base.split(b"/") if part]
    for raw in reference.split("/"):
        part = unquote_to_bytes(raw)
        if part in (b"", b"."):
            continue
        if part == b"..":
            if parts:
                parts.pop()
            continue
        parts.append(part)
    return b"/".join(parts) or None


def label_of(element: ET.Element) -> bytes:
    """An entry's label: its text with each run of spaces one space, and no more than 256 bytes of it."""
    label = SPACE.sub(" ", "".join(element.itertext())).strip(" ").encode("utf-8")
    if len(label) > MAX_LABEL:
        label = label[:MAX_LABEL]
        while label:  # A character cut part way goes whole.
            try:
                label.decode("utf-8")
                break
            except UnicodeDecodeError:
                label = label[:-1]
    return label.rstrip(b" ")


def printable(text: bytes) -> bytes:
    """As bn-epub prints a label: control characters and the backslash by their numbers."""
    return b"".join(b"\\x%02X" % byte if byte < 0x20 or byte == 0x5C else bytes([byte]) for byte in text)


def tables_of_contents(path: Path) -> dict[str, list[tuple[bytes, bytes]] | None]:
    """Each table of contents the book has, "nav" and "ncx", as its entries; None for a file with no table in it.

    An entry is a pair: the line bn-epub prints for it, and the line that
    goes into the checksum, which has the label and the place as they are
    and not as they are printed.
    """
    with zipfile.ZipFile(path) as archive:
        # The first entry of each name, the names read as UTF-8 whatever the ZIP file says of them.
        names: dict[str, zipfile.ZipInfo] = {}
        for info in archive.infolist():
            names.setdefault(epub_tool.entry_name(info), info)
        container = ET.fromstring(archive.read("META-INF/container.xml"))
        package_path = next(e.get("full-path") for e in container.iter() if local(e.tag) == "rootfile")
        package = parsed(archive.read(package_path))
        base = posixpath.dirname(package_path).encode()
        items = [e for e in package.iter() if local(e.tag) == "item" and resolved(base, e.get("href") or "")]
        by_id: dict[str, ET.Element] = {}
        for item in items:
            if item.get("id"):
                by_id.setdefault(item.get("id"), item)

        def where(item: ET.Element) -> bytes:
            return resolved(base, item.get("href"))

        spine_element = next(e for e in package if local(e.tag) == "spine")
        spine = [where(by_id[e.get("idref")]) for e in spine_element
                 if local(e.tag) == "itemref" and e.get("idref") in by_id]
        nav = next((e for e in items if "nav" in (e.get("properties") or "").split()), None)
        ncx = by_id.get(spine_element.get("toc")) if spine_element.get("toc") else None
        if ncx is None:
            ncx = next((e for e in items if (e.get("media-type") or "").lower() == "application/x-dtbncx+xml"), None)

        tables: dict[str, list[tuple[bytes, bytes]] | None] = {}
        for kind, item in (("nav", nav), ("ncx", ncx)):
            if item is None or where(item).decode("utf-8", "surrogateescape") not in names:
                continue
            own = where(item)
            folder = own.rsplit(b"/", 1)[0] if b"/" in own else b""
            root = parsed(archive.read(names[own.decode("utf-8", "surrogateescape")]))
            entries: list[tuple[bytes, bytes]] = []

            def entry(depth: int, text: ET.Element, reference: str | None) -> None:
                document = fragment = None
                if reference is not None:
                    file = resolved(folder, reference)
                    if file is None and reference.startswith("#"):
                        file = own
                    if file is not None and file in spine:
                        document = spine.index(file)
                        if "#" in reference:
                            place = unquote_to_bytes(reference.split("#", 1)[1])
                            fragment = place if 0 < len(place) <= MAX_FRAGMENT else None
                start = b"%d\t%s\t" % (depth, str(document).encode() if document is not None else b"-")
                label = label_of(text)
                entries.append((start + (printable(fragment) if fragment else b"-") + b"\t" + printable(label),
                                start + (fragment or b"-") + b"\t" + label))

            if kind == "ncx":
                maps = [e for e in root.iter() if local(e.tag) == "navMap"]
                if not maps:
                    tables[kind] = None
                    continue

                def points(element: ET.Element):
                    for child in element:
                        if local(child.tag) == "navPoint":
                            yield child
                        else:
                            yield from points(child)

                def before_the_first_point(element: ET.Element):
                    """What a navPoint holds ahead of the first navPoint inside it. Returns False once it has met one."""
                    for child in element:
                        if local(child.tag) == "navPoint":
                            return False
                        yield child
                        if (yield from before_the_first_point(child)) is False:
                            return False
                    return True

                def walk(element: ET.Element, depth: int) -> None:
                    for point in points(element):
                        text = source = None
                        for inner in before_the_first_point(point):
                            if text is None and local(inner.tag) == "text":
                                text = inner
                            if source is None and local(inner.tag) == "content" and inner.get("src") is not None:
                                source = inner.get("src")
                        entry(depth, text if text is not None else ET.Element("none"), source)
                        walk(point, depth + 1)

                walk(maps[0], 0)
            else:
                lists = [e for e in root.iter() if local(e.tag) == "nav"
                         and any(local(name) == "type" and "toc" in SPACE.split(value) for name, value in e.attrib.items())]
                if not lists:
                    tables[kind] = None
                    continue
                has_entry: dict[int, bool] = {}

                def walk(element: ET.Element, depth: int) -> None:
                    for child in element:
                        name = local(child.tag)
                        if name in ("ol", "ul"):
                            walk(child, depth + 1)
                        elif depth == 0:
                            walk(child, depth)
                        elif name == "li":
                            has_entry[depth] = False
                            walk(child, depth)
                        elif name == "a":
                            entry(depth - 1, child, child.get("href"))
                            has_entry[depth] = True
                        elif name == "span" and not has_entry.get(depth):
                            # A heading with no link of its own, which takes the first link inside it if there is one.
                            inner = next((e.get("href") for e in child.iter()
                                          if e is not child and local(e.tag) == "a" and e.get("href") is not None), None)
                            entry(depth - 1, child, inner)
                            has_entry[depth] = True
                        else:
                            walk(child, depth)

                walk(lists[0], 0)
            tables[kind] = entries
        return tables


def check_zip(tool: str, path: Path, figures: dict[str, int]) -> list[str]:
    try:
        figures["entries"] = zip_tool.compare(tool, path, cat_limit=2)
    except zip_tool.Failure as failure:
        return [f"bn-zip: {failure}"]
    return []


def check_xml(tool: str, path: Path, figures: dict[str, int]) -> list[str]:
    result = run(tool, "book", str(path))
    lines = result.stdout.decode("utf-8", "replace").splitlines()
    if not lines:
        return [f"bn-xml: ended with {result.returncode} and printed nothing: {result.stderr[-120:]!r}"]
    said = {}
    for line in lines[:-1]:
        how, crc, count, _memory, _line, name = line.split(None, 5)
        said[name] = (how, int(crc, 16), int(count))
    problems = []
    everything: list[str] = []
    failed = 0
    with zipfile.ZipFile(path) as archive:
        wanted = [info for info in archive.infolist() if info.filename.lower().endswith(XML_SUFFIXES)]
        named = {info.filename: zip_tool.name_bytes(info).decode("utf-8", "replace") for info in wanted}
        if set(said) != set(named.values()) or len(said) != len(wanted):
            return [f"bn-xml: it parsed {len(said)} documents, and Python finds {len(wanted)} by their names, not all the same"]
        for info in wanted:
            well_formed, expected = xml_tool.reference_events(archive.read(info))
            everything += expected
            how, crc, count = said[named[info.filename]]
            if not well_formed:
                failed += 1
                if how == "ok":
                    problems.append(f"bn-xml: {info.filename}: taken for XML, and Python's expat does not take it for XML")
            elif (how, crc, count) != ("ok", xml_tool.checksum(expected), len(expected)):
                problems.append(f"bn-xml: {info.filename}: {how}, {count} events, checksum {crc:08x}; Python's expat has "
                                f"{len(expected)} events, checksum {xml_tool.checksum(expected):08x}")
    if not failed and not lines[-1].startswith(f"{len(wanted)} documents, 0 failed; {len(everything)} events, "
                                               f"checksum of all {xml_tool.checksum(everything):08x}; "):
        problems.append(f"bn-xml: the last line is {lines[-1]!r}")
    figures["documents"] = len(wanted)
    figures["events"] = len(everything)
    figures["not_xml"] = failed
    return problems[:4]


def check_package(tool: str, path: Path, figures: dict[str, int]) -> list[str]:
    try:
        expected = epub_tool.reference(path)
    except Exception as error:  # Whatever a book can do to a parser.
        return [f"bn-epub info: Python could not read the package: {error!r}"]
    result = run(tool, "info", str(path))
    if result.returncode != 0:
        return [f"bn-epub info: ended with {result.returncode}: {result.stderr[-120:]!r}"]
    lines = result.stdout.decode("utf-8", "replace").splitlines()
    if len(lines) < 2 or not lines[-2].startswith("memory: ") or not lines[-1].startswith("cost: "):
        return [f"bn-epub info: the last two lines are {lines[-2:]!r}"]
    got = lines[:-2]
    for number, (mine, theirs) in enumerate(zip(got, expected)):
        if mine != theirs:
            return [f"bn-epub info: line {number + 1} is {mine[:100]!r}, a reading by Python has {theirs[:100]!r}"]
    if len(got) != len(expected):
        return [f"bn-epub info: {len(got)} lines, a reading by Python has {len(expected)}"]
    figures["spine"] = int(next(line for line in got if line.startswith("spine: ")).split()[1])
    return []


def check_tables(tool: str, path: Path, figures: dict[str, int]) -> list[str]:
    try:
        tables = tables_of_contents(path)
    except Exception as error:  # Whatever a book can do to a parser.
        return [f"bn-epub toc: Python could not read the tables of contents: {error!r}"]
    problems = []
    read_by = "nav" if "nav" in tables else "ncx" if "ncx" in tables else None
    for option, kind in ((None, read_by), ("--nav", "nav"), ("--ncx", "ncx")):
        what = f"bn-epub toc {option or ''}".rstrip()
        result = run(tool, "toc", str(path), *([option] if option else []))
        if kind is None or kind not in tables:
            if result.returncode != 1 or b"no table of contents" not in result.stderr:
                problems.append(f"{what}: the book has none, and it ended with {result.returncode}: {result.stderr[-80:]!r}")
            continue
        entries = tables[kind]
        if entries is None:
            if result.returncode != 1 or b"not a table of contents" not in result.stderr:
                problems.append(f"{what}: Python finds no table in the file, and it ended with {result.returncode}: "
                                f"{result.stderr[-80:]!r}")
            continue
        if result.returncode != 0:
            problems.append(f"{what}: ended with {result.returncode}: {result.stderr[-120:]!r}")
            continue
        checksum = zlib.crc32(b"".join(summed + b"\n" for _, summed in entries))
        expected = [b"toc: " + kind.encode(), *(line for line, _ in entries), b"entries: %d" % len(entries),
                    b"checksum: %08x" % checksum]
        got = result.stdout.split(b"\n")
        different = next((number for number, (mine, theirs) in enumerate(zip(got, expected)) if mine != theirs), None)
        if different is not None:
            problems.append(f"{what}: line {different + 1} is {got[different][:100]!r}, a reading by Python has "
                            f"{expected[different][:100]!r}")
        elif len(got) <= len(expected) or not got[len(expected)].startswith(b"cost: "):
            problems.append(f"{what}: after the entries comes {got[len(expected):len(expected) + 1]!r}")
        else:
            figures["tables"] = figures.get("tables", 0) + 1
            figures["toc_entries"] = figures.get("toc_entries", 0) + len(entries)
    return problems


KNOWN_FAMILIES: dict[str, list] = {}


def known_families(tool: str) -> list:
    """The names of faces that `bn-css` knows, asked of it once."""
    if tool not in KNOWN_FAMILIES:
        KNOWN_FAMILIES[tool] = css_tool.known_families(tool)
    return KNOWN_FAMILIES[tool]


def check_css(tool: str, path: Path, figures: dict) -> list[str]:
    result = run(tool, "book", str(path), "--dropped")
    lines = result.stdout.decode("utf-8", "replace").splitlines()
    # After the summary come the things passed over and the families nothing classed, each by itself.
    unclassed = {}
    while lines and lines[-1].startswith(("dropped ", "unclassed ")):
        last = lines.pop()
        if last.startswith("unclassed "):
            _, count, _, names = last.split(" ", 3)
            unclassed[names] = int(count)
    if len(lines) < 3 or not lines[-3].startswith("css_families=") or not lines[-2].startswith("css_sheets=") or \
            not lines[-1].startswith("css_table_bytes="):
        return [f"bn-css: ended with {result.returncode} and its last lines are {lines[-3:]!r}: {result.stderr[-120:]!r}"]
    table = {key: int(value) for key, value in (item.split("=") for item in lines[-1].split())}
    families = {key: int(value) for key, value in (item.split("=") for item in lines[-3].split())}
    faces: list[str] = []
    said = {}
    for line in lines[:-3]:
        how, crc, rules, declarations, dropped, name = line.split(None, 5)
        said[name] = (how, crc, int(rules), int(declarations), int(dropped))
    problems = []
    all_rules = all_declarations = all_dropped = 0
    with zipfile.ZipFile(path) as archive:
        wanted = [info for info in archive.infolist() if info.filename.lower().endswith(".css")]
        named = {info.filename: zip_tool.name_bytes(info).decode("utf-8", "replace") for info in wanted}
        if set(said) != set(named.values()) or len(said) != len(wanted):
            return [f"bn-css: it read {len(said)} sheets, and Python finds {len(wanted)} by their names, not all the same"]
        for info in wanted:
            expected = css_tool.read_css(archive.read(info), faces=faces)
            rules = sum(1 for line in expected if line == b"}")
            declarations = sum(1 for line in expected if line.startswith(b"D "))
            dropped = sum(1 for line in expected if line.startswith(b"X "))
            mine = ("ok", f"{css_tool.checksum(expected):08x}", rules, declarations, dropped)
            if said[named[info.filename]] != mine:
                problems.append(f"bn-css: {info.filename}: {said[named[info.filename]]}; the second reading has {mine}")
            all_rules += rules
            all_declarations += declarations
            all_dropped += dropped
            # What was passed over, for the report: by kind, and for the
            # kinds whose text is a name, by name. A book counts once for each.
            seen = set()
            for line in expected:
                if not line.startswith(b"X "):
                    continue
                kind, _, text = line[2:].decode("utf-8", "replace").partition(": ")
                keys = [f"dropped\t{kind}"]
                if kind in ("property", "pseudo-class", "pseudo-element", "at-rule", "media block"):
                    keys.append(f"dropped\t{kind}\t{text.lower()}")
                for key in keys:
                    figures[key] = figures.get(key, 0) + 1
                    seen.add(key)
            for key in seen:
                figures["books " + key] = 1
    # The kind of face each font-family value comes to, by the second working-out.
    mine = {key: 0 for key in families}
    mine_unclassed: dict[str, int] = {}
    known = known_families(tool)
    for written in faces:
        kind, how = css_tool.class_of_families(written, known)
        mine["css_families"] += 1
        mine["css_families_" + kind] += 1
        mine["css_families_" + {"keyword": "keyword", "name": "name", "-": "unclassed"}[how]] += 1
        if how == "-":
            names = css_tool.families(written).translate(css_tool.ESCAPES).encode("latin-1").decode("utf-8", "replace")
            mine_unclassed[names] = mine_unclassed.get(names, 0) + 1
    if families != mine or unclassed != mine_unclassed:
        problems.append(f"bn-css: the kinds of face its font-family values come to: {families}, not classed "
                        f"{sorted(unclassed.items())[:3]}; the second working-out has {mine}, "
                        f"{sorted(mine_unclassed.items())[:3]}")
    for key, count in families.items():
        figures[key] = count
    if families["css_families"]:
        figures["books css_families"] = 1
    if families["css_families_unclassed"]:
        figures["books css_families_unclassed"] = 1
    for names, count in unclassed.items():
        figures[f"unclassed\t{names}"] = count
        figures[f"books unclassed\t{names}"] = 1
    figures["sheets"] = len(wanted)
    figures["css_rules"] = all_rules
    figures["css_declarations"] = all_declarations
    figures["css_dropped"] = all_dropped
    # What a table of all the book's rules takes: kept book by book, for
    # the report.
    if wanted:
        figures["table bytes"] = [table["css_table_bytes"]]
        figures["table rules"] = [table["css_table_rules"]]
        figures["table nodes"] = [table["css_table_nodes"]]
    figures["table_left_out"] = table["css_table_left_out"] + table["css_table_sheets_left_out"]
    return problems[:4]


def elements_of(document: bytes) -> int | None:
    """How many elements a document has; None if it is not XML."""
    elements = 0

    def start(name: str, attributes: dict[str, str]) -> None:
        nonlocal elements
        elements += 1

    parser = xml_tool.expat.ParserCreate()
    parser.StartElementHandler = start
    try:
        parser.Parse(document, True)
    except xml_tool.expat.ExpatError:
        return None
    return elements


def check_sheets(tool: str, path: Path, figures: dict) -> list[str]:
    """The sheets each document asks for: what `bn-css sheets` finds, beside the second working-out."""
    said = sheets_tool.said_by(tool, path)
    if isinstance(said, str):
        return [f"bn-css sheets: {said}"]
    lists, left, took = said
    problems = []
    with zipfile.ZipFile(path) as archive:
        by_name = {zip_tool.name_bytes(info): info for info in archive.infolist()}
        if len(by_name) != len(archive.infolist()):
            return []  # Two entries of one name: which the tool finds is not this check's to say.
        documents = [name for name in by_name if name.lower().endswith((b".xhtml", b".html", b".htm"))]
        if list(lists) != documents:
            return [f"bn-css sheets: it went through {len(lists)} documents, and Python finds {len(documents)} by their "
                    "names, not all the same"]
        figures["asked documents"] = len(documents)
        figures["asked sheets"] = took["sheets_given"]
        figures["sheets read"] = [took["sheets_read"]]
        figures["sheets names"] = [took["sheets_names_bytes"]]
        for kind, count in left.items():
            figures[f"left out\t{kind}"] = count
            figures[f"books left out\t{kind}"] = 1
        # Where the tool ran out of room, or a file is damaged, what it gives
        # is not what the second working-out has: it does not work those out.
        if any(left[kind] for kind in sheets_tool.NOT_WORKED_OUT):
            figures["sheets_not_compared"] = 1
            return []
        book = sheets_tool.Book(lambda name: name in by_name, lambda name: archive.read(by_name[name]))
        expected_left: Counter = Counter()
        most_links = 0
        for name in documents:
            listed, document_left, own = book.sheets_of(name)
            expected_left += document_left
            most_links = max(most_links, len(listed))
            if own:
                figures["own styles"] = figures.get("own styles", 0) + 1
                figures["own style bytes"] = figures.get("own style bytes", []) + [sum(len(text) + 1 for text in own)]
            if lists[name] != listed and len(problems) < 4:
                problems.append(f"bn-css sheets: {name!r} is given {lists[name]}; the second working-out has {listed}")
        figures["most sheets"] = [most_links]
        if left != expected_left:
            problems.append(f"bn-css sheets: left out, by kind: {dict(left)}; the second working-out has "
                            f"{dict(expected_left)}")
        problems += check_spine_sheets(tool, path, archive, by_name, figures)
    return problems[:4]


def check_spine_sheets(tool: str, path: Path, archive: zipfile.ZipFile, by_name: dict, figures: dict) -> list[str]:
    """The sheets of the documents of the spine, in reading order: `bn-css sheets --spine`, which opens each
    document from where the package says it is, as a reader goes through a book, beside the second working-out,
    by the checksum of what every document is given. And what finding them costs in reads of the file: that way,
    and with each document found in the ZIP file by its name."""
    if "spine" not in figures:
        return []  # A package that could not be read: check_package has said so.
    lines = epub_tool.reference(path)
    spine = [line.split(" ", 2)[2] for line in lines[9:9 + int(lines[8].split()[1])]]
    entries: dict[str, zipfile.ZipInfo] = {}
    for info in archive.infolist():
        entries.setdefault(epub_tool.entry_name(info), info)
    said = sheets_tool.said_by(tool, path, "--spine")
    if isinstance(said, str):
        return [f"bn-css sheets --spine: {said}"]
    _, left, took = said
    figures["spine asked documents"] = took["sheets_documents"]
    figures["spine reads by place"] = took["sheets_reads"]
    figures["most spine reads by place"] = [took["sheets_reads"]]
    problems = []
    by_their_names = sheets_tool.said_by(tool, path, "--spine", "--by-name")
    if isinstance(by_their_names, str):
        problems.append(f"bn-css sheets --spine --by-name: {by_their_names}")
    else:
        figures["spine reads by name"] = by_their_names[2]["sheets_reads"]
        figures["most spine reads by name"] = [by_their_names[2]["sheets_reads"]]
        if by_their_names[2]["sheets_checksum"] != took["sheets_checksum"]:
            problems.append("bn-css sheets --spine: the documents found by their names are given other sheets than "
                            "the same documents opened from their places")
    # What the second working-out does not work out, as for the documents by
    # their names; and a document the tool could not open is not one it can
    # be asked about.
    if any(left[kind] for kind in sheets_tool.NOT_WORKED_OUT) or took["sheets_unopened"]:
        figures["spine_not_compared"] = 1
        return problems
    book = sheets_tool.Book(lambda name: name in by_name, lambda name: archive.read(by_name[name]))
    found = []
    absent = 0
    for name in spine:
        if name not in entries:
            absent += 1
            continue
        key = zip_tool.name_bytes(entries[name])
        listed, document_left, _ = book.sheets_of(key)
        found.append((key, listed, document_left))
    if (took["sheets_documents"], took["sheets_absent"]) != (len(found), absent):
        problems.append(f"bn-css sheets --spine: it went through {took['sheets_documents']} documents and found "
                        f"{took['sheets_absent']} not there; Python has {len(found)} and {absent}")
    elif took["sheets_checksum"] != sheets_tool.digest_of(found):
        problems.append(f"bn-css sheets --spine: what the spine's documents are given has the checksum "
                        f"{took['sheets_checksum']:08x}; the second working-out comes to "
                        f"{sheets_tool.digest_of(found):08x}")
    return problems


def check_cascade(tool: str, path: Path, figures: dict) -> list[str]:
    """What the rules come to for the elements of a few documents: `bn-css document`, which finds each one's
    sheets itself, beside the second working-out of the cascade given the sheets the second finding has."""
    problems = []
    with zipfile.ZipFile(path) as archive:
        by_name = {zip_tool.name_bytes(info): info for info in archive.infolist()}
        if len(by_name) != len(archive.infolist()):
            return []  # Two entries of one name: which the tool finds is not this check's to say.
        documents = [(name, info) for name, info in by_name.items() if name.lower().endswith((b".xhtml", b".html", b".htm"))]
        book = sheets_tool.Book(lambda name: name in by_name, lambda name: archive.read(by_name[name]))
        left = MAX_CASCADE_ELEMENTS
        done = 0
        for name, info in documents[:MAX_CASCADE_LOOKED_AT]:
            if done == MAX_CASCADE_DOCUMENTS:
                break
            document = archive.read(info)
            elements = elements_of(document)
            if elements is None or elements > left:
                continue

            # The sheets it is given, by the second finding: those it links,
            # each after the sheets it imports, and then its own.
            listed, _, own = book.sheets_of(name)
            sheets = [archive.read(by_name[sheet]) for sheet in listed if sheet != sheets_tool.OWN_STYLE] + own

            result = run(tool, "document", "--sum", str(path), name)
            said = result.stdout.decode("utf-8", "replace").split()
            if result.returncode != 0 or len(said) < 4 or said[0] != "document":
                problems.append(f"bn-css document: {name!r}: ended with {result.returncode}: {result.stderr[-120:]!r}")
                continue
            took = {key: int(value) for key, value in (item.split("=") for item in said[2:])}
            if took["left_out"] or took["too_deep"]:
                figures["cascade_not_compared"] = figures.get("cascade_not_compared", 0) + 1
                continue
            expected, counted = css_cascade_tool.cascade_lines(sheets, document, known_families(tool))
            mine = (said[1], took["elements"], took["lines"])
            theirs = (f"{css_cascade_tool.checksum(expected):08x}", counted, len(expected))
            if mine != theirs:
                # Where they part, from the lines themselves: those of the
                # elements come after the lines for the sheets.
                got = [line for line in run(tool, "document", str(path), name).stdout.split(b"\n")[:-1]
                       if line[:2] not in (b"S ", b"X ")]
                at = next((i for i, (a, b) in enumerate(zip(got, expected)) if a != b), min(len(got), len(expected)))
                problems.append(f"bn-css document: {name!r} with {len(sheets)} sheets: {mine}; the second working-out "
                                f"has {theirs}; line {at + 1} is {got[at:at + 1]!r} and {expected[at:at + 1]!r}")
                continue
            done += 1
            left -= elements
            figures["cascade_documents"] = figures.get("cascade_documents", 0) + 1
            figures["cascade_elements"] = figures.get("cascade_elements", 0) + elements
            figures["cascade_set"] = figures.get("cascade_set", 0) + len(expected) - (2 * elements)
            figures["cascade_too_many_classes"] = figures.get("cascade_too_many_classes", 0) + took["too_many_classes"]
            figures["level bytes"] = [max(figures.get("level bytes", [0])[0], took["level_bytes"])]
            figures["most work"] = [max(figures.get("most work", [0])[0], took["most_work"])]

            # What each element then comes to, by the stack of styles and by the second working-out of that.
            result = run(tool, "document", "--styles", "--sum", str(path), name)
            said = result.stdout.decode("utf-8", "replace").split()
            if result.returncode != 0 or len(said) < 4 or said[0] != "styles":
                problems.append(f"bn-css document --styles: {name!r}: ended with {result.returncode}: "
                                f"{result.stderr[-120:]!r}")
                continue
            took = {key: int(value) for key, value in (item.split("=") for item in said[2:])}
            expected, counted, left_out, languages = styles_tool.styles_lines(sheets, document, known_families(tool))
            theirs = dict(left_out, languages=languages, elements=counted, lines=len(expected))
            mine = {key: took.get(key) for key in theirs}
            if said[1] != f"{css_cascade_tool.checksum(expected):08x}" or mine != theirs:
                got = [line for line in run(tool, "document", "--styles", str(path), name).stdout.split(b"\n")[:-1]
                       if line[:2] not in (b"S ", b"X ")]
                at = next((i for i, (a, b) in enumerate(zip(got, expected)) if a != b), min(len(got), len(expected)))
                problems.append(f"bn-css document --styles: {name!r}: {said[1]} {mine}; the second working-out has "
                                f"{css_cascade_tool.checksum(expected):08x} {theirs}; line {at + 1} is "
                                f"{got[at:at + 1]!r} and {expected[at:at + 1]!r}")
                continue
            figures["styles_documents"] = figures.get("styles_documents", 0) + 1
            figures["styles_elements"] = figures.get("styles_elements", 0) + counted
            figures["styles_lines"] = figures.get("styles_lines", 0) + len(expected) - (2 * counted)
            figures["most languages"] = [max(figures.get("most languages", [0])[0], languages)]
            for kind, count in left_out.items():
                if count:
                    figures[f"styles left out\t{kind}"] = figures.get(f"styles left out\t{kind}", 0) + count
                    figures[f"books styles left out\t{kind}"] = 1

            # The blocks the document is made into, by the tool and by the second working-out of those; which does
            # not go deeper than the tool keeps.
            if left_out["styles_too_deep"]:
                figures["blocks_not_compared"] = figures.get("blocks_not_compared", 0) + 1
                continue
            result = run(tool, "document", "--blocks", "--sum", str(path), name)
            said = result.stdout.decode("utf-8", "replace").split()
            if result.returncode != 0 or len(said) < 4 or said[0] != "blocks":
                problems.append(f"bn-css document --blocks: {name!r}: ended with {result.returncode}: "
                                f"{result.stderr[-120:]!r}")
                continue
            took = {key: int(value) for key, value in (item.split("=") for item in said[2:])}
            expected, counted, counts, left_out, languages = blocks_tool.blocks_lines(sheets, document,
                                                                                     known_families(tool))
            theirs = dict(counts, **left_out, languages=languages, elements=counted, lines=len(expected), work_over=0)
            mine = {key: took.get(key) for key in theirs}
            if said[1] != f"{css_cascade_tool.checksum(expected):08x}" or mine != theirs:
                got = [line for line in run(tool, "document", "--blocks", str(path), name).stdout.split(b"\n")[:-1]
                       if line[:2] != b"X "]
                at = next((i for i, (a, b) in enumerate(zip(got, expected)) if a != b), min(len(got), len(expected)))
                differ = {key: (mine[key], theirs[key]) for key in theirs if mine[key] != theirs[key]}
                problems.append(f"bn-css document --blocks: {name!r}: {said[1]}; the second working-out has "
                                f"{css_cascade_tool.checksum(expected):08x}; they differ in {differ}; line {at + 1} "
                                f"is {got[at:at + 1]!r} and {expected[at:at + 1]!r}, after {got[max(0, at - 4):at]!r}")
                continue
            figures["blocks_documents"] = figures.get("blocks_documents", 0) + 1
            figures["blocks most work"] = [max(figures.get("blocks most work", [0])[0], took["blocks_most_work"])]
            for kind, count in counts.items():
                figures[f"blocks counted\t{kind}"] = figures.get(f"blocks counted\t{kind}", 0) + count
                if count:
                    figures[f"books blocks counted\t{kind}"] = 1
    return problems[:4]


def check_png(tool: str, path: Path, figures: dict) -> list[str]:
    result = run(tool, "book", str(path))
    lines = result.stdout.decode("utf-8", "replace").splitlines()
    if not lines or not lines[-1].startswith("png_files="):
        return [f"bn-png: ended with {result.returncode} and its last line is {lines[-1:]!r}: {result.stderr[-120:]!r}"]
    said = {}
    for line in lines[:-1]:
        if line.startswith("unreadable "):
            said[line[len("unreadable "):]] = "unreadable"
        else:
            fields = line.split(" ", 10)
            said[fields[10]] = " ".join(fields[:10])
    problems = []
    pictures = 0
    pixels = 0
    most_room = 0
    with zipfile.ZipFile(path) as archive:
        wanted = [info for info in archive.infolist() if info.filename.lower().endswith(".png")]
        named = {info.filename: zip_tool.name_bytes(info).decode("utf-8", "replace") for info in wanted}
        if set(said) != set(named.values()) or len(said) != len(wanted):
            return [f"bn-png: it read {len(said)} pictures, and Python finds {len(wanted)} by their names, not all the "
                    "same"]
        for info in wanted:
            expected = png_tool.read_png(archive.read(info))
            theirs = said[named[info.filename]]
            if expected.exact and theirs != expected.line():
                problems.append(f"bn-png: {info.filename}: {theirs}; the second reading has {expected.line()}")
            elif not expected.exact and theirs.split(" ", 1)[0] == "ok" and expected.status != "ok":
                problems.append(f"bn-png: {info.filename}: {theirs}; the second reading has {expected.line()}")
            pictures += 1
            if expected.header:
                pixels += expected.header[0] * expected.header[1]
                most_room = max(most_room, expected.room)
            if expected.status != "ok":
                figures[f"png {expected.status}"] = figures.get(f"png {expected.status}", 0) + 1
    figures["pngs"] = pictures
    figures["png_pixels"] = pixels
    if pictures:
        figures["png room"] = [most_room]
    return problems[:4]


def is_jpeg(name: str) -> bool:
    """Whether bn-jpeg book takes an entry for a picture: by the ending of its name, in any case."""
    lowered = name.lower()
    return any(len(name) > len(ending) and lowered.endswith(ending) for ending in (".jpg", ".jpeg"))


def check_jpeg(tool: str, path: Path, figures: dict) -> list[str]:
    result = run(tool, "book", str(path), "--reduce", "8")
    lines = result.stdout.decode("utf-8", "replace").splitlines()
    if not lines or not lines[-1].startswith("jpeg_files="):
        return [f"bn-jpeg: ended with {result.returncode} and its last line is {lines[-1:]!r}: {result.stderr[-120:]!r}"]
    said = {}
    for line in lines[:-1]:
        if line.startswith("unreadable "):
            said[line[len("unreadable "):]] = "unreadable"
        else:
            fields = line.split(" ", 11)
            said[fields[11]] = " ".join(fields[:11])
    problems = []
    pictures = 0
    pixels = 0
    most_room = 0
    with zipfile.ZipFile(path) as archive:
        named = {info.filename: zip_tool.name_bytes(info).decode("utf-8", "replace") for info in archive.infolist()}
        wanted = [info for info in archive.infolist() if is_jpeg(named[info.filename])]
        if set(said) != {named[info.filename] for info in wanted} or len(said) != len(wanted):
            return [f"bn-jpeg: it read {len(said)} pictures, and Python finds {len(wanted)} by their names, not all the "
                    "same"]
        for info in wanted:
            expected = jpeg_tool.read_jpeg(archive.read(info), 8)
            theirs = said[named[info.filename]]
            if theirs != expected.line():
                problems.append(f"bn-jpeg: {info.filename}: {theirs}; the second reading has {expected.line()}")
            pictures += 1
            if expected.frame:
                pixels += expected.frame.width * expected.frame.height
                most_room = max(most_room, expected.frame.room(1))
            if expected.status != "ok":
                figures[f"jpeg {expected.status}"] = figures.get(f"jpeg {expected.status}", 0) + 1
    figures["jpegs"] = pictures
    figures["jpeg_pixels"] = pixels
    if pictures:
        figures["jpeg room"] = [most_room]
    return problems[:4]


class DocumentFigures(HTMLParser):
    """Counts the paragraphs and the ids of one document.

    A paragraph is where tools/layout-text.py has one: the text between the
    starts and ends of that script's block elements, with what is inside
    the elements it leaves out left out here too.
    """

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.pieces: list[str] = []
        self.left_out = 0  # How deep inside elements whose text is left out.
        self.paragraphs = 0
        self.largest = 0
        self.ids: set[str] = set()

    def end_paragraph(self) -> None:
        text = re.sub(" +", " ", "".join(self.pieces)).strip(" \n")
        self.pieces = []
        if text:
            self.paragraphs += 1
            self.largest = max(self.largest, len(text.encode("utf-8", "replace")))

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        given = dict(attrs)
        tag = tag.lower().split(":")[-1]
        name = given.get("id") or (given.get("name") if tag == "a" else None)
        if name:
            self.ids.add(name)
        if tag in layout_text.LEFT_OUT:
            self.left_out += 1
        elif tag in layout_text.BLOCKS:
            self.end_paragraph()
        elif tag == "br" and not self.left_out:
            self.pieces.append("\n")

    def handle_endtag(self, tag: str) -> None:
        tag = tag.lower().split(":")[-1]
        if tag in layout_text.LEFT_OUT:
            self.left_out = max(0, self.left_out - 1)
        elif tag in layout_text.BLOCKS:
            self.end_paragraph()

    def handle_data(self, data: str) -> None:
        if not self.left_out:
            self.pieces.append(layout_text.SPACE.sub(" ", data))

    def close(self) -> None:
        super().close()
        self.end_paragraph()


def count_documents(path: Path, figures: dict) -> None:
    """What the documents of the book's spine hold: their paragraphs and their ids. Nothing here is compared with
    a tool, since none reads a document's text yet; it is counted so that the figures are at hand."""
    try:
        lines = epub_tool.reference(path)
    except Exception:  # Whatever a book can do to a parser: check_package has said so.
        return
    # The package's reading has a line for each item of the spine after the
    # line that counts them: whether it is in the run of the book, the kind
    # its media type makes it, and its name, or ? for a file the book lacks.
    spine = [line.split(" ", 2) for line in lines[9:9 + int(lines[8].split()[1])]]
    most: dict[str, tuple[int, str]] = {}
    read = set()
    with zipfile.ZipFile(path) as archive:
        entries: dict[str, zipfile.ZipInfo] = {}
        for info in archive.infolist():
            entries.setdefault(epub_tool.entry_name(info), info)
        for _, kind, name in spine:
            if name not in entries:
                figures["spine not there"] = figures.get("spine not there", 0) + 1
                continue
            if name in read:
                continue
            read.add(name)
            if kind != "xhtml":
                figures[f"spine of kind\t{kind}"] = figures.get(f"spine of kind\t{kind}", 0) + 1
                continue
            data = archive.read(entries[name])
            counted = DocumentFigures()
            try:
                counted.feed(data.decode("utf-16" if data[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8", "replace"))
                counted.close()
            except Exception:  # Python's reader of HTML gives up on a few things a document can hold.
                figures["documents given up"] = figures.get("documents given up", 0) + 1
                continue
            names = [len(given.encode("utf-8", "replace")) for given in counted.ids]
            for key, value in (("bytes", len(data)), ("paragraphs", counted.paragraphs), ("largest", counted.largest),
                               ("ids", len(names)), ("id bytes", sum(names)), ("longest id", max(names, default=0))):
                figures.setdefault(f"document {key}", []).append(value)
                if key not in most or value > most[key][0]:
                    most[key] = (value, name)
    # Of each book, its documents' most of each thing, and which document has it.
    for key, (value, name) in most.items():
        figures[f"most {key}"] = [(value, path.name, name)]


def check_bundle(tool: str, path: Path, figures: dict) -> list[str]:
    """The book's bundle: what `bn-bundle make` writes, read a second way, beside the book as Python reads it."""
    # The table of contents the book is read by, as the reading above has
    # it: none, one that is no table, or its entries.
    entries: list[tuple] | None = []
    whole = True
    try:
        tables = tables_of_contents(path)
        read_by = "nav" if "nav" in tables else "ncx" if "ncx" in tables else None
        if read_by is not None and tables[read_by] is None:
            whole = False
        elif read_by is not None:
            for _, summed in tables[read_by]:
                depth, document, fragment, label = summed.split(b"\t", 3)
                entries.append((int(depth), None if document == b"-" else int(document),
                                b"" if fragment == b"-" else fragment, label))
    except Exception:  # Whatever a book can do to a parser: check_tables has said so.
        entries = None
    with tempfile.TemporaryDirectory(prefix="bn-check-books-") as work:
        made = Path(work) / "book.bundle"
        problems = bundle_tool.compare(tool, path, made, entries, whole)
        if not problems:
            bundle = bundle_tool.read_bundle(made.read_bytes())
            figures["bundles"] = 1
            figures["bundle bytes"] = [made.stat().st_size]
            figures["bundle pictures"] = [len(bundle["pictures"])]
            figures["bundle contents"] = [len(bundle["contents"])]
            figures["bundle records"] = len(bundle["spine"]) + len(bundle["pictures"]) + len(bundle["contents"])
            if bundle["amiss"] & bundle_tool.CONTENTS_NOT_WHOLE:
                figures["bundle contents not whole"] = 1
            if entries is None:
                figures["bundle contents not compared"] = 1
    return problems


def check_book(arguments: tuple[str, str, str, str, str, str, str, str]) -> tuple[str, list[str], dict]:
    (zip_tool_path, xml_tool_path, epub_tool_path, css_tool_path, png_tool_path, jpeg_tool_path, bundle_tool_path,
     name) = arguments
    path = Path(name)
    # A number is added up over the books; a list is kept, a book to an item.
    figures: dict = {}
    problems: list[str] = []
    try:
        problems += check_zip(zip_tool_path, path, figures)
        problems += check_xml(xml_tool_path, path, figures)
        problems += check_package(epub_tool_path, path, figures)
        problems += check_tables(epub_tool_path, path, figures)
        problems += check_css(css_tool_path, path, figures)
        problems += check_sheets(css_tool_path, path, figures)
        problems += check_cascade(css_tool_path, path, figures)
        problems += check_png(png_tool_path, path, figures)
        problems += check_jpeg(jpeg_tool_path, path, figures)
        problems += check_bundle(bundle_tool_path, path, figures)
        count_documents(path, figures)
    except Exception as error:  # A book that the comparison itself trips over is a finding, not a crash.
        problems.append(f"the comparison failed: {error!r}")
    return name, problems, figures


def spread(values: list[int]) -> str:
    """The middle of some figures, what nine in ten of them are within, and the most."""
    values = sorted(values)
    return (f"{values[len(values) // 2]} at the median, {values[min(len(values) - 1, (len(values) * 9) // 10)]} for nine "
            f"in ten, {values[-1]} at the most")


def print_css_report(totals: dict, books: int) -> None:
    """What the style sheets lost to what is not read, from the figures every book gave."""
    print(f"\nStyle sheets: {totals.get('css_rules', 0)} rules read, with {totals.get('css_declarations', 0)} "
          f"declarations; {totals.get('css_dropped', 0)} things passed over, in {books} books:")
    kinds = sorted(((count, key.split("\t")[1]) for key, count in totals.items()
                    if key.startswith("dropped\t") and key.count("\t") == 1), reverse=True)
    for count, kind in kinds:
        print(f"  {count:8}  in {totals.get('books dropped' + chr(9) + kind, 0):4} books  {kind}")
    for kind, most in (("property", 30), ("pseudo-class", 12), ("pseudo-element", 8), ("at-rule", 8), ("media block", 12)):
        named = sorted(((count, key.split("\t", 2)[2]) for key, count in totals.items()
                        if key.startswith(f"dropped\t{kind}\t")), reverse=True)
        if not named:
            continue
        print(f"  {kind}, the {min(most, len(named))} passed over most of {len(named)}:")
        for count, text in named[:most]:
            in_books = totals.get(f"books dropped\t{kind}\t{text}", 0)
            print(f"    {count:8}  in {in_books:4} books  {text}")
    values = totals.get("css_families", 0)
    if values:
        def share(key: str) -> str:
            return f"{totals.get(key, 0)} ({100 * totals.get(key, 0) / values:.1f}%)"
        print(f"\nKinds of face: {values} font-family values in the sheets of {totals.get('books css_families', 0)} "
              "books:")
        print(f"  classed by a keyword {share('css_families_keyword')}, by a known name {share('css_families_name')}, "
              f"by nothing {share('css_families_unclassed')} in {totals.get('books css_families_unclassed', 0)} books")
        print("  they come to: " + ", ".join(f"{kind} {totals.get('css_families_' + kind, 0)}"
                                             for kind in ("serif", "sans", "mono", "script", "other")))
        named = sorted(((totals.get("books " + key, 0), count, key.split("\t", 1)[1]) for key, count in totals.items()
                        if key.startswith("unclassed\t")), reverse=True)
        if named:
            print(f"  the values nothing classed, the {min(20, len(named))} in most books of {len(named)}:")
            for in_books, count, names in named[:20]:
                print(f"    {count:8}  in {in_books:4} books  {names}")
    if totals.get("table bytes"):
        sizes = totals["table bytes"]
        print(f"\nA table of all a book's rules, in the {len(sizes)} books with style sheets:")
        print(f"  bytes: {spread(sizes)}")
        print(f"  rules kept: {spread(totals['table rules'])}")
        print(f"  nodes: {spread(totals['table nodes'])}")
        print("  books whose table is over 8 KB, 12 KB, 16 KB, 20 KB and 24 KB: " +
              ", ".join(str(sum(1 for size in sizes if size > limit)) for limit in (8192, 12288, 16384, 20480, 24576)))
        print(f"  rules left out of a table of 64 KB: {totals.get('table_left_out', 0)}")
    if totals.get("level bytes"):
        print(f"  bytes kept for each open element, in the documents the cascade was tried on: {spread(totals['level bytes'])}")
        print(f"  the most work for one element: {spread(totals['most work'])}")
    if totals.get("styles_documents"):
        print(f"\nWhat each element comes to, in the {totals['styles_documents']} documents the stack of styles was "
              f"tried on: {totals.get('styles_elements', 0)} elements, of which {totals.get('styles_lines', 0)} "
              "things are said:")
        print(f"  languages a document's elements name between them: {spread(totals['most languages'])}")
        kinds = sorted(((count, key.split("\t")[1]) for key, count in totals.items()
                        if key.startswith("styles left out\t")), reverse=True)
        if not kinds:
            print("  nothing was left out")
        names = {"styles_too_deep": "an element deeper than is kept", "page_margins": "a margin or a padding of the page",
                 "inherits_of_own": "inherit for what is an element's own",
                 "languages_left_out": "a language there was no room for",
                 "breaks_avoided_before": "no page to end before an element",
                 "not_shown": "mathematics, or a note of a ruby"}
        for count, kind in kinds:
            in_books = totals.get("books styles left out" + chr(9) + kind, 0)
            print(f"  left out: {count:6}  in {in_books:4} books  {names.get(kind, kind)}")
    if totals.get("blocks_documents"):
        def counted(kind: str) -> int:
            return totals.get("blocks counted" + chr(9) + kind, 0)

        def in_books(kind: str) -> int:
            return totals.get("books blocks counted" + chr(9) + kind, 0)

        print(f"\nThe blocks of the {totals['blocks_documents']} documents they were looked for in: "
              f"{counted('blocks')} blocks:")
        print(f"  {counted('paragraphs')} paragraphs, {counted('headings')} headings, {counted('list_items')} items "
              f"of lists, {counted('cells')} cells of tables, {counted('rules')} rules")
        for kind, words in (("markers", "the first block of an item, with its marker"),
                            ("line_ends", "a line ended by br"), ("pictures", "a picture"),
                            ("first_letters", "a first letter with a style of its own"),
                            ("floated_letters", "of those, floated: a dropped capital"),
                            ("begin_floated", "a block that begins in floated text"),
                            ("floated", "a block in something floated"),
                            ("centred", "a block centred by its margins")):
            print(f"  {counted(kind):8}  in {in_books(kind):4} books  {words}")
        print(f"  the most work for one call: {spread(totals['blocks most work'])}")
    print(f"\nWhat documents ask for: {totals.get('asked documents', 0)} documents are given "
          f"{totals.get('asked sheets', 0)} sheets, {totals.get('own styles', 0)} of them with rules of their own:")
    if totals.get("sheets read"):
        print(f"  sheets a book's documents ask for between them: {spread(totals['sheets read'])}")
        print(f"  the most sheets one document is given: {spread(totals['most sheets'])}")
        print(f"  bytes for the names of a book's sheets: {spread(totals['sheets names'])}")
    if totals.get("own style bytes"):
        print(f"  bytes of a document's own `<style>` text, where it has any: {spread(totals['own style bytes'])}")
    kinds = sorted(((count, key.split("\t")[1]) for key, count in totals.items() if key.startswith("left out\t")),
                   reverse=True)
    if not kinds:
        print("  nothing was left out")
    for count, kind in kinds:
        print(f"  left out: {count:6}  in {totals.get('books left out' + chr(9) + kind, 0):4} books  {kind}")
    if totals.get("most spine reads by place"):
        print(f"\nThrough the spines, in reading order: {totals.get('spine asked documents', 0)} documents. Reads of "
              "the file to find their sheets:")
        print(f"  each document opened from where the package says it is: {totals.get('spine reads by place', 0)} "
              f"in all; for a book, {spread(totals['most spine reads by place'])}")
        if totals.get("most spine reads by name"):
            print(f"  each document found in the ZIP file by its name: {totals.get('spine reads by name', 0)} in "
                  f"all; for a book, {spread(totals['most spine reads by name'])}")
        if totals.get("spine_not_compared"):
            print(f"  books whose spine was not set beside the second working-out: {totals['spine_not_compared']}")


def print_document_report(totals: dict) -> None:
    """What the documents of the spines hold, from the figures every book gave."""
    sizes = totals.get("document bytes", [])
    print(f"\nThe documents of the spines: {len(sizes)} of them, in {len(totals.get('most bytes', []))} books:")
    if not sizes:
        return
    for key, what in (("bytes", "bytes of a document"), ("paragraphs", "paragraphs in a document"),
                      ("largest", "bytes of a document's largest paragraph"), ("ids", "ids in a document"),
                      ("id bytes", "bytes of the names of a document's ids"),
                      ("longest id", "bytes of the longest name of an id in a document")):
        print(f"  {what}: {spread(totals[f'document {key}'])}")
    print(f"  documents with a paragraph over {', '.join(str(size // 1024) for size in PARAGRAPH_SIZES[:-1])} and "
          f"{PARAGRAPH_SIZES[-1] // 1024} KB: " +
          ", ".join(str(sum(1 for largest in totals["document largest"] if largest > size)) for size in PARAGRAPH_SIZES))
    # A book is named once for each: by the document of its own that has the most.
    for key, what in (("largest", "the largest paragraphs, in bytes"), ("paragraphs", "the most paragraphs in one document"),
                      ("ids", "the most ids in one document"), ("bytes", "the largest documents, in bytes")):
        print(f"  the books with {what}:")
        for value, book, name in sorted(totals[f"most {key}"], reverse=True)[:MOST_NAMED]:
            print(f"    {value:8}  {book}: {name}")


def check_sums(sums: Path) -> tuple[int, list[str]]:
    """Checks the folder of a SHA256SUMS file against it. Returns how many files it lists, and what is wrong."""
    problems = []
    listed = set()
    for line in sums.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        digest, _, name = line.partition("  ")
        file = sums.parent / name
        listed.add(file.resolve())
        if not file.is_file():
            problems.append(f"{file}: listed in {sums.name} and not there")
        elif hashlib.sha256(file.read_bytes()).hexdigest() != digest:
            problems.append(f"{file}: not the file {sums.name} lists: its SHA-256 is different")
    for book in books_under(sums.parent):
        if book.resolve() not in listed:
            problems.append(f"{book}: not listed in {sums}")
    return len(listed), problems


def books_under(place: Path) -> list[Path]:
    if place.is_file():
        return [place]
    found = []
    for folder, _, names in os.walk(place, followlinks=True):
        found += [Path(folder) / name for name in names if name.lower().endswith(".epub") and not name.startswith(".")]
    return sorted(found)


def find_tool(given: str | None, build: str | None, part: str, name: str) -> str | None:
    if given:
        return str(Path(given).resolve())
    builds = [Path(build)] if build else [REPOSITORY / "build" / "host-release", REPOSITORY / "build" / "host"]
    for folder in builds:
        tool = folder / "tools" / part / name
        if tool.is_file():
            return str(tool.resolve())
    return None


def main() -> int:
    parser = argparse.ArgumentParser(usage=__doc__, add_help=False)
    parser.add_argument("--tools")
    parser.add_argument("--zip")
    parser.add_argument("--xml")
    parser.add_argument("--epub")
    parser.add_argument("--css")
    parser.add_argument("--png")
    parser.add_argument("--jpeg")
    parser.add_argument("--bundle")
    parser.add_argument("--css-report", action="store_true")
    parser.add_argument("--document-report", action="store_true")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("places", nargs="+")
    options = parser.parse_args()
    tools = [find_tool(options.zip, options.tools, "zip", "bn-zip"), find_tool(options.xml, options.tools, "xml", "bn-xml"),
             find_tool(options.epub, options.tools, "epub", "bn-epub"), find_tool(options.css, options.tools, "css", "bn-css"),
             find_tool(options.png, options.tools, "png", "bn-png"),
             find_tool(options.jpeg, options.tools, "jpeg", "bn-jpeg"),
             find_tool(options.bundle, options.tools, "bundle", "bn-bundle")]
    if None in tools:
        print("check-books: bn-zip, bn-xml, bn-epub, bn-css, bn-png, bn-jpeg and bn-bundle are not all built: "
              "cmake --build --preset host-release", file=sys.stderr)
        return 2
    places = [Path(place) for place in options.places]
    missing = [place for place in places if not place.exists()]
    if missing:
        print(f"check-books: there is no {missing[0]}", file=sys.stderr)
        return 2

    wrong = 0
    listed = 0
    for place in places:
        if not place.is_dir():
            continue
        for folder, _, names in os.walk(place, followlinks=True):
            if "SHA256SUMS" in names:
                count, problems = check_sums(Path(folder) / "SHA256SUMS")
                listed += count
                wrong += len(problems)
                for problem in problems:
                    print(f"CHANGED  {problem}")

    books = [book for place in places for book in books_under(place)]
    if not books:
        print("check-books: no EPUB files there", file=sys.stderr)
        return 2
    totals: dict = {}
    different = 0
    with concurrent.futures.ProcessPoolExecutor(max_workers=max(1, options.jobs)) as pool:
        for name, problems, figures in pool.map(check_book, [(*tools, str(book)) for book in books], chunksize=2):
            for key, value in figures.items():
                totals[key] = totals.get(key, [] if isinstance(value, list) else 0) + value
            if problems:
                different += 1
                for problem in problems:
                    print(f"DIFFERENT  {name}: {problem}")

    print(f"{len(books)} books: {totals.get('entries', 0)} entries in their ZIP files, {totals.get('documents', 0)} XML "
          f"documents with {totals.get('events', 0)} events, {totals.get('spine', 0)} documents in their spines, "
          f"{totals.get('tables', 0)} readings of tables of contents with {totals.get('toc_entries', 0)} entries, "
          f"{totals.get('sheets', 0)} style sheets with {totals.get('css_rules', 0)} rules, "
          f"{totals.get('asked sheets', 0)} sheets asked for by {totals.get('asked documents', 0)} documents, "
          f"{totals.get('cascade_set', 0)} "
          f"properties set for {totals.get('cascade_elements', 0)} elements of {totals.get('cascade_documents', 0)} "
          f"documents, {totals.get('pngs', 0)} PNG pictures of {totals.get('png_pixels', 0)} pixels, "
          f"{totals.get('jpegs', 0)} JPEG pictures of {totals.get('jpeg_pixels', 0)} pixels, "
          f"{totals.get('bundles', 0)} bundles of {totals.get('bundle records', 0)} records.")
    if totals.get("png room"):
        print(f"Memory for the rows of a book's widest PNG picture, in bytes: {spread(totals['png room'])}; over the "
              f"{len(totals['png room'])} books that have one.")
    for key in sorted(key for key in totals if key.startswith("png ") and key != "png room"):
        print(f"{totals[key]} PNG pictures end as {key[4:]}, by the tool and by Python alike.")
    if totals.get("jpeg room"):
        print(f"Memory for the rows of a book's widest JPEG picture read at its whole size (a progressive one at an "
              f"eighth), in bytes: {spread(totals['jpeg room'])}; over the {len(totals['jpeg room'])} books that have "
              "one.")
    for key in sorted(key for key in totals if key.startswith("jpeg ") and key != "jpeg room"):
        print(f"{totals[key]} JPEG pictures end as {key[5:]}, by the tool and by Python alike.")
    if totals.get("bundle bytes"):
        print(f"A book's bundle before any chapter is imported, in bytes: {spread(totals['bundle bytes'])}; its "
              f"pictures: {spread(totals['bundle pictures'])}; the entries of its contents: "
              f"{spread(totals['bundle contents'])}.")
    if totals.get("bundle contents not whole"):
        print(f"{totals['bundle contents not whole']} bundles say their contents are not the whole of the book's, by "
              "the tool and by Python alike.")
    if totals.get("bundle contents not compared"):
        print(f"{totals['bundle contents not compared']} bundles' contents were not compared: Python could not read "
              "the book's tables of contents.")
    if totals.get("most largest"):
        print(f"Of the {len(totals['document bytes'])} documents of their spines, a book's largest paragraph, in bytes: "
              f"{spread([value for value, _, _ in totals['most largest']])}; the most paragraphs in one of its "
              f"documents: {spread([value for value, _, _ in totals['most paragraphs']])}; the most ids in one: "
              f"{spread([value for value, _, _ in totals['most ids']])}.")
    kinds = sorted(key for key in totals if key.startswith("spine of kind\t"))
    if kinds:
        print("Items of the spines that are not XHTML by their media type, and were not counted: " +
              ", ".join(f"{totals[key]} of the kind {key.split(chr(9))[1]}" for key in kinds) + ".")
    if totals.get("spine not there"):
        print(f"Items of the spines that are files their books do not have: {totals['spine not there']}.")
    if totals.get("documents given up"):
        print(f"{totals['documents given up']} documents of the spines were not counted: Python's reader of HTML gave "
              "them up.")
    if options.css_report:
        print_css_report(totals, len(books))
    if options.document_report:
        print_document_report(totals)
    if totals.get("sheets_not_compared"):
        print(f"{totals['sheets_not_compared']} books were not put through the second finding of their documents' "
              "sheets: the tool had no room for a sheet, or one was damaged.")
    if totals.get("blocks_not_compared"):
        print(f"{totals['blocks_not_compared']} documents were not put through the second working-out of their "
              "blocks: an element was deeper than the tool keeps.")
    if totals.get("cascade_not_compared"):
        print(f"{totals['cascade_not_compared']} documents were not put through the second working-out of the cascade: "
              "the tool had left a rule out, or an element was too deep.")
    if totals.get("not_xml"):
        print(f"{totals['not_xml']} documents are not XML, by the tools and by Python's expat alike.")
    if listed:
        print(f"{listed} files listed in SHA256SUMS: {'all there and unchanged' if not wrong else 'NOT all as listed'}.")
    if different or wrong:
        print(f"NOT THE SAME: {different} books on which the tools and Python differ, {wrong} files not as listed.")
        return 1
    print("The tools and Python agree about every book.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
