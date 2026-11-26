#!/usr/bin/env python3
"""End-to-end check of the ligatures bn-font puts in a fonts image, run by CTest.

The unit tests give the reader of a font's ligatures tables written for it.
Here bn-font is set beside a second reading, which shares none of its code,
of two real fonts that make ligatures, which the build downloads: Noto
Serif Italic and Noto Sans Regular. Each makes five, of f with f, i and l.

  - The fonts' own tables are read here from the description of their
    layout (GSUB of the OpenType specification): the lookups of `liga` for
    the default language of every script, the ligature substitutions among
    them, and each glyph's ligatures in the order the font has them.
  - `bn-font build` makes an image of the two at two sizes, twice. Once
    with the characters U+FB00 to U+FB04 among those it draws, whose glyphs
    are the ligatures': each face then has the font's ligatures with those
    characters' glyphs standing for them, and no glyph more. And once
    without them: each face then has five glyphs after its characters',
    which no character has, as wide as the font has the ligatures.
  - The image is taken apart here, from the description of its layout
    (font_tool.py's reading of it): a face's list of ligatures has to be the
    font's, in the font's order, by the face's own numbers for its glyphs.
  - A few words are joined by the image's list and have to come out as a
    reader would expect: an f, an f and an i as one glyph, and the like.
  - The glyph of a ligature is kerned as the font kerns it, a character's
    or not: before and after every character of the image, by what
    font_kerning.py's reading of the font's tables comes to.
  - `bn-font info --ligatures`, which reads the image by the code the
    firmware reads it with, has to say the same ligatures.

usage: font_ligatures.py BN_FONT FONT_DIR WORK_DIR
"""

from __future__ import annotations

import re
import shutil
import struct
import sys
from pathlib import Path

# The other tests' readers of a font file and of an image are used here, and
# nothing is to be left beside them in the source tree for that.
sys.dont_write_bytecode = True
from font_kerning import Kerning, coverage, u16  # noqa: E402
from font_kerning import quarters as kerning_quarters  # noqa: E402
from font_tool import Failure, FontFile, expect, read_image, run  # noqa: E402

SIZES = (22, 30)
LETTERS = ((0x20, 0x7E), (0xA0, 0xFF))
LIGATURES = ((0xFB00, 0xFB04),)
FACES = ((0, 2, "serif", "italic", "NotoSerif-Italic.ttf"), (1, 0, "sans", "regular", "NotoSans-Regular.ttf"))
# What each of the two fonts joins, and the character whose glyph each
# ligature is: found with a shaping engine, HarfBuzz, when this was written.
KNOWN = {"ff": 0xFB00, "fi": 0xFB01, "fl": 0xFB02, "ffi": 0xFB03, "ffl": 0xFB04}

BUILD_LINE = re.compile(r"^(\S+) +(\S+) +(\d+) px: +(\d+) glyphs .*; (\d+) ligatures"
                        r"(?:, (\d+) of their glyphs no character's)?$")
INFO_LINE = re.compile(r"^(\S+) +(\S+) +(\d+) px: +(\d+) glyphs; .*; ligatures (\d+)$")
LIGATURE_LINE = re.compile(r"^  ((?:U\+[0-9A-F]{4,6} ?){2,3}): glyph (\d+), advance (\d+)$")


def font_ligatures(font: FontFile) -> list[tuple[tuple[int, ...], int]]:
    """The ligatures of a font's `liga` feature, read the plain way: the glyphs each is made of and the glyph that
    stands for them, in the order the font has them."""
    gsub = font.tables["GSUB"]
    scripts, features, lookups = u16(gsub, 4), u16(gsub, 6), u16(gsub, 8)
    wanted = set()
    for s in range(u16(gsub, scripts)):
        script = scripts + u16(gsub, scripts + 2 + 6 * s + 4)
        if u16(gsub, script) == 0:
            continue
        language = script + u16(gsub, script)
        for f in range(u16(gsub, language + 4)):
            record = features + 2 + 6 * u16(gsub, language + 6 + 2 * f)
            if gsub[record:record + 4] != b"liga":
                continue
            feature = features + u16(gsub, record + 4)
            wanted.update(u16(gsub, feature + 4 + 2 * i) for i in range(u16(gsub, feature + 2)))
    found = []
    for index in sorted(wanted):
        at = lookups + u16(gsub, lookups + 2 + 2 * index)
        kind, _, count = struct.unpack_from(">HHH", gsub, at)
        for i in range(count):
            sub = at + u16(gsub, at + 6 + 2 * i)
            sub_kind = kind
            if kind == 7:
                sub_kind = u16(gsub, sub + 2)
                sub += struct.unpack_from(">I", gsub, sub + 4)[0]
            expect(sub_kind == 4 and u16(gsub, sub) == 1, "a subtable of `liga` is not a ligature substitution")
            firsts = coverage(gsub, sub + u16(gsub, sub + 2))
            expect(len(firsts) == u16(gsub, sub + 4), "a ligature subtable has not a set for each glyph it covers")
            for place, first in enumerate(firsts):
                ligature_set = sub + u16(gsub, sub + 6 + 2 * place)
                for k in range(u16(gsub, ligature_set)):
                    ligature = ligature_set + u16(gsub, ligature_set + 2 + 2 * k)
                    stands, made = u16(gsub, ligature), u16(gsub, ligature + 2)
                    rest = [u16(gsub, ligature + 4 + 2 * n) for n in range(made - 1)]
                    found.append(((first, *rest), stands))
    return found


def quarters(units: int, size: int, units_per_em: int) -> int:
    return (units * 8 * size + units_per_em) // (2 * units_per_em)


def written(ranges: tuple[tuple[int, int], ...]) -> str:
    return " ".join(f"U+{first:04X}-U+{last:04X}" for first, last in ranges)


def check_image(tool: str, fonts: Path, work: Path, name: str, with_characters: bool) -> tuple[int, int]:
    """Builds an image of the two fonts, with the ligatures' own characters among those drawn or without, and
    holds it and what bn-font says of it to the fonts' tables. Returns how many ligatures were looked at, and how
    many times one was found kerned with a character."""
    ranges = LETTERS + (LIGATURES if with_characters else ())
    asked = [code for first, last in ranges for code in range(first, last + 1)]
    preset = work / f"{name}.preset"
    preset.write_text(f"sizes {' '.join(str(size) for size in SIZES)}\n" +
                      "".join(f"{role} {style} {fonts / file}\n" for _, _, role, style, file in FACES) +
                      f"characters {written(ranges)}\n")
    image_path = work / f"{name}.bnfn"
    result = run(tool, "build", str(preset), "--out", str(image_path))
    expect(result.returncode == 0, f"{name}: build ended with {result.returncode} and said {result.stderr!r}")
    expect(b"ligature" not in result.stderr, f"{name}: build warned of something in the ligatures: {result.stderr!r}")
    said = result.stdout.decode("utf-8").splitlines()
    faces = read_image(image_path.read_bytes())
    expect(len(faces) == len(FACES) * len(SIZES), f"{name}: the image has {len(faces)} faces")

    result = run(tool, "info", str(image_path), "--ligatures", "U+0066", "U+0069", "U+006C", "U+0074")
    expect(result.returncode == 0, f"{name}: info ended with {result.returncode} and said {result.stderr!r}")
    info = result.stdout.decode("utf-8").splitlines()[1:]

    number = 0
    looked_at = 0
    kerned = 0
    for role, style, role_name, style_name, file in FACES:
        font = FontFile(fonts / file)
        kerning = Kerning(font)
        codes = [code for code in asked if font.has(code)]
        code_of = {font.glyphs[code]: code for code in reversed(codes)}
        # The font's ligatures of the characters drawn: all of whose letters
        # are among them.
        wanted = [(letters, stands) for letters, stands in font_ligatures(font)
                  if all(letter in code_of for letter in letters)]
        spelt = {"".join(chr(code_of[letter]) for letter in letters): stands for letters, stands in wanted}
        expect(sorted(spelt) == sorted(KNOWN), f"{file}: the reading here finds the ligatures {sorted(spelt)}")
        for letters, code in KNOWN.items():
            expect(spelt[letters] == font.glyphs[code],
                   f"{file}: the ligature of {letters} is not the glyph of U+{code:04X}")

        for size in SIZES:
            face = faces[number]
            what = f"{name}, {role_name} {style_name} at {size}"
            expect((face.role, face.style, face.size) == (role, style, size), f"face {number} is not {what}")
            expect(sorted(face.by_code) == codes, f"{what}: the image has not the font's characters")
            # Its list is the font's, in the font's order, by its own
            # numbers. The glyph that stands for a ligature is a character's
            # if the image has the character, and one of its own if not.
            extras = face.glyph_count - face.character_glyphs
            expect(extras == (0 if with_characters else len(KNOWN)), f"{what}: {extras} glyphs are no character's")
            expect(len(face.ligatures) == len(wanted), f"{what}: {len(face.ligatures)} ligatures, not {len(wanted)}")
            for (letters, stands), (first, second, third, glyph) in zip(wanted, face.ligatures):
                made_of = [face.by_code[code_of[letter]]["index"] for letter in letters]
                expect([first, second, third] == (made_of + [0])[:3] and len(made_of) in (2, 3),
                       f"{what}: a ligature is of glyphs {first}, {second}, {third}, and the font's is of {made_of}")
                if with_characters:
                    expect(glyph == face.by_code[code_of[stands]]["index"],
                           f"{what}: a ligature's glyph is {glyph}, not its character's")
                else:
                    expect(glyph >= face.character_glyphs, f"{what}: a ligature's glyph {glyph} is a character's")
                # Either way it is as wide as the font has the ligature, and
                # has a picture.
                drawn = face.glyphs[glyph]
                wide = quarters(font.widths[min(stands, len(font.widths) - 1)], size, font.units_per_em)
                expect(drawn["advance"] == wide and drawn["ink"] > 0,
                       f"{what}: the glyph of a ligature moves the pen {drawn['advance']} quarters, and the font's "
                       f"comes to {wide}")
                looked_at += 1
                # And it is kerned as the font kerns the ligature's glyph.
                after = kerning.after(stands, [font.glyphs[code] for code in codes])
                for code in codes:
                    other = font.glyphs[code]
                    before = kerning.after(other, [stands])
                    for one, next_one, units in ((drawn, face.by_code[code], after.get(other, 0)),
                                                 (face.by_code[code], drawn, before.get(stands, 0))):
                        comes_to = max(-128, min(127, kerning_quarters(units, size, font.units_per_em)))
                        expect(face.kern(one, next_one) == comes_to,
                               f"{what}: the glyph of a ligature and U+{code:04X} are kerned by "
                               f"{face.kern(one, next_one)}, and the font's tables come to {comes_to}")
                        kerned += 1 if comes_to else 0

            # Words, joined by the image's list.
            def glyphs_of(word: str) -> list[int]:
                return [glyph["index"] for glyph in face.joined([face.by_code[ord(c)] for c in word])]

            def stands_for(letters: str) -> int:
                return next(glyph for (made, _), (_, _, _, glyph) in zip(wanted, face.ligatures)
                            if "".join(chr(code_of[letter]) for letter in made) == letters)

            def plain(word: str) -> list[int]:
                return [face.by_code[ord(c)]["index"] for c in word]

            expect(glyphs_of("office") == plain("o") + [stands_for("ffi")] + plain("ce"), f"{what}: office")
            expect(glyphs_of("fifl") == [stands_for("fi"), stands_for("fl")], f"{what}: fifl")
            expect(glyphs_of("off") == plain("o") + [stands_for("ff")], f"{what}: off")
            expect(glyphs_of("fffi") == [stands_for("ff"), stands_for("fi")], f"{what}: fffi")
            expect(glyphs_of("if left") == plain("if left"), f"{what}: letters that are not joined")

            # What build said of it, and what info says.
            line = BUILD_LINE.match(said[number])
            expect(line is not None, f"{what}: build's line: {said[number]!r}")
            expect(line.group(1, 2) == (role_name, style_name) and
                   [int(n) for n in line.group(3, 4, 5)] == [size, face.glyph_count, len(wanted)] and
                   int(line.group(6) or 0) == extras, f"{what}: build said {said[number]!r}")
            line = INFO_LINE.match(info[0])
            expect(line is not None and line.group(1, 2) == (role_name, style_name) and
                   [int(n) for n in line.group(3, 4, 5)] == [size, face.glyph_count, len(wanted)],
                   f"{what}: info's line: {info[0]!r}")
            info = info[1:]
            # Of f, i, l and t, in that order: for each two, the two of them
            # and then the three with each third.
            named = "filt"
            expected = []
            for a in named:
                for b in named:
                    if a + b in KNOWN:
                        expected.append((a + b, stands_for(a + b)))
                    for c in named:
                        if a + b + c in KNOWN:
                            expected.append((a + b + c, stands_for(a + b + c)))
            for letters, glyph in expected:
                line = LIGATURE_LINE.match(info[0]) if info else None
                expect(line is not None and
                       [int(code[2:], 16) for code in line.group(1).split()] == [ord(c) for c in letters] and
                       [int(n) for n in line.group(2, 3)] == [glyph, face.glyphs[glyph]["advance"]],
                       f"{what}: info's next ligature is {info[0] if info else None!r}, and the image has "
                       f"{letters} as glyph {glyph}")
                info = info[1:]
            number += 1
    expect(not info, f"{name}: info said more than was asked: {info[:2]!r}")
    return looked_at, kerned


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    tool, fonts, work = sys.argv[1], Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        looked_at, kerned = check_image(tool, fonts, work, "with-their-characters", True)
        more, more_kerned = check_image(tool, fonts, work, "without", False)
        looked_at += more
        kerned += more_kerned
        expect(more_kerned > 0, "no ligature's glyph is kerned with anything: nothing was tried")
        # What info refuses.
        image = str(work / "without.bnfn")
        expect(run(tool, "info", image, "--ligatures").returncode == 1, "info --ligatures with no characters")
        expect(run(tool, "info", image, "--ligatures", "--glyphs", "U+0066").returncode == 1,
               "info --ligatures with no characters before --glyphs")
    except Failure as failure:
        print(f"font_ligatures: {failure}", file=sys.stderr)
        return 1
    print(f"font_ligatures: {looked_at} ligatures as the fonts' tables have them, kerned with a character "
          f"{kerned} times")
    return 0


if __name__ == "__main__":
    sys.exit(main())
