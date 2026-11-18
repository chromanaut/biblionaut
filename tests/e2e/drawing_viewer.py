#!/usr/bin/env python3
"""End-to-end check of the drawing viewer in the desktop application, run by CTest.

The desktop application is run with BN_DRAWINGS over a folder standing for
the card, whose drawings folder holds a few drawings and a picture made
here, and Down is pressed to go from one to the next and round. The simulated X4 saves a
picture of its panel at each refresh, and each is set beside what the
drawing should come to there, worked out here: `bn-svg draw` draws the
file at the size it should have on the page, which is held as a book is
read, and that picture is turned a quarter, as the X4's panel is, and put
in the middle of the page. The picture, a PNG file, is set beside what
`bn-png read` makes of it at the size it should be shown at, in the same
way. The panel is black and white, its grays
dithered, so the two are compared in blocks of pixels: how dark each block
is, on the whole. The lines of log text the viewer sends are read back
with bn-trace.

The presses are timed by the clock, not by the application's turns, and a
run that has not shown every drawing in time is run again with more.

usage: drawing_viewer.py APP BN_SVG BN_PNG BN_TRACE WORK_DIR
"""

from __future__ import annotations

import os
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
BLOCK = 16

DRAWINGS = {
    # Wider than the page: as wide as it, in the middle down it.
    "a-shapes.svg": "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 60 40'>"
                    "<rect x='2' y='2' width='20' height='12'/><circle cx='45' cy='28' r='9' fill='#666'/>"
                    "<path d='M5 35h20l-10-12z' fill='#aaa'/></svg>",
    # Taller than the page: as high as it, in the middle across it. A
    # gradient, and a sheet.
    "b-gradient.svg": "<svg xmlns='http://www.w3.org/2000/svg' width='30' height='60'>"
                      "<style>.band { fill: url(#g) }</style>"
                      "<linearGradient id='g' x2='0' y2='1'><stop stop-color='#000'/><stop offset='1' stop-color='#fff'/>"
                      "</linearGradient><rect class='band' x='3' y='3' width='24' height='54'/></svg>",
    # Many shapes, so that drawing it takes many turns; and text, which is
    # left out and said to be.
    "c-many.svg": "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"
                  + "".join(f"<rect x='{(n * 7) % 90}' y='{(n * 13) % 90}' width='8' height='8' fill='#{(n * 37) % 200:02x}{(n * 37) % 200:02x}{(n * 37) % 200:02x}'/>"
                            for n in range(300))
                  + "<text x='10' y='50'>Words</text></svg>",
}
# What the viewer is to pass over.
OTHERS = {"notes.txt": "Not a drawing."}


def made_picture() -> bytes:
    """A PNG picture, taller than the page is for its width, so that it is as high as the page and in the middle
    across: a gradient from black to white down it, and a black bar and a white one across it."""
    width, height = 90, 200
    rows = []
    for y in range(height):
        row = bytearray(min(255, y * 256 // height) for _ in range(width))
        if 40 <= y < 60:
            row[10:80] = bytes(70)
        if 140 <= y < 160:
            row[10:80] = bytes([255]) * 70
        rows.append(b"\x00" + bytes(row))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


PICTURES = {"d-picture.png": made_picture()}


def run(command: list, **more) -> subprocess.CompletedProcess:
    return subprocess.run([str(part) for part in command], capture_output=True, check=False, **more)


def read_png(path: Path) -> list[list[int]]:
    """A panel picture: rows of 1 for white, 0 for black. As the simulated panel writes them: one bit a pixel,
    stored, each row with a filter byte of none in front."""
    data = path.read_bytes()
    expect(data[:8] == b"\x89PNG\r\n\x1a\n", f"{path} is not a PNG file")
    at, idat, width, height = 8, b"", 0, 0
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height, depth = struct.unpack(">IIB", body[:9])
            expect(depth == 1, f"{path} is not one bit a pixel")
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    raw = zlib.decompress(idat)
    row_bytes = (width + 7) // 8
    rows = []
    for y in range(height):
        line = raw[y * (row_bytes + 1):(y + 1) * (row_bytes + 1)]
        expect(line[0] == 0, f"{path}: a row with a filter")
        rows.append([(line[1 + x // 8] >> (7 - x % 8)) & 1 for x in range(width)])
    return rows


def read_pgm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    fields, at = [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end])
        at = end
    width, height = int(fields[1]), int(fields[2])
    return width, height, data[at + 1:at + 1 + width * height]


def expected_panel(bn_svg: str, work: Path, name: str) -> list[list[float]]:
    """How dark each pixel of the panel should be, from 0 to 1: the drawing at its size on the page, in the
    middle of it, turned into the panel's rows."""
    file = work / "card" / "drawings" / name
    size = run([bn_svg, "draw", file, "--width", "1"])
    words = size.stderr.split(b"a drawing of ")[1].split(b",")[0].split(b" by ")
    across, down = float(words[0]), float(words[1])
    fit = ["--width", str(PAGE_WIDTH)] if across * PAGE_HEIGHT >= down * PAGE_WIDTH else ["--height", str(PAGE_HEIGHT)]
    picture = work / f"{name}.pgm"
    done = run([bn_svg, "draw", file, *fit, "--out", picture])
    expect(done.returncode == 0, f"bn-svg could not draw {name}: {done.stderr!r}")
    width, height, grays = read_pgm(picture)
    left, top = (PAGE_WIDTH - width) / 2, (PAGE_HEIGHT - height) / 2
    dark = [[0.0] * PANEL_WIDTH for _ in range(PANEL_HEIGHT)]
    for y in range(height):
        for x in range(width):
            page_x, page_y = int(round(left + x)), int(round(top + y))
            # Turned back a quarter: the page's top is the panel's left.
            panel_x, panel_y = page_y, PANEL_HEIGHT - 1 - page_x
            if 0 <= panel_x < PANEL_WIDTH and 0 <= panel_y < PANEL_HEIGHT:
                dark[panel_y][panel_x] = (255 - grays[y * width + x]) / 255
    return dark


def expected_picture(bn_png: str, work: Path, name: str) -> list[list[float]]:
    """The same for a picture: as `bn-png` scales it to the size it is shown at, as large as fits on the page and
    in the middle of it, turned into the panel's rows."""
    file = work / "card" / "drawings" / name
    header = run([bn_png, "header", file]).stdout.split()
    across, down = int(header[1]), int(header[2])
    fit = ["--width", str(PAGE_WIDTH)] if across * PAGE_HEIGHT >= down * PAGE_WIDTH else ["--height", str(PAGE_HEIGHT)]
    picture = work / f"{name}.pgm"
    done = run([bn_png, "read", file, *fit, "--out", picture])
    expect(done.returncode == 0, f"bn-png could not read {name}: {done.stderr!r}")
    width, height, grays = read_pgm(picture)
    left, top = (PAGE_WIDTH - width) // 2, (PAGE_HEIGHT - height) // 2
    dark = [[0.0] * PANEL_WIDTH for _ in range(PANEL_HEIGHT)]
    for y in range(height):
        for x in range(width):
            panel_x, panel_y = top + y, PANEL_HEIGHT - 1 - (left + x)
            dark[panel_y][panel_x] = (255 - grays[y * width + x]) / 255
    return dark


def compare(shown: list[list[int]], dark: list[list[float]], what: str) -> float:
    """Sets the panel beside how dark it should be, a block at a time. Returns the largest difference."""
    worst, total, blocks = 0.0, 0.0, 0
    for top in range(0, PANEL_HEIGHT, BLOCK):
        for left in range(0, PANEL_WIDTH, BLOCK):
            black = sum(1 - shown[y][x] for y in range(top, top + BLOCK) for x in range(left, left + BLOCK))
            wanted = sum(dark[y][x] for y in range(top, top + BLOCK) for x in range(left, left + BLOCK))
            apart = abs(black - wanted) / (BLOCK * BLOCK)
            worst = max(worst, apart)
            total += apart
            blocks += 1
    mean = total / blocks
    # A block an edge goes through can be a few pixels' worth out, by where
    # the drawing's edge falls and where the dots fall beside it; over the
    # picture, very little.
    expect(worst <= 0.2 and mean <= 0.01, f"{what}: blocks of the panel are up to {worst:.3f} darker or lighter than "
                                           f"they should be, {mean:.4f} on average")
    return worst


def log_lines(bn_trace: str, capture: Path, kind: str = "what") -> list[str]:
    """The lines of log text the viewer sent: those that say what each drawing came to ("what"), those that say where
    its time went ("timed"), or those that say what the cache did ("cache")."""
    done = run([bn_trace, capture])
    lines = [line.split(" log  ", 1)[1] for line in done.stdout.decode("utf-8", "replace").splitlines() if " log  " in line]

    def kind_of(line: str) -> str:
        return "timed" if " timed: " in line else "cache" if " cache: " in line else "what"

    return [line for line in lines if kind_of(line) == kind]


def main() -> int:
    if len(sys.argv) != 6:
        print(__doc__, file=sys.stderr)
        return 2
    app, bn_svg, bn_png, bn_trace, work = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], Path(sys.argv[5])
    shutil.rmtree(work, ignore_errors=True)
    folder = work / "card" / "drawings"
    folder.mkdir(parents=True)
    for name, text in {**DRAWINGS, **OTHERS}.items():
        (folder / name).write_text(text)
    for name, data in PICTURES.items():
        (folder / name).write_bytes(data)
    names = sorted([*DRAWINGS, *PICTURES])
    try:
        # Down after each drawing has had its time, and round to the first.
        for seconds in (4, 8, 16):
            for old in work.glob("panel-*.png"):
                old.unlink()
            # Nothing kept from the attempt before.
            shutil.rmtree(work / "card" / ".picture-cache", ignore_errors=True)
            presses = ",".join(f"{seconds * 1000 * step}:down" for step in (1, 2, 3, 4))
            environment = {**os.environ, "BN_STORAGE": str(work / "card"), "BN_DRAWINGS": "1",
                           "BN_DISPLAY_OUT": str(work / "panel"), "BN_SERIAL_OUT": str(work / "serial.bin"),
                           "BN_INPUT": presses}
            done = run([app, "--run-ms", str(seconds * 1000 * 5)], env=environment)
            expect(done.returncode == 0, f"the application ended with {done.returncode}: {done.stderr!r}")
            lines = log_lines(bn_trace, work / "serial.bin")
            if len(list(work.glob("panel-*.png"))) >= 5 and len(lines) >= 5:
                break
        pictures = sorted(work.glob("panel-*.png"))
        expect(len(pictures) == 5, f"the panel was refreshed {len(pictures)} times, not 5: {lines}")
        # Each line says which drawing or picture it was, and what it came to;
        # the first, gone round to, comes from the cache.
        shown = [names[0], names[1], names[2], names[3], names[0]]
        for number, (line, name) in enumerate(zip(lines, shown)):
            kind = "picture" if name in PICTURES else "drawing"
            came = "from the cache" if number == 4 else "ok"
            expect(line.startswith(f"{kind} {names.index(name) + 1} of 4: {name}, {came}, "), f"line {number + 1}: {line!r}")
        cached = log_lines(bn_trace, work / "serial.bin", "cache")
        expect(len(cached) == 5 and all(" cache: kept, " in line for line in cached[:4]) and " cache: read " in cached[4],
               f"what the cache did: {cached}")
        expect("left out: text," in lines[2], f"the text was not said to be left out: {lines[2]!r}")
        expect(", 90x200 gray shown 360x800: reading " in lines[3], f"the picture's line: {lines[3]!r}")
        worst = 0.0
        for picture, name in zip(pictures, shown):
            dark = expected_picture(bn_png, work, name) if name in PICTURES else expected_panel(bn_svg, work, name)
            worst = max(worst, compare(read_png(picture), dark, f"{picture.name} ({name})"))
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"3 drawings and a picture shown and gone round, the first again from the cache, each as bn-svg draws it "
          f"or bn-png reads it turned onto the panel, every block of "
          f"{BLOCK} by {BLOCK} pixels within {worst:.3f} of as dark as it should be")
    return 0


if __name__ == "__main__":
    sys.exit(main())
