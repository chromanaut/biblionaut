#!/usr/bin/env python3
"""End-to-end check of bn-lines, and through it of the paragraph setter, run by CTest.

The unit tests say what the setter should make of text written for them.
Here bn-lines is set beside a second reading of the same paragraphs that
shares none of its code. That reading takes a whole paragraph apart at once:
into the stretches that no line can end inside, and the places between them
where one can. It then finds what the cheapest way of ending the lines
costs, by trying for every place every place a line ending there could have
begun.

For paragraphs made up at random, with words that have hyphens, dashes, soft
hyphens and zero width spaces in them, parts in a second style, line ends
the text asks for, and a made-up language's hyphenation:

  - every line bn-lines prints has to be a run of those stretches, in order
    and with none left out, as wide as its text adds up to, with as many
    places between words as it has, and marked as what it is;
  - the lines have to cost exactly what the cheapest way costs: the squares
    of the gaps left on them, and the price of each hyphen;
  - with lines too narrow for a word, every line still has to be text of
    the paragraph that adds up, and none may be too wide with more than one
    character on it;
  - `bn-lines --figures` has to count what the lines were.

It also checks what a missing file, a table that is none and a width of
nothing are answered with.

usage: lines_tool.py BN_LINES BN_HYPHEN WORK_DIR
"""

from __future__ import annotations

import random
import shutil
import subprocess
import sys
from pathlib import Path

SOFT_HYPHEN = "­"
ZERO_WIDTH_SPACE = "​"
DASHES = "-‐‒–—"
LONGEST_WORD = 63  # Characters: the hyphenator gives a longer word no places.

# The letters of the made-up language, and characters that are none of its.
LETTERS = "abcdefg"
OTHERS = "x0123456789'é ‑"


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


# ---- The made-up face, as bn-lines says it is ----


def character_width(character: str, style: int) -> int:
    first = character.encode()[0]
    if first in (0xCC, 0xCD):
        return 0
    if character in "iljtf.,;:'!|":
        width = 18
    elif character in "mwMW":
        width = 54
    elif "A" <= character <= "Z":
        width = 44
    elif character == "-":
        width = 22
    elif first >= 0xC0:
        width = 40
    else:
        width = 36
    return width + 4 * style


def space_width(style: int) -> int:
    return 18 + 4 * style


def hyphen_width(style: int) -> int:
    return 22 + 4 * style


# ---- The language ----


class Language:
    """Patterns as they are handed to bn-hyphen, and as this reading keeps them."""

    def __init__(self, rng: random.Random):
        self.left = rng.randint(1, 3)
        self.right = rng.randint(1, 3)
        self.written: list[str] = []
        self.patterns: dict[str, list[int]] = {}
        for _ in range(60):
            letters = "".join(rng.choice(LETTERS) for _ in range(rng.randint(1, 4)))
            if rng.randrange(8) == 0:
                letters = "." + letters
            numbers = [rng.randrange(1, 6) if rng.randrange(3) == 0 else 0 for _ in range(len(letters) + 1)]
            if letters.startswith("."):
                numbers[0] = 0
            self.written.append(
                "".join((str(n) if n else "") + letter for n, letter in zip(numbers, letters))
                + (str(numbers[-1]) if numbers[-1] else "")
            )
            before = self.patterns.get(letters, [0] * len(numbers))
            self.patterns[letters] = [max(a, b) for a, b in zip(before, numbers)]
        # Its letters are the ones its patterns use; a to z have their capitals.
        self.letters = {letter for text in self.patterns for letter in text} - {"."}

    def is_letter(self, character: str) -> bool:
        return character.lower() in self.letters and len(character.lower()) == 1 and character.isascii()

    def places_in(self, stretch: str) -> set[int]:
        """Where a stretch of letters may be broken: how many letters come before each break."""
        marked = "." + stretch.lower() + "."
        levels = [0] * (len(marked) + 1)
        for at in range(len(marked)):
            for length in range(1, len(marked) - at + 1):
                numbers = self.patterns.get(marked[at : at + length])
                if numbers:
                    for place, number in enumerate(numbers):
                        levels[at + place] = max(levels[at + place], number)
        return {
            before
            for before in range(self.left, len(stretch) - self.right + 1)
            if levels[before + 1] % 2 == 1
        }

    def places_in_word(self, word: str) -> set[int]:
        """The same for a whole word: how many characters come before each break."""
        if len(word) > LONGEST_WORD:
            return set()
        places: set[int] = set()
        start = 0
        while start < len(word):
            if not self.is_letter(word[start]):
                start += 1
                continue
            end = start
            while end < len(word) and self.is_letter(word[end]):
                end += 1
            places |= {start + before for before in self.places_in(word[start:end])}
            start = end
        return places


# ---- A paragraph taken apart ----

SPACE, HYPHEN, FREE, LINE_END = "space", "hyphen", "free", "line end"


class Stretch:
    """Text that no line ends inside: where it is in the paragraph's bytes, and how wide."""

    def __init__(self, begin: int):
        self.begin = begin
        self.end = begin
        self.width = 0


class Place:
    """Where a line may end, between two stretches."""

    def __init__(self, kind: str, width: int = 0, extra: int = 0):
        self.kind = kind
        self.width = width  # Of a space, where no line ends at it.
        self.extra = extra  # Of the hyphen shown, where one does.


def take_apart(written: str, language: Language | None) -> tuple[str, list]:
    """A paragraph as the file has it -> its text, and its stretches with the places between them."""
    # The asterisks are not text: each turns the second style on or off.
    characters: list[tuple[str, int, int]] = []  # The character, its style, the bytes before it.
    style = 0
    offset = 0
    for character in written:
        if character == "*":
            style = 1 - style
            continue
        characters.append((character, style, offset))
        offset += len(character.encode())
    text = "".join(character for character, _, _ in characters)

    parts: list = []  # Stretches and places, in order.

    def place(new: Place) -> None:
        """Adds a place after the stretch before it, if there is one for it to follow."""
        if not parts or (isinstance(parts[-1], Place) and parts[-1].kind == LINE_END):
            if new.kind == LINE_END:
                parts.append(new)
            return  # Nothing has come since the paragraph began or a line was ended.
        if isinstance(parts[-1], Place):
            before = parts[-1]
            if new.kind == SPACE and before.kind == SPACE:
                before.width = min(before.width + new.width, 0xFFFF)  # Spaces in a row are one.
            elif new.kind in (SPACE, LINE_END):
                parts[-1] = new  # A place inside a word gives way to what ends the word.
            return  # And a second place inside a word, straight after a first, is none.
        parts.append(new)

    at = 0
    while at < len(characters):
        character, style, _ = characters[at]
        if character == "\n":
            place(Place(LINE_END))
            at += 1
            continue
        if character in " \t\r":
            place(Place(SPACE, width=space_width(style)))
            at += 1
            continue
        end = at
        while end < len(characters) and characters[end][0] not in " \t\r\n":
            end += 1
        word = characters[at:end]
        letters = "".join(character for character, _, _ in word)
        breaks = set()
        if language is not None and SOFT_HYPHEN not in letters:
            breaks = language.places_in_word(letters)

        stretch: Stretch | None = None
        letter_seen = False
        for index, (character, style, begins) in enumerate(word):
            if index in breaks and stretch is not None:
                place(Place(HYPHEN, extra=hyphen_width(word[index - 1][1])))
                stretch = None
            if character == SOFT_HYPHEN:
                # It is on no line and has no width. A mark that no stretch
                # of its word comes before is nothing at all.
                if stretch is not None:
                    place(Place(HYPHEN, extra=hyphen_width(word[index - 1][1])))
                stretch = None
                continue
            if character == ZERO_WIDTH_SPACE:
                if stretch is not None:
                    place(Place(FREE))
                stretch = None
                continue
            if stretch is None:
                stretch = Stretch(begins)
                parts.append(stretch)
            stretch.width += character_width(character, style)
            stretch.end = begins + len(character.encode())
            if character in DASHES:
                following = word[index + 1][0] if index + 1 < len(word) else ""
                minus_before_figure = character == "-" and following.isdigit() and following.isascii()
                if letter_seen and following and following not in DASHES and not minus_before_figure:
                    place(Place(FREE))
                    stretch = None
            else:
                letter_seen = True
        at = end

    # A place that nothing follows is none.
    while parts and isinstance(parts[-1], Place):
        parts.pop()
    return text, parts


def cheapest(parts: list, widths: list[int], rest: int, penalty: int) -> int | None:
    """What the cheapest way of ending the lines costs. None if some line cannot be made to fit."""
    if not parts:
        return 0
    first = len(widths)

    def allowed(line: int) -> int:
        return widths[line] if line < first else rest

    widest = max(widths + [rest])
    # before[i] is the width of everything before part i, as it is where no
    # line ends: stretches, and the spaces between them.
    before = [0]
    for part in parts:
        before.append(before[-1] + part.width)

    # The places a line can end: each place, and the paragraph's end.
    ends = [index for index, part in enumerate(parts) if isinstance(part, Place)] + [len(parts)]
    # best[(where a line begins, how many lines came before, counted no further than `first`)] = cost so far.
    best: dict[tuple[int, int], int] = {(0, 0): 0}
    for end in ends:
        last = end == len(parts) or parts[end].kind == LINE_END
        extra = parts[end].extra if end < len(parts) else 0
        found: dict[int, int] = {}
        too_far_back = []
        for (begin, lines), cost in best.items():
            inside = before[end] - before[begin]
            if inside > widest:
                too_far_back.append((begin, lines))  # And further back still for every place after this.
                continue
            width = inside + extra
            if width > allowed(lines):
                continue
            gap = allowed(lines) - width
            total = cost + (0 if last else gap * gap)
            if end < len(parts) and parts[end].kind == HYPHEN:
                total += penalty
            after = min(lines + 1, first)
            if after not in found or total < found[after]:
                found[after] = total
        for key in too_far_back:
            del best[key]
        if last:
            best = {}  # Nothing reaches across a line end the text asks for.
        for lines, cost in found.items():
            best[(end + 1, lines)] = cost
        if not best:
            return None
    return min(best.values())


# ---- What bn-lines printed ----


class Line:
    def __init__(self, printed: str):
        fields = printed.split("\t")
        expect(len(fields) == 6, f"a line is not six fields: {printed!r}")
        self.begin, self.end, self.width, self.spaces = (int(field) for field in fields[:4])
        self.flags = "" if fields[4] == "-" else fields[4]
        self.text = fields[5]


def run(command: list, **more) -> subprocess.CompletedProcess:
    return subprocess.run([str(part) for part in command], capture_output=True, check=False, **more)


def lines_of(tool: Path, file: Path, options: list) -> list[list[Line]]:
    done = run([tool, file, *options])
    expect(done.returncode == 0, f"bn-lines failed on {file.name}: {done.stderr.decode(errors='replace')}")
    paragraphs: list[list[Line]] = [[]]
    for printed in done.stdout.decode().split("\n")[:-1]:
        if printed == "":
            paragraphs.append([])
        else:
            paragraphs[-1].append(Line(printed))
    expect(paragraphs[-1] == [], "bn-lines did not end a paragraph with an empty line")
    return paragraphs[:-1]


def styles_of(written: str) -> list[int]:
    """The style of each byte of a paragraph's text."""
    styles: list[int] = []
    style = 0
    for character in written:
        if character == "*":
            style = 1 - style
        else:
            styles += [style] * len(character.encode())
    return styles


def added_up(text: bytes, styles: list[int], begin: int, end: int, hyphen: bool) -> tuple[int, int, int]:
    """A line's width, its places between words and its characters, from its bytes alone."""
    on = text[begin:end].decode()
    width = spaces = characters = 0
    offset = begin
    last_style = 0
    in_space = False
    for character in on:
        style = styles[offset]
        offset += len(character.encode())
        if character in " \t\r":
            width += space_width(style)
            spaces += 0 if in_space else 1
            in_space = True
            continue
        if character in (SOFT_HYPHEN, ZERO_WIDTH_SPACE):
            continue  # It is nothing: spaces on either side of it are spaces in a row.
        in_space = False
        width += character_width(character, style)
        characters += 1
        last_style = style
    return width + (hyphen_width(last_style) if hyphen else 0), spaces, characters


def check_paragraph(what: str, written: str, lines: list[Line], language: Language | None, widths: list[int],
                    rest: int, penalty: int, fits: bool) -> None:
    text, parts = take_apart(written, language)
    data = text.encode()
    styles = styles_of(written)
    stretches = [part for part in parts if isinstance(part, Stretch)]

    # Every line is text of the paragraph that adds up.
    for number, line in enumerate(lines):
        expect(0 <= line.begin <= line.end <= len(data), f"{what}: line {number} is not in the text")
        shown = data[line.begin : line.end].decode() + ("-" if "h" in line.flags else "")
        expect(line.text == shown, f"{what}: line {number} is printed as {line.text!r}, and its bytes are {shown!r}")
        width, spaces, characters = added_up(data, styles, line.begin, line.end, "h" in line.flags)
        expect(line.width == width, f"{what}: line {number} is said to be {line.width} wide and adds up to {width}")
        expect(line.spaces == spaces, f"{what}: line {number} is said to have {line.spaces} spaces, and has {spaces}")
        allowed = widths[number] if number < len(widths) else rest
        expect(("w" in line.flags) == (line.width > allowed), f"{what}: line {number} is wrong about being too wide")
        expect("w" not in line.flags or characters <= 1,
               f"{what}: line {number} is too wide with {characters} characters on it")

    # Between them the lines hold every stretch once, in order.
    held = b"".join(data[line.begin : line.end] for line in lines)
    wanted = b"".join(data[stretch.begin : stretch.end] for stretch in stretches)

    def bare(some: bytes) -> bytes:
        for nothing in (" ", "\t", "\r", SOFT_HYPHEN, ZERO_WIDTH_SPACE):
            some = some.replace(nothing.encode(), b"")
        return some

    expect(bare(held) == bare(wanted), f"{what}: the lines do not hold the paragraph's words, each once and in order")
    if not fits:
        return

    # Each line is a run of whole stretches, and ends at a place; and the
    # lines cost what the cheapest way costs.
    begins = {stretch.begin: index for index, stretch in enumerate(parts) if isinstance(stretch, Stretch)}
    ends = {stretch.end: index for index, stretch in enumerate(parts) if isinstance(stretch, Stretch)}
    cost = 0
    expected_begin = 0
    for number, line in enumerate(lines):
        expect(line.begin in begins and line.end in ends, f"{what}: line {number} begins or ends inside a stretch")
        begin, end = begins[line.begin], ends[line.end] + 1
        expect(begin == expected_begin, f"{what}: line {number} does not begin where the line before ended")
        ended_at = parts[end] if end < len(parts) else None
        last = ended_at is None or ended_at.kind == LINE_END
        expect(("l" in line.flags) == last, f"{what}: line {number} is wrong about being a last line")
        expect(("h" in line.flags) == (ended_at is not None and ended_at.kind == HYPHEN),
               f"{what}: line {number} is wrong about showing a hyphen")
        allowed = widths[number] if number < len(widths) else rest
        if not last:
            cost += (allowed - line.width) ** 2
        if ended_at is not None and ended_at.kind == HYPHEN:
            cost += penalty
        expected_begin = end + 1
    expect(expected_begin == len(parts) + 1 or not parts, f"{what}: the lines stop before the paragraph does")
    least = cheapest(parts, widths, rest, penalty)
    expect(least is not None, f"{what}: the second reading finds no way to end the lines")
    expect(cost == least, f"{what}: the lines cost {cost}, and the cheapest way costs {least}")


# ---- Paragraphs made up at random ----


def made_up_word(rng: random.Random, most: int) -> str:
    word = ""
    for _ in range(rng.randint(1, most)):
        chance = rng.randrange(40)
        if chance < 30:
            letter = rng.choice(LETTERS)
            word += letter.upper() if rng.randrange(12) == 0 else letter
        elif chance < 34:
            word += rng.choice(OTHERS)
        elif chance < 36:
            word += rng.choice(DASHES)
        elif chance < 37:
            word += "--"
        elif chance < 38:
            word += SOFT_HYPHEN
        elif chance < 39:
            word += ZERO_WIDTH_SPACE
        else:
            word += "*"
    return word


def made_up_paragraph(rng: random.Random, longest_word: int) -> str:
    """A paragraph as it goes into the file: lines of text, none of them empty."""
    lines = [""]
    for _ in range(rng.randint(1, 70)):
        word = made_up_word(rng, longest_word)
        if rng.randrange(10) == 0:
            word = "*" + word + "*"
        lines[-1] += word
        chance = rng.randrange(20)
        if chance == 0 and lines[-1].replace("*", "").strip():
            lines.append("")
        elif chance == 1:
            lines[-1] += "   "
        else:
            lines[-1] += " "
    # No line of the file may be empty, or it would end the paragraph; and
    # one of asterisks alone would be a line end with nothing on it.
    lines = [line for line in lines if line.replace("*", "").strip(" ")]
    return "\n".join(lines) if lines else "a"


def check_made_up(tool: Path, hyphen_tool: Path, work: Path, seed: int) -> int:
    rng = random.Random(seed)
    language = Language(rng) if seed % 4 != 0 else None
    options: list = ["--nodes", 8192, "--records", 16384]
    if language is not None:
        patterns = work / f"patterns-{seed}.txt"
        table = work / f"table-{seed}.bnhy"
        patterns.write_text("\n".join(language.written) + "\n", encoding="utf-8")
        packed = run([hyphen_tool, "pack", patterns, "--out", table, "--left", language.left, "--right",
                      language.right])
        expect(packed.returncode == 0, f"seed {seed}: bn-hyphen would not pack the patterns: {packed.stderr.decode()}")
        options += ["--table", table]

    rest = rng.randint(1100, 2400)
    widths = [rng.randint(1000, rest) for _ in range(rng.randrange(4))] if seed % 3 == 0 else []
    penalty = rng.choice([0, 0, 400, 12100, 250000])
    options += ["--width", rest, "--penalty", penalty]
    if widths:
        options += ["--first", ",".join(str(width) for width in widths)]

    paragraphs = [made_up_paragraph(rng, 14) for _ in range(12)]
    file = work / f"text-{seed}.txt"
    file.write_text("\n\n".join(paragraphs) + "\n", encoding="utf-8")

    printed = lines_of(tool, file, options)
    expect(len(printed) == len(paragraphs), f"seed {seed}: {len(printed)} paragraphs came of {len(paragraphs)}")
    for number, (written, lines) in enumerate(zip(paragraphs, printed)):
        check_paragraph(f"seed {seed}, paragraph {number}", written, lines, language, widths, rest, penalty, True)

    # What it says the lines were.
    figures = run([tool, file, *options, "--figures"])
    expect(figures.returncode == 0, f"seed {seed}: bn-lines --figures failed")
    said = figures.stdout.decode()
    count = sum(len(lines) for lines in printed)
    hyphens = sum(1 for lines in printed for line in lines if "h" in line.flags)
    expect(f"paragraphs {len(paragraphs)}, " in said and f" lines {count}, " in said,
           f"seed {seed}: the figures do not count the paragraphs and lines: {said}")
    expect(f"lines with a hyphen shown {hyphens}, too wide 0\n" in said,
           f"seed {seed}: the figures do not count the hyphens: {said}")
    expect("lines given out early 0, in 0 paragraphs; ended for want of records 0\n" in said,
           f"seed {seed}: lines were given out early with memory to spare: {said}")

    # The same text on lines too narrow for some of its words, and with
    # little memory: nothing is promised of the cost, only that every line
    # is text of the paragraph that adds up.
    narrow = rng.randint(20, 400)
    cramped = ["--width", narrow, "--nodes", 32, "--records", 64, "--penalty", penalty]
    if language is not None:
        cramped += ["--table", work / f"table-{seed}.bnhy"]
    printed = lines_of(tool, file, cramped)
    for number, (written, lines) in enumerate(zip(paragraphs, printed)):
        check_paragraph(f"seed {seed} at {narrow}, paragraph {number}", written, lines, language, [], narrow, penalty,
                        False)
    return count


def check_known(tool: Path, work: Path) -> None:
    """A paragraph whose lines can be said beforehand."""
    file = work / "known.txt"
    file.write_text(
        "aaaa bbb cc dddddddd ee\n\nopen-air aaaa mmm­iiii *wide*\nlast\n\nxxxx open-air\n", encoding="utf-8"
    )
    printed = lines_of(tool, file, ["--width", 350])
    shown = [[line.text for line in lines] for lines in printed]
    # Filled in turn, the first paragraph's second line would be "cc" alone.
    expect(shown[0] == ["aaaa", "bbb cc", "dddddddd", "ee"], f"the known paragraph came out as {shown[0]}")
    # A soft hyphen where the line ends is shown as a hyphen, and is in
    # neither line's bytes; the one further in is in its line's.
    expect(shown[1] == ["open-air", "aaaa mmm-", "iiii wide", "last"],
           f"the second known paragraph came out as {shown[1]}")
    second = printed[1]
    expect([line.flags for line in second] == ["", "h", "l", "l"], "the known lines are not marked as what they are")
    expect((second[1].begin, second[1].end, second[2].begin) == (9, 17, 19), "the known lines are not where they are")
    expect(second[1].width == 144 + 18 + 162 + 22, f"the line with a hyphen shown is {second[1].width} wide")
    # "iiii", a space, and "wide" in the second style, each character 4 wider.
    expect(second[2].width == 72 + 18 + (58 + 22 + 40 + 40), f"the styled line is {second[2].width} wide")
    expect(second[2].spaces == 1, "the styled line's space is not counted")
    # A line may end after a hyphen that is written, and shows no other.
    expect(shown[2] == ["xxxx open-", "air"], f"the third known paragraph came out as {shown[2]}")
    expect([line.flags for line in printed[2]] == ["", "l"], "a hyphen that is written is taken for one shown")


def check_refusals(tool: Path, work: Path) -> None:
    file = work / "known.txt"
    missing = run([tool, work / "no-such-file.txt"])
    expect(missing.returncode == 1 and b"cannot read" in missing.stderr, "a missing file was not refused")
    not_a_table = work / "not-a-table.bnhy"
    not_a_table.write_bytes(b"BNHY and then nothing a table has")
    refused = run([tool, file, "--table", not_a_table])
    expect(refused.returncode == 1 and b"bad table" in refused.stderr, "what is not a table was not refused")
    no_width = run([tool, file, "--width", 0])
    expect(no_width.returncode == 1 and b"bad widths" in no_width.stderr, "a width of nothing was not refused")
    no_memory = run([tool, file, "--nodes", 8])
    expect(no_memory.returncode == 1 and b"no memory" in no_memory.stderr, "too few nodes were not refused")
    unknown = run([tool, file, "--no-such-option"])
    expect(unknown.returncode == 1 and b"usage:" in unknown.stderr, "an option it does not have was not refused")
    from_input = run([tool, "--width", 350], input="aaaa bbb cc dddddddd ee\n".encode())
    expect(from_input.returncode == 0 and from_input.stdout.count(b"\n") == 5, "text it was given to read was not set")


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    tool, hyphen_tool, work = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        check_known(tool, work)
        check_refusals(tool, work)
        lines = sum(check_made_up(tool, hyphen_tool, work, seed) for seed in range(1, 41))
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"bn-lines agrees with the second reading: 480 paragraphs, {lines} lines at their full width")
    return 0


if __name__ == "__main__":
    sys.exit(main())
