#!/usr/bin/env python3
"""End-to-end check of the drawing viewer in the desktop application, run by CTest.

The desktop application is run with BN_DRAWINGS over a folder standing for
the card, whose drawings folder holds a few drawings and pictures made
here, and Down is pressed to go from one to the next and round. The
simulated X4 saves a picture of its panel at each refresh, and each is set
beside what the drawing should come to there, worked out here: `bn-svg
draw` draws the file at the size it should have on the page, which is held
as a book is read, and that picture is turned a quarter, as the X4's panel
is, and put in the middle of the page. The pictures are set beside what
`bn-png read` or `bn-jpeg read` makes of each at the size it should be
shown at, in the same way: a PNG file; a baseline JPEG large enough to be
read at a half; and a progressive JPEG, read from its first scan at an
eighth and made larger. The simulated X4 shows four levels: a drawing is
dithered to black and white, and a picture to the four, shown first in
black and white and then by a gray pass, which saves a second picture of
the panel. So the two are compared in blocks of pixels: how dark each
block is, on the whole. The lines of log text the viewer sends are read
back with bn-trace.

The presses are timed by the clock, not by the application's turns, and a
run that has not shown every drawing in time is run again with more.

usage: drawing_viewer.py APP BN_SVG BN_PNG BN_JPEG BN_TRACE WORK_DIR
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True  # Or Python leaves a compiled copy of jpeg_tool beside it.

import jpeg_tool  # noqa: E402  (found through the path set just above)


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


def made_jpegs() -> dict[str, bytes]:
    """Two JPEG pictures, made by jpeg_tool's encoder. A baseline gray one of 960 by 576, wider than the page is for
    its height, so that it is as wide as the page and read at a half: a dark bar and a gray disc on a light ground.
    And a progressive one in colour, sampled 4:2:0, of 240 by 384, also as wide as the page: its gray a dark band
    across a light ground and a gradient below it, read at an eighth."""
    width, height = 960, 576
    rows = []
    for y in range(height):
        row = bytearray([230]) * width
        if 96 <= y < 200:
            row[96:864] = bytes([20]) * 768
        for x in range(384, 576):
            if (x - 480) ** 2 + (y - 400) ** 2 < 96 ** 2:
                row[x] = 100
        rows.append(bytes(row))
    steps = {0: jpeg_tool.STANDARD_STEPS[0]}
    baseline = jpeg_tool.Plan(width, height, 0xC0, [(1, 1, 1, 0)], [rows], steps, scans=[([0], 0, 63, 0, 0)])
    width, height = 240, 384
    gray = []
    for y in range(height):
        if 64 <= y < 160:
            gray.append(bytes([30]) * width)
        elif y >= 224:
            gray.append(bytes(min(255, x) for x in range(width)))
        else:
            gray.append(bytes([220]) * width)
    colour = [bytes([128]) * (width // 2) for _ in range(height // 2)]
    progressive = jpeg_tool.Plan(
        width, height, 0xC2, [(1, 2, 2, 0), (2, 1, 1, 1), (3, 1, 1, 1)], [gray, colour, colour],
        {0: jpeg_tool.STANDARD_STEPS[0], 1: jpeg_tool.STANDARD_STEPS[1]},
        scans=[([0, 1, 2], 0, 0, 0, 1), ([0], 1, 5, 0, 0), ([1], 1, 63, 0, 0), ([2], 1, 63, 0, 0),
               ([0], 6, 63, 0, 0), ([0, 1, 2], 0, 0, 1, 0)])
    rng = jpeg_tool.random.Random(1)
    return {"e-photograph.jpg": jpeg_tool.encode(baseline, rng, extras=False),
            "f-progressive.jpeg": jpeg_tool.encode(progressive, rng, extras=False)}


PICTURES = {"d-picture.png": made_picture(), **made_jpegs()}


def run(command: list, **more) -> subprocess.CompletedProcess:
    return subprocess.run([str(part) for part in command], capture_output=True, check=False, **more)


def read_png(path: Path) -> tuple[int, list[list[float]]]:
    """A panel picture's bits a pixel, and its rows, each pixel how light it is: 1 for white, 0 for black, and the
    grays between. As the simulated panel writes them: one bit a pixel while it shows only black and white, two
    once it shows grays, stored, each row with a filter byte of none in front."""
    data = path.read_bytes()
    expect(data[:8] == b"\x89PNG\r\n\x1a\n", f"{path} is not a PNG file")
    at, idat, width, height = 8, b"", 0, 0
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height, depth = struct.unpack(">IIB", body[:9])
            expect(depth in (1, 2), f"{path} is not one or two bits a pixel")
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    raw = zlib.decompress(idat)
    row_bytes = (width * depth + 7) // 8
    top = (1 << depth) - 1
    rows = []
    for y in range(height):
        line = raw[y * (row_bytes + 1):(y + 1) * (row_bytes + 1)]
        expect(line[0] == 0, f"{path}: a row with a filter")
        rows.append([((line[1 + x * depth // 8] >> (8 - depth - x * depth % 8)) & top) / top for x in range(width)])
    return depth, rows


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


def expected_picture(reader: str, work: Path, name: str) -> list[list[float]]:
    """The same for a picture: as `bn-png` or `bn-jpeg` reads it and scales it to the size it is shown at, as large
    as fits on the page and in the middle of it, turned into the panel's rows."""
    file = work / "card" / "drawings" / name
    header = run([reader, "header", file]).stdout.split()
    across, down = int(header[1]), int(header[2])
    fit = ["--width", str(PAGE_WIDTH)] if across * PAGE_HEIGHT >= down * PAGE_WIDTH else ["--height", str(PAGE_HEIGHT)]
    picture = work / f"{name}.pgm"
    done = run([reader, "read", file, *fit, "--out", picture])
    expect(done.returncode == 0, f"{Path(reader).name} could not read {name}: {done.stderr!r}")
    width, height, grays = read_pgm(picture)
    left, top = (PAGE_WIDTH - width) // 2, (PAGE_HEIGHT - height) // 2
    dark = [[0.0] * PANEL_WIDTH for _ in range(PANEL_HEIGHT)]
    for y in range(height):
        for x in range(width):
            panel_x, panel_y = top + y, PANEL_HEIGHT - 1 - (left + x)
            dark[panel_y][panel_x] = (255 - grays[y * width + x]) / 255
    return dark


def compare(shown: list[list[float]], dark: list[list[float]], what: str) -> float:
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
    if len(sys.argv) != 7:
        print(__doc__, file=sys.stderr)
        return 2
    app, bn_svg, bn_png, bn_jpeg, bn_trace = sys.argv[1:6]
    work = Path(sys.argv[6])
    shutil.rmtree(work, ignore_errors=True)
    folder = work / "card" / "drawings"
    folder.mkdir(parents=True)
    for name, text in {**DRAWINGS, **OTHERS}.items():
        (folder / name).write_text(text)
    for name, data in PICTURES.items():
        (folder / name).write_bytes(data)
    names = sorted([*DRAWINGS, *PICTURES])
    # Each in turn, and round to the first. A picture is two refreshes: in
    # black and white, and the gray pass.
    shown = names + names[:1]
    refreshes = len(shown) + sum(1 for name in shown if name in PICTURES)
    try:
        # Down after each drawing has had its time, and round to the first.
        for seconds in (4, 8, 16):
            for old in work.glob("panel-*.png"):
                old.unlink()
            # Nothing kept from the attempt before.
            shutil.rmtree(work / "card" / ".picture-cache", ignore_errors=True)
            presses = ",".join(f"{seconds * 1000 * step}:down" for step in range(1, len(shown)))
            environment = {**os.environ, "BN_STORAGE": str(work / "card"), "BN_DRAWINGS": "1",
                           "BN_DISPLAY_OUT": str(work / "panel"), "BN_SERIAL_OUT": str(work / "serial.bin"),
                           "BN_INPUT": presses}
            done = run([app, "--run-ms", str(seconds * 1000 * len(shown))], env=environment)
            expect(done.returncode == 0, f"the application ended with {done.returncode}: {done.stderr!r}")
            lines = log_lines(bn_trace, work / "serial.bin")
            if len(list(work.glob("panel-*.png"))) >= refreshes and len(lines) >= len(shown):
                break
        pictures = sorted(work.glob("panel-*.png"))
        expect(len(pictures) == refreshes, f"the panel was refreshed {len(pictures)} times, not {refreshes}: {lines}")
        # Each line says which drawing or picture it was, and what it came to;
        # the first, gone round to, comes from the cache.
        for number, (line, name) in enumerate(zip(lines, shown)):
            kind = "picture" if name in PICTURES else "drawing"
            came = "from the cache" if number == len(names) else "ok"
            expect(line.startswith(f"{kind} {names.index(name) + 1} of {len(names)}: {name}, {came}, "),
                   f"line {number + 1}: {line!r}")
        cached = log_lines(bn_trace, work / "serial.bin", "cache")
        expect(len(cached) == len(shown) and all(" cache: kept, " in line for line in cached[:-1]) and
               " cache: read " in cached[-1], f"what the cache did: {cached}")
        expect("left out: text," in lines[2], f"the text was not said to be left out: {lines[2]!r}")
        expect(", 90x200 gray shown 360x800: reading " in lines[3], f"the picture's line: {lines[3]!r}")
        expect(", 960x576 gray read at 1/2 shown 480x288: reading " in lines[4], f"the baseline JPEG's line: {lines[4]!r}")
        expect(", 240x384 progressive 4:2:0 read at 1/8 shown 480x768: reading " in lines[5],
               f"the progressive JPEG's line: {lines[5]!r}")
        # The first timed line of each: not a drawing's second, of its spool.
        timed = [line for line in log_lines(bn_trace, work / "serial.bin", "timed")
                 if " timed: spool " not in line and " timed: not spooled" not in line]
        for line, name in zip(timed, shown):
            expect((name in PICTURES) == ("; gray planes " in line), f"the gray pass of {name}: {line!r}")
        worst = 0.0
        panel = iter(pictures)
        for name in shown:
            if name in PICTURES:
                # First in black and white, then in grays.
                first, second = next(panel), next(panel)
                expect(read_png(first)[0] == 1, f"{first.name} ({name}) is not black and white")
                depth, levels = read_png(second)
                expect(depth == 2, f"{second.name} ({name}) shows no grays")
                dark = expected_picture(bn_png if name.endswith(".png") else bn_jpeg, work, name)
                worst = max(worst, compare(levels, dark, f"{second.name} ({name})"))
            else:
                picture = next(panel)
                dark = expected_panel(bn_svg, work, name)
                worst = max(worst, compare(read_png(picture)[1], dark, f"{picture.name} ({name})"))
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"3 drawings and 3 pictures shown and gone round, the first again from the cache, each as bn-svg draws it "
          f"or bn-png or bn-jpeg reads it turned onto the panel, the pictures in four levels, every block of "
          f"{BLOCK} by {BLOCK} pixels within {worst:.3f} of as dark as it should be")
    return 0


if __name__ == "__main__":
    sys.exit(main())
