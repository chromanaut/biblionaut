#!/usr/bin/env python3
"""End-to-end check of the layout check, run by CTest.

The layout check (reader/layout_check.h) is what a device runs to find out
how long the layout code takes there, and whether it sets the text on its
card in the lines the desktop does. Here the desktop application runs it
over a folder standing for the card, and the figures it reports for each
setting of the text are compared with what `bn-lines --figures` prints for
the same file and table: the same paragraphs, bytes and lines, the same
hyphens, the same words asked about and bytes measured, and the same
checksum of the lines. That is how a run on a device is compared with the
desktop.

The text on the card is made by tools/layout-text.py from books made here,
and what it makes of one of them is held to a reading of that book written
out by hand. With no table on the card, the application gives the check the
table for American English that the build made, and bn-lines is given the
same table from the file it was made in (BUILT_IN_TABLE).

usage: layout_check.py APP BN_TRACE BN_LINES BN_HYPHEN LAYOUT_TEXT BUILT_IN_TABLE WORK_DIR
"""

from __future__ import annotations

import os
import random
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def run(command: list, **more) -> subprocess.CompletedProcess:
    return subprocess.run([str(part) for part in command], capture_output=True, check=False, **more)


# ---- Books ----

def write_book(path: Path, documents: list[tuple[str, str]], spine: list[str], package: bool = True) -> None:
    """An EPUB of `documents` (name, XHTML), read in the order of `spine`."""
    manifest = "".join(f'<item id="d{number}" href="{name}" media-type="application/xhtml+xml"/>'
                       for number, (name, _) in enumerate(documents))
    ids = {name: f"d{number}" for number, (name, _) in enumerate(documents)}
    itemrefs = "".join(f'<itemref idref="{ids[name]}"/>' for name in spine)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
        if package:
            archive.writestr("META-INF/container.xml",
                             '<container><rootfiles><rootfile full-path="OEBPS/content.opf"/></rootfiles></container>')
            archive.writestr("OEBPS/content.opf",
                             '<package xmlns="http://www.idpf.org/2007/opf" version="3.0">'
                             f"<manifest>{manifest}</manifest><spine>{itemrefs}</spine></package>",
                             zipfile.ZIP_DEFLATED)
        for name, text in documents:
            archive.writestr(f"OEBPS/{name}", text, zipfile.ZIP_DEFLATED)


def xhtml(body: str, head: str = "") -> str:
    return ('<?xml version="1.0" encoding="utf-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml">'
            f"<head>{head}</head>\n<body>\n{body}\n</body></html>\n")


# A book whose paragraphs are known: its documents are in the ZIP file in
# another order than its spine's, and one is not in the spine at all.
KNOWN_FIRST = xhtml(
    "<h1>Chapter   One</h1>\n"
    "<p>The lamp was <i>lit</i> before\n  anyone had asked for it.</p>\n"
    "<p>A line<br/>ended, and <em>a word in <b>two</b> styles</em> &amp; an asterisk * too.</p>\n"
    "<div><p>Inside a div.</p>Then text after it.</div>\n"
    '<script>var left = "out";</script>\n'
    "<p>   </p>\n"
    "<p><br/><br/>Two<br/><br/>breaks<br/></p>",
    head="<title>Left out</title><style>p { margin: 0 }</style>")
KNOWN_SECOND = xhtml(
    "<section><h2>Two</h2><ul><li>first</li><li>second <strong>strong</strong></li></ul>"
    "<table><tr><td>cell</td><td>other</td></tr></table><p>Last&#160;one.</p></section>")
KNOWN_NOTES = xhtml("<p>Not in the spine.</p>")
KNOWN_TEXT = "\n\n".join([
    "Chapter One",
    "The lamp was *lit* before anyone had asked for it.",
    "A line\nended, and *a word in two styles* & an asterisk ∗ too.",
    "Inside a div.",
    "Then text after it.",
    "Two\n \nbreaks",
    "Two",
    "first",
    "second *strong",
    "cell",
    "other",
    "Last one.",
]) + "\n"

LETTERS = "abcdeilmnorstu"
PATTERNS = "a1b e1l 1ti n1n o1r 2st s1t 1mu r1d 1ce i1o"


def made_up_book(path: Path, seed: int) -> None:
    """A long book of made-up words, some long enough to want breaking."""
    rng = random.Random(seed)
    documents = []
    for chapter in range(4):
        paragraphs = []
        for _ in range(rng.randint(30, 60)):
            words = []
            for _ in range(rng.randint(3, 160)):
                word = "".join(rng.choice(LETTERS) for _ in range(rng.choice([2, 3, 4, 5, 7, 9, 12, 15])))
                if rng.randrange(12) == 0:
                    word = f"<i>{word}</i>"
                elif rng.randrange(25) == 0:
                    word += "<br/>"
                words.append(word)
            paragraphs.append("<p>" + " ".join(words) + "</p>")
        documents.append((f"chapter{chapter}.xhtml", xhtml("\n".join(paragraphs))))
    write_book(path, documents, [name for name, _ in documents])


# ---- What the check and bn-lines say ----

def log_lines(trace_tool: Path, capture: Path) -> list[str]:
    result = run([trace_tool, capture])
    expect(result.returncode == 0, f"bn-trace ended with {result.returncode}: {result.stderr!r}")
    lines = []
    for line in result.stdout.decode("utf-8", "replace").splitlines():
        match = re.match(r"\[\s*[\d.]+\] log  (.*)$", line)
        if match:
            lines.append(match.group(1))
    return lines


def run_check(app: Path, trace_tool: Path, card: Path, capture: Path) -> list[str]:
    """Runs the check until it has said its last, giving it longer each time it has not."""
    lines: list[str] = []
    for milliseconds in (3_000, 12_000, 60_000):
        capture.unlink(missing_ok=True)
        environment = dict(os.environ, BN_SERIAL_OUT=str(capture), BN_STORAGE=str(card), BN_LAYOUT_CHECK="1",
                           BN_DISPLAY_SPEED="20")
        for other in ("BN_STORAGE_CHECK", "BN_BOOK_CHECK", "BN_BOOK_CHECK_BRIEF"):
            environment.pop(other, None)
        result = run([app, "--run-ms", milliseconds], env=environment)
        expect(result.returncode == 0, f"the desktop application ended with {result.returncode}: {result.stderr!r}")
        lines = log_lines(trace_tool, capture)
        if any(line.startswith("layout check: the longest step: ") for line in lines):
            break
    return lines


def settings_in(lines: list[str]) -> dict[str, dict[str, int]]:
    """What the check said of each setting of the text, by its name."""
    found: dict[str, dict[str, int]] = {}
    for line in lines:
        match = re.match(r"layout check: ((?:not )?hyphenated(?:, every call timed)?): (\d+) paragraphs?, (\d+) bytes, "
                         r"(\d+) lines? \((\d+) with a hyphen, (\d+) too wide\); checksum ([0-9a-f]{8})\.$", line)
        if match:
            found.setdefault(match.group(1), {}).update(
                paragraphs=int(match.group(2)), bytes=int(match.group(3)), lines=int(match.group(4)),
                hyphens=int(match.group(5)), too_wide=int(match.group(6)), checksum=int(match.group(7), 16))
        match = re.match(r"layout check: ((?:not )?hyphenated(?:, every call timed)?): (\d+) words? asked about, "
                         r"(\d+) last resort, (\d+) lines? early in (\d+), (\d+) cut short; (\d+) nodes; "
                         r"(\d+) bytes measured\.$", line)
        if match:
            found.setdefault(match.group(1), {}).update(
                words_asked=int(match.group(2)), last_resort=int(match.group(3)), early=int(match.group(4)),
                early_paragraphs=int(match.group(5)), cut_short=int(match.group(6)), nodes=int(match.group(7)),
                measured=int(match.group(8)))
    return found


def figures_of(lines_tool: Path, text: Path, table: Path | None) -> dict[str, int]:
    """What bn-lines --figures prints for the file, in the check's terms."""
    result = run([lines_tool, text, "--figures"] + (["--table", table] if table else []))
    expect(result.returncode == 0, f"bn-lines ended with {result.returncode}: {result.stderr!r}")
    printed = result.stdout.decode()
    pattern = (r"paragraphs (\d+), bytes of text (\d+), lines (\d+), the most in one paragraph \d+\n"
               r"lines with a hyphen shown (\d+), too wide (\d+)\n"
               r"words the hyphenator was asked about (\d+), places made as a last resort (\d+)\n"
               r"lines given out early (\d+), in (\d+) paragraphs; ended for want of records (\d+)\n"
               r"nodes in use at the most (\d+) of \d+\n"
               r"calls \d+; the most work in one, by the setter \d+ of \d+, by the breaker \d+ of \d+\n"
               r"bytes measured (\d+); checksum of the lines ([0-9a-f]{8})\n$")
    match = re.match(pattern, printed)
    expect(match is not None, f"what bn-lines printed:\n{printed}")
    names = ["paragraphs", "bytes", "lines", "hyphens", "too_wide", "words_asked", "last_resort", "early",
             "early_paragraphs", "cut_short", "nodes", "measured"]
    figures = {name: int(match.group(number + 1)) for number, name in enumerate(names)}
    figures["checksum"] = int(match.group(len(names) + 1), 16)
    return figures


def compare(said: dict[str, dict[str, int]], name: str, expected: dict[str, int], everything: str) -> None:
    expect(name in said, f"nothing said of the setting {name!r}:\n{everything}")
    for figure, value in expected.items():
        expect(said[name].get(figure) == value,
               f"{name}: {figure} is {said[name].get(figure)} in the check and {value} in bn-lines:\n{everything}")


# ---- The test ----

def main() -> int:
    if len(sys.argv) != 8:
        print(__doc__, file=sys.stderr)
        return 2
    app, trace_tool, lines_tool, hyphen_tool, layout_text, built_in, work = (Path(argument)
                                                                             for argument in sys.argv[1:])
    shutil.rmtree(work, ignore_errors=True)
    books = work / "books"
    card = work / "card"
    layout = card / "layout"
    books.mkdir(parents=True)
    layout.mkdir(parents=True)
    capture = work / "capture.bin"
    try:
        # What the script makes of a book whose paragraphs are known.
        known = books / "a-known.epub"
        write_book(known, [("notes.xhtml", KNOWN_NOTES), ("second.xhtml", KNOWN_SECOND), ("first.xhtml", KNOWN_FIRST)],
                   ["first.xhtml", "second.xhtml"])
        made = run([sys.executable, "-I", layout_text, known])
        expect(made.returncode == 0, f"layout-text.py ended with {made.returncode}: {made.stderr.decode()}")
        expect(made.stdout.decode("utf-8") == KNOWN_TEXT,
               f"what layout-text.py made of the known book:\n{made.stdout.decode('utf-8')!r}\nand what it should be:\n"
               f"{KNOWN_TEXT!r}")
        # A book with no package is read in the order of its ZIP file.
        unpackaged = work / "unpackaged.epub"
        write_book(unpackaged, [("b.xhtml", xhtml("<p>Bee.</p>")), ("a.xhtml", xhtml("<p>Ay.</p>"))], [],
                   package=False)
        made = run([sys.executable, "-I", layout_text, unpackaged])
        expect(made.stdout.decode() == "Bee.\n\nAy.\n", f"a book with no package: {made.stdout.decode()!r}")

        # The card: the text of some books, and a table for their words.
        for seed in range(1, 4):
            made_up_book(books / f"made-up-{seed}.epub", seed)
        text = layout / "paragraphs.txt"
        made = run([sys.executable, "-I", layout_text, books, "--out", text])
        expect(made.returncode == 0, f"layout-text.py ended with {made.returncode}: {made.stderr.decode()}")
        expect(re.match(r"layout-text: 4 books, 14 documents, \d+ paragraphs, \d+ bytes\n$", made.stderr.decode())
               is not None, f"what layout-text.py said: {made.stderr.decode()!r}")
        patterns = work / "patterns.txt"
        patterns.write_text(PATTERNS)
        table = layout / "hyphenation.bnhy"
        packed = run([hyphen_tool, "pack", patterns, "--out", table, "--left", 2, "--right", 2])
        expect(packed.returncode == 0, f"bn-hyphen would not pack the patterns: {packed.stderr.decode()}")

        lines = run_check(app, trace_tool, card, capture)
        everything = "\n".join(lines)
        expect("layout check: passed." in lines, f"the check should pass:\n{everything}")
        said = settings_in(lines)
        expect(list(said) == ["hyphenated", "not hyphenated", "hyphenated, every call timed"],
               f"the three settings, in order:\n{everything}")
        hyphenated = figures_of(lines_tool, text, table)
        plain = figures_of(lines_tool, text, None)
        expect(hyphenated["hyphens"] > 50, f"the text should be one that is hyphenated: {hyphenated}")
        expect(hyphenated["checksum"] != plain["checksum"], "hyphenating should change the lines")
        compare(said, "hyphenated", hyphenated, everything)
        compare(said, "not hyphenated", plain, everything)
        compare(said, "hyphenated, every call timed", hyphenated, everything)
        for expected_line in (r"layout check: the made-up face alone, 65536 bytes in \d+ us \(\d+ KB/s\)\.",
                              r"layout check: breaker, 1024 nodes: the dearest call \d+ us, \d+ of 544768 work; "
                              r"6148 calls in \d+ us\.",
                              r"layout check: table: \d+ bytes, read in \d+ ms; no break before 2 letters or after 2 "
                              r"letters\.",
                              r"layout check: hyphenated, every call timed: \d+ calls, the longest \d+ us\.",
                              rf"layout check: text: {text.stat().st_size} bytes in /layout/paragraphs\.txt\."):
            expect(any(re.fullmatch(expected_line, line) for line in lines),
                   f"a line like {expected_line!r}:\n{everything}")

        # Without a table on the card, it takes the one that is built in.
        table.unlink()
        lines = run_check(app, trace_tool, card, capture)
        everything = "\n".join(lines)
        expect("layout check: passed." in lines, f"without a table, the check should pass:\n{everything}")
        expect(f"layout check: no table at /layout/hyphenation.bnhy (not found): the built-in table, "
               f"{built_in.stat().st_size} bytes; no break before 2 letters or after 3 letters." in lines,
               f"without a table on the card, the built-in one:\n{everything}")
        said = settings_in(lines)
        expect(list(said) == ["hyphenated", "not hyphenated", "hyphenated, every call timed"],
               f"with the built-in table, the three settings:\n{everything}")
        english = figures_of(lines_tool, text, built_in)
        expect(english["hyphens"] > 0, f"English breaks some of the made-up words: {english}")
        expect(english["checksum"] != hyphenated["checksum"], "the built-in table should set other lines")
        compare(said, "hyphenated", english, everything)
        compare(said, "not hyphenated", plain, everything)
        compare(said, "hyphenated, every call timed", english, everything)

        # Without the text, it fails, and says how to make it.
        text.unlink()
        lines = run_check(app, trace_tool, card, capture)
        everything = "\n".join(lines)
        expect("layout check: FAILED. no text at /layout/paragraphs.txt (not found): make it with "
               "tools/layout-text.py." in lines, f"without the text, the check should fail:\n{everything}")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"the layout check agrees with bn-lines about {hyphenated['paragraphs']} paragraphs, set three ways: "
          f"{hyphenated['lines']} lines hyphenated ({hyphenated['hyphens']} with a hyphen) and {plain['lines']} not")
    return 0


if __name__ == "__main__":
    sys.exit(main())
