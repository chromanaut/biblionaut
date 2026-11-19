#!/usr/bin/env python3
"""End-to-end check of bn-jpeg, and through it of the JPEG reader, run by CTest.

bn-jpeg is set beside a second reading of the same pictures that shares none
of its code: the file taken apart all at once by the rules the reader's
header states (draw/jpeg.h), its coded data decoded a symbol at a time, and
Y's blocks turned back into pixels. Read at an eighth of its size, a
picture is each block's average, worked out in whole numbers as the reader
does, and the two have to agree byte for byte. Read larger, the second
reading turns each block into pixels in real numbers by T.81's inverse
transform (at a half or a quarter, each pixel the average of those it
stands for), and every pixel bn-jpeg gives has to be within a gray of it.

Pictures are made up at random by an encoder written here: baseline and
extended (some with quantization tables of 16 bits), progressive, gray and
YCbCr sampled in each way books have them, sides that are not whole
blocks, restart markers, the standard tables and tables made for the
picture, a baseline picture in several scans (a colour one first, at
times), segments to be passed over, bytes between segments that are not
markers, and fill bytes before markers. For each:

  - `bn-jpeg read --reduce 8` has to say what the second reading says: how
    the reading ended, the checksum of the gray, the header, the reduction,
    the rows, the room they need, and the segments, scans and bytes passed
    over; given the file in pieces of one byte or of seven, the same;
  - read at its whole size, a half or a quarter, it has to say the same but
    for the checksum, and the rows it writes with --out have to be within a
    gray of the second reading's, and be the rows its checksum is of;
  - `bn-jpeg header` has to read the header, and no more of the file than
    the reader's buffer of 512 bytes holds past it.

Then the kinds the reader does not read, refused with their header read;
and the same pictures spoiled: cut short anywhere, a byte changed, a
restart marker wrong or left out, a table a scan needs left out. bn-jpeg
has to end as the second reading does, with the same rows before it.

And an EPUB-like ZIP file of pictures, read with `bn-jpeg book`; and some of
the good pictures made larger, smaller, or both at once through the
firmware's scaler (`--width`, `--height`), at the reduction the firmware
would read them at, against a plain scaling of the second reading's rows.

The second reading is also what tools/check-books.py sets beside bn-jpeg on
real books, at an eighth (read_jpeg).

usage: jpeg_tool.py BN_JPEG WORK_DIR
"""

from __future__ import annotations

import math
import random
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path

# Where each coefficient goes in its block, in the order they are coded.
NATURAL = (0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7,
           14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46,
           53, 60, 61, 54, 47, 55, 62, 63)
# The frame headers: every marker from 0xC0 to 0xCF but the Huffman tables,
# JPG and the arithmetic conditioning.
FRAMES = frozenset(range(0xC0, 0xD0)) - {0xC4, 0xC8, 0xCC}
PROGRESSIVE_FRAMES = frozenset((0xC2, 0xC6, 0xCA, 0xCE))
READ_FRAMES = frozenset((0xC0, 0xC1, 0xC2))
# What draw/jpeg.h states: the fill bytes allowed before a marker in coded
# data, the most a coefficient made whole and an average are held to, and
# how much the reader reads from its source at a time.
MOST_FILL = 16
MOST_COEFFICIENT = 16383
MOST_AVERAGE = 32767
INPUT_BYTES = 512
# The most memory bn-jpeg gives a reader for rows: a picture that needs more
# has no room.
MOST_ROOM = 268435456
KINDS = {(1, 1): "4:4:4", (2, 1): "4:2:2", (2, 2): "4:2:0", (1, 2): "4:4:0", (4, 1): "4:1:1"}


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def clamp(value: int, most: int) -> int:
    return -most if value < -most else most if value > most else value


# ---- The second reading ----

@dataclass
class Reading:
    status: str = "ok"
    header: tuple[int, int, str] | None = None  # Width, height, kind.
    reduction: int = 1  # The reduction taken.
    # The rows given: bytes at an eighth; lists of real numbers, each pixel
    # before it is rounded, at any other reduction.
    rows: list = field(default_factory=list)
    room: int = 0
    passed: int = 0
    scans: int = 0
    junk: int = 0
    header_bytes: int = 0  # How far into the file the header ends.
    frame: Header | None = None  # All the header says, once it is read.

    def crc(self) -> int:
        crc = 0
        for row in self.rows:
            crc = zlib.crc32(row, crc)
        return crc

    def line(self, *, with_crc: bool = True) -> str:
        """The line `bn-jpeg read` prints for this reading; with dashes for the checksum if it is not known."""
        crc = f"{self.crc():08x}" if with_crc or not self.rows else "--------"
        if self.header is None:
            return f"{self.status} {crc} - - - - 0 0 {self.passed} {self.scans} {self.junk}"
        width, height, kind = self.header
        return (f"{self.status} {crc} {width} {height} {kind} {self.reduction} {len(self.rows)} {self.room} "
                f"{self.passed} {self.scans} {self.junk}")


@dataclass
class Header:
    width: int
    height: int
    components: int
    progressive: bool
    gray: tuple[int, int]
    colour: tuple[int, int]

    def kind(self) -> str:
        if self.components == 1:
            name = "gray"
        elif self.components != 3:
            return "cmyk" if self.components == 4 else "other"
        elif self.colour == (1, 1) and self.gray in KINDS:
            name = KINDS[self.gray]
        else:
            name = "ycbcr"
        return f"progressive-{name}" if self.progressive else name

    def taken(self, asked: int) -> int:
        return 8 if self.progressive else asked

    def size_at(self, asked: int) -> tuple[int, int]:
        by = self.taken(asked)
        return (self.width + by - 1) // by, (self.height + by - 1) // by

    def room(self, asked: int) -> int:
        across, down = self.gray
        if self.width == 0 or self.height == 0 or across == 0 or down == 0:
            return 0
        side = 8 // self.taken(asked)
        if self.components == 1:
            return (self.width + 7) // 8 * side * side
        if self.components != 3 or self.colour[0] > across or self.colour[1] > down:
            return 0
        units = (self.width + 8 * across - 1) // (8 * across)
        return units * across * side * down * side


def reduction_for(header: Header, width: int, height: int) -> int:
    """The reduction the firmware reads a picture at to show it at a size."""
    if header.progressive:
        return 8
    for reduction in (8, 4, 2):
        across, down = header.size_at(reduction)
        if across >= width and down >= height:
            return reduction
    return 1


class Table:
    """A table of Huffman codes: what each 16 bits that may come next begin with, (length << 8) | value, or None."""

    def __init__(self, counts: bytes, values: bytes):
        self.lookup: list = [None] * 65536
        code = 0
        at = 0
        self.sound = True
        for length in range(1, 17):
            for _ in range(counts[length - 1]):
                first = code << (16 - length)
                span = 1 << (16 - length)
                if code < (1 << length):
                    self.lookup[first:first + span] = [(length << 8) | values[at]] * span
                code += 1
                at += 1
            # None of the codes may be all ones.
            if counts[length - 1] and code >= (1 << length):
                self.sound = False
                return
            code <<= 1


def _data_run(data: bytes, pos: int) -> tuple[bytes, int, int]:
    """A scan's coded data from `pos`: its bytes with the 0 after each 0xFF taken out; the marker it runs into, 0 for
    the end of the file, or 0xFF for more fill bytes than are allowed; and where what follows the marker begins."""
    out = bytearray()
    end = len(data)
    while True:
        ff = data.find(b"\xff", pos)
        if ff < 0:
            out += data[pos:]
            return bytes(out), 0, end
        out += data[pos:ff]
        pos = ff + 1
        following = 0xFF
        more = True
        fill = 0
        while following == 0xFF and more and fill <= MOST_FILL:
            if pos < end:
                following = data[pos]
                pos += 1
            else:
                more = False
            fill += 1
        if not more:
            return bytes(out), 0, pos
        if following != 0:
            return bytes(out), following, pos
        out.append(0xFF)


class Bits:
    """The bits of one stretch of coded data, 0 past its end."""

    __slots__ = ("data", "at", "real", "marker", "after")

    def __init__(self, data: bytes, pos: int):
        run, self.marker, self.after = _data_run(data, pos)
        self.real = len(run) * 8
        # Enough 0 bytes for a unit of blocks of the longest codes past the end.
        self.data = run + bytes(4096)
        self.at = 0

    def symbol(self, table: Table) -> int:
        at = self.at
        i = at >> 3
        window = int.from_bytes(self.data[i:i + 3], "big")
        entry = table.lookup[(window >> (8 - (at & 7))) & 0xFFFF]
        if entry is None:
            return -1
        self.at = at + (entry >> 8)
        return entry & 0xFF

    def value(self, count: int) -> int:
        """`count` bits, as T.81 F.2.2.1 makes a number of them."""
        if count == 0:
            return 0
        at = self.at
        i = at >> 3
        window = int.from_bytes(self.data[i:i + 3], "big")
        number = (window >> (24 - (at & 7) - count)) & ((1 << count) - 1)
        self.at = at + count
        if number < (1 << (count - 1)):
            number -= (1 << count) - 1
        return number


def _transform_table(side: int) -> list[list[float]]:
    """[u][X]: what frequency u gives pixel X of `side` across a block, the average over the pixels it stands for."""
    cells = 8 // side
    table = []
    for u in range(8):
        scale = (math.sqrt(0.5) if u == 0 else 1.0) / 2
        table.append([sum(scale * math.cos((2 * x + 1) * u * math.pi / 16) for x in range(X * cells, (X + 1) * cells))
                      / cells for X in range(side)])
    return table


TRANSFORMS = {side: _transform_table(side) for side in (8, 4, 2)}


class _Reader:
    """The second reading of one file."""

    def __init__(self, data: bytes, reduction: int, rows_wanted: bool, rows_given: int | None):
        self.data = data
        self.asked = reduction
        self.rows_wanted = rows_wanted
        self.rows_given = rows_given  # Memory for rows, if not what the picture needs.
        self.reading = Reading()
        self.pos = 2
        self.frame_seen = False
        self.adobe = -1
        self.restart = 0
        self.tables: dict[tuple[int, int], Table] = {}
        self.steps: dict[int, list[int]] = {}
        self.header: Header | None = None
        self.components: list[list[int]] = []  # [id, across, down, table] of the first four.

    def fail(self, status: str) -> Reading:
        self.reading.status = status
        return self.reading

    def take(self, count: int) -> bytes | None:
        if self.pos + count > len(self.data):
            return None
        piece = self.data[self.pos:self.pos + count]
        self.pos += count
        return piece

    def read(self) -> Reading:
        data = self.data
        if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:
            return self.fail("not-jpeg")
        saw_ff = False
        while True:
            # Between segments: anything until a marker.
            if self.pos >= len(data):
                return self.fail("truncated" if self.frame_seen else "bad-header")
            byte = data[self.pos]
            self.pos += 1
            if not saw_ff:
                if byte == 0xFF:
                    saw_ff = True
                else:
                    self.reading.junk += 1
                continue
            if byte == 0xFF:
                continue
            saw_ff = False
            if byte == 0:
                self.reading.junk += 2
                continue
            done = self.marker(byte)
            if done is not None:
                return done

    def marker(self, marker: int) -> Reading | None:
        """Takes a marker and what follows it. A reading when that ends the reading."""
        while True:
            if marker == 0xD8:
                return self.fail("bad-marker")
            if marker == 0xD9:
                return self.fail("truncated" if self.frame_seen else "bad-header")
            if marker == 0x01 or 0xD0 <= marker <= 0xD7:
                self.reading.passed += 1
                return None
            length = self.take(2)
            if length is None:
                return self.fail("truncated")
            left = (length[0] << 8 | length[1]) - 2
            if left < 0:
                return self.fail("bad-marker")
            if marker in FRAMES:
                return self.frame(marker, left)
            if marker == 0xC4:
                return self.huffman(left)
            if marker == 0xDB:
                return self.quantization(left)
            if marker == 0xDD:
                if left != 2:
                    return self.fail("bad-marker")
                body = self.take(2)
                if body is None:
                    return self.fail("truncated")
                self.restart = body[0] << 8 | body[1]
                return None
            if marker == 0xDA:
                if not self.frame_seen:
                    return self.fail("bad-header")
                result = self.scan(left)
                if isinstance(result, int):
                    marker = result  # The marker after a scan passed over.
                    continue
                return result
            if marker == 0xEE and not self.frame_seen:
                if left >= 12:
                    body = self.take(12)
                    if body is None:
                        return self.fail("truncated")
                    if body[:5] == b"Adobe":
                        self.adobe = body[11]
                    left -= 12
            else:
                self.reading.passed += 1
            if self.take(left) is None:
                return self.fail("truncated")
            return None

    def huffman(self, left: int) -> Reading | None:
        while left > 0:
            if left < 17:
                return self.fail("bad-table")
            head = self.take(17)
            if head is None:
                return self.fail("truncated")
            kind, number = head[0] >> 4, head[0] & 15
            if kind > 1 or number > 3:
                return self.fail("bad-table")
            total = sum(head[1:])
            if total > 256 or total > left - 17:
                return self.fail("bad-table")
            values = self.take(total)
            if values is None:
                return self.fail("truncated")
            table = Table(head[1:], values)
            if not table.sound:
                return self.fail("bad-table")
            self.tables[(kind, number)] = table
            left -= 17 + total
        return None

    def quantization(self, left: int) -> Reading | None:
        while left > 0:
            head = self.take(1)
            if head is None:
                return self.fail("truncated")
            precision, number = head[0] >> 4, head[0] & 15
            if precision > 1 or number > 3:
                return self.fail("bad-table")
            size = 64 if precision == 0 else 128
            if left - 1 < size:
                return self.fail("bad-table")
            body = self.take(size)
            if body is None:
                return self.fail("truncated")
            self.steps[number] = list(body) if precision == 0 else list(struct.unpack(">64H", body))
            left -= 1 + size
        return None

    def frame(self, marker: int, left: int) -> Reading | None:
        if self.frame_seen:
            return self.fail("bad-header")
        self.frame_seen = True
        if left < 6:
            return self.fail("bad-header")
        head = self.take(6)
        if head is None:
            return self.fail("truncated")
        precision, height, width, count = head[0], head[1] << 8 | head[2], head[3] << 8 | head[4], head[5]
        if count == 0 or left != 6 + 3 * count or width == 0:
            return self.fail("bad-header")
        for i in range(count):
            entry = self.take(3)
            if entry is None:
                return self.fail("truncated")
            across, down = entry[1] >> 4, entry[1] & 15
            if not 1 <= across <= 4 or not 1 <= down <= 4 or entry[2] > 3:
                return self.fail("bad-header")
            if i < 4:
                if any(component[0] == entry[0] for component in self.components):
                    return self.fail("bad-header")
                self.components.append([entry[0], across, down, entry[2], 0, 0])
        if precision not in (8, 12, 16):
            return self.fail("bad-header")
        first = self.components[0]
        colour = (self.components[1][1], self.components[1][2]) if count > 1 else (0, 0)
        self.header = header = Header(width, height, count, marker in PROGRESSIVE_FRAMES, (first[1], first[2]), colour)
        reading = self.reading
        reading.frame = header
        reading.header = (width, height, header.kind())
        reading.reduction = header.taken(self.asked)
        reading.room = header.room(self.asked)
        reading.header_bytes = self.pos
        unsupported = marker not in READ_FRAMES or precision != 8 or height == 0 or count not in (1, 3)
        if count == 3:
            ids = [component[0] for component in self.components]
            unsupported = (unsupported or self.adobe == 0 or ids == [ord("R"), ord("G"), ord("B")] or
                           any(component[1] > first[1] or component[2] > first[2] for component in self.components[1:]))
        if unsupported:
            return self.fail("unsupported")
        if not self.rows_wanted:
            return reading
        memory = reading.room if self.rows_given is None else self.rows_given
        if reading.room > memory or memory > MOST_ROOM:
            return self.fail("no-room")
        return None

    def scan(self, left: int) -> Reading | int | None:
        """Reads a scan's header, and the scan or past it. The marker after it if it is passed over."""
        if left < 1:
            return self.fail("bad-scan")
        head = self.take(1)
        if head is None:
            return self.fail("truncated")
        count = head[0]
        if not 1 <= count <= 4 or left != 4 + 2 * count:
            return self.fail("bad-scan")
        chosen: list[int] = []
        blocks = 0
        for _ in range(count):
            entry = self.take(2)
            if entry is None:
                return self.fail("truncated")
            found = [i for i, component in enumerate(self.components) if component[0] == entry[0]]
            if not found or entry[1] >> 4 > 3 or entry[1] & 15 > 3 or found[-1] in chosen:
                return self.fail("bad-scan")
            index = found[-1]
            chosen.append(index)
            self.components[index][4] = entry[1] >> 4
            self.components[index][5] = entry[1] & 15
            blocks += self.components[index][1] * self.components[index][2]
        tail = self.take(3)
        if tail is None:
            return self.fail("truncated")
        start, end, high, low = tail[0], tail[1], tail[2] >> 4, tail[2] & 15
        if count > 1 and blocks > 10:
            return self.fail("bad-scan")
        progressive = self.header.progressive
        if progressive and (start > end or end > 63 or (start == 0 and end != 0) or (start > 0 and count != 1) or
                            high > 13 or low > 13):
            return self.fail("bad-scan")
        if 0 not in chosen or (progressive and (start != 0 or high != 0)):
            self.reading.scans += 1
            return self.pass_scan()
        for index in chosen:
            component = self.components[index]
            if (0, component[4]) not in self.tables or (not progressive and (1, component[5]) not in self.tables):
                return self.fail("bad-scan")
        if self.components[0][3] not in self.steps:
            return self.fail("bad-scan")
        return self.rows(chosen, low if progressive else 0)

    def pass_scan(self) -> Reading | int:
        data = self.data
        saw_ff = False
        while True:
            if self.pos >= len(data):
                return self.fail("truncated")
            byte = data[self.pos]
            self.pos += 1
            if not saw_ff:
                saw_ff = byte == 0xFF
                continue
            if byte == 0xFF:
                continue
            saw_ff = False
            if byte == 0 or 0xD0 <= byte <= 0xD7:
                continue
            return byte

    def rows(self, chosen: list[int], successive: int) -> Reading:
        header = self.header
        reading = self.reading
        reduction = reading.reduction
        side = 8 // reduction
        progressive = header.progressive
        gray = self.components[0]
        interleaved = len(chosen) > 1
        if interleaved:
            across = (header.width + 8 * gray[1] - 1) // (8 * gray[1])
            width = across * gray[1] * side
            rows_per = gray[2] * side
        else:
            across = (header.width + 7) // 8
            width = across * side
            rows_per = side
        memory = reading.room if self.rows_given is None else self.rows_given
        if width * rows_per > memory:
            return self.fail("no-room")
        out_width, out_height = header.size_at(self.asked)
        steps = self.steps[gray[3]]
        transform = TRANSFORMS.get(side)
        # For each component of the scan: its index, its blocks across and down
        # in a unit, its tables, and its steps of quantization.
        parts = []
        for index in chosen:
            component = self.components[index]
            ac = self.tables.get((1, component[5])) if not progressive else None
            parts.append((index, component[1] if interleaved else 1, component[2] if interleaved else 1,
                          self.tables[(0, component[4])], ac))
        last = [0, 0, 0, 0]
        bits = Bits(self.data, self.pos)
        to_restart = self.restart
        next_restart = 0
        while True:
            buffer = [bytearray(width) if side == 1 else [0.0] * width for _ in range(rows_per)]
            for unit in range(across):
                if self.restart:
                    if to_restart == 0:
                        if bits.real - bits.at >= 8:
                            return self.fail("bad-data")
                        if bits.marker in (0, 0xD9):
                            return self.fail("truncated")
                        if bits.marker != 0xD0 + next_restart:
                            return self.fail("bad-data")
                        next_restart = (next_restart + 1) & 7
                        last = [0, 0, 0, 0]
                        bits = Bits(self.data, bits.after)
                        to_restart = self.restart
                    to_restart -= 1
                decoded = True
                for index, blocks_across, blocks_down, dc, ac in parts:
                    wanted = index == 0
                    for down in range(blocks_down):
                        for block_across in range(blocks_across):
                            size = bits.symbol(dc)
                            if size < 0 or size > 11:
                                decoded = False
                                break
                            last[index] = clamp(last[index] + bits.value(size), MOST_AVERAGE)
                            if wanted:
                                average = last[index] * (1 << successive) * steps[0]
                                coefficients = {0: clamp(average, MOST_COEFFICIENT)}
                            if ac is not None:
                                # The rest of the block, a code and its bits from one window of 40 bits; the
                                # bits only passed over where the coefficients are not wanted.
                                keep = wanted and side > 1
                                lookup = ac.lookup
                                coded = bits.data
                                at = bits.at
                                k = 1
                                while k < 64:
                                    offset = at & 7
                                    window = int.from_bytes(coded[at >> 3:(at >> 3) + 5], "big")
                                    entry = lookup[(window >> (24 - offset)) & 0xFFFF]
                                    if entry is None:
                                        decoded = False
                                        break
                                    length = entry >> 8
                                    run, size = (entry >> 4) & 15, entry & 15
                                    at += length
                                    if size == 0:
                                        if run != 15:
                                            break
                                        k += 16
                                        continue
                                    k += run
                                    if k > 63 or size > 10:
                                        decoded = False
                                        break
                                    if keep:
                                        number = (window >> (40 - offset - length - size)) & ((1 << size) - 1)
                                        if number < (1 << (size - 1)):
                                            number -= (1 << size) - 1
                                        coefficients[NATURAL[k]] = clamp(number * steps[k], MOST_COEFFICIENT)
                                    at += size
                                    k += 1
                                bits.at = at
                                if not decoded:
                                    break
                            if wanted:
                                place(buffer, coefficients, side, transform,
                                      (unit * blocks_across + block_across) * side, down * side)
                        if not decoded:
                            break
                    if not decoded:
                        break
                if bits.at > bits.real:
                    # Into the end of the file or of the picture: cut short.
                    return self.fail("truncated" if bits.marker in (0, 0xD9) else "bad-data")
                if not decoded:
                    return self.fail("bad-data")
            for row in buffer:
                if len(reading.rows) == out_height:
                    break
                reading.rows.append(bytes(row[:out_width]) if side == 1 else row[:out_width])
            if len(reading.rows) == out_height:
                return reading


def place(buffer: list, coefficients: dict[int, int], side: int, transform: list[list[float]] | None, left: int,
          top: int) -> None:
    """A block's pixels at their place in a row of blocks: at an eighth its average as the reader works it out, and
    otherwise in real numbers, before they are rounded."""
    if side == 1:
        buffer[top][left] = min(255, max(0, ((coefficients[0] + 4) >> 3) + 128))
        return
    # Across each row of frequencies, then down.
    rows: dict[int, list[float]] = {}
    for at, value in coefficients.items():
        partial = rows.get(at >> 3)
        if partial is None:
            partial = rows[at >> 3] = [0.0] * side
        across = transform[at & 7]
        for x in range(side):
            partial[x] += across[x] * value
    for y in range(side):
        row = buffer[top + y]
        for x in range(side):
            total = 128.0
            for v, partial in rows.items():
                total += transform[v][y] * partial[x]
            row[left + x] = min(255.0, max(0.0, total))


def read_jpeg(data: bytes, reduction: int = 1, *, rows_wanted: bool = True, rows_given: int | None = None) -> Reading:
    """What the reader should make of `data` read at a reduction, by the rules its header states. `rows_given` is the
    memory for rows, if not what the picture needs."""
    return _Reader(data, reduction, rows_wanted, rows_given).read()


# ---- Making pictures ----

# The tables of T.81 Annex K: quantization for Y and for colour, in their
# natural order, and the codes for each.
STANDARD_STEPS = (
    (16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29,
     51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121,
     120, 101, 72, 92, 95, 98, 112, 100, 103, 99),
    (17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99,
     99, 99, 99, 99) + (99,) * 32,
)
STANDARD_CODES = {
    (0, 0): (bytes((0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0)), bytes(range(12))),
    (0, 1): (bytes((0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0)), bytes(range(12))),
    (1, 0): (bytes((0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D)), bytes.fromhex(
        "01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738"
        "393a434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5"
        "a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa")),
    (1, 1): (bytes((0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77)), bytes.fromhex(
        "000102031104052131061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a262728292a3536"
        "3738393a434445464748494a535455565758595a636465666768696a737475767778797a82838485868788898a92939495969798999aa2"
        "a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9fa")),
}

FORWARD = [[(math.sqrt(0.5) if u == 0 else 1.0) / 2 * math.cos((2 * x + 1) * u * math.pi / 16) for x in range(8)]
           for u in range(8)]


def segment(marker: int, body: bytes) -> bytes:
    return bytes((0xFF, marker)) + struct.pack(">H", len(body) + 2) + body


def magnitude(value: int) -> tuple[int, int]:
    """How many bits a number takes, and the bits, as T.81 codes it."""
    size = abs(value).bit_length()
    return size, (value if value >= 0 else value + (1 << size) - 1)


def made_codes(counts: dict[int, int]) -> tuple[bytes, bytes]:
    """A table of codes for the symbols counted, as T.81 K.2 makes one: no code longer than 16 bits or all ones."""
    frequency = [0] * 257
    for symbol, number in counts.items():
        frequency[symbol] = number
    frequency[256] = 1  # So that no code is all ones.
    size = [0] * 257
    others = [-1] * 257
    while True:
        first = second = -1
        least = 1 << 62
        for i in range(257):
            if frequency[i] and frequency[i] <= least:
                least, first = frequency[i], i
        least = 1 << 62
        for i in range(257):
            if frequency[i] and frequency[i] <= least and i != first:
                least, second = frequency[i], i
        if second < 0:
            break
        frequency[first] += frequency[second]
        frequency[second] = 0
        size[first] += 1
        while others[first] >= 0:
            first = others[first]
            size[first] += 1
        others[first] = second
        size[second] += 1
        while others[second] >= 0:
            second = others[second]
            size[second] += 1
    bits = [0] * 33
    for i in range(257):
        if size[i]:
            bits[size[i]] += 1
    for i in range(32, 16, -1):
        while bits[i] > 0:
            j = i - 2
            while bits[j] == 0:
                j -= 1
            bits[i] -= 2
            bits[i - 1] += 1
            bits[j + 1] += 2
            bits[j] -= 1
    i = 16
    while bits[i] == 0:
        i -= 1
    bits[i] -= 1
    values = [symbol for length in range(1, 33) for symbol in range(256) if size[symbol] == length]
    return bytes(bits[1:17]), bytes(values)


def code_book(counts: bytes, values: bytes) -> dict[int, tuple[int, int]]:
    book = {}
    code = 0
    at = 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            book[values[at]] = (code, length)
            code += 1
            at += 1
        code <<= 1
    return book


class Writer:
    """Bits of coded data, each 0xFF followed by a 0."""

    def __init__(self):
        self.out = bytearray()
        self.bits = 0
        self.count = 0

    def put(self, value: int, count: int) -> None:
        self.bits = (self.bits << count) | (value & ((1 << count) - 1))
        self.count += count
        while self.count >= 8:
            self.count -= 8
            byte = (self.bits >> self.count) & 0xFF
            self.out.append(byte)
            if byte == 0xFF:
                self.out.append(0)
        self.bits &= (1 << self.count) - 1

    def pad(self) -> None:
        if self.count:
            self.put((1 << (8 - self.count)) - 1, 8 - self.count)


@dataclass
class Plan:
    """A picture to be made: its planes and how they are coded."""
    width: int
    height: int
    marker: int  # The frame's: 0xC0, 0xC1 or 0xC2.
    components: list[tuple[int, int, int, int]]  # Id, across, down, table of quantization.
    planes: list[list[bytes]]
    steps: dict[int, tuple[int, ...]]  # In their natural order.
    wide_steps: bool = False  # Quantization tables of 16 bits.
    made_tables: bool = False
    restart: int = 0
    # The scans, each its components (indices) and Ss, Se, Ah, Al.
    scans: list[tuple[list[int], int, int, int, int]] = field(default_factory=list)
    fill_before_restarts: int = 0
    end: bool = True


def block_of(plan: Plan, cache: list[dict], index: int, bx: int, by: int) -> list[int]:
    """A block of a component, quantized, in the order its coefficients are coded: worked out when first wanted,
    the plane's last row and column taken again past its edges."""
    if (bx, by) in cache[index]:
        return cache[index][(bx, by)]
    plane = plan.planes[index]
    height, width = len(plane), len(plane[0])
    pixels = [[plane[min(height - 1, by * 8 + y)][min(width - 1, bx * 8 + x)] - 128 for x in range(8)]
              for y in range(8)]
    if all(value == pixels[0][0] for row in pixels for value in row):
        # One gray: its average alone, which is what the transform would give.
        natural = [8.0 * pixels[0][0]] + [0.0] * 63
    else:
        # Across each row, then down each column.
        rows = [[sum(FORWARD[u][x] * row[x] for x in range(8)) for u in range(8)] for row in pixels]
        natural = [sum(FORWARD[v][y] * rows[y][u] for y in range(8)) for v in range(8) for u in range(8)]
    steps = plan.steps[plan.components[index][3]]
    out = []
    for k in range(64):
        value = natural[NATURAL[k]] / steps[NATURAL[k]]
        out.append(int(math.floor(abs(value) + 0.5)) * (1 if value >= 0 else -1))
    cache[index][(bx, by)] = out
    return out


def scan_units(plan: Plan, scan: tuple) -> list[list[tuple[int, int]]]:
    """The units of a scan, each its blocks: (component, block across, block down) in coding order."""
    chosen = scan[0]
    most_across = max(component[1] for component in plan.components)
    most_down = max(component[2] for component in plan.components)
    if len(chosen) == 1:
        index = chosen[0]
        _, across, down, _ = plan.components[index]
        width = (plan.width * across + most_across - 1) // most_across
        height = (plan.height * down + most_down - 1) // most_down
        return [[(index, bx, by)] for by in range((height + 7) // 8) for bx in range((width + 7) // 8)]
    units = []
    for my in range((plan.height + 8 * most_down - 1) // (8 * most_down)):
        for mx in range((plan.width + 8 * most_across - 1) // (8 * most_across)):
            unit = []
            for index in chosen:
                _, across, down, _ = plan.components[index]
                unit += [(index, mx * across + h, my * down + v) for v in range(down) for h in range(across)]
            units.append(unit)
    return units


def scan_symbols(plan: Plan, cache: list[dict], scan: tuple, tables: dict[int, tuple[int, int]]) -> list[list[tuple]]:
    """What a scan codes, unit by unit: (table, symbol, bits, count), the table None for bits by themselves.
    `tables` gives each component its (DC, AC) table numbers."""
    chosen, start, end, high, low = scan
    units = []
    last = {index: 0 for index in chosen}
    restart = plan.restart
    for number, unit in enumerate(scan_units(plan, scan)):
        if restart and number % restart == 0:
            last = {index: 0 for index in chosen}
        coded = []
        for index, bx, by in unit:
            block = block_of(plan, cache, index, bx, by)
            dc_table, ac_table = tables[index]
            if start == 0 and high == 0:
                average = block[0] >> low
                size, bits = magnitude(average - last[index])
                last[index] = average
                coded.append(((0, dc_table), size, bits, size))
                if plan.marker == 0xC2:
                    continue
                run = 0
                for k in range(1, 64):
                    if block[k] == 0:
                        run += 1
                        continue
                    while run > 15:
                        coded.append(((1, ac_table), 0xF0, 0, 0))
                        run -= 16
                    size, bits = magnitude(block[k])
                    coded.append(((1, ac_table), (run << 4) | size, bits, size))
                    run = 0
                if run:
                    coded.append(((1, ac_table), 0x00, 0, 0))
            elif start == 0:
                coded.append((None, 0, (block[0] >> low) & 1, 1))
            else:
                run = 0
                for k in range(start, end + 1):
                    value = int(block[k] / (1 << low))
                    if value == 0:
                        run += 1
                        continue
                    while run > 15:
                        coded.append(((1, ac_table), 0xF0, 0, 0))
                        run -= 16
                    size, bits = magnitude(value)
                    coded.append(((1, ac_table), (run << 4) | size, bits, size))
                    run = 0
                if run:
                    coded.append(((1, ac_table), 0x00, 0, 0))
        units.append(coded)
    return units


def encode(plan: Plan, rng: random.Random, *, extras: bool = True) -> bytes:
    """The file a plan comes to, with what may come with it if `extras`."""
    cache: list[dict] = [{} for _ in plan.components]
    # Y's tables are 0 and the colour's 1, or now and then the other way round.
    swapped = rng.random() < 0.2
    tables = {index: ((1, 1) if (index > 0) != swapped else (0, 0)) for index in range(len(plan.components))}
    if plan.marker != 0xC0 and rng.random() < 0.3:
        tables = {index: (2 + index % 2, 3 - index % 2) for index in range(len(plan.components))}
    scans = [(scan, scan_symbols(plan, cache, scan, tables)) for scan in plan.scans]

    out = bytearray(b"\xff\xd8")

    def between() -> None:
        """What may lie before a marker: bytes that are not one, and fill bytes."""
        if extras and rng.random() < 0.1:
            out.extend(bytes(rng.choice((0, 1, 0x7F, 0xFE)) for _ in range(rng.randint(1, 4))))
        if extras and rng.random() < 0.05:
            out.extend(b"\xff\x00")
        if extras and rng.random() < 0.1:
            out.extend(b"\xff" * rng.randint(1, 20))

    def add(marker: int, body: bytes) -> None:
        between()
        out.extend(segment(marker, body))

    if extras and rng.random() < 0.6:
        add(0xE0, b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")
    if extras and rng.random() < 0.2:
        add(0xE1, b"Exif\x00\x00" + rng.randbytes(rng.randint(0, 40)))
    if extras and rng.random() < 0.1 and len(plan.components) == 3:
        add(0xEE, b"Adobe\x00\x64\x00\x00\x00\x00\x01")
    if extras and rng.random() < 0.1:
        add(0xFE, b"made up for a test")
    if extras and rng.random() < 0.05:
        between()
        out.extend(b"\xff\x01")
    precision = 1 if plan.wide_steps else 0
    body = bytearray()
    for number, steps in sorted(plan.steps.items()):
        body.append(precision << 4 | number)
        zigzag = [steps[NATURAL[k]] for k in range(64)]
        body += bytes(zigzag) if precision == 0 else struct.pack(">64H", *zigzag)
    add(0xDB, bytes(body))
    frame = bytearray(struct.pack(">BHHB", 8, plan.height, plan.width, len(plan.components)))
    for identity, across, down, table in plan.components:
        frame += bytes((identity, across << 4 | down, table))
    add(plan.marker, bytes(frame))
    if extras and rng.random() < 0.1:
        add(0xEE, b"Adobe\x00\x64\x00\x00\x00\x00\x00")  # After the frame, where it is not looked at.
    if plan.restart or (extras and rng.random() < 0.05):
        add(0xDD, struct.pack(">H", plan.restart))
    defined: dict[tuple[int, int], tuple[bytes, bytes]] = {}
    for scan, units in scans:
        wanted: dict[tuple[int, int], dict[int, int]] = {}
        for unit in units:
            for table, symbol, _, _ in unit:
                if table is not None:
                    wanted.setdefault(table, {})
                    wanted[table][symbol] = wanted[table].get(symbol, 0) + 1
        body = bytearray()
        for table, counts in sorted(wanted.items()):
            if plan.made_tables:
                codes = made_codes(counts)
            else:
                codes = STANDARD_CODES[(table[0], table[1] % 2)]
            if defined.get(table) != codes:
                body += bytes((table[0] << 4 | table[1],)) + codes[0] + codes[1]
                defined[table] = codes
        if body:
            add(0xC4, bytes(body))
        chosen, start, end, high, low = scan
        header = bytearray((len(chosen),))
        for index in chosen:
            dc_table, ac_table = tables[index]
            header += bytes((plan.components[index][0], dc_table << 4 | ac_table))
        header += bytes((start, end, high << 4 | low))
        add(0xDA, bytes(header))
        books = {table: code_book(*codes) for table, codes in defined.items()}
        writer = Writer()
        for number, unit in enumerate(units):
            if plan.restart and number and number % plan.restart == 0:
                writer.pad()
                writer.out += b"\xff" * (1 + plan.fill_before_restarts) + bytes((0xD0 + (number // plan.restart - 1) % 8,))
            for table, symbol, bits, count in unit:
                if table is not None:
                    code, length = books[table][symbol]
                    writer.put(code, length)
                writer.put(bits, count)
        writer.pad()
        out += writer.out
    if plan.end:
        between()
        out += b"\xff\xd9"
    return bytes(out)


def made_plane(rng: random.Random, width: int, height: int) -> list[bytes]:
    """Pixels of one kind or another: smooth, noisy, hard-edged, or one gray."""
    kind = rng.randrange(5)
    if kind == 0:
        dx, dy, base = rng.uniform(-6, 6), rng.uniform(-6, 6), rng.randint(0, 255)
        return [bytes(min(255, max(0, int(base + dx * x + dy * y + rng.randint(-3, 3)))) for x in range(width))
                for y in range(height)]
    if kind == 1:
        return [rng.randbytes(width) for _ in range(height)]
    if kind == 2:
        size = rng.choice((1, 3, 5, 8))
        return [bytes(255 if ((x // size) + (y // size)) % 2 else 0 for x in range(width)) for y in range(height)]
    if kind == 3:
        value = rng.randint(0, 255)
        return [bytes([value]) * width for _ in range(height)]
    return [bytes(int(127.5 + 127.5 * math.sin(x / rng.uniform(1, 9)) * math.cos(y / 4)) for x in range(width))
            for y in range(height)]


def made_steps(rng: random.Random, which: int, wide: bool) -> tuple[int, ...]:
    quality = rng.choice((5, 25, 50, 75, 90, 100, rng.randint(1, 100)))
    scale = 5000 // quality if quality < 50 else 200 - 2 * quality
    most = 2000 if wide else 255
    steps = [min(most, max(1, (base * scale + 50) // 100)) for base in STANDARD_STEPS[which]]
    if wide and rng.random() < 0.5:
        steps = [min(most, step * rng.randint(1, 8)) for step in steps]
    return tuple(steps)


def made_plan(rng: random.Random) -> Plan:
    """A picture of a kind chosen at random."""
    marker = rng.choice((0xC0, 0xC0, 0xC1, 0xC2, 0xC2))
    gray = rng.random() < 0.3
    across, down = rng.choice(((1, 1), (2, 1), (2, 2), (1, 2), (4, 1))) if not gray else rng.choice(((1, 1), (2, 2)))
    width = rng.choice((1, 7, 8, 9, 16, 17, rng.randint(1, 72)))
    height = rng.choice((1, 8, 15, rng.randint(1, 40)))
    if gray:
        components = [(1, across, down, 0)]
    else:
        ids = (1, 2, 3) if rng.random() < 0.8 else (0, 1, 2)
        components = [(ids[0], across, down, 0), (ids[1], 1, 1, 1), (ids[2], 1, 1, 1)]
    planes = []
    for _, h, v, _ in components:
        planes.append(made_plane(rng, (width * h + across - 1) // across, (height * v + down - 1) // down))
    wide = marker != 0xC0 and rng.random() < 0.4
    steps = {0: made_steps(rng, 0, wide)}
    if not gray:
        steps[1] = made_steps(rng, 1, wide)
    plan = Plan(width, height, marker, components, planes, steps, wide_steps=wide, made_tables=rng.random() < 0.5,
                restart=rng.choice((0, 0, 0, 1, 2, 3, 5)), end=rng.random() < 0.9)
    plan.fill_before_restarts = rng.choice((0, 0, 0, 0, 1, 16))
    every = list(range(len(components)))
    if marker == 0xC2:
        low = rng.choice((0, 1))
        if gray or rng.random() < 0.5:
            plan.scans = [([0], 0, 0, 0, low)] + [([index], 0, 0, 0, low) for index in every[1:]]
            if not gray and rng.random() < 0.3:
                plan.scans = [plan.scans[1], plan.scans[0]] + plan.scans[2:]
        else:
            plan.scans = [(every, 0, 0, 0, low)]
        plan.scans += [([0], 1, 5, 0, 0)] + [([index], 1, 63, 0, 0) for index in every[1:]] + [([0], 6, 63, 0, 0)]
        if low:
            plan.scans.append((every, 0, 0, 1, 0))
    elif gray or rng.random() < 0.7:
        plan.scans = [(every, 0, 63, 0, 0)]
    else:
        order = every[:]
        rng.shuffle(order)
        plan.scans = [([index], 0, 63, 0, 0) for index in order]
    return plan


def made_up(rng: random.Random) -> bytes:
    return encode(made_plan(rng), rng)


def refused(rng: random.Random) -> list[tuple[str, bytes]]:
    """Pictures of the kinds the reader does not read, each made from a picture it does by changing its header."""
    plan = made_plan(random.Random(rng.randrange(1 << 30)))
    while len(plan.components) != 3 or plan.marker == 0xC2:
        plan = made_plan(random.Random(rng.randrange(1 << 30)))
    sound = encode(plan, rng, extras=False)
    frame_at, marker, length = next(place for place in segments_of(sound) if place[1] in (0xC0, 0xC1))
    frame = bytearray(sound[frame_at + 4:frame_at + 2 + length])
    before, after = sound[:frame_at], sound[frame_at + 2 + length:]

    def with_frame(marker: int, body: bytes, *, ahead: bytes = b"") -> bytes:
        return before[:2] + ahead + before[2:] + segment(marker, body) + after

    made = [("adobe-rgb", with_frame(marker, bytes(frame), ahead=segment(0xEE, b"Adobe\x00\x64\x00\x00\x00\x00\x00")))]
    named = bytearray(frame)
    named[6], named[9], named[12] = ord("R"), ord("G"), ord("B")
    made.append(("named-rgb", with_frame(0xC0, bytes(named))))
    cmyk = bytearray(frame)
    cmyk[5] = 4
    cmyk += bytes((4, 0x11, 1))
    made.append(("cmyk", with_frame(0xC0, bytes(cmyk))))
    twelve = bytearray(frame)
    twelve[0] = 12
    made.append(("twelve-bit", with_frame(0xC1, bytes(twelve))))
    for marker in (0xC3, 0xC5, 0xC9, 0xCA, 0xCF):
        made.append((f"frame-{marker:x}", with_frame(marker, bytes(frame))))
    later = bytearray(frame)
    later[1:3] = b"\x00\x00"
    made.append(("height-later", with_frame(0xC0, bytes(later))))
    less = bytearray(frame)
    less[7], less[10] = 0x11, 0x22
    made.append(("gray-sampled-less", with_frame(0xC0, bytes(less))))
    two = bytearray(frame[:12])
    two[5] = 2
    made.append(("two-components", with_frame(0xC0, bytes(two))))
    return made


def segments_of(data: bytes) -> list[tuple[int, int, int]]:
    """Where each marker segment of a file is, up to its first scan's data: start, marker, length."""
    found = []
    at = 2
    while at + 4 <= len(data):
        if data[at] != 0xFF or data[at + 1] in (0xFF, 0x00, 0x01) or 0xD0 <= data[at + 1] <= 0xD9:
            at += 1
            continue
        length = struct.unpack(">H", data[at + 2:at + 4])[0]
        found.append((at, data[at + 1], length))
        at += 2 + length
    return found


def spoiled(rng: random.Random, data: bytes) -> bytes:
    """`data` with one thing wrong with it: most often in the coded data, where rows have been given before it."""
    how = rng.randrange(7)
    places = segments_of(data)
    scans = [at + 2 + length for at, marker, length in places if marker == 0xDA]
    coded = scans[0] if scans else 2
    changed = bytearray(data)
    if how == 0:
        return data[:rng.randrange(len(data))]
    if how in (1, 2) and coded < len(data):
        at = rng.randrange(coded, len(data))
        if how == 1:
            return data[:at]
        changed[at] = rng.randrange(256) if rng.random() < 0.5 else changed[at] ^ (1 << rng.randrange(8))
        return bytes(changed)
    restarts = [at for at in range(coded, len(data) - 1) if data[at] == 0xFF and 0xD0 <= data[at + 1] <= 0xD7]
    if how == 3 and restarts:
        at = rng.choice(restarts)
        if rng.random() < 0.5:
            changed[at + 1] = 0xD0 + (changed[at + 1] - 0xD0 + rng.randint(1, 7)) % 8
            return bytes(changed)
        return bytes(changed[:at] + changed[at + 2:])
    tables = [place for place in places if place[1] in (0xC4, 0xDB)]
    if how == 4 and tables:
        at, _, length = rng.choice(tables)
        return bytes(changed[:at] + changed[at + 2 + length:])
    if how == 5 or not places:
        at = rng.randrange(len(data))
        changed[at] ^= 1 << rng.randrange(8)
        return bytes(changed)
    at, _, length = rng.choice(places)
    changed[at + rng.randrange(2 + length)] = rng.randrange(256)
    return bytes(changed)


# ---- Running bn-jpeg ----

def run(*command: str) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, timeout=120, check=False)


def pgm_rows(path: Path, width: int) -> list[bytes]:
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    expect(parts[0] == b"P5" and parts[2] == b"255", f"{path}: not a PGM file as bn-jpeg writes one")
    columns, count = (int(number) for number in parts[1].split())
    expect(columns == width or count == 0, f"{path}: {columns} wide, where the picture is {width}")
    return [parts[3][y * columns:(y + 1) * columns] for y in range(count)]


def within_a_gray(given: list[bytes], plain: list[list[float]], what: str) -> None:
    expect(len(given) == len(plain), f"{what}: {len(given)} rows, where the second reading has {len(plain)}")
    for y, (row, wanted) in enumerate(zip(given, plain)):
        expect(len(row) == len(wanted), f"{what}: row {y} is {len(row)} wide, where it should be {len(wanted)}")
        for x, (pixel, value) in enumerate(zip(row, wanted)):
            expect(abs(pixel - value) <= 1.0, f"{what}: pixel {x},{y} is {pixel}, where the transform in real "
                                              f"numbers has {value:.3f}")


def check_one(tool: str, work: Path, name: str, data: bytes, reduction: int, *, pieces: str | None = None) -> Reading:
    path = work / f"{name}.jpg"
    path.write_bytes(data)
    expected = read_jpeg(data, reduction)
    out = work / f"{name}.pgm"
    command = [tool, "read", str(path), "--reduce", str(reduction), "--out", str(out)]
    if pieces:
        command += ["--pieces", pieces]
    result = run(*command)
    said = result.stdout.decode("utf-8", "replace").strip()
    expect(result.returncode == (0 if said.startswith("ok ") else 1),
           f"{name}: ended with {result.returncode} after {said!r}: {result.stderr[-200:]!r}")
    exact = expected.reduction == 8 or not expected.rows
    if exact:
        expect(said == expected.line(), f"{name} at 1/{reduction}: bn-jpeg says {said!r}; the second reading "
                                        f"{expected.line()!r}; and on its errors {result.stderr[-200:]!r}")
    else:
        fields = said.split()
        expect(len(fields) == 11 and " ".join(fields[:1] + ["--------"] + fields[2:]) == expected.line(with_crc=False),
               f"{name} at 1/{reduction}: bn-jpeg says {said!r}; the second reading {expected.line(with_crc=False)!r}")
    if expected.header is not None and out.exists():
        width = (expected.header[0] + expected.reduction - 1) // expected.reduction
        given = pgm_rows(out, width)
        crc = 0
        for row in given:
            crc = zlib.crc32(row, crc)
        expect(said.split()[1] == f"{crc:08x}", f"{name}: --out wrote rows other than the checksum is of")
        if exact:
            expect(given == expected.rows, f"{name}: --out wrote other rows")
        else:
            within_a_gray(given, expected.rows, f"{name} at 1/{reduction}")
    return expected


def check_header(tool: str, work: Path, name: str, data: bytes) -> None:
    path = work / f"{name}.jpg"
    path.write_bytes(data)
    expected = read_jpeg(data, rows_wanted=False)
    result = run(tool, "header", str(path))
    said = result.stdout.decode("utf-8", "replace").strip()
    if expected.header is None:
        expect(said.startswith(f"{expected.status} - - - "), f"{name}: header says {said!r}; expected "
                                                               f"{expected.status}")
        return
    width, height, kind = expected.header
    taken = min(len(data), (expected.header_bytes + INPUT_BYTES - 1) // INPUT_BYTES * INPUT_BYTES)
    wanted = f"{expected.status} {width} {height} {kind} {taken}"
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


def plain_scale(rows: list, width: int, height: int) -> list[list[float]]:
    across = [plain_line([float(v) for v in row], width) for row in rows]
    columns = [plain_line([across[y][x] for y in range(len(rows))], height) for x in range(width)]
    return [[columns[x][y] for x in range(width)] for y in range(height)]


def check_scaled(tool: str, work: Path, name: str, data: bytes, width: int | None, height: int | None) -> None:
    path = work / f"{name}.jpg"
    path.write_bytes(data)
    header = _Reader(data, 1, False, None)
    header.read()
    old_width, old_height = header.header.width, header.header.height
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
    reduction = reduction_for(header.header, width, height)
    expected = read_jpeg(data, reduction)
    result = run(*command)
    said = result.stdout.decode("utf-8", "replace").strip()
    fields = said.split()
    expect(result.returncode == 0 and fields[0] == "ok" and fields[-3:] == ["scaled", str(width), str(height)] and
           fields[5] == str(expected.reduction) and fields[6] == str(height),
           f"{name}: scaled to {width}x{height}, read at 1/{expected.reduction}, bn-jpeg says {said!r}: "
           f"{result.stderr!r}")
    given = pgm_rows(out, width)
    plain = plain_scale(expected.rows, width, height)
    furthest = max(abs(given[y][x] - plain[y][x]) for y in range(height) for x in range(width))
    # Rounded once more when the rows are not the reader's exactly.
    most = 1.0 if expected.reduction == 8 else 2.0
    expect(furthest <= most, f"{name}: scaled to {width}x{height}, a pixel is {furthest:.3f} grays from the "
                             "working-out in real numbers")


def check_book(tool: str, work: Path, pictures: list[bytes]) -> int:
    book = work / "book.epub"
    names = []
    with zipfile.ZipFile(book, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        archive.writestr("OEBPS/text.xhtml", "<html/>")
        archive.writestr("OEBPS/images/not-a-jpeg.png", b"\x89PNG")
        for number, data in enumerate(pictures):
            name = f"OEBPS/images/p{number}.{('JPG', 'jpeg', 'jpg')[number % 3]}"
            archive.writestr(name, data, zipfile.ZIP_DEFLATED if number % 2 else zipfile.ZIP_STORED)
            names.append(name)
    for reduction in (8, 1):
        command = [tool, "book", str(book)] + (["--reduce", "8"] if reduction == 8 else [])
        result = run(*command)
        lines = result.stdout.decode("utf-8", "replace").splitlines()
        expect(len(lines) == len(pictures) + 1, f"book: {len(lines)} lines for {len(pictures)} pictures")
        failed = 0
        most = 0
        pixels = 0
        for line, name, data in zip(lines, names, pictures):
            expected = read_jpeg(data, reduction)
            failed += expected.status != "ok"
            if expected.header is not None:
                most = max(most, expected.room)
                pixels += expected.header[0] * expected.header[1]
            if reduction == 8 or not expected.rows:
                expect(line == f"{expected.line()} {name}", f"book: {line!r}; the second reading {expected.line()!r}")
            else:
                fields = line.split(" ", 11)
                expect(" ".join(fields[:1] + ["--------"] + fields[2:]) == f"{expected.line(with_crc=False)} {name}",
                       f"book: {line!r}; the second reading {expected.line(with_crc=False)!r}")
        total = sum(len(data) for data in pictures)
        expect(lines[-1] == f"jpeg_files={len(pictures)} jpeg_failed={failed} jpeg_bytes={total} "
                            f"jpeg_pixels={pixels} jpeg_most_room={most}", f"book: the last line is {lines[-1]!r}")
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
        for seed in range(160):
            rng = random.Random(seed)
            data = made_up(rng)
            reading = check_one(tool, work, f"made{seed}", data, 8, pieces=rng.choice((None, None, "1", "7")))
            expect(reading.status == "ok", f"made{seed}: the second reading says {reading.status}")
            counts[reading.header[2]] = counts.get(reading.header[2], 0) + 1
            if not reading.header[2].startswith("progressive"):
                check_one(tool, work, f"made{seed}", data, rng.choice((1, 2, 4)))
            if seed % 4 == 0:
                check_header(tool, work, f"made{seed}", data)
            good.append(data)
        unread = 0
        for name, data in refused(random.Random(5)):
            reading = check_one(tool, work, name, data, 8)
            expect(reading.status == "unsupported", f"{name}: the second reading says {reading.status}")
            check_header(tool, work, name, data)
            unread += 1
        spoilt = 0
        for seed in range(300):
            rng = random.Random(10000 + seed)
            data = spoiled(rng, rng.choice(good))
            reading = check_one(tool, work, f"spoiled{seed}", data, 8 if seed % 3 else rng.choice((1, 2, 4)))
            if seed % 6 == 0:
                check_header(tool, work, f"spoiled{seed}", data)
            spoilt += reading.status != "ok"
        # Less room for rows than the picture needs.
        roomy = next(number for number, data in enumerate(good) if read_jpeg(data, 8).room > 1)
        expected = read_jpeg(good[roomy], 8)
        result = run(tool, "read", str(work / f"made{roomy}.jpg"), "--reduce", "8", "--rows", str(expected.room - 1))
        wanted = read_jpeg(good[roomy], 8, rows_given=expected.room - 1).line()
        expect(result.stdout.decode().strip() == wanted and wanted.startswith("no-room 00000000 "),
               f"rows: {result.stdout!r}, where {wanted!r}")
        books = check_book(tool, work, good[:24] + [spoiled(random.Random(7), good[3]), b"not a picture"])
        # Scaled: smaller, larger, both at once, and one side asked for.
        scaled = 0
        for seed in range(0, 160, 4):
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
    print(f"jpeg_tool: {len(good)} pictures ({', '.join(f'{n} {k}' for k, n in sorted(counts.items()))}) read as the "
          f"second reading reads them; {unread} of kinds not read refused with their header read; {spoilt} spoiled "
          f"ones refused as it says; {books} pictures in a book; {scaled + 2} scaled within a gray or two of the "
          "working-out in real numbers.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
