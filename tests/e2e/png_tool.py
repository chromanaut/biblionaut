#!/usr/bin/env python3
"""End-to-end check of bn-png, and through it of the PNG reader, run by CTest.

bn-png is set beside a second reading of the same pictures that shares none
of its code: the file taken apart all at once, its data decompressed by
Python's zlib, its filters undone and its pixels turned to gray a row at a
time, by the rules the reader's header states.

Pictures are made up at random, of every kind and depth PNG has, with
palettes, transparency of each kind, chunks to be passed over (some with
wrong checksums), and their data cut into chunks of many sizes. For each:

  - `bn-png read` has to say what the second reading says: how the reading
    ended, the checksum of the gray, the header, the rows, the room the rows
    need, and the chunks passed over; given the file in pieces of one byte
    or of seven, it has to say the same;
  - `bn-png read --out` has to write the rows as the second reading has
    them;
  - `bn-png header` has to read the header, and no more than it.

Then the same pictures spoiled: cut short anywhere, a byte changed with the
checksums put right after, and the data broken. Where the reading can only
fail one way, bn-png has to fail that way, with the same rows before it;
where where it stops depends on how far the inflater has read ahead (a
wrong checksum on a chunk of data, data that zlib refuses), the rows it
gave have to be the second reading's, as far as they go.

And an EPUB-like ZIP file of pictures, read with `bn-png book`.

Some of the good pictures are also read made larger, smaller, or both at
once (`--width`, `--height`), through the firmware's scaler: every new pixel
has to be within a gray of a working-out in real numbers of the second
reading's rows, each new pixel the average of the old ones it covers or a
blend of the two nearest its middle, each direction by itself.

The second reading is also what tools/check-books.py sets beside bn-png on
real books (read_png).

usage: png_tool.py BN_PNG WORK_DIR
"""

from __future__ import annotations

import random
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path

SIGNATURE = b"\x89PNG\r\n\x1a\x0a"
LONGEST_CHUNK = 0x7FFFFFFF
MOST_EMPTY_IN_A_ROW = 2
# The most memory bn-png gives a reader for rows: a picture that needs more
# has no room.
MOST_ROOM = 268435456
SAMPLES = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
NAMES = {0: "gray", 2: "rgb", 3: "palette", 4: "gray+alpha", 6: "rgba"}
DEPTHS = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


# ---- The second reading ----

def gray_of(red: int, green: int, blue: int) -> int:
    return (77 * red + 150 * green + 29 * blue + 128) >> 8


def over_white(gray: int, alpha: int) -> int:
    return (2 * (gray * alpha + 255 * (255 - alpha)) + 255) // 510


@dataclass
class Reading:
    status: str = "ok"
    header: tuple[int, int, str, int] | None = None  # Width, height, kind, depth.
    interlaced: bool = False
    rows: list[bytes] = field(default_factory=list)
    room: int = 0
    passed: int = 0
    bad: int = 0
    # Whether where the reading stops is known for sure. When it is not, the
    # reader may stop at the same place or later, having given more rows.
    exact: bool = True
    header_bytes: int = 0  # How much of the file the header took.

    def crc(self) -> int:
        crc = 0
        for row in self.rows:
            crc = zlib.crc32(row, crc)
        return crc

    def line(self) -> str:
        """The line `bn-png read` prints for this reading."""
        if self.header is None:
            return f"{self.status} {self.crc():08x} - - - - 0 0 {self.passed} {self.bad}"
        width, height, kind, depth = self.header
        return (f"{self.status} {self.crc():08x} {width} {height} {kind} {depth} {len(self.rows)} {self.room} "
                f"{self.passed} {self.bad}")


def is_name(name: bytes) -> bool:
    return all(65 <= letter <= 90 or 97 <= letter <= 122 for letter in name)


_MASKS: dict[int, tuple[int, int]] = {}


def add_bytes(row: bytes, above: bytes) -> bytes:
    """Each byte of `row` plus the byte of `above` under it, modulo 256: done on
    the rows as two whole numbers. The low seven bits of each byte are added
    with room for their carry, and the top bit is set apart from it, so that
    nothing carries from one byte into the next."""
    size = len(row)
    if size not in _MASKS:
        _MASKS[size] = (int.from_bytes(b"\x7f" * size, "big"), int.from_bytes(b"\x80" * size, "big"))
    low, high = _MASKS[size]
    a = int.from_bytes(row, "big")
    b = int.from_bytes(above, "big")
    return (((a & low) + (b & low)) ^ ((a ^ b) & high)).to_bytes(size, "big")


def unfilter(kind: int, row: bytes, above: bytes, back: int) -> bytes:
    if kind == 0:
        return row
    if kind == 2:
        return add_bytes(row, above)
    out = bytearray(row)
    size = len(out)
    if kind == 1:
        for at in range(back, size):
            out[at] = (out[at] + out[at - back]) & 255
    elif kind == 3:
        for at in range(size):
            left = out[at - back] if at >= back else 0
            out[at] = (out[at] + ((left + above[at]) >> 1)) & 255
    else:
        for at in range(size):
            left = out[at - back] if at >= back else 0
            up = above[at]
            corner = above[at - back] if at >= back else 0
            guess = left + up - corner
            to_left, to_up, to_corner = abs(guess - left), abs(guess - up), abs(guess - corner)
            if to_left <= to_up and to_left <= to_corner:
                predicted = left
            elif to_up <= to_corner:
                predicted = up
            else:
                predicted = corner
            out[at] = (out[at] + predicted) & 255
    return bytes(out)


def samples(row: bytes, depth: int, count: int) -> list[int]:
    """The first `count` samples of a row of samples of fewer than 8 bits."""
    per_byte = 8 // depth
    mask = (1 << depth) - 1
    shifts = [8 - depth * (i + 1) for i in range(per_byte)]
    values = [(byte >> shift) & mask for byte in row for shift in shifts]
    return values[:count]


class Picture:
    """What the second reading knows of a picture once its header is read."""

    def __init__(self, width: int, height: int, depth: int, colour: int):
        self.width, self.height, self.depth, self.colour = width, height, depth, colour
        self.stride = (width * SAMPLES[colour] * depth + 7) // 8
        self.back = max(1, SAMPLES[colour] * depth // 8)
        self.palette = bytearray(256)  # As grays, with their alphas taken in.
        self.entries = 0
        self.key: tuple[int, ...] | None = None

    def gray(self, row: bytes) -> bytes:
        width, depth, colour, key = self.width, self.depth, self.colour, self.key
        if colour == 0:
            if depth == 16:
                if key is None:
                    return row[0::2][:width]
                return bytes(255 if (row[2 * x] << 8 | row[2 * x + 1]) == key[0] else row[2 * x] for x in range(width))
            if depth == 8:
                if key is None:
                    return row[:width]
                table = bytearray(range(256))
                if key[0] < 256:
                    table[key[0]] = 255
                return row[:width].translate(table)
            stretch = 255 // ((1 << depth) - 1)
            table = bytes(255 if key is not None and value == key[0] else (value * stretch) & 255
                          for value in range(256))
            return bytes(samples(row, depth, width)).translate(table)
        if colour == 2:
            if depth == 16:
                out = bytearray()
                for x in range(width):
                    pixel = row[6 * x:6 * x + 6]
                    whole = struct.unpack(">3H", pixel)
                    out.append(255 if key is not None and whole == key else gray_of(pixel[0], pixel[2], pixel[4]))
                return bytes(out)
            if key is None:
                return bytes(gray_of(r, g, b) for r, g, b in zip(row[0::3], row[1::3], row[2::3]))
            return bytes(255 if (r, g, b) == key else gray_of(r, g, b) for r, g, b in zip(row[0::3], row[1::3], row[2::3]))
        if colour == 3:
            indices = row[:width] if depth == 8 else bytes(samples(row, depth, width))
            return indices.translate(bytes(self.palette))
        if colour == 4:
            step = 2 if depth == 8 else 4
            return bytes(over_white(row[step * x], row[step * x + step // 2]) for x in range(width))
        step = 4 if depth == 8 else 8
        one = step // 4
        return bytes(over_white(gray_of(row[step * x], row[step * x + one], row[step * x + 2 * one]),
                                row[step * x + 3 * one]) for x in range(width))


def read_png(data: bytes, *, rows_wanted: bool = True) -> Reading:
    """What the reader should make of `data`, by the rules its header states."""
    reading = Reading()
    if len(data) < 8 or data[:8] != SIGNATURE:
        reading.status = "not-png"
        return reading
    pos = 8
    picture: Picture | None = None
    seen_palette = False
    seen_transparency = False
    while True:
        if pos + 8 > len(data):
            reading.status = "truncated"
            return reading
        length, name = struct.unpack(">I4s", data[pos:pos + 8])
        if length > LONGEST_CHUNK or not is_name(name):
            reading.status = "bad-chunk" if picture else "bad-header"
            return reading
        if picture is None:
            if name != b"IHDR" or length != 13:
                reading.status = "bad-header"
                return reading
            kind = "header"
        elif name == b"IHDR":
            reading.status = "bad-chunk"
            return reading
        elif name == b"IDAT":
            if picture.colour == 3 and not seen_palette:
                reading.status = "no-palette"
                return reading
            return read_data(data, pos, picture, reading)
        elif name == b"IEND":
            reading.status = "truncated"
            return reading
        elif name == b"PLTE":
            if picture.colour != 3:
                kind = "other"
            elif seen_palette or length == 0 or length % 3 or length > 768:
                reading.status = "bad-chunk"
                return reading
            else:
                kind = "palette"
        elif name == b"tRNS":
            usable = not seen_transparency and (
                (picture.colour == 0 and length == 2) or (picture.colour == 2 and length == 6) or
                (picture.colour == 3 and seen_palette and length <= picture.entries))
            kind = "transparency" if usable else "other"
        elif 65 <= name[0] <= 90:
            reading.status = "unknown-chunk"
            return reading
        else:
            kind = "other"
        end = pos + 8 + length
        if end + 4 > len(data):
            reading.status = "truncated"
            return reading
        body = data[pos + 8:end]
        sound = zlib.crc32(name + body) == struct.unpack(">I", data[end:end + 4])[0]
        pos = end + 4
        if kind == "header":
            if not sound:
                reading.status = "bad-checksum"
                return reading
            width, height, depth, colour, compression, filtering, interlace = struct.unpack(">IIBBBBB", body)
            if (not 1 <= width <= LONGEST_CHUNK or not 1 <= height <= LONGEST_CHUNK or colour not in DEPTHS or
                    depth not in DEPTHS[colour] or compression or filtering or interlace > 1):
                reading.status = "bad-header"
                return reading
            picture = Picture(width, height, depth, colour)
            reading.header = (width, height, NAMES[colour], depth)
            reading.interlaced = interlace == 1
            reading.room = 2 * max(picture.stride + 1, width)
            reading.header_bytes = pos
            if not rows_wanted:
                return reading
            if interlace:
                reading.status = "interlaced"
                return reading
            if reading.room > MOST_ROOM:
                reading.status = "no-room"
                return reading
        elif kind == "palette":
            if not sound:
                reading.status = "bad-checksum"
                return reading
            picture.entries = length // 3
            for entry in range(picture.entries):
                picture.palette[entry] = gray_of(*body[3 * entry:3 * entry + 3])
            seen_palette = True
        elif kind == "transparency" and sound:
            seen_transparency = True
            if picture.colour == 3:
                for entry, alpha in enumerate(body):
                    picture.palette[entry] = over_white(picture.palette[entry], alpha)
            else:
                picture.key = struct.unpack(f">{length // 2}H", body)
        else:
            reading.passed += 1
            reading.bad += 0 if sound else 1


def read_data(data: bytes, pos: int, picture: Picture, reading: Reading) -> Reading:
    # The data of the IDAT chunks that follow one another, to where they end
    # or the file does, or to where a chunk goes wrong.
    stream = bytearray()
    failure = None
    length = struct.unpack(">I", data[pos:pos + 4])[0]
    empty = 1 if length == 0 else 0
    while True:
        start = pos + 8
        stream += data[start:start + length]
        if start + length > len(data):
            break
        seam = data[start + length:start + length + 12]
        if len(seam) >= 4 and zlib.crc32(b"IDAT" + data[start:start + length]) != struct.unpack(">I", seam[:4])[0]:
            failure = "bad-checksum"
            break
        if len(seam) < 12 or seam[8:12] != b"IDAT":
            break
        pos = start + length + 4  # The next chunk's start.
        length = struct.unpack(">I", seam[4:8])[0]
        if length > LONGEST_CHUNK:
            failure = "bad-chunk"
            break
        empty = 0 if length else empty + 1
        if empty > MOST_EMPTY_IN_A_ROW:
            failure = "bad-data"
            break
    return decompress(bytes(stream), failure, picture, reading)


def decompress(stream: bytes, failure: str | None, picture: Picture, reading: Reading) -> Reading:
    # Past the end of what was gathered, the reading fails as the chunk did,
    # or finds the data cut short. When it fails depends on how far ahead the
    # inflater has read, so a failure makes the reading not exact.
    ended = failure or "truncated"
    if failure:
        reading.exact = False
    if len(stream) < 2:
        reading.status = ended
        return reading
    method, flags = stream[0], stream[1]
    if (method & 15) != 8 or (method >> 4) > 7 or ((method << 8) | flags) % 31 or flags & 0x20:
        reading.status = "bad-data"
        return reading
    # DEFLATE, with nothing round it: the two bytes in front were read
    # above, and the Adler-32 after it is not looked at.
    inflater = zlib.decompressobj(-15)
    row_bytes = picture.stride + 1
    wanted = row_bytes * picture.height
    out = bytearray()
    at = 2
    zlib_failed = False
    try:
        while len(out) < wanted and not inflater.eof:
            if inflater.unconsumed_tail:
                piece = inflater.unconsumed_tail
            elif at < len(stream):
                piece = stream[at:at + 65536]
                at += len(piece)
            else:
                break
            out += inflater.decompress(piece, wanted - len(out))
    except zlib.error:
        # What came out of the last piece before the fault is lost here, and
        # not by the reader, which gives it: the reading is not exact.
        zlib_failed = True
        reading.exact = False
    above = bytes(picture.stride)
    for y in range(min(picture.height, len(out) // row_bytes)):
        kind = out[y * row_bytes]
        if kind > 4:
            reading.status = "bad-data"
            return reading
        row = unfilter(kind, bytes(out[y * row_bytes + 1:(y + 1) * row_bytes]), above, picture.back)
        reading.rows.append(picture.gray(row))
        above = row
    if len(reading.rows) == picture.height:
        reading.status = "ok"
    elif zlib_failed:
        reading.status = "bad-data"
    else:
        reading.status = ended
    return reading


# ---- Making pictures ----

def chunk(name: bytes, body: bytes, *, spoil: bool = False) -> bytes:
    crc = zlib.crc32(name + body) ^ (1 if spoil else 0)
    return struct.pack(">I", len(body)) + name + body + struct.pack(">I", crc)


def paeth(left: int, up: int, corner: int) -> int:
    guess = left + up - corner
    to_left, to_up, to_corner = abs(guess - left), abs(guess - up), abs(guess - corner)
    if to_left <= to_up and to_left <= to_corner:
        return left
    return up if to_up <= to_corner else corner


def filtered(rows: list[bytes], back: int, kinds: list[int]) -> bytes:
    """The rows as PNG stores them: each with its filter byte, filtered."""
    out = bytearray()
    above = bytes(len(rows[0]))
    for y, row in enumerate(rows):
        kind = kinds[y % len(kinds)]
        out.append(kind)
        for at, value in enumerate(row):
            left = row[at - back] if at >= back else 0
            corner = above[at - back] if at >= back else 0
            predicted = (0, left, above[at], (left + above[at]) >> 1, paeth(left, above[at], corner))[kind]
            out.append((value - predicted) & 255)
        above = row
    return bytes(out)


def data_chunks(stream: bytes, piece: int, empty: int) -> bytes:
    """`stream` in IDAT chunks of `piece` bytes, with `empty` empty ones between."""
    out = bytearray()
    at = 0
    while True:
        out += chunk(b"IDAT", stream[at:at + piece])
        at += piece
        if at >= len(stream):
            return bytes(out)
        for _ in range(empty):
            out += chunk(b"IDAT", b"")


def made_up(rng: random.Random) -> bytes:
    """A picture of a kind chosen at random, with what may come with it."""
    colour = rng.choice(list(DEPTHS))
    depth = rng.choice(DEPTHS[colour])
    width = rng.choice((1, 2, 3, 5, 8, 9, rng.randint(1, 70)))
    height = rng.randint(1, 24)
    stride = (width * SAMPLES[colour] * depth + 7) // 8
    rows = [rng.randbytes(stride) for _ in range(height)]
    if rng.random() < 0.3 and depth == 8:
        # A few values over and over, so that keys and palettes are met.
        rows = [bytes(rng.choice((0, 7, 200, 255)) for _ in range(stride)) for _ in range(height)]
    back = max(1, SAMPLES[colour] * depth // 8)
    before = bytearray()
    if rng.random() < 0.3:
        before += chunk(b"gAMA", struct.pack(">I", 45455))
    if rng.random() < 0.2:
        before += chunk(b"tEXt", b"Comment\x00made up", spoil=rng.random() < 0.5)
    if colour == 3:
        entries = rng.randint(1, 1 << depth)
        if rng.random() < 0.2:
            # Alphas before the palette: passed over.
            before += chunk(b"tRNS", bytes(rng.randint(0, 255) for _ in range(rng.randint(0, entries))))
        before += chunk(b"PLTE", rng.randbytes(3 * entries))
        if rng.random() < 0.6:
            count = rng.randint(0, entries + (1 if rng.random() < 0.2 else 0))
            before += chunk(b"tRNS", rng.randbytes(count), spoil=rng.random() < 0.1)
    elif colour in (0, 2) and rng.random() < 0.6:
        count = SAMPLES[colour]
        if rng.random() < 0.7:
            # The first pixel's samples, so that some pixel is "none".
            if depth < 8:
                key = samples(rows[0], depth, 1)
            elif depth == 8:
                key = list(rows[0][:count])
            else:
                key = list(struct.unpack(f">{count}H", rows[0][:2 * count]))
        else:
            key = [rng.randint(0, 65535) for _ in range(count)]
        body = struct.pack(f">{count}H", *key)
        if rng.random() < 0.1:
            body += b"\x00"
        before += chunk(b"tRNS", body)
        if rng.random() < 0.1:
            before += chunk(b"tRNS", body)
    elif colour in (2, 6) and rng.random() < 0.2:
        before += chunk(b"PLTE", rng.randbytes(3 * rng.randint(1, 4)))
    kinds = [rng.randint(0, 4) for _ in range(rng.randint(1, 5))]
    stream = zlib.compress(filtered(rows, back, kinds), rng.choice((0, 1, 6, 9)))
    piece = rng.choice((1, 3, 50, 1000, len(stream)))
    empty = rng.choice((0, 0, 0, 1, 2))
    header = chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, colour, 0, 0, 0))
    end = chunk(b"IEND", b"") if rng.random() < 0.9 else b""
    return SIGNATURE + header + bytes(before) + data_chunks(stream, piece, empty) + end


def chunks_of(data: bytes) -> list[tuple[int, int]]:
    """Where each whole chunk of a file is: its start, and its length."""
    found = []
    pos = 8
    while pos + 12 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        if pos + 12 + length > len(data):
            break
        found.append((pos, length))
        pos += 12 + length
    return found


def spoiled(rng: random.Random, data: bytes) -> bytes:
    """`data` with one thing wrong with it."""
    how = rng.randrange(4)
    places = chunks_of(data)
    if how == 0 or not places:
        return data[:rng.randrange(len(data))]
    pos, length = rng.choice(places)
    changed = bytearray(data)
    if how == 1 or length == 0:
        # A bit of the chunk's start or its body, its checksum not put right.
        at = pos + rng.randrange(8 + length)
        changed[at] ^= 1 << rng.randrange(8)
        return bytes(changed)
    # A byte of its body, with its checksum put right, so that the reader
    # goes on to what the byte says.
    at = pos + 8 + rng.randrange(length)
    changed[at] = rng.randrange(256) if how == 2 else changed[at] ^ (1 << rng.randrange(8))
    crc = zlib.crc32(bytes(changed[pos + 4:pos + 8 + length]))
    changed[pos + 8 + length:pos + 12 + length] = struct.pack(">I", crc)
    return bytes(changed)


# ---- Running bn-png ----

def run(*command: str) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, timeout=120, check=False)


def pgm_rows(path: Path, width: int) -> list[bytes]:
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    expect(parts[0] == b"P5" and parts[2] == b"255", f"{path}: not a PGM file as bn-png writes one")
    columns, count = (int(number) for number in parts[1].split())
    expect(columns == width or count == 0, f"{path}: {columns} wide, where the picture is {width}")
    return [parts[3][y * columns:(y + 1) * columns] for y in range(count)]


def check_one(tool: str, work: Path, name: str, data: bytes, *, pieces: str | None = None) -> Reading:
    path = work / f"{name}.png"
    path.write_bytes(data)
    expected = read_png(data)
    out = work / f"{name}.pgm"
    command = [tool, "read", str(path), "--out", str(out)]
    if pieces:
        command += ["--pieces", pieces]
    result = run(*command)
    said = result.stdout.decode("utf-8", "replace").strip()
    expect(result.returncode == (0 if said.startswith("ok ") else 1),
           f"{name}: ended with {result.returncode} after {said!r}: {result.stderr[-200:]!r}")
    if expected.exact:
        expect(said == expected.line(), f"{name}: bn-png says {said!r}; the second reading {expected.line()!r}; "
                                        f"and on its errors {result.stderr[-200:]!r}")
    else:
        # It may have read further, or not as far: the rows have to agree as
        # far as both go, and it can only call the picture whole if it gave
        # every row.
        width = expected.header[0] if expected.header else 0
        given = pgm_rows(out, width) if out.exists() and expected.header else []
        common = min(len(given), len(expected.rows))
        expect(given[:common] == expected.rows[:common],
               f"{name}: rows differ from the second reading's: {said!r} against {expected.line()!r}")
        expect(not said.startswith("ok ") or (expected.header is not None and len(given) == expected.header[1]),
               f"{name}: called whole with too few rows: {said!r}")
    if expected.exact and expected.header and out.exists():
        expect(pgm_rows(out, expected.header[0]) == expected.rows, f"{name}: --out wrote other rows")
    return expected


def check_header(tool: str, work: Path, name: str, data: bytes) -> None:
    path = work / f"{name}.png"
    path.write_bytes(data)
    expected = read_png(data, rows_wanted=False)
    result = run(tool, "header", str(path))
    said = result.stdout.decode("utf-8", "replace").strip()
    if expected.header is None:
        expect(said.startswith(f"{expected.status} - "), f"{name}: header says {said!r}; expected {expected.status}")
        return
    width, height, kind, depth = expected.header
    wanted = f"ok {width} {height} {kind} {depth} {1 if expected.interlaced else 0} {expected.header_bytes}"
    expect(said == wanted, f"{name}: header says {said!r}, where {wanted!r}")


def plain_line(old: list[float], size: int) -> list[float]:
    """One direction of the working-out in real numbers: `old` made `size` long."""
    count = len(old)
    out = []
    for at in range(size):
        if size == count:
            out.append(old[at])
        elif size < count:
            start, end = at * count / size, (at + 1) * count / size
            total = 0.0
            i = int(start)
            while i < count and i < end:
                total += old[i] * (min(i + 1, end) - max(i, start))
                i += 1
            out.append(total / (end - start))
        else:
            middle = (at + 0.5) * count / size - 0.5
            if middle <= 0:
                out.append(old[0])
            elif middle >= count - 1:
                out.append(old[-1])
            else:
                before = int(middle)
                out.append(old[before] * (before + 1 - middle) + old[before + 1] * (middle - before))
    return out


def plain_scale(rows: list[bytes], width: int, height: int) -> list[list[float]]:
    across = [plain_line([float(v) for v in row], width) for row in rows]
    columns = [plain_line([across[y][x] for y in range(len(rows))], height) for x in range(width)]
    return [[columns[x][y] for x in range(width)] for y in range(height)]


def check_scaled(tool: str, work: Path, name: str, data: bytes, width: int | None, height: int | None) -> None:
    path = work / f"{name}.png"
    path.write_bytes(data)
    expected = read_png(data)
    old_width, old_height = expected.header[0], expected.header[1]
    out = work / f"{name}-scaled.pgm"
    command = [tool, "read", str(path), "--out", str(out)]
    # Only the sides asked for: a side left out keeps the picture's shape.
    if width is not None:
        command += ["--width", str(width)]
    if height is not None:
        command += ["--height", str(height)]
    if width is None:
        width = max(1, (height * old_width * 2 + old_height) // (old_height * 2))
    if height is None:
        height = max(1, (width * old_height * 2 + old_width) // (old_width * 2))
    result = run(*command)
    said = result.stdout.decode("utf-8", "replace").strip()
    fields = said.split()
    expect(result.returncode == 0 and fields[0] == "ok" and fields[-3:] == ["scaled", str(width), str(height)] and
           fields[6] == str(height), f"{name}: scaled to {width}x{height}, bn-png says {said!r}: {result.stderr!r}")
    given = pgm_rows(out, width)
    plain = plain_scale(expected.rows, width, height)
    furthest = max(abs(given[y][x] - plain[y][x]) for y in range(height) for x in range(width))
    expect(furthest <= 1.0, f"{name}: scaled to {width}x{height}, a pixel is {furthest:.3f} grays from the "
                            "working-out in real numbers")


def check_book(tool: str, work: Path, pictures: list[bytes]) -> int:
    book = work / "book.epub"
    names = []
    with zipfile.ZipFile(book, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        archive.writestr("OEBPS/text.xhtml", "<html/>")
        for number, data in enumerate(pictures):
            name = f"OEBPS/images/p{number}.{'PNG' if number % 5 == 0 else 'png'}"
            archive.writestr(name, data, zipfile.ZIP_DEFLATED if number % 2 else zipfile.ZIP_STORED)
            names.append(name)
    result = run(tool, "book", str(book))
    lines = result.stdout.decode("utf-8", "replace").splitlines()
    expect(len(lines) == len(pictures) + 1, f"book: {len(lines)} lines for {len(pictures)} pictures")
    failed = 0
    most = 0
    for line, name, data in zip(lines, names, pictures):
        expected = read_png(data)
        failed += expected.status != "ok"
        most = max(most, expected.room)
        if expected.exact:
            expect(line == f"{expected.line()} {name}", f"book: {line!r}; the second reading {expected.line()!r}")
    expect(lines[-1].startswith(f"png_files={len(pictures)} png_failed={failed} ") and
           lines[-1].endswith(f" png_most_room={most}"), f"book: the last line is {lines[-1]!r}")
    expect(result.returncode == (1 if failed else 0), f"book: ended with {result.returncode}")
    return len(pictures)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tool = sys.argv[1]
    work = Path(sys.argv[2])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    counts: dict[str, int] = {}
    try:
        good = []
        for seed in range(400):
            rng = random.Random(seed)
            data = made_up(rng)
            reading = check_one(tool, work, f"made{seed}", data, pieces=rng.choice((None, None, "1", "7")))
            expect(reading.status == "ok", f"made{seed}: the second reading says {reading.status}")
            counts[reading.header[2]] = counts.get(reading.header[2], 0) + 1
            if seed % 4 == 0:
                check_header(tool, work, f"made{seed}", data)
            good.append(data)
        spoilt = 0
        not_exact = 0
        for seed in range(600):
            rng = random.Random(10000 + seed)
            data = spoiled(rng, rng.choice(good))
            reading = check_one(tool, work, f"spoiled{seed}", data)
            if seed % 6 == 0:
                check_header(tool, work, f"spoiled{seed}", data)
            spoilt += reading.status != "ok"
            not_exact += not reading.exact
        # Interlaced: the header is read, and the rows refused.
        interlaced = (SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 1)) +
                      data_chunks(zlib.compress(bytes(20)), 100, 0) + chunk(b"IEND", b""))
        check_one(tool, work, "interlaced", interlaced)
        check_header(tool, work, "interlaced", interlaced)
        # Less room for rows than the picture needs.
        expected = read_png(good[0])
        result = run(tool, "read", str(work / "made0.png"), "--rows", str(expected.room - 1))
        width, height, kind, depth = expected.header
        wanted = f"no-room 00000000 {width} {height} {kind} {depth} 0 {expected.room} 0 0"
        expect(result.stdout.decode().strip() == wanted, f"rows: {result.stdout!r}, where {wanted!r}")
        books = check_book(tool, work, good[:30] + [spoiled(random.Random(7), good[3]), b"not a picture"])
        # Scaled: smaller, larger, both at once, and one side asked for.
        scaled = 0
        for seed in range(0, 400, 5):
            rng = random.Random(20000 + seed)
            width = rng.choice((1, 3, rng.randint(1, 40), rng.randint(40, 120)))
            height = rng.choice((1, 2, rng.randint(1, 40), rng.randint(40, 90)))
            check_scaled(tool, work, f"made{seed}", good[seed], width, height)
            scaled += 1
        check_scaled(tool, work, "made1", good[1], 57, None)
        check_scaled(tool, work, "made2", good[2], None, 33)
    except Failure as failure:
        print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"png_tool: {len(good)} pictures ({', '.join(f'{n} {k}' for k, n in sorted(counts.items()))}) read as the "
          f"second reading reads them; {spoilt} spoiled ones refused as it says, {not_exact} of them where only the "
          f"rows could be compared; {books} pictures in a book; {scaled + 2} scaled within a gray of the working-out "
          "in real numbers.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
