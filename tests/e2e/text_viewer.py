#!/usr/bin/env python3
"""End-to-end check of the text viewer in the desktop application, run by CTest.

`bn-font` makes a fonts image of a real font, Courier Prime, which the
build downloads: its regular as the serif and its bold as the sans, at two
sizes. The desktop application is run with BN_TEXT naming that image, over a
folder standing for the card whose layout/paragraphs.txt is two pages of
text written here; and the buttons are pressed: on to the second page,
back to the first, the other size, the other face. The simulated X4 saves a
picture of its panel at each refresh: each page first in black and white,
and then after its gray pass.

Nothing here sets the text in lines a second time: that is the paragraph
setter's, which has its own checks. What is held to here is what must be so
however the lines fall:

  - a page's two pictures agree: what was black is what has ink;
  - the text keeps to the page's margins, the page held as a book is read;
  - gone through from its start to its end, the text is on the panel once:
    the ink of the two pages, added up from the pictures, is the ink of
    every character of the text in that face, which `bn-font info` gives
    for each glyph, and of a hyphen for each word that was broken, and
    nothing else; and the glyphs the viewer says it placed are as many;
  - going back shows the first page again, pixel for pixel;
  - the other size and the other face begin where the page shown began,
    and the bold face puts more ink on the same letters;
  - at the start and at each change of size or face the viewer says which
    faces it holds of the image, which it reads a face at a time as a
    device does: the one face text is set in, mapped in whole pages;
  - with BN_TEXT_TOUR it goes through both sizes in both faces by itself,
    and says when it is done; and an image that cannot be read is said.

The presses are timed by the clock, not by the application's turns, and a
run that has not shown everything in time is run again with more.

usage: text_viewer.py APP BN_FONT BN_TRACE FONT_DIR WORK_DIR
"""

from __future__ import annotations

import os
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


# The X4's panel, in its own orientation, and the page as it is read: the
# panel turned a quarter clockwise.
PANEL_WIDTH, PANEL_HEIGHT = 800, 480
PAGE_WIDTH, PAGE_HEIGHT = PANEL_HEIGHT, PANEL_WIDTH
MARGIN = 24
SIZES = (22, 30)

# Two pages of it at the larger size. No m and no w, whose pictures in this
# font are wider than the pen moves for them, so that no two letters' ink
# meets and the ink of a page is the ink of its letters added up. What is
# between asterisks is in italic, which this image has no face for.
TEXT = """The lantern stood by the door of the inn, and nobody had cause to look at it. A cart passed in the dusk; its driver nodded, *said nothing*, and drove on up the hill road to the north.

In the yard a girl counted eggs into a basket, and the old dog slept through all of it. Nothing else stirred. The baker's boy ran by, late as on any other day, and the geese by the pond took no notice of that either.

By nine the road lay still and the lantern burned on, as it had every night for thirty years. "It is not for us," the innkeeper used to say, "it is for those not yet here." Nobody asked again after that.

The first frost of the year settled on the roofs before light, and in the kitchen the fire had to be laid afresh. Outside, a single track of prints led across the yard to the stable door and did not return. The horses breathed in the dark, and the girl stood listening, a basket on one hip, until the cold put an end to that and the door shut.
"""


def run(command: list, **more) -> subprocess.CompletedProcess:
    return subprocess.run([str(part) for part in command], capture_output=True, check=False, **more)


def read_png(path: Path) -> tuple[int, list[list[int]]]:
    """A panel picture's bits a pixel, and its rows, each pixel its level from 0 for black. As the simulated panel
    writes them: one bit a pixel while it shows only black and white, two once it shows grays, stored, each row with
    a filter byte of none in front."""
    data = path.read_bytes()
    expect(data[:8] == b"\x89PNG\r\n\x1a\n", f"{path} is not a PNG file")
    at, idat, width, height, depth = 8, b"", 0, 0, 0
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height, depth = struct.unpack(">IIB", body[:9])
            expect(depth in (1, 2), f"{path} is not one or two bits a pixel")
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    expect((width, height) == (PANEL_WIDTH, PANEL_HEIGHT), f"{path} is {width} by {height}")
    raw = zlib.decompress(idat)
    row_bytes = (width * depth + 7) // 8
    top = (1 << depth) - 1
    rows = []
    for y in range(height):
        line = raw[y * (row_bytes + 1):(y + 1) * (row_bytes + 1)]
        expect(line[0] == 0, f"{path}: a row with a filter")
        rows.append([(line[1 + x * depth // 8] >> (8 - depth - x * depth % 8)) & top for x in range(width)])
    return depth, rows


def page_of(rows: list[list[int]], lightest: int) -> list[list[int]]:
    """The ink of each pixel of the page, from 0 for paper: the panel's rows turned back a quarter, the page's top
    being the panel's left."""
    return [[lightest - rows[PANEL_HEIGHT - 1 - x][y] for x in range(PAGE_WIDTH)] for y in range(PAGE_HEIGHT)]


def glyph_ink(bn_font: str, image: Path, characters: str) -> dict[tuple[str, int], dict[str, dict]]:
    """For each face of the image, by role and size, what `bn-font info` says of each character."""
    asked = [f"U+{ord(character):04X}" for character in sorted(set(characters))]
    done = run([bn_font, "info", image, "--glyphs", *asked])
    expect(done.returncode == 0, f"bn-font info ended with {done.returncode}: {done.stderr!r}")
    faces: dict[tuple[str, int], dict[str, dict]] = {}
    face = None
    for line in done.stdout.decode("utf-8").splitlines():
        heading = re.match(r"^(\S+) +(\S+) +(\d+) px:", line)
        if heading:
            face = faces.setdefault((heading.group(1), int(heading.group(3))), {})
            continue
        glyph = re.match(r"^  U\+([0-9A-F]+): glyph \d+, advance (\d+), (\d+) by (\d+) at (-?\d+), (-?\d+), ink (\d+),",
                         line)
        if glyph and face is not None:
            advance, width, height, left, top, ink = (int(number) for number in glyph.groups()[1:])
            face[chr(int(glyph.group(1), 16))] = {"advance": advance, "width": width, "left": left, "ink": ink}
    return faces


def log_lines(bn_trace: str, capture: Path) -> list[str]:
    done = run([bn_trace, capture])
    return [line.split(" log  ", 1)[1] for line in done.stdout.decode("utf-8", "replace").splitlines()
            if " log  text: " in line]


LINE = re.compile(r"^text: page (\d+), (serif|sans) (\d+) px, (\d+) lines?, (\d+) glyphs?: set \d+ ms, planes \d+ ms, "
                  r"refresh \d+ ms, gray planes \d+ ms, gray pass \d+ ms(; the end of the text)?$")
# What the viewer says when the faces text is set in change.
HELD = re.compile(r"^text: (serif|sans) (\d+) px: (\d+) faces? held, (\d+) KB of the image; (\d+) KB mapped$")
# The pages an X4 maps its flash in, which the desktop counts room in.
PAGE_KB = 64


def main() -> int:
    if len(sys.argv) != 6:
        print(__doc__, file=sys.stderr)
        return 2
    app, bn_font, bn_trace = sys.argv[1:4]
    fonts, work = Path(sys.argv[4]).resolve(), Path(sys.argv[5]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    (work / "card" / "layout").mkdir(parents=True)
    (work / "card" / "layout" / "paragraphs.txt").write_text(TEXT)
    try:
        # The image: the regular as the serif, the bold as the sans.
        preset = work / "courier.preset"
        preset.write_text(f"sizes {' '.join(str(size) for size in SIZES)}\n"
                          f"serif regular {fonts / 'CourierPrime-Regular.ttf'}\n"
                          f"sans regular  {fonts / 'CourierPrime-Bold.ttf'}\n"
                          "characters U+0020-U+007E\n")
        image = work / "courier.bnfn"
        done = run([bn_font, "build", preset, "--out", image])
        expect(done.returncode == 0, f"bn-font build ended with {done.returncode}: {done.stderr!r}")
        glyphs = glyph_ink(bn_font, image, TEXT.replace("\n", " ") + "-")
        serif, bold = glyphs[("serif", SIZES[1])], glyphs[("sans", SIZES[1])]
        # The letters of the text: what is drawn of it.
        letters = [character for character in TEXT if character not in " \n*"]
        for character in set(letters) | {"-"}:
            expect(character in serif, f"the font has no glyph for {character!r}")
            glyph = serif[character]
            expect(glyph["left"] >= 0 and (glyph["left"] + glyph["width"]) * 4 <= glyph["advance"],
                   f"the picture of {character!r} is wider than the pen moves for it: the text here was written "
                   f"without such letters")

        # The first page; Right for the second, which is the last; Left for
        # the first again; Confirm for the other size; Back for the other
        # face. Each is two refreshes: in black and white, and the gray pass.
        buttons = ("right", "left", "confirm", "back")
        shown = 1 + len(buttons)
        lines: list[str] = []
        for seconds in (5, 10, 20):
            for old in work.glob("panel-*.png"):
                old.unlink()
            presses = ",".join(f"{seconds * 1000 * (step + 1)}:{button}" for step, button in enumerate(buttons))
            environment = {**os.environ, "BN_STORAGE": str(work / "card"), "BN_TEXT": str(image),
                           "BN_DISPLAY_OUT": str(work / "panel"), "BN_SERIAL_OUT": str(work / "serial.bin"),
                           "BN_INPUT": presses}
            done = run([app, "--run-ms", str(seconds * 1000 * shown)], env=environment)
            expect(done.returncode == 0, f"the application ended with {done.returncode}: {done.stderr!r}")
            every = log_lines(bn_trace, work / "serial.bin")
            lines = [line for line in every if line.startswith("text: page ")]
            # A press that came before a page's grays leaves it without them.
            if (len(list(work.glob("panel-*.png"))) >= 2 * shown and len(lines) >= shown and
                    not any("no grays" in line for line in lines)):
                break
        pictures = sorted(work.glob("panel-*.png"))
        expect(len(pictures) == 2 * shown and len(lines) == shown,
               f"the panel was refreshed {len(pictures)} times, not {2 * shown}: {lines}")
        said = []
        for line in lines:
            match = LINE.match(line)
            expect(match is not None, f"a line of the viewer's: {line!r}")
            said.append({"page": int(match.group(1)), "face": match.group(2), "size": int(match.group(3)),
                         "lines": int(match.group(4)), "glyphs": int(match.group(5)), "end": bool(match.group(6))})
        expect([(page["page"], page["face"], page["size"], page["end"]) for page in said] ==
               [(1, "serif", 30, False), (2, "serif", 30, True), (1, "serif", 30, False), (1, "serif", 22, False),
                (1, "sans", 22, False)], f"the pages shown: {lines}")

        # The image is read as a device reads it, a face at a time: at the
        # start, and at each change of size or face, the viewer says which
        # faces it holds. Each of these has a regular and no italic, so one
        # is held, in whole pages beside the page the table of faces is in.
        held = [line for line in every if not line.startswith("text: page ")]
        faces = []
        for line in held:
            match = HELD.match(line)
            expect(match is not None, f"a line of the viewer's: {line!r}")
            faces.append((match.group(1), int(match.group(2)), int(match.group(3))))
            in_image, mapped = int(match.group(4)), int(match.group(5))
            expect(mapped % PAGE_KB == 0 and PAGE_KB + in_image <= mapped <= 3 * PAGE_KB + in_image,
                   f"{mapped} KB mapped for a face of {in_image} KB and the table of faces: {line!r}")
        expect(faces == [("serif", 30, 1), ("serif", 22, 1), ("sans", 22, 1)], f"the faces held: {held}")

        # Each page's two pictures.
        pages = []
        for number in range(shown):
            first, second = pictures[2 * number], pictures[2 * number + 1]
            depth, rows = read_png(first)
            expect(depth == 1, f"{first.name} is not black and white")
            black = page_of(rows, 1)
            depth, rows = read_png(second)
            expect(depth == 2, f"{second.name} shows no grays")
            ink = page_of(rows, 3)
            levels = {level for row in ink for level in row}
            expect(levels == {0, 1, 2, 3}, f"{second.name} has ink of the levels {sorted(levels)}")
            for y in range(PAGE_HEIGHT):
                expect([level != 0 for level in ink[y]] == [level != 0 for level in black[y]],
                       f"page {number + 1}, row {y}: what was black before the gray pass is not what has ink after")
                inked = [x for x in range(PAGE_WIDTH) if ink[y][x]]
                if inked:
                    # A letter's lowest row may hang a little below the last
                    # line's own room.
                    expect(MARGIN <= y < PAGE_HEIGHT - MARGIN + 6 and MARGIN <= inked[0] and
                           inked[-1] < PAGE_WIDTH - MARGIN,
                           f"page {number + 1}: ink outside the margins at row {y}, from {inked[0]} to {inked[-1]}")
            pages.append(ink)

        # From its start to its end, the text is on the panel once: its
        # letters' ink, and a hyphen's for each broken word.
        total = sum(sum(row) for page in pages[:2] for row in page)
        wanted = sum(serif[character]["ink"] for character in letters)
        hyphens = said[0]["glyphs"] + said[1]["glyphs"] - len(letters)
        expect(0 <= hyphens <= said[0]["lines"] + said[1]["lines"],
               f"the two pages have {said[0]['glyphs']} and {said[1]['glyphs']} glyphs, and the text {len(letters)} "
               f"letters")
        expect(total == wanted + hyphens * serif["-"]["ink"],
               f"the two pages have ink of {total}, and the text's letters {wanted} with {hyphens} hyphens of "
               f"{serif['-']['ink']} each")
        expect(hyphens > 0, "no word was broken, in two pages of a face this wide")
        # Lines enough to fill the first page, and no more than fit.
        fit = (PAGE_HEIGHT - 2 * MARGIN) // 34
        expect(fit - 3 <= said[0]["lines"] <= fit, f"the first page has {said[0]['lines']} lines, and {fit} fit")

        # Back to the first page: the same picture.
        expect(pages[2] == pages[0], "the first page shown again is not as it was")
        expect(pages[1] != pages[0], "the second page is the first")
        # The other size, and then the other face, from where the first page
        # begins: the same letters, monospace as both are, and more ink in
        # the bold.
        expect(said[3]["glyphs"] > said[0]["glyphs"], "the smaller size has no more glyphs on its page")
        expect(said[4]["glyphs"] in range(said[3]["glyphs"] - 3, said[3]["glyphs"] + 4),
               f"the bold's page has {said[4]['glyphs']} glyphs and the regular's {said[3]['glyphs']}")
        smaller, bolder = (sum(sum(row) for row in page) for page in pages[3:5])
        expect(bolder > smaller * 1.05, f"the bold's page has ink of {bolder}, and the regular's {smaller}")
        expect(bold["a"]["ink"] > serif["a"]["ink"], "the bold has no more ink in an a")

        # A tour: the application goes through the text by itself, from the
        # smaller size, in the serif and then the sans, and at the larger in
        # the sans and then the serif; and says when it is done. The panel
        # is made twenty times quicker than an X4's, and no button pressed.
        legs: list[tuple[str, int]] = []
        for seconds in (8, 20, 60):
            environment = {**os.environ, "BN_STORAGE": str(work / "card"), "BN_TEXT": str(image),
                           "BN_TEXT_TOUR": "1", "BN_DISPLAY_SPEED": "20", "BN_SERIAL_OUT": str(work / "tour.bin")}
            environment.pop("BN_DISPLAY_OUT", None)
            done = run([app, "--run-ms", str(seconds * 1000)], env=environment)
            expect(done.returncode == 0, f"the application on a tour ended with {done.returncode}: {done.stderr!r}")
            every = log_lines(bn_trace, work / "tour.bin")
            if every and every[-1] == "text: the tour is done.":
                break
        expect(bool(every) and every[-1] == "text: the tour is done.", f"the tour did not end: {every}")
        for line in every:
            match = LINE.match(line)
            if match is not None and (not legs or legs[-1] != (match.group(2), int(match.group(3)))):
                legs.append((match.group(2), int(match.group(3))))
        expect(legs == [("serif", 22), ("sans", 22), ("sans", 30), ("serif", 30)], f"the tour's faces and sizes: {every}")
        toured = len([line for line in every if line.startswith("text: page ")])
        expect(4 <= toured <= 8, f"the tour showed {toured} pages, and two at the most in each of four: {every}")

        # An image that is not there is said, and nothing is run.
        environment = {**os.environ, "BN_STORAGE": str(work / "card"), "BN_TEXT": str(work / "no-such-image.bnfn")}
        done = run([app, "--run-ms", "100"], env=environment)
        expect(done.returncode == 2 and b"BN_TEXT: cannot read" in done.stderr,
               f"an image that is not there: {done.returncode}, {done.stderr!r}")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"{shown} pages shown, each in black and white and then in grays: {len(letters)} letters and {hyphens} hyphens "
          f"on two pages, their ink {total} as bn-font has it for each glyph")
    return 0


if __name__ == "__main__":
    sys.exit(main())
