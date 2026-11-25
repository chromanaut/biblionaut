#!/usr/bin/env python3
"""End-to-end check of bn-font, and through it of the font builder and the reader of the fonts image, run by CTest.

The unit tests say what the builder and the reader should make of presets
and images written for them. Here bn-font is set beside a second reading
that shares none of its code, of a real font: Courier Prime, which the
build downloads.

  - `bn-font build` has to make an image of a preset, and say what went
    into each face.
  - The image is taken apart here, from the description of its layout: its
    header and faces, each face's ranges, glyphs and pictures, all of which
    have to lie inside it, in order and apart.
  - The font file is taken apart here too, as far as its tables of
    characters (`cmap`) and of widths (`hmtx`): a character has a glyph in
    the image exactly when the font has one for it, and the glyph moves the
    pen as far as the font's width for it comes to at the face's size, in
    quarters of a pixel.
  - `bn-font info --glyphs`, which reads the image by the code the firmware
    reads it with, has to say of every character what the image's bytes say
    here: its glyph, its advance, its picture's size and place, and its ink
    added up.

Nothing here draws a glyph a second way: that its picture is the letter is
for whoever looks at it. What is checked of the pictures is what must be so
of any: a space has none, a letter has ink, and the bold has more of it.

It also checks that what is not a preset, not a font and not an image are
refused.

usage: font_tool.py BN_FONT FONT_DIR WORK_DIR
"""

from __future__ import annotations

import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

SIZES = (13, 22, 37)
# ASCII, Latin-1, some Greek, the dashes and quotation marks, and the private
# use area, which no text font has anything in.
CHARACTERS = ((0x20, 0x7E), (0xA0, 0xFF), (0x391, 0x3A9), (0x2010, 0x2026), (0xE000, 0xE003))
FILES = (("regular", 0, "CourierPrime-Regular.ttf"), ("bold", 1, "CourierPrime-Bold.ttf"))


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def run(tool: str, *arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run([tool, *arguments], capture_output=True, check=False)


# ---- The font file, as far as its characters and their widths ----


class FontFile:
    """A TrueType file's tables of characters and widths, read the plain way."""

    def __init__(self, path: Path):
        data = path.read_bytes()
        count = struct.unpack_from(">H", data, 4)[0]
        tables = {}
        for i in range(count):
            tag, _, offset, length = struct.unpack_from(">4sIII", data, 12 + 16 * i)
            tables[tag.decode("ascii")] = data[offset:offset + length]
        self.units_per_em = struct.unpack_from(">H", tables["head"], 18)[0]
        widths_given = struct.unpack_from(">H", tables["hhea"], 34)[0]
        hmtx = tables["hmtx"]
        self.widths = [struct.unpack_from(">H", hmtx, 4 * i)[0] for i in range(widths_given)]
        self.glyphs: dict[int, int] = {}
        self.read_cmap(tables["cmap"])

    def read_cmap(self, cmap: bytes) -> None:
        count = struct.unpack_from(">H", cmap, 2)[0]
        # The table for Unicode with the most in it: every plane (format
        # 12) sooner than the first alone (format 4).
        best = None
        for i in range(count):
            platform, encoding, offset = struct.unpack_from(">HHI", cmap, 4 + 8 * i)
            form = struct.unpack_from(">H", cmap, offset)[0]
            unicode = platform == 0 or (platform == 3 and encoding in (1, 10))
            if unicode and form in (4, 12) and (best is None or form > best[0]):
                best = (form, offset)
        expect(best is not None, "the font has no table of Unicode characters this reads")
        form, at = best
        if form == 12:
            groups = struct.unpack_from(">I", cmap, at + 12)[0]
            for i in range(groups):
                first, last, glyph = struct.unpack_from(">III", cmap, at + 16 + 12 * i)
                for code in range(first, last + 1):
                    self.glyphs[code] = glyph + code - first
            return
        segments = struct.unpack_from(">H", cmap, at + 6)[0] // 2
        ends = at + 14
        starts = ends + 2 * segments + 2
        deltas = starts + 2 * segments
        offsets = deltas + 2 * segments
        for i in range(segments):
            end = struct.unpack_from(">H", cmap, ends + 2 * i)[0]
            start = struct.unpack_from(">H", cmap, starts + 2 * i)[0]
            delta = struct.unpack_from(">H", cmap, deltas + 2 * i)[0]
            offset = struct.unpack_from(">H", cmap, offsets + 2 * i)[0]
            for code in range(start, end + 1):
                if code == 0xFFFF:
                    continue
                if offset == 0:
                    glyph = (code + delta) & 0xFFFF
                else:
                    glyph = struct.unpack_from(">H", cmap, offsets + 2 * i + offset + 2 * (code - start))[0]
                    if glyph != 0:
                        glyph = (glyph + delta) & 0xFFFF
                self.glyphs[code] = glyph

    def has(self, code: int) -> bool:
        return self.glyphs.get(code, 0) != 0

    def width(self, code: int) -> int:
        """In the font's units. Glyphs past the last width given have the last."""
        glyph = self.glyphs.get(code, 0)
        return self.widths[min(glyph, len(self.widths) - 1)]


# ---- The image ----


class ImageFace:
    def __init__(self, image: bytes, number: int):
        record = 16 + 48 * number
        (self.role, self.style, self.size, self.ascent, self.descent, self.line, self.x_height, range_count,
         ranges_at, self.glyph_count, glyphs_at, pictures_at, picture_bytes) = struct.unpack_from(
             "<BBHhhHHIIIIII", image, record)
        what = f"face {number}"
        expect(image[record + 36:record + 48] == bytes(12), f"{what}: the bytes kept for kerning are not nothing")
        expect(ranges_at + 12 * range_count <= len(image), f"{what}: its ranges run outside the image")
        expect(glyphs_at + 12 * self.glyph_count <= len(image), f"{what}: its glyphs run outside the image")
        expect(pictures_at + picture_bytes <= len(image), f"{what}: its pictures run outside the image")
        self.picture_bytes = picture_bytes

        # Glyphs: each picture follows the one before, and the last ends
        # where the face's pictures do.
        self.glyphs = []
        at = 0
        for index in range(self.glyph_count):
            advance, width, height, left, top, kept, picture = struct.unpack_from(
                "<HBBbbHI", image, glyphs_at + 12 * index)
            expect(kept == 0, f"{what}, glyph {index}: the bytes kept for kerning are not nothing")
            expect(picture == at, f"{what}, glyph {index}: its picture is at {picture}, not {at}")
            size = (width * height * 2 + 7) // 8
            packed = image[pictures_at + picture:pictures_at + picture + size]
            pixels = [(packed[i // 4] >> (6 - (i % 4) * 2)) & 3 for i in range(width * height)]
            # What is left of the last byte is nothing.
            if (width * height) % 4:
                expect(packed[-1] & ((1 << (8 - (width * height % 4) * 2)) - 1) == 0,
                       f"{what}, glyph {index}: the last byte of its picture is not filled out with nothing")
            at += size
            self.glyphs.append({"index": index, "advance": advance, "width": width, "height": height, "left": left,
                                "top": top, "ink": sum(pixels), "pixels": pixels})
        expect(at == picture_bytes, f"{what}: its pictures take {at} bytes, and it says {picture_bytes}")

        # Ranges: in order, apart, their glyphs one after another from 1.
        self.by_code: dict[int, dict] = {}
        free = 0
        glyph = 1
        for i in range(range_count):
            first, count, begins = struct.unpack_from("<III", image, ranges_at + 12 * i)
            expect(count > 0 and first >= free, f"{what}: range {i} is empty or out of order")
            expect(i == 0 or first > free, f"{what}: range {i} follows straight on from the one before")
            expect(begins == glyph, f"{what}: range {i} begins at glyph {begins}, not {glyph}")
            for code in range(first, first + count):
                self.by_code[code] = self.glyphs[glyph]
                glyph += 1
            free = first + count
        expect(glyph == self.glyph_count, f"{what}: its ranges come to {glyph} glyphs of {self.glyph_count}")


def read_image(image: bytes) -> list[ImageFace]:
    expect(image[:4] == b"BNFN" and image[4] == 1 and image[5] == 0, "the image does not begin as one")
    faces, length, kept = struct.unpack_from("<HII", image, 6)
    expect(length == len(image), f"the image says it is {length} bytes and is {len(image)}")
    expect(kept == 0, "the header's last four bytes are not nothing")
    return [ImageFace(image, number) for number in range(faces)]


BUILD_LINE = re.compile(r"^(\S+) +(\S+) +(\d+) px: +(\d+) glyphs \((\d+) lacking\), pictures +(\d+) bytes; "
                        r"ascent (-?\d+), descent (-?\d+), line (\d+), x-height (\d+)$")
INFO_LINE = re.compile(r"^(\S+) +(\S+) +(\d+) px: +(\d+) glyphs; ascent (-?\d+), descent (-?\d+), line (\d+), "
                       r"x-height (\d+)$")
GLYPH_LINE = re.compile(r"^  U\+([0-9A-F]{4,6}): (?:no glyph|glyph (\d+), advance (\d+), (\d+) by (\d+) at (-?\d+), "
                        r"(-?\d+), ink (\d+))$")
STYLES = ("regular", "bold", "italic", "bold-italic")


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    tool, fonts, work = sys.argv[1], Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    asked = [code for first, last in CHARACTERS for code in range(first, last + 1)]
    written = " ".join(f"U+{first:04X}-U+{last:04X}" for first, last in CHARACTERS)
    try:
        # The preset beside a folder of its own, so that a file is found
        # from where the preset is, and one named from the root as well.
        preset = work / "presets" / "courier.preset"
        (work / "presets" / "faces").mkdir(parents=True)
        shutil.copy(fonts / FILES[1][2], work / "presets" / "faces" / "The Bold One.ttf")
        preset.write_text("# Courier Prime, as the monospace face.\n"
                          f"sizes {' '.join(str(size) for size in reversed(SIZES))}\n"
                          f"mono bold     faces/The Bold One.ttf   # Found from here.\n"
                          f"mono regular  {fonts / FILES[0][2]}\n"
                          f"characters {written}\n")
        image_path = work / "courier.bnfn"
        result = run(tool, "build", str(preset), "--out", str(image_path))
        expect(result.returncode == 0, f"build ended with {result.returncode} and said {result.stderr!r}")
        expect(result.stderr == b"", f"build warned of something: {result.stderr!r}")
        said = result.stdout.decode("utf-8").splitlines()
        image = image_path.read_bytes()
        faces = read_image(image)
        expect(len(faces) == len(FILES) * len(SIZES), f"the image has {len(faces)} faces")
        expect(len(said) == len(faces) + 1, f"build said {len(said)} lines for {len(faces)} faces")
        expect(said[-1] == f"faces {len(faces)}, pictures {sum(face.picture_bytes for face in faces)} bytes, "
               f"image {len(image)} bytes", f"build's last line: {said[-1]!r}")

        # What the reader makes of the same image.
        result = run(tool, "info", str(image_path), "--glyphs", *written.split())
        expect(result.returncode == 0, f"info ended with {result.returncode} and said {result.stderr!r}")
        info = result.stdout.decode("utf-8").splitlines()
        expect(info[0] == f"image {len(image)} bytes, faces {len(faces)}", f"info's first line: {info[0]!r}")
        expect(len(info) == 1 + len(faces) * (1 + len(asked)), f"info said {len(info)} lines")

        glyphs_checked = 0
        ink: dict[tuple[int, int, int], int] = {}
        number = 0
        for style_name, style, file in FILES:
            font = FontFile(fonts / file)
            for size in SIZES:
                face = faces[number]
                what = f"mono {style_name} at {size}"
                expect((face.role, face.style, face.size) == (2, style, size),
                       f"face {number} is role {face.role}, style {face.style}, size {face.size}: not {what}")
                has = [code for code in asked if font.has(code)]
                expect(sorted(face.by_code) == has,
                       f"{what}: the image has glyphs for {len(face.by_code)} characters, the font for {len(has)}")
                expect(0 < len(has) < len(asked), f"{what}: the font has all or none of what was asked")

                # What build said of it, and what info says.
                line = BUILD_LINE.match(said[number])
                expect(line is not None, f"{what}: build's line: {said[number]!r}")
                expect(line.group(1, 2) == ("mono", style_name) and
                       [int(n) for n in line.groups()[2:]] == [size, len(has) + 1, len(asked) - len(has),
                                                              face.picture_bytes, face.ascent, face.descent,
                                                              face.line, face.x_height],
                       f"{what}: build said {said[number]!r}")
                at = 1 + number * (1 + len(asked))
                line = INFO_LINE.match(info[at])
                expect(line is not None, f"{what}: info's line: {info[at]!r}")
                expect(line.group(1, 2) == ("mono", style_name) and
                       [int(n) for n in line.groups()[2:]] == [size, face.glyph_count, face.ascent, face.descent,
                                                              face.line, face.x_height],
                       f"{what}: info said {info[at]!r}")

                # Its numbers are a typeface's at that size.
                expect(size / 2 < face.ascent <= size * 3 / 2 and 0 < face.descent < size and face.line >= size,
                       f"{what}: ascent {face.ascent}, descent {face.descent}, line {face.line}")
                expect(face.x_height == face.by_code[ord("x")]["top"], f"{what}: the x-height is not the top of the x")

                for i, code in enumerate(asked):
                    line = GLYPH_LINE.match(info[at + 1 + i])
                    expect(line is not None and int(line.group(1), 16) == code,
                           f"{what}: info's line for U+{code:04X}: {info[at + 1 + i]!r}")
                    glyph = face.by_code.get(code)
                    if glyph is None:
                        expect(line.group(2) is None, f"{what}: info found a glyph for U+{code:04X}")
                        continue
                    expect(line.group(2) is not None and
                           [int(n) for n in line.groups()[1:]] == [glyph["index"], glyph["advance"], glyph["width"],
                                                                  glyph["height"], glyph["left"], glyph["top"],
                                                                  glyph["ink"]],
                           f"{what}: info said {info[at + 1 + i]!r} of U+{code:04X}, and the image has {glyph}")
                    # The font's width for it, at this size, in quarters of
                    # a pixel, to the nearest.
                    wanted = (font.width(code) * size * 4 * 2 + font.units_per_em) // (2 * font.units_per_em)
                    expect(glyph["advance"] == wanted,
                           f"{what}: U+{code:04X} moves the pen {glyph['advance']} quarters, and the font's width "
                           f"of {font.width(code)} in {font.units_per_em} comes to {wanted}")
                    glyphs_checked += 1

                # The pictures: a space has none, a letter has ink inside
                # its picture's edges, and no picture is all ink.
                expect(face.by_code[0x20]["width"] == 0 and face.by_code[0x20]["ink"] == 0, f"{what}: a space has ink")
                for letter in "AMgx@":
                    glyph = face.by_code[ord(letter)]
                    expect(0 < glyph["ink"] < 3 * glyph["width"] * glyph["height"], f"{what}: {letter} has no shape")
                    expect(glyph["width"] <= size and glyph["height"] <= size * 3 // 2,
                           f"{what}: {letter} is {glyph['width']} by {glyph['height']}")
                expect(face.by_code[ord("g")]["height"] > face.by_code[ord("g")]["top"],
                       f"{what}: a g does not go below the line")
                expect(face.by_code[ord(".")]["ink"] < face.by_code[ord("x")]["ink"] < face.by_code[ord("M")]["ink"],
                       f"{what}: a full stop, an x and an M are not in the order of their ink")
                ink[(style, size, 0)] = sum(glyph["ink"] for glyph in face.glyphs)
                number += 1
        for size in SIZES:
            expect(ink[(1, size, 0)] > ink[(0, size, 0)], f"at {size}, the bold has no more ink than the regular")
        expect(ink[(0, SIZES[0], 0)] < ink[(0, SIZES[1], 0)] < ink[(0, SIZES[2], 0)],
               "a larger size has no more ink than a smaller")

        # The same preset makes the same image.
        again = work / "again.bnfn"
        expect(run(tool, "build", str(preset), "--out", str(again)).returncode == 0, "building again")
        expect(again.read_bytes() == image, "the same preset made another image the second time")

        # A reading face without all its styles is warned of, and built.
        (work / "serif.preset").write_text(f"sizes 20\ncharacters U+0041\nserif regular {fonts / FILES[0][2]}\n")
        result = run(tool, "build", str(work / "serif.preset"), "--out", str(work / "serif.bnfn"))
        warnings = result.stderr.decode("utf-8").splitlines()
        expect(result.returncode == 0 and len(warnings) == 3 and
               all(line.startswith("bn-font: warning: serif ") for line in warnings),
               f"a serif with one style: ended with {result.returncode} and said {result.stderr!r}")
        expect(len(read_image((work / "serif.bnfn").read_bytes())) == 1, "a serif with one style: not one face")

        # What is refused: a preset that is wrong, a font that is not there
        # or is not one, and an image that is not one.
        refused = {
            "no-sizes": (f"mono regular {fonts / FILES[0][2]}\n", "no sizes are given"),
            "a-size-too-large": (f"sizes 20 200\nmono regular {fonts / FILES[0][2]}\n", "line 1: a size"),
            "a-role-there-is-not": ("sizes 20\nscript regular a.ttf\n", "line 2: a line begins with"),
            "a-face-twice": ("sizes 20\nmono bold a.ttf\nmono bold b.ttf\n", "line 3: mono bold was named already"),
            "no-such-font": ("sizes 20\nmono regular no-such-font.ttf\n", "FreeType cannot read it as a font"),
            "not-a-font": ("sizes 20\nmono regular not-a-font.preset\n", "FreeType cannot read it as a font"),
        }
        for name, (text, error) in refused.items():
            (work / f"{name}.preset").write_text(text)
            out = work / f"{name}.bnfn"
            result = run(tool, "build", str(work / f"{name}.preset"), "--out", str(out))
            expect(result.returncode == 1 and error in result.stderr.decode("utf-8"),
                   f"{name}: build ended with {result.returncode} and said {result.stderr!r}")
            expect(not out.exists(), f"{name}: an image was written all the same")
        short = work / "short.bnfn"
        short.write_bytes(image[:-1])
        for name, path in (("an image cut short", short), ("a preset as an image", preset)):
            result = run(tool, "info", str(path))
            expect(result.returncode == 1 and result.stderr.decode("utf-8").strip().endswith(": bad image"),
                   f"{name}: info ended with {result.returncode} and said {result.stderr!r}")

        # And what it is asked that it does not do.
        expect(run(tool).returncode == 1, "with nothing asked of it")
        expect(run(tool, "build", str(preset)).returncode == 1, "build with nowhere to write")
        expect(run(tool, "build", str(work / "no-such.preset"), "--out", str(work / "x.bnfn")).returncode == 1,
               "a preset that is not there")
        expect(run(tool, "info", str(work / "no-such.bnfn")).returncode == 1, "an image that is not there")
        expect(run(tool, "info", str(image_path), "--glyphs").returncode == 1, "info --glyphs with no characters")
        expect(run(tool, "info", str(image_path), "--glyphs", "0041").returncode == 1, "info --glyphs 0041")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"{len(faces)} faces of Courier Prime at {len(SIZES)} sizes, {glyphs_checked} glyphs: the image's characters "
          f"and advances are the font's, and bn-font reads back what the image's bytes say")
    return 0


if __name__ == "__main__":
    sys.exit(main())
