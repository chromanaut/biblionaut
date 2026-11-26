#!/usr/bin/env python3
"""End-to-end check of the kerning bn-font puts in a fonts image, run by CTest.

The unit tests give the reader of a font's kerning tables written for it.
Here bn-font is set beside a second reading, which shares none of its code,
of two real fonts that kern, which the build downloads: Noto Serif Italic,
which kerns Greek by other lookups than Latin and Cyrillic, and Noto Sans
Regular, which has a rule of kerning that looks at more than two glyphs.

  - `bn-font build` makes an image of the two at two sizes, and says of each
    face how many pairs it kerns and in how many classes its glyphs are.
  - The fonts' own tables are read here from the description of their
    layout (GPOS and GDEF of the OpenType specification): the lookups of
    `kern` for the default language of every script, the pair adjustments
    among them, a lookup's subtables in order, and what several lookups say
    of a pair added up; no pair with a glyph in it that its lookup passes
    over.
  - The image is taken apart here, from the description of its layout
    (font_tool.py's reading of it): for every two characters of a face, its
    classes and its table have to give what the font's units come to at the
    face's size in quarters of a pixel, a half going away from nothing; and
    nothing for a pair the font does not kern.
  - A few pairs the fonts are known to kern are named here with what they
    are kerned by, so that the reading here is held to something too.
  - `bn-font info --kerning` and `--glyphs`, which read the image by the
    code the firmware reads it with, have to say the same pairs and the
    same classes.

usage: font_kerning.py BN_FONT FONT_DIR WORK_DIR
"""

from __future__ import annotations

import re
import shutil
import struct
import sys
from pathlib import Path

# The other test's readers of a font file and of an image are used here, and
# nothing is to be left beside it in the source tree for that.
sys.dont_write_bytecode = True
from font_tool import Failure, FontFile, expect, read_image, run  # noqa: E402

SIZES = (22, 30)
# Latin with its first extension, Greek, Cyrillic, the punctuation, and the
# ligatures; and marks, which the fonts' kerning passes over.
CHARACTERS = ((0x20, 0x7E), (0xA0, 0x17F), (0x300, 0x30F), (0x391, 0x3C9), (0x410, 0x44F), (0x2010, 0x2026),
              (0xFB00, 0xFB04))
# The faces of the image, in the order an image has them: role, style, the
# names bn-font gives the two, and the file.
FACES = ((0, 2, "serif", "italic", "NotoSerif-Italic.ttf"), (1, 0, "sans", "regular", "NotoSans-Regular.ttf"))
# Pairs the fonts kern, in their own units of a thousand to the em: read
# from the fonts by a shaping engine, HarfBuzz, when this was written.
KNOWN = {
    "NotoSerif-Italic.ttf": {("r", ","): -170, ("r", "e"): -20, ("y", ","): -90, ("o", "o"): 0, ("e", "r"): 0},
    "NotoSans-Regular.ttf": {("r", ","): -160, ("r", "e"): -20, ("y", ","): -40, ("o", "o"): 0, ("e", "r"): 0},
}
# How many lookups of `kern` each has that are not pair adjustments.
LEFT_OUT = {"NotoSerif-Italic.ttf": 0, "NotoSans-Regular.ttf": 1}

BUILD_LINE = re.compile(r"^(\S+) +(\S+) +(\d+) px: .*; kerning (\d+) pairs, (\d+) by (\d+)"
                        r"(?:, (\d+) lookups? that looks? at more than two glyphs left out)?; \d+ ligatures$")
INFO_LINE = re.compile(r"^(\S+) +(\S+) +(\d+) px: .*; kerning (\d+) by (\d+); ligatures \d+$")
PAIR_LINE = re.compile(r"^  U\+([0-9A-F]{4,6}) U\+([0-9A-F]{4,6}): (-?\d+)$")
GLYPH_LINE = re.compile(r"^  U\+([0-9A-F]{4,6}): glyph (\d+), .*, kerns as (\d+) and (\d+)$")


def u16(data: bytes, at: int) -> int:
    return struct.unpack_from(">H", data, at)[0]


def s16(data: bytes, at: int) -> int:
    return struct.unpack_from(">h", data, at)[0]


def coverage(data: bytes, at: int) -> list[int]:
    """The glyphs a coverage table covers, in its order."""
    form, count = u16(data, at), u16(data, at + 2)
    if form == 1:
        return [u16(data, at + 4 + 2 * i) for i in range(count)]
    expect(form == 2, f"a coverage table of format {form}")
    glyphs = []
    for i in range(count):
        first, last, place = struct.unpack_from(">HHH", data, at + 4 + 6 * i)
        expect(place == len(glyphs), "a coverage table's ranges are out of order")
        glyphs.extend(range(first, last + 1))
    return glyphs


def classes(data: bytes, at: int) -> dict[int, int]:
    """The class of each glyph a table of classes names."""
    form = u16(data, at)
    if form == 1:
        first, count = u16(data, at + 2), u16(data, at + 4)
        return {first + i: u16(data, at + 6 + 2 * i) for i in range(count)}
    expect(form == 2, f"a table of classes of format {form}")
    of = {}
    for i in range(u16(data, at + 2)):
        first, last, value = struct.unpack_from(">HHH", data, at + 4 + 6 * i)
        for glyph in range(first, last + 1):
            of[glyph] = value
    return of


def value_size(form: int) -> int:
    return 2 * bin(form & 0xFF).count("1")


def x_advance(data: bytes, at: int, form: int) -> int:
    """The advance in a value record, which comes after the placements."""
    if not form & 4:
        return 0
    return s16(data, at + 2 * bin(form & 3).count("1"))


class Kerning:
    """A font's pair adjustments, read the plain way."""

    def __init__(self, font: FontFile):
        gpos = font.tables["GPOS"]
        self.marks = {glyph for glyph, kind in self.glyph_classes(font.tables.get("GDEF")).items() if kind == 3}
        scripts, features, lookups = u16(gpos, 4), u16(gpos, 6), u16(gpos, 8)
        wanted = set()
        for s in range(u16(gpos, scripts)):
            script = scripts + u16(gpos, scripts + 2 + 6 * s + 4)
            if u16(gpos, script) == 0:
                continue
            language = script + u16(gpos, script)
            for f in range(u16(gpos, language + 4)):
                record = features + 2 + 6 * u16(gpos, language + 6 + 2 * f)
                if gpos[record:record + 4] != b"kern":
                    continue
                feature = features + u16(gpos, record + 4)
                wanted.update(u16(gpos, feature + 4 + 2 * i) for i in range(u16(gpos, feature + 2)))
        self.lookups = []
        self.left_out = 0
        for index in sorted(wanted):
            at = lookups + u16(gpos, lookups + 2 + 2 * index)
            kind, flag, count = struct.unpack_from(">HHH", gpos, at)
            subtables = []
            for i in range(count):
                sub = at + u16(gpos, at + 6 + 2 * i)
                sub_kind = kind
                if kind == 9:
                    sub_kind = u16(gpos, sub + 2)
                    sub += struct.unpack_from(">I", gpos, sub + 4)[0]
                subtables.append((sub_kind, sub))
            if any(sub_kind != 2 for sub_kind, _ in subtables):
                self.left_out += 1
                continue
            expect(flag in (0, 8), f"a lookup of kerning passes over something other than marks: flag {flag}")
            self.lookups.append((bool(flag & 8), [self.pairs(gpos, sub) for _, sub in subtables]))

    @staticmethod
    def glyph_classes(gdef: bytes | None) -> dict[int, int]:
        if not gdef or u16(gdef, 4) == 0:
            return {}
        return classes(gdef, u16(gdef, 4))

    @staticmethod
    def pairs(gpos: bytes, at: int):
        """A subtable of pair adjustments: a list of pairs as ("list", {first: {second: units}}), a table of
        classes as ("classes", the glyphs covered, the classes of first and of second glyphs, the table)."""
        form, covered_at, form1, form2 = struct.unpack_from(">HHHH", gpos, at)
        covered = coverage(gpos, at + covered_at)
        if form == 1:
            expect(u16(gpos, at + 8) == len(covered), "a list of pairs has not a set for each glyph it covers")
            sets = {}
            for i, first in enumerate(covered):
                pair_set = at + u16(gpos, at + 10 + 2 * i)
                each = 2 + value_size(form1) + value_size(form2)
                seconds = {}
                for r in range(u16(gpos, pair_set)):
                    record = pair_set + 2 + r * each
                    seconds.setdefault(u16(gpos, record), x_advance(gpos, record + 2, form1))
                sets[first] = seconds
            return ("list", sets)
        expect(form == 2, f"pair adjustments of format {form}")
        class1_at, class2_at, count1, count2 = struct.unpack_from(">HHHH", gpos, at + 8)
        each = value_size(form1) + value_size(form2)
        table = [[x_advance(gpos, at + 16 + (c1 * count2 + c2) * each, form1) for c2 in range(count2)]
                 for c1 in range(count1)]
        return ("classes", set(covered), classes(gpos, at + class1_at), classes(gpos, at + class2_at), table)

    def after(self, first: int, glyphs: list[int]) -> dict[int, int]:
        """What each of `glyphs` is moved by after `first`, in the font's units: those that are moved."""
        moved: dict[int, int] = {}
        for passes_marks, subtables in self.lookups:
            if passes_marks and first in self.marks:
                continue
            settled: dict[int, int] = {}
            for subtable in subtables:
                if subtable[0] == "list":
                    for second, units in subtable[1].get(first, {}).items():
                        settled.setdefault(second, units)
                    continue
                _, covered, class1, class2, table = subtable
                if first not in covered:
                    continue
                row = table[class1.get(first, 0)]
                for second in glyphs:
                    if second not in settled:
                        settled[second] = row[class2.get(second, 0)]
                break
            for second, units in settled.items():
                if units and not (passes_marks and second in self.marks):
                    moved[second] = moved.get(second, 0) + units
        return {second: units for second, units in moved.items() if units}


def quarters(units: int, size: int, units_per_em: int) -> int:
    """Font units at `size` pixels to the em, in quarters of a pixel: a half goes away from nothing."""
    whole = (abs(units) * 8 * size + units_per_em) // (2 * units_per_em)
    return whole if units >= 0 else -whole


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    tool, fonts, work = sys.argv[1], Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    written = " ".join(f"U+{first:04X}-U+{last:04X}" for first, last in CHARACTERS)
    asked = [code for first, last in CHARACTERS for code in range(first, last + 1)]
    try:
        preset = work / "kerned.preset"
        preset.write_text(f"sizes {' '.join(str(size) for size in SIZES)}\n" +
                          "".join(f"{role} {style} {fonts / file}\n" for _, _, role, style, file in FACES) +
                          f"characters {written}\n")
        image_path = work / "kerned.bnfn"
        result = run(tool, "build", str(preset), "--out", str(image_path))
        expect(result.returncode == 0, f"build ended with {result.returncode} and said {result.stderr!r}")
        # It warns of the styles the preset does not name, and of nothing to
        # do with kerning.
        expect(b"kern" not in result.stderr, f"build warned of something in the kerning: {result.stderr!r}")
        said = result.stdout.decode("utf-8").splitlines()
        image = image_path.read_bytes()
        faces = read_image(image)
        expect(len(faces) == len(FACES) * len(SIZES), f"the image has {len(faces)} faces")

        result = run(tool, "info", str(image_path), "--kerning", "U+0020-U+007E", "U+2019", "--glyphs", "U+0041-U+007A")
        expect(result.returncode == 0, f"info ended with {result.returncode} and said {result.stderr!r}")
        info = result.stdout.decode("utf-8").splitlines()[1:]
        near = [code for code in asked if 0x20 <= code <= 0x7E or code == 0x2019]

        number = 0
        pairs_checked = 0
        kerning_bytes = 0
        for role, style, role_name, style_name, file in FACES:
            font = FontFile(fonts / file)
            kerning = Kerning(font)
            expect(kerning.left_out == LEFT_OUT[file],
                   f"{file}: {kerning.left_out} of its lookups of kerning are not pair adjustments")
            codes = [code for code in asked if font.has(code)]
            glyphs = sorted({font.glyphs[code] for code in codes})
            units: dict[tuple[int, int], int] = {}
            for first in glyphs:
                for second, moved in kerning.after(first, glyphs).items():
                    units[(first, second)] = moved
            expect(len(units) > 5000, f"{file}: the reading here finds only {len(units)} pairs kerned")
            for (first, second), wanted in KNOWN[file].items():
                got = units.get((font.glyphs[ord(first)], font.glyphs[ord(second)]), 0)
                expect(got == wanted, f"{file}: the reading here kerns {first}{second} by {got}, and it is {wanted}")
            expect(any(font.glyphs[code] in kerning.marks for code in codes), f"{file}: none of the characters is a mark")

            for size in SIZES:
                face = faces[number]
                what = f"{role_name} {style_name} at {size}"
                expect((face.role, face.style, face.size) == (role, style, size), f"face {number} is not {what}")
                expect(sorted(face.by_code) == codes, f"{what}: the image has not the font's characters")
                # Every two characters, by the image's bytes.
                kerned = 0
                for first in codes:
                    first_glyph = face.by_code[first]
                    for second in codes:
                        wanted = quarters(units.get((font.glyphs[first], font.glyphs[second]), 0), size,
                                          font.units_per_em)
                        got = face.kern(first_glyph, face.by_code[second])
                        expect(got == wanted, f"{what}: U+{second:04X} after U+{first:04X} is kerned by {got} "
                               f"quarters in the image, and the font's tables come to {wanted}")
                        kerned += got != 0
                        pairs_checked += 1
                expect(kerned > 5000, f"{what}: only {kerned} pairs are kerned")
                expect(face.kern(face.glyphs[0], face.by_code[ord(",")]) == 0 and
                       face.kern(face.by_code[ord("r")], face.glyphs[0]) == 0,
                       f"{what}: the glyph for what the face lacks is kerned")
                kerning_bytes += len(face.kerning)

                # What build said of it.
                line = BUILD_LINE.match(said[number])
                expect(line is not None, f"{what}: build's line: {said[number]!r}")
                expect(line.group(1, 2) == (role_name, style_name) and
                       [int(n) for n in line.group(3, 4, 5, 6)] == [size, kerned, face.kern_firsts, face.kern_seconds]
                       and int(line.group(7) or 0) == LEFT_OUT[file], f"{what}: build said {said[number]!r}")

                # What the reader makes of it: the face's line, the classes
                # of the letters, and the pairs among the characters named.
                line = INFO_LINE.match(info[0])
                expect(line is not None and line.group(1, 2) == (role_name, style_name) and
                       [int(n) for n in line.group(3, 4, 5)] == [size, face.kern_firsts, face.kern_seconds],
                       f"{what}: info's line: {info[0]!r}")
                info = info[1:]
                for code in range(0x41, 0x7B):
                    line = GLYPH_LINE.match(info[0])
                    glyph = face.by_code[code]
                    expect(line is not None and
                           [int(line.group(1), 16), *(int(n) for n in line.group(2, 3, 4))] ==
                           [code, glyph["index"], glyph["kern_first"], glyph["kern_second"]],
                           f"{what}: info said {info[0]!r} of U+{code:04X}, whose classes are "
                           f"{glyph['kern_first']} and {glyph['kern_second']}")
                    info = info[1:]
                wanted_pairs = [(first, second, face.kern(face.by_code[first], face.by_code[second]))
                                for first in near for second in near]
                wanted_pairs = [pair for pair in wanted_pairs if pair[2] != 0]
                expect(len(wanted_pairs) > 300, f"{what}: only {len(wanted_pairs)} pairs of ASCII are kerned")
                for first, second, moved in wanted_pairs:
                    line = PAIR_LINE.match(info[0]) if info else None
                    expect(line is not None and
                           (int(line.group(1), 16), int(line.group(2), 16), int(line.group(3))) ==
                           (first, second, moved),
                           f"{what}: info's next pair is {info[0] if info else None!r}, and the image kerns "
                           f"U+{first:04X} U+{second:04X} by {moved}")
                    info = info[1:]
                number += 1
        expect(not info, f"info said more than was asked: {info[:2]!r}")
        expect(re.fullmatch(rf"faces {len(faces)}, pictures \d+ bytes, kerning {kerning_bytes} bytes, "
                            rf"image {len(image)} bytes", said[-1]) is not None, f"build's last line: {said[-1]!r}")

        # A pair kerned by less than an eighth of a pixel at a size is not
        # kerned there: re, 20 units, is 1.76 quarters at 22 and 2.4 at 30.
        sans = [face for face in faces if face.role == 1]
        expect([face.kern(face.by_code[ord("r")], face.by_code[ord("e")]) for face in sans] == [-2, -2],
               "the sans does not kern re by two quarters at both sizes")
        expect([face.kern(face.by_code[ord("r")], face.by_code[ord(",")]) for face in sans] == [-14, -19],
               "the sans does not kern a comma after an r by 14 and 19 quarters")

        # What info refuses.
        expect(run(tool, "info", str(image_path), "--kerning").returncode == 1, "info --kerning with no characters")
        expect(run(tool, "info", str(image_path), "--kerning", "--glyphs", "U+0041").returncode == 1,
               "info --kerning with no characters before --glyphs")
        expect(run(tool, "info", str(image_path), "U+0041").returncode == 1, "info with characters and no option")
    except Failure as failure:
        print(f"font_kerning: {failure}", file=sys.stderr)
        return 1
    print(f"font_kerning: {len(faces)} faces, {pairs_checked} pairs as the fonts' tables have them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
