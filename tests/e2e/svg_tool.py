#!/usr/bin/env python3
"""End-to-end check of bn-svg, and through it of how SVG path data is read and flattened, run by CTest.

The unit tests say what the reader and the flattener should make of paths
written for them. Here bn-svg is set beside a second reading of the same
path data that shares none of its code: path data picked apart with regular
expressions, transforms multiplied out, and curves and arcs worked out in
floating point from the formulas that define them.

For each path, made up at random in every way SVG lets one be written:

  - the commands bn-svg reports have to be the commands that were written,
    with every distance turned into a place;
  - the straight lines it draws for each have to stay within the tolerance
    of the true line, curve or arc, once put through the transform, and the
    true curve within the tolerance of them;
  - it has to say the same whatever pieces the data is read in.

It also checks a few paths against what they are known to mean, written out
by hand, and that what is not path data is refused where it goes wrong.

Then the pictures `bn-svg fill` draws are set beside a second way of filling
the same shapes: no cover is added up, the outline is traced finely and
lines are run through each row of pixels to see where they are inside it.
The two have to agree on how much of every pixel is covered, and the picture
has to come out the same byte for byte however it is cut into strips.

usage: svg_tool.py BN_SVG WORK_DIR
"""

from __future__ import annotations

import math
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path

TOLERANCE = 0.25  # Pixels: what bn-svg flattens to unless told otherwise.
# A place is held in 4096ths and printed to four places of decimals.
NEAR = 0.0004

NUMBER = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")
SEPARATOR = re.compile(r"[ \t\r\n\f]*,?[ \t\r\n\f]*")
NUMBERS_OF = {"m": 2, "l": 2, "h": 1, "v": 1, "c": 6, "s": 4, "q": 4, "t": 2, "a": 7, "z": 0}

Point = tuple[float, float]
Command = tuple  # ("M", x, y), ("L", x, y), ("Q", ...), ("C", ...), ("A", rx, ry, degrees, large, sweep, x, y), ("Z",)


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


# ---- A second reading of path data ----


def reference_commands(data: str) -> list[Command]:
    """What well-formed path data draws, a command at a time, every place being where it is and not how far."""
    commands: list[Command] = []
    at = 0
    current = (0.0, 0.0)
    start = (0.0, 0.0)
    control: Point | None = None  # The last curve's control point, and
    curve = ""  # which kind of curve it was.
    closed = False

    def number() -> float:
        nonlocal at
        at = SEPARATOR.match(data, at).end()
        found = NUMBER.match(data, at)
        assert found, f"no number at {at} of {data!r}"
        at = found.end()
        return float(found.group())

    def flag() -> float:
        nonlocal at
        at = SEPARATOR.match(data, at).end()
        assert data[at] in "01", f"no flag at {at} of {data!r}"
        at += 1
        return float(data[at - 1])

    def begin() -> None:
        nonlocal closed
        if closed:
            commands.append(("M", *start))
            closed = False

    while True:
        at = SEPARATOR.match(data, at).end()
        if at >= len(data):
            return commands
        letter = data[at]
        at += 1
        kind = letter.lower()
        relative = letter.islower()
        first = True
        while first or (NUMBERS_OF[kind] and NUMBER.match(data, SEPARATOR.match(data, at).end())):
            first = False
            values = [flag() if kind == "a" and index in (3, 4) else number() for index in range(NUMBERS_OF[kind])]

            def place(index: int) -> Point:
                if relative:
                    return (current[0] + values[index], current[1] + values[index + 1])
                return (values[index], values[index + 1])

            mirrored = (2 * current[0] - control[0], 2 * current[1] - control[1]) if control else current
            new_control, new_curve = None, ""
            if kind == "m":
                current = start = place(0)
                commands.append(("M", *current))
                closed = False
                control, curve = None, ""
                kind = "l"  # More pairs of numbers after a move are lines.
                continue
            if kind == "z":
                if not closed:
                    commands.append(("Z",))
                current = start
                closed = True
                control, curve = None, ""
                break
            begin()
            if kind == "l":
                to = place(0)
                commands.append(("L", *to))
            elif kind == "h":
                to = (current[0] + values[0] if relative else values[0], current[1])
                commands.append(("L", *to))
            elif kind == "v":
                to = (current[0], current[1] + values[0] if relative else values[0])
                commands.append(("L", *to))
            elif kind == "c":
                new_control, new_curve, to = place(2), "cubic", place(4)
                commands.append(("C", *place(0), *new_control, *to))
            elif kind == "s":
                new_control, new_curve, to = place(0), "cubic", place(2)
                commands.append(("C", *(mirrored if curve == "cubic" else current), *new_control, *to))
            elif kind == "q":
                new_control, new_curve, to = place(0), "quad", place(2)
                commands.append(("Q", *new_control, *to))
            elif kind == "t":
                new_control, new_curve, to = (mirrored if curve == "quad" else current), "quad", place(0)
                commands.append(("Q", *new_control, *to))
            else:
                to = place(5)
                commands.append(("A", abs(values[0]), abs(values[1]), values[2] % 360.0, values[3], values[4], *to))
            current = to
            control, curve = new_control, new_curve


# ---- The curves themselves ----


def arc_points(start: Point, rx: float, ry: float, degrees: float, large: bool, sweep: bool, end: Point):
    """A function from 0..1 to the points of an arc, by the working in the SVG specification; or None for no arc."""
    if start == end:
        return None
    if rx == 0 or ry == 0:
        return lambda t: (start[0] + (end[0] - start[0]) * t, start[1] + (end[1] - start[1]) * t)
    turn = math.radians(degrees)
    cos, sin = math.cos(turn), math.sin(turn)
    half_x = cos * (start[0] - end[0]) / 2 + sin * (start[1] - end[1]) / 2
    half_y = -sin * (start[0] - end[0]) / 2 + cos * (start[1] - end[1]) / 2
    reach = half_x ** 2 / rx ** 2 + half_y ** 2 / ry ** 2
    if reach > 1:
        rx, ry = rx * math.sqrt(reach), ry * math.sqrt(reach)
    under = rx ** 2 * half_y ** 2 + ry ** 2 * half_x ** 2
    factor = math.sqrt(max(0.0, (rx ** 2 * ry ** 2 - under) / under))
    if large == sweep:
        factor = -factor
    centre_x, centre_y = factor * rx * half_y / ry, -factor * ry * half_x / rx
    middle_x = cos * centre_x - sin * centre_y + (start[0] + end[0]) / 2
    middle_y = sin * centre_x + cos * centre_y + (start[1] + end[1]) / 2

    def angle(ux: float, uy: float, vx: float, vy: float) -> float:
        return math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)

    from_x, from_y = (half_x - centre_x) / rx, (half_y - centre_y) / ry
    first = angle(1, 0, from_x, from_y)
    round_ = angle(from_x, from_y, (-half_x - centre_x) / rx, (-half_y - centre_y) / ry)
    if not sweep and round_ > 0:
        round_ -= 2 * math.pi
    elif sweep and round_ < 0:
        round_ += 2 * math.pi

    def point(t: float) -> Point:
        x, y = rx * math.cos(first + round_ * t), ry * math.sin(first + round_ * t)
        return (middle_x + cos * x - sin * y, middle_y + sin * x + cos * y)

    return point


def curve_of(command: Command, start: Point):
    """A function from 0..1 to the points of what a command draws from `start`; or None if it draws nothing."""
    kind = command[0]
    if kind == "L":
        end = command[1:3]
        return lambda t: (start[0] + (end[0] - start[0]) * t, start[1] + (end[1] - start[1]) * t)
    if kind == "Q":
        (x1, y1), (x2, y2) = command[1:3], command[3:5]
        return lambda t: ((1 - t) ** 2 * start[0] + 2 * (1 - t) * t * x1 + t ** 2 * x2,
                          (1 - t) ** 2 * start[1] + 2 * (1 - t) * t * y1 + t ** 2 * y2)
    if kind == "C":
        (x1, y1), (x2, y2), (x3, y3) = command[1:3], command[3:5], command[5:7]
        return lambda t: ((1 - t) ** 3 * start[0] + 3 * (1 - t) ** 2 * t * x1 + 3 * (1 - t) * t ** 2 * x2 + t ** 3 * x3,
                          (1 - t) ** 3 * start[1] + 3 * (1 - t) ** 2 * t * y1 + 3 * (1 - t) * t ** 2 * y2 + t ** 3 * y3)
    return arc_points(start, command[1], command[2], command[3], bool(command[4]), bool(command[5]), command[6:8])


def traced(curve, matrix) -> list[Point]:
    """Points along a curve, in pixels, close enough together that the lines between them are the curve."""
    def at(t: float) -> Point:
        x, y = curve(t)
        return (matrix[0] * x + matrix[2] * y + matrix[4], matrix[1] * x + matrix[3] * y + matrix[5])

    points: list[Point] = []

    def between(t0: float, p0: Point, t1: float, p1: Point, depth: int) -> None:
        tm = (t0 + t1) / 2
        pm = at(tm)
        if depth < 14 and (depth < 3 or to_line(pm, p0, p1) > 0.004):
            between(t0, p0, tm, pm, depth + 1)
            between(tm, pm, t1, p1, depth + 1)
        else:
            points.append(p1)

    first = at(0.0)
    points.append(first)
    between(0.0, first, 1.0, at(1.0), 0)
    return points


def to_line(point: Point, a: Point, b: Point) -> float:
    dx, dy = b[0] - a[0], b[1] - a[1]
    length = dx * dx + dy * dy
    if length == 0:
        return math.hypot(point[0] - a[0], point[1] - a[1])
    along = max(0.0, min(1.0, ((point[0] - a[0]) * dx + (point[1] - a[1]) * dy) / length))
    return math.hypot(point[0] - a[0] - along * dx, point[1] - a[1] - along * dy)


def lengths_along(lines: list[Point]) -> list[float]:
    lengths = [0.0]
    for a, b in zip(lines, lines[1:]):
        lengths.append(lengths[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    return lengths


def stray(points: list[Point], lines: list[Point]) -> float:
    """The furthest any of `points` is from `lines`, both following one curve from its start to its end.

    A point is measured to the lines that are as far along as it is, give or
    take a few pixels, so it is not excused by being near some other part
    of a curve that doubles back on itself.
    """
    slack = 6.0
    along, lengths = lengths_along(points), lengths_along(lines)
    scale = lengths[-1] / along[-1] if along[-1] > 0 else 0.0
    furthest, first = 0.0, 1
    for point, at in zip(points, along):
        if len(lines) < 2:
            furthest = max(furthest, math.hypot(point[0] - lines[0][0], point[1] - lines[0][1]))
            continue
        target = at * scale
        while first + 1 < len(lines) and lengths[first] < target - slack:
            first += 1
        nearest = to_line(point, lines[first - 1], lines[first])
        index = first + 1
        while index < len(lines) and lengths[index - 1] <= target + slack:
            nearest = min(nearest, to_line(point, lines[index - 1], lines[index]))
            index += 1
        furthest = max(furthest, nearest)
    return furthest


def furthest_apart(drawn: list[Point], true: list[Point]) -> float:
    with_middles = [drawn[0]]
    for a, b in zip(drawn, drawn[1:]):
        with_middles += [((a[0] + b[0]) / 2, (a[1] + b[1]) / 2), b]
    return max(stray(true, drawn), stray(with_middles, true))


# ---- Transforms ----


def multiplied(outer, inner):
    return (outer[0] * inner[0] + outer[2] * inner[1], outer[1] * inner[0] + outer[3] * inner[1],
            outer[0] * inner[2] + outer[2] * inner[3], outer[1] * inner[2] + outer[3] * inner[3],
            outer[0] * inner[4] + outer[2] * inner[5] + outer[4], outer[1] * inner[4] + outer[3] * inner[5] + outer[5])


def a_transform(rng: random.Random) -> tuple[str, tuple]:
    """A transform as SVG writes one, and the matrix it comes to."""
    text, matrix = [], (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
    for _ in range(rng.randrange(1, 4)):
        kind = rng.randrange(6)
        if kind == 0:
            x, y = rng.randrange(-200, 200) / 2, rng.randrange(-200, 200) / 2
            text.append(f"translate({x} {y})")
            one = (1.0, 0.0, 0.0, 1.0, x, y)
        elif kind == 1:
            x, y = rng.choice([0.25, 0.5, 1.5, 2, 3, -1, -2]), rng.choice([0.25, 0.5, 1.5, 2, 3, -1])
            text.append(f"scale({x},{y})")
            one = (float(x), 0.0, 0.0, float(y), 0.0, 0.0)
        elif kind == 2:
            degrees = rng.choice([30, 45, 90, -60, 135, 200.5])
            text.append(f"rotate({degrees})")
            one = (math.cos(math.radians(degrees)), math.sin(math.radians(degrees)),
                   -math.sin(math.radians(degrees)), math.cos(math.radians(degrees)), 0.0, 0.0)
        elif kind == 3:
            degrees = rng.choice([10, -20, 30])
            text.append(f"skewX({degrees})")
            one = (1.0, 0.0, math.tan(math.radians(degrees)), 1.0, 0.0, 0.0)
        elif kind == 4:
            degrees = rng.choice([10, -20, 30])
            text.append(f"skewY({degrees})")
            one = (1.0, math.tan(math.radians(degrees)), 0.0, 1.0, 0.0, 0.0)
        else:
            one = tuple(rng.randrange(-32, 33) / 16 for _ in range(4)) + (rng.randrange(-50, 50), rng.randrange(-50, 50))
            text.append("matrix(" + " ".join(str(value) for value in one) + ")")
        matrix = multiplied(matrix, one)
    return rng.choice([" ", ",", "", " , "]).join(text), matrix


# ---- Paths made up at random ----


def a_number(rng: random.Random, value: float) -> str:
    """One of the ways a number may be written. `value` is a multiple of 1/64, so each is exact."""
    kind = rng.randrange(8)
    text = repr(value)
    if text.endswith(".0"):
        text = text[:-2] if kind != 5 else text[:-1]  # "12" or "12."
    if kind == 1 and value > 0:
        text = "+" + text
    elif kind == 2 and text.startswith("0."):
        text = text[1:]
    elif kind == 2 and text.startswith("-0."):
        text = "-" + text[2:]
    elif kind == 3 and "e" not in text and "." not in text:
        text += "e0"
    elif kind == 4 and "e" not in text and value == int(value) and value % 10 == 0 and value != 0:
        text = repr(int(value) // 10) + "E1"
    return text


def joined(rng: random.Random, parts: list[str]) -> str:
    """Numbers put together with what SVG lets come between them, which may be nothing."""
    text = parts[0]
    for before, part in zip(parts, parts[1:]):
        # Nothing at all is enough before a sign, and before a point when the number before it has had its own.
        bare = part[0] in "+-" or (part[0] == "." and ("." in before or "e" in before.lower()))
        if not (bare and rng.random() < 0.5):
            text += rng.choice([" ", ",", " , ", "\n", "  "])
        text += part
    return text


def a_path(rng: random.Random) -> str:
    """Path data with something of everything, written every which way."""
    def length(limit: int = 300) -> float:
        return rng.randrange(-limit * 64, limit * 64) / 64

    def numbers(letter: str) -> list[str]:
        kind = letter.lower()
        if kind == "a":
            radius_x = rng.choice([0.0, 0.5, 5.0, 20.25, 80.0, 150.5, -30.0, rng.randrange(1, 9600) / 64])
            radius_y = radius_x if rng.random() < 0.4 else rng.choice([0.0, 1.0, 12.5, 60.0, rng.randrange(1, 9600) / 64])
            turn = rng.choice([0.0, 0.0, 30.0, 45.0, 90.0, -120.0, 405.0])
            flags = rng.choice(["0", "1"]) + rng.choice(["", " ", ","]) + rng.choice(["0", "1"])
            return [a_number(rng, radius_x), a_number(rng, radius_y), a_number(rng, turn), flags + rng.choice(["", " "]),
                    a_number(rng, length(120)), a_number(rng, length(120))]
        return [a_number(rng, length(120 if letter.islower() else 300)) for _ in range(NUMBERS_OF[kind])]

    def written(letter: str) -> str:
        parts = numbers(letter)
        if letter.lower() != "a":
            return joined(rng, parts)
        # The flags of an arc are already one piece, and need nothing after them.
        text = joined(rng, parts[:3]) + rng.choice([" ", ","]) + parts[3]
        tail = joined(rng, parts[4:])
        return text + ("" if parts[3][-1] == " " or tail[0] in "+-." else rng.choice(["", " "])) + tail

    text = rng.choice(["M", "m"]) + rng.choice(["", " "]) + written("M")
    for _ in range(rng.randrange(3, 10)):
        letter = rng.choice("MmLlHhVvCcSsQqTtAaZzLlCcQqAaSsTt")
        text += rng.choice(["", " ", "\n"]) + letter
        if letter in "Zz":
            continue
        text += rng.choice(["", " "])
        sets = [written(letter) for _ in range(rng.choice([1, 1, 1, 2, 3]))]
        text += sets[0]
        for more in sets[1:]:
            text += (rng.choice(["", " "]) if more[0] in "+-" else rng.choice([" ", ",", " , "])) + more
    return text + rng.choice(["", " ", "z", "\n"])


# ---- bn-svg ----


def run(tool: str, work: Path, data: str, *options: str) -> subprocess.CompletedProcess:
    (work / "path.d").write_bytes(data.encode("utf-8", "surrogateescape"))
    return subprocess.run([tool, "path", str(work / "path.d"), *options], capture_output=True, check=False)


def said(output: bytes) -> list[tuple[Command, list[Point], bool]]:
    """What bn-svg printed: each command, the pixels it drew to, and whether it closed the outline."""
    result: list[tuple[Command, list[Point], bool]] = []
    for line in output.decode("ascii").splitlines():
        words = line.split()
        if line.startswith(" "):
            if words[0] == "z":
                result[-1] = (result[-1][0], result[-1][1], True)
            else:
                result[-1][1].append((float(words[1]), float(words[2])))
        else:
            result.append(((words[0], *(float(word) for word in words[1:])), [], False))
    return result


def check_path(tool: str, work: Path, data: str, transform: str, matrix: tuple, pieces: list[str]) -> int:
    """Checks one well-formed path, read whole and in pieces of each of the sizes given. Returns how many commands it had."""
    what = f"{data!r} through {transform!r}"
    result = run(tool, work, data, "--transform", transform)
    expect(result.returncode == 0, f"{what}: bn-svg ended with {result.returncode}: {result.stderr!r}")
    expect(b"held at the largest" not in result.stderr, f"{what}: a number was too large: the test is at fault")
    for size in pieces:
        cut = run(tool, work, data, "--transform", transform, "--pieces", size)
        expect(cut.stdout == result.stdout and cut.returncode == 0, f"{what}: read differently {size} bytes at a time")

    wanted = reference_commands(data)
    got = said(result.stdout)
    expect([c[0][0] for c in got] == [c[0] for c in wanted],
           f"{what}: commands {[c[0][0] for c in got]}, should be {[c[0] for c in wanted]}")

    def in_pixels(point: Point) -> Point:
        return (matrix[0] * point[0] + matrix[2] * point[1] + matrix[4],
                matrix[1] * point[0] + matrix[3] * point[1] + matrix[5])

    # How much a 4096th of a unit of the drawing can come to in pixels.
    grows = max(abs(matrix[0]) + abs(matrix[2]), abs(matrix[1]) + abs(matrix[3]), 1.0)
    current = start = (0.0, 0.0)
    pen = pen_start = (0.0, 0.0)  # Where bn-svg's own lines have got to, in pixels.
    for (command, pixels, closed), want in zip(got, wanted):
        for index, (value, should) in enumerate(zip(command[1:], want[1:])):
            near = 0.0002 if command[0] == "A" and index == 2 else NEAR
            expect(abs(value - should) <= near, f"{what}: {command} should be {want}")
        if command[0] == "Z":
            expect(closed and not pixels, f"{what}: Z drew {pixels}")
            current, pen = start, pen_start
            continue
        expect(not closed, f"{what}: {command} closed the outline")
        end = want[-2:]
        if command[0] == "M":
            expect(len(pixels) == 1, f"{what}: {command} drew {pixels}")
            current = start = end
            pen = pen_start = pixels[0]
        else:
            curve = curve_of(want, current)
            if curve is None:
                expect(not pixels, f"{what}: {command} goes nowhere and drew {pixels}")
            else:
                expect(len(pixels) >= 1, f"{what}: {command} drew nothing")
                apart = furthest_apart([pen] + pixels, traced(curve, matrix))
                expect(apart <= TOLERANCE + 0.03 + 0.001 * grows,
                       f"{what}: {command} is {apart:.3f} pixels from the true curve at its worst")
                pen = pixels[-1]
            current = end
        # A matrix's factors are held in 1,048,576ths, which far from (0, 0) is a few hundredths of a pixel.
        should_be = in_pixels(current)
        expect(math.hypot(pen[0] - should_be[0], pen[1] - should_be[1]) <= 0.03 + 0.001 * grows,
               f"{what}: after {command} the lines are at {pen}, should be at {should_be}")
    return len(got)


# ---- A second way of filling ----

ROWS_A_PIXEL = 32  # How many lines the second way runs through each row of pixels.


def outlines_of(data: str, matrix: tuple) -> list[list[Point]]:
    """The outlines path data draws, in pixels, with every curve traced finely."""
    outlines: list[list[Point]] = []
    current = (0.0, 0.0)
    for command in reference_commands(data):
        if command[0] == "M":
            current = command[1:3]
            outlines.append([(matrix[0] * current[0] + matrix[2] * current[1] + matrix[4],
                              matrix[1] * current[0] + matrix[3] * current[1] + matrix[5])])
        elif command[0] != "Z":
            curve = curve_of(command, current)
            if curve is not None:
                outlines[-1].extend(traced(curve, matrix)[1:])
            current = command[-2:]
    return outlines


def counted(outlines: list[list[Point]], width: int, height: int, rule: str):
    """How much of each pixel a shape covers, from nothing to one, and which pixels are plain.

    Lines are run across each row of pixels. Along a line, the outline goes
    round every point between two crossings the same number of times, which
    says whether those points are inside; how much of each pixel's width is
    inside is then exact, and its height is counted in lines.

    A pixel is plain if the outline goes round all of it the same number of
    times, or one more in some parts than in others. That is every pixel but
    those where the outline crosses itself or lies twice over, which bn-svg
    says it only draws nearly.
    """
    edges: list[list[tuple[Point, Point]]] = [[] for _ in range(height)]
    for outline in outlines:
        for a, b in zip(outline, outline[1:] + outline[:1]):
            if a[1] != b[1]:
                top, bottom = min(a[1], b[1]), max(a[1], b[1])
                for row in range(max(0, math.floor(top)), min(height - 1, math.floor(bottom)) + 1):
                    edges[row].append((a, b))
    cover = [[0.0] * width for _ in range(height)]
    plain = [[True] * width for _ in range(height)]
    for row in range(height):
        fewest: list[int | None] = [None] * width
        most: list[int | None] = [None] * width
        for line in range(ROWS_A_PIXEL):
            y = row + (line + 0.5) / ROWS_A_PIXEL
            crossings = sorted((a[0] + (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]), 1 if b[1] > a[1] else -1)
                               for a, b in edges[row] if (a[1] <= y) != (b[1] <= y))
            times, at = 0, -math.inf
            for x, way in crossings + [(math.inf, 0)]:
                left, right = max(at, 0.0), min(x, float(width))
                if left < right:
                    inside = times != 0 if rule == "nonzero" else times % 2 != 0
                    for pixel in range(int(left), min(width - 1, int(right)) + 1):
                        share = min(right, pixel + 1.0) - max(left, float(pixel))
                        if share > 0:
                            if inside:
                                cover[row][pixel] += share / ROWS_A_PIXEL
                            fewest[pixel] = times if fewest[pixel] is None else min(fewest[pixel], times)
                            most[pixel] = times if most[pixel] is None else max(most[pixel], times)
                times, at = times + way, x
        for pixel in range(width):
            plain[row][pixel] = most[pixel] - fewest[pixel] <= 1
    return cover, plain


def a_shape(rng: random.Random, width: int, height: int) -> str:
    """Path data for a few outlines that lie over a picture of this size and some way beyond it."""
    def place() -> str:
        return f"{rng.randrange(-32, (width + 8) * 4) / 4} {rng.randrange(-32, (height + 8) * 4) / 4}"

    text = ""
    for _ in range(rng.randrange(1, 4)):
        text += "M" + place()
        for _ in range(rng.randrange(2, 6)):
            kind = rng.choice("LLLCCQSTA")
            if kind == "A":
                text += (f"A{rng.randrange(4, 160) / 4} {rng.randrange(4, 160) / 4} {rng.choice([0, 30, 90, -45])} "
                         f"{rng.randrange(2)} {rng.randrange(2)} {place()}")
            else:
                text += kind + " ".join(place() for _ in range(NUMBERS_OF[kind.lower()] // 2))
        text += rng.choice(["Z", ""])
    return text


def fill(tool: str, work: Path, data: str, width: int, height: int, *options: str):
    """Has bn-svg draw path data. What it ended with and said, and the picture's pixels, or None if it wrote none."""
    (work / "path.d").write_bytes(data.encode("utf-8"))
    out = work / "picture.pgm"
    out.unlink(missing_ok=True)
    result = subprocess.run([tool, "fill", str(work / "path.d"), "--size", f"{width}x{height}", "--out", str(out),
                             *options], capture_output=True, check=False)
    if not out.exists():
        return result, None
    written = out.read_bytes()
    header = b"P5\n%d %d\n255\n" % (width, height)
    expect(written.startswith(header) and len(written) == len(header) + width * height,
           f"{data!r}: what bn-svg wrote is not a picture of {width} by {height}")
    return result, written[len(header):]


FILL_TRANSFORMS = [
    ("", (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)),
    ("scale(1.5)", (1.5, 0.0, 0.0, 1.5, 0.0, 0.0)),
    ("translate(-6 4.5)", (1.0, 0.0, 0.0, 1.0, -6.0, 4.5)),
    ("matrix(0.75 0.25 -0.5 1 9 -3)", (0.75, 0.25, -0.5, 1.0, 9.0, -3.0)),
]


def check_fill(tool: str, work: Path, rng: random.Random, number: int) -> tuple[int, int, float]:
    """Checks one picture. Returns how many of its pixels were compared, how many were not plain, and the worst."""
    width, height = rng.randrange(30, 49), rng.randrange(20, 37)
    data = a_shape(rng, width, height)
    transform, matrix = FILL_TRANSFORMS[number % len(FILL_TRANSFORMS)]
    rule = "evenodd" if number % 2 else "nonzero"
    what = f"{data!r} through {transform!r} by {rule} at {width}x{height}"
    # Curves are cut finely, so that what is compared is the filling.
    options = ["--transform", transform, "--rule", rule, "--tolerance", "0.02"]
    result, drawn = fill(tool, work, data, width, height, *options)
    expect(result.returncode == 0 and drawn is not None, f"{what}: bn-svg ended with {result.returncode}: {result.stderr!r}")
    expect(f"{width} by {height} in 1 strip of {height} rows".encode() in result.stderr, f"{what}: {result.stderr!r}")
    for rows in ("1", "7"):
        cut, strips = fill(tool, work, data, width, height, *options, "--rows", rows)
        expect(cut.returncode == 0 and strips == drawn, f"{what}: drawn differently in strips of {rows} rows")
        expect(f"in {math.ceil(height / int(rows))} strips of {rows} rows".encode() in cut.stderr, f"{what}: {cut.stderr!r}")

    cover, plain = counted(outlines_of(data, matrix), width, height, rule)
    compared, skipped, worst, total = 0, 0, 0.0, 0.0
    for row in range(height):
        for pixel in range(width):
            if not plain[row][pixel]:
                skipped += 1
                continue
            apart = abs((255 - drawn[row * width + pixel]) / 255 - cover[row][pixel])
            expect(apart <= 0.05, f"{what}: the pixel at {pixel},{row} is {255 - drawn[row * width + pixel]} parts in "
                                  f"255 covered, and should be {cover[row][pixel] * 255:.1f}")
            compared += 1
            worst = max(worst, apart)
            total += apart
    expect(total / max(compared, 1) <= 0.003, f"{what}: {total / max(compared, 1):.4f} of a pixel out on average")
    return compared, skipped, worst


def check_filling(tool: str, work: Path) -> str:
    """Everything about `bn-svg fill`. Returns a sentence saying what was found."""
    rng = random.Random(20261115)
    pictures, compared, skipped, worst = 0, 0, 0, 0.0
    for number in range(32):
        one, left_out, apart = check_fill(tool, work, rng, number)
        pictures, compared, skipped, worst = pictures + 1, compared + one, skipped + left_out, max(worst, apart)
    expect(skipped * 10 < compared, f"{skipped} pixels of {compared + skipped} were left out: the test is at fault")

    # A rectangle whose edges fall part of the way across pixels, by hand: three quarters of the pixel at its left
    # edge, half of the one at its right, half of each along its top and three quarters along its bottom.
    result, drawn = fill(tool, work, "M2.25 1.5H6.5V4.75H2.25Z", 10, 8)
    expect(result.returncode == 0 and drawn is not None, f"a rectangle: {result.stderr!r}")
    for (x, y), gray in {(4, 3): 0, (0, 0): 255, (7, 3): 255, (2, 3): 64, (6, 3): 128, (4, 1): 128, (4, 4): 64,
                         (2, 1): 159, (6, 4): 159}.items():
        expect(abs(drawn[y * 10 + x] - gray) <= 1, f"a rectangle: the pixel at {x},{y} is {drawn[y * 10 + x]}, not {gray}")

    # A gray, and a gray that lets half of the white under it show.
    result, drawn = fill(tool, work, "M2 2H8V6H2Z", 10, 8, "--gray", "100")
    expect(result.returncode == 0 and drawn[3 * 10 + 5] == 100 and drawn[0] == 255, f"a gray: {result.stderr!r}")
    result, drawn = fill(tool, work, "M2 2H8V6H2Z", 10, 8, "--gray", "100", "--opacity", "128")
    expect(result.returncode == 0 and abs(drawn[3 * 10 + 5] - 177) <= 1, f"a thin gray: {drawn[3 * 10 + 5]}")

    # What comes before a mistake in the data is still drawn.
    result, drawn = fill(tool, work, "M2 2H8V6H2Z K", 10, 8)
    expect(result.returncode == 1 and b"at byte 12" in result.stderr and drawn is not None and drawn[3 * 10 + 5] == 0,
           f"a mistake after a shape: ended with {result.returncode}: {result.stderr!r}")

    # What it will not do.
    (work / "path.d").write_bytes(b"M2 2H8V6H2Z")
    for options in ([], ["--size", "0x8"], ["--size", "10"], ["--size", "10x8", "--rule", "sideways"],
                    ["--size", "10x8", "--gray", "256"], ["--size", "10x8", "--rows", "0"],
                    ["--size", "10x8", "--count"], ["--size", "99999x8"]):
        result = subprocess.run([tool, "fill", str(work / "path.d"), *options], capture_output=True, check=False)
        expect(result.returncode == 2 and b"usage:" in result.stderr, f"fill {options}: ended with {result.returncode}")
    result = subprocess.run([tool, "path", str(work / "path.d"), "--rows", "4"], capture_output=True, check=False)
    expect(result.returncode == 2, f"path --rows: ended with {result.returncode}")
    result = subprocess.run([tool, "fill", str(work / "path.d"), "--size", "10x8", "--out", str(work / "no" / "p.pgm")],
                            capture_output=True, check=False)
    expect(result.returncode == 1 and b"cannot write" in result.stderr, f"nowhere to write: {result.stderr!r}")
    result = subprocess.run([tool, "fill", str(work / "not-there"), "--size", "10x8"], capture_output=True, check=False)
    expect(result.returncode == 1 and b"cannot read" in result.stderr, f"a missing file: {result.stderr!r}")

    return (f"{pictures} pictures, {compared} pixels: what bn-svg fills agrees with a second way of filling to within "
            f"{worst:.3f} of a pixel, and is the same in strips of any height")


KNOWN = [
    # What is written, and what it means, by hand.
    ("M10 20", ["M 10 20"]),
    ("m10 20 5 5-5 5", ["M 10 20", "L 15 25", "L 10 30"]),
    ("M1.5.5L10-5", ["M 1.5 0.5", "L 10 -5"]),
    ("M0 0h10v10H0z", ["M 0 0", "L 10 0", "L 10 10", "L 0 10", "Z"]),
    ("M0 0a1 1 0 011 1", ["M 0 0", "A 1 1 0 0 1 1 1"]),
    ("M0 0a1 1 0 10-5-5", ["M 0 0", "A 1 1 0 1 0 -5 -5"]),
    ("M0 0C10 0 20 40 30 30S50 10 60 30", ["M 0 0", "C 10 0 20 40 30 30", "C 40 20 50 10 60 30"]),
    ("M0 0Q10 20 20 0T40 0", ["M 0 0", "Q 10 20 20 0", "Q 30 -20 40 0"]),
    ("M1 1L2 2zl3 0", ["M 1 1", "L 2 2", "Z", "M 1 1", "L 4 1"]),
    ("M0 0A-5-6 450 1 0 7 8", ["M 0 0", "A 5 6 90 1 0 7 8"]),
    ("M0,0L1e1,2.5E-1", ["M 0 0", "L 10 0.25"]),
    ("", []),
]

BROKEN = [
    # What is written, how many commands come before the mistake, and the byte it is at.
    ("L1 2", 0, 0),
    ("M0 0 K1 2", 1, 5),
    ("M0 0 L1", 1, 7),
    ("M0 0 L1 1 L2,,2", 2, 13),
    ("M0 0 L1 1 Z 5 5", 3, 12),
    ("M0 0 A1 1 0 2 0 5 5", 1, 12),
    ("M0 0 L1 1 \xff L2 2", 2, 10),
    ("M0 0 L1e 1", 1, 8),
]


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tool = sys.argv[1]
    work = Path(sys.argv[2])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        for data, meaning in KNOWN:
            result = run(tool, work, data)
            got = said(result.stdout)
            want = [(line.split()[0], *(float(word) for word in line.split()[1:])) for line in meaning]
            expect(result.returncode == 0 and [command for command, _, _ in got] == want,
                   f"{data!r}: bn-svg says {[command for command, _, _ in got]}, it means {want}")

        rng = random.Random(20261114)
        paths, commands = 0, 0
        identity = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
        for number in range(120):
            data = a_path(rng)
            transform, matrix = ("", identity) if number % 3 == 0 else a_transform(rng)
            commands += check_path(tool, work, data, transform, matrix, ["1", "5"] if number % 4 == 0 else [])
            paths += 1

        # A looser tolerance is kept to as well.
        loose = run(tool, work, "M0 0C100 300 400 300 500 0", "--tolerance", "2")
        fine = run(tool, work, "M0 0C100 300 400 300 500 0")
        drawn = [(0.0, 0.0)] + said(loose.stdout)[1][1]
        true = traced(curve_of(("C", 100.0, 300.0, 400.0, 300.0, 500.0, 0.0), (0.0, 0.0)), identity)
        expect(loose.returncode == 0 and furthest_apart(drawn, true) <= 2.03, "a tolerance of 2 was not kept to")
        expect(len(drawn) < len(said(fine.stdout)[1][1]), "a tolerance of 2 took as many lines as a quarter")

        for data, before, byte in BROKEN:
            result = run(tool, work, data)
            got = said(result.stdout)
            expect(result.returncode == 1 and len(got) == before and f"at byte {byte}," in result.stderr.decode(),
                   f"{data!r}: bn-svg ended with {result.returncode} after {len(got)} commands: {result.stderr!r}")

        # A radius of seventeen digits, as two books have them: read, held, and drawn as the line it is.
        result = run(tool, work, "M0 0a17394593339182234 17394593339182234 0 0 0 5 5")
        got = said(result.stdout)
        expect(result.returncode == 0 and b"held at the largest" in result.stderr and len(got[1][1]) == 1
               and got[1][1][0] == (5.0, 5.0), f"an enormous radius: {result.stdout!r} {result.stderr!r}")

        result = subprocess.run([tool, "path", str(work / "not-there")], capture_output=True, check=False)
        expect(result.returncode == 1 and b"cannot read" in result.stderr, f"a missing file: {result.stderr!r}")
        result = run(tool, work, "M0 0", "--transform", "wobble(3)")
        expect(result.returncode == 2 and b"not a transform" in result.stderr, f"a bad transform: {result.stderr!r}")

        filling = check_filling(tool, work)
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"{paths} paths, {commands} commands: bn-svg agrees with a second reading of every one, "
          f"and its lines stay within {TOLERANCE} pixels of the true curves; {len(BROKEN)} that go wrong refused "
          f"where they do")
    print(filling)
    return 0


if __name__ == "__main__":
    sys.exit(main())
