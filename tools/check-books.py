#!/usr/bin/env python3
"""Checks what the book tools make of real books against a reading of each by Python.

bn-zip, bn-xml and bn-epub run the firmware's own code on one file. This
runs them over every EPUB under the folders it is given and compares, book
by book:

  - bn-zip with Python's zipfile: every entry of the directory, every
    entry's contents by its checksum, and a few entries byte for byte;
  - bn-xml with Python's expat: every XML document, event for event;
  - bn-epub info with a reading of the package by ElementTree;
  - bn-epub toc with a reading of each table of contents the book has.

The readings of the ZIP file, the documents and the package are the ones
the end-to-end tests use on the books they make (tests/e2e). The reading
of a table of contents is written here, since real tables do things the
tests' own do not.

A folder with a file named SHA256SUMS in it is also checked against that
file: every file it lists has to be there and unchanged, and every EPUB
beside or below it has to be listed.

usage: tools/check-books.py [--tools BUILD] [--zip BN_ZIP] [--xml BN_XML] [--epub BN_EPUB]
                            [--jobs N] FOLDER_OR_FILE...

BUILD is a build folder to take the tools from: build/host-release if it
has them, or else build/host. Each of the three can be named by itself.

It ends with 0 if everything agreed, 1 if anything did not, 2 if it could
not start.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import os
import posixpath
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
import zlib
from pathlib import Path
from urllib.parse import unquote_to_bytes

REPOSITORY = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPOSITORY / "tests" / "e2e"))
sys.dont_write_bytecode = True  # Or Python leaves compiled copies of the tests' scripts beside them.

import epub_tool  # noqa: E402  (found through the path set just above)
import xml_tool  # noqa: E402
import zip_tool  # noqa: E402

XML_SUFFIXES = (".xhtml", ".html", ".htm", ".xml", ".opf", ".ncx")
SPACE = re.compile(r"[ \t\r\n]+")
MAX_LABEL = 256
MAX_FRAGMENT = 128


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


def check_book(arguments: tuple[str, str, str, str]) -> tuple[str, list[str], dict[str, int]]:
    zip_tool_path, xml_tool_path, epub_tool_path, name = arguments
    path = Path(name)
    figures: dict[str, int] = {}
    problems: list[str] = []
    try:
        problems += check_zip(zip_tool_path, path, figures)
        problems += check_xml(xml_tool_path, path, figures)
        problems += check_package(epub_tool_path, path, figures)
        problems += check_tables(epub_tool_path, path, figures)
    except Exception as error:  # A book that the comparison itself trips over is a finding, not a crash.
        problems.append(f"the comparison failed: {error!r}")
    return name, problems, figures


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
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("places", nargs="+")
    options = parser.parse_args()
    tools = [find_tool(options.zip, options.tools, "zip", "bn-zip"), find_tool(options.xml, options.tools, "xml", "bn-xml"),
             find_tool(options.epub, options.tools, "epub", "bn-epub")]
    if None in tools:
        print("check-books: bn-zip, bn-xml and bn-epub are not all built: cmake --build --preset host-release",
              file=sys.stderr)
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
    totals: dict[str, int] = {}
    different = 0
    with concurrent.futures.ProcessPoolExecutor(max_workers=max(1, options.jobs)) as pool:
        for name, problems, figures in pool.map(check_book, [(*tools, str(book)) for book in books], chunksize=2):
            for key, value in figures.items():
                totals[key] = totals.get(key, 0) + value
            if problems:
                different += 1
                for problem in problems:
                    print(f"DIFFERENT  {name}: {problem}")

    print(f"{len(books)} books: {totals.get('entries', 0)} entries in their ZIP files, {totals.get('documents', 0)} XML "
          f"documents with {totals.get('events', 0)} events, {totals.get('spine', 0)} documents in their spines, "
          f"{totals.get('tables', 0)} readings of tables of contents with {totals.get('toc_entries', 0)} entries.")
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
