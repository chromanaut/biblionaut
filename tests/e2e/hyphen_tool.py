#!/usr/bin/env python3
"""End-to-end check of bn-hyphen, and through it of the hyphenator and the tables it reads, run by CTest.

The unit tests say what the hyphenator should make of tables written for
them. Here bn-hyphen is set beside a second way of breaking the same words
that shares none of its code: the patterns kept in a dictionary as they were
written, and every piece of a word looked up in it.

For each language, made up at random, with letters of one byte and of
several, patterns of every shape, words written out with their breaks, and
sometimes a list of the letters' other forms:

  - `bn-hyphen pack` has to make a table of it, and say how much went in;
  - `bn-hyphen words` has to break every word as the second way does: words
    of the language's letters, in capitals and small letters, with other
    characters among them, and longer than it looks at;
  - `bn-hyphen words --count` has to count what it broke;
  - `bn-hyphen info` has to say what the table was made with.

It also checks a word whose breaks are known, and that what is not a set of
patterns, and what is not a table, are refused.

usage: hyphen_tool.py BN_HYPHEN WORK_DIR
"""

from __future__ import annotations

import random
import shutil
import subprocess
import sys
from pathlib import Path

LONGEST_WORD = 63  # Characters: a longer word is given no places.

# Letters a made-up language may have: of one byte, two, three and four.
LETTERS = "abcdefg" + "éñöж" + "λ" + "ḉ" + "𝒶"
# Characters that are never letters of one.
OTHERS = "0123456789'-—!ŧ€"


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


class Language:
    """A language as it is handed to bn-hyphen, and as the second way keeps it."""

    def __init__(self, rng: random.Random):
        self.letters = rng.sample(LETTERS, rng.randint(2, 6))
        self.left = rng.randint(1, 3)
        self.right = rng.randint(1, 3)

        # The patterns as they are written, and as letters -> numbers.
        self.pattern_text: list[str] = []
        self.patterns: dict[str, list[int]] = {}
        for _ in range(rng.randint(0, 80)):
            letters = "".join(rng.choice(self.letters) for _ in range(rng.randint(1, 6)))
            if rng.randrange(6) == 0:
                letters = "." + letters
            if rng.randrange(6) == 0:
                letters = letters + "."
            numbers = [rng.randrange(10) if rng.randrange(3) == 0 else 0 for _ in range(len(letters) + 1)]
            if letters.startswith("."):
                numbers[0] = 0  # Nothing is written before the mark for a word's start,
            if letters.endswith("."):
                numbers[-1] = 0  # or after the one for its end.
            written = "".join((str(number) if number else "") + letter for number, letter in zip(numbers, letters))
            written += str(numbers[-1]) if numbers[-1] else ""
            self.pattern_text.append(written)
            # The same pattern twice says the larger of its numbers at each place.
            before = self.patterns.get(letters, [0] * len(numbers))
            self.patterns[letters] = [max(a, b) for a, b in zip(before, numbers)]

        # Words written out with their breaks: the word -> how many letters come before each break.
        self.exception_text: list[str] = []
        self.exceptions: dict[str, set[int]] = {}
        for _ in range(rng.randrange(6)):
            word = "".join(rng.choice(self.letters) for _ in range(rng.randint(2, 8)))
            if word in self.exceptions:
                continue
            breaks = {before for before in range(1, len(word)) if rng.randrange(3) == 0}
            self.exceptions[word] = breaks
            self.exception_text.append("".join(("-" if i in breaks else "") + letter for i, letter in enumerate(word)))

        # The language's letters are the ones its patterns and those words
        # use, and any more its list of letters names. The characters that
        # stand for them: each letter for itself, and its other forms. With
        # no list, a to z have their capitals.
        used = sorted({letter for text in list(self.patterns) + list(self.exceptions) for letter in text} - {"."})
        self.unused = [letter for letter in self.letters if letter not in used]
        self.forms = {letter: letter for letter in used}
        self.letter_lines: list[str] | None = None
        if rng.randrange(2) == 0:
            self.letter_lines = []
            for letter in used:
                capital = letter.upper()
                if len(capital) == 1 and capital != letter and rng.randrange(4) != 0:
                    self.forms[capital] = letter
                    self.letter_lines.append(letter + capital)
            # A letter of the language that no pattern has.
            spare = [letter for letter in LETTERS if letter not in self.letters]
            if spare and rng.randrange(2) == 0:
                extra = rng.choice(spare)
                self.forms[extra] = extra
                self.letter_lines.append(extra)
        else:
            for letter in used:
                if "a" <= letter <= "z":
                    self.forms[letter.upper()] = letter

    def places_in(self, stretch: str) -> set[int]:
        """Where a stretch of letters may be broken: how many letters come before each break."""
        if stretch in self.exceptions:
            places = set(self.exceptions[stretch])
        else:
            marked = "." + stretch + "."
            levels = [0] * (len(marked) + 1)
            for start in range(len(marked)):
                for end in range(start + 1, len(marked) + 1):
                    numbers = self.patterns.get(marked[start:end])
                    if numbers:
                        for place, number in enumerate(numbers):
                            levels[start + place] = max(levels[start + place], number)
            places = {before for before in range(1, len(stretch)) if levels[before + 1] % 2 == 1}
        return {before for before in places if before >= self.left and len(stretch) - before >= self.right}

    def broken(self, word: str) -> str:
        """The word with a - at every place it may be broken."""
        if len(word) > LONGEST_WORD:
            return word
        letters = [self.forms.get(character) for character in word]
        breaks: set[int] = set()
        start = 0
        while start < len(word):
            if letters[start] is None:
                start += 1
                continue
            end = start
            while end < len(word) and letters[end] is not None:
                end += 1
            breaks |= {start + before for before in self.places_in("".join(letters[start:end]))}
            start = end
        return "".join(("-" if i in breaks else "") + character for i, character in enumerate(word))

    def words(self, rng: random.Random, count: int) -> list[str]:
        # A language with no patterns has no letters; its words are then of
        # characters that are none.
        shapes = list(self.forms) or list(OTHERS)
        others = list(OTHERS) + self.unused
        found = []
        for _ in range(count):
            kind = rng.randrange(10)
            if kind == 0 and self.exceptions:
                word = rng.choice(sorted(self.exceptions))
                if rng.randrange(2) == 0:
                    word = rng.choice(others) + word + rng.choice(others)
            elif kind == 1:
                # With characters among its letters that are none.
                word = "".join(rng.choice(shapes + others) for _ in range(rng.randint(1, 30)))
            elif kind == 2:
                # About as long as a word that is looked at can be, and longer.
                word = "".join(rng.choice(shapes) for _ in range(rng.randint(LONGEST_WORD - 2, LONGEST_WORD + 2)))
            else:
                word = "".join(rng.choice(shapes) for _ in range(rng.randint(1, 24)))
            found.append(word)
        return found


def run(tool: str, *arguments: str, given: str | None = None) -> subprocess.CompletedProcess:
    return subprocess.run([tool, *arguments], input=None if given is None else given.encode("utf-8"),
                          capture_output=True, check=False)


def pack(tool: str, work: Path, name: str, language: Language) -> Path:
    patterns = work / f"{name}.pat.txt"
    lines = [f"% Patterns of {name}, made up."]
    for i, pattern in enumerate(language.pattern_text):
        # Several to a line, and now and then something after a % sign.
        lines.append(pattern + (" % not a pattern" if i % 7 == 3 else ""))
    patterns.write_text("\n".join(lines[:1]) + "\n" + " ".join(lines[1:3]) + "\n" + "\n".join(lines[3:]) + "\n",
                        encoding="utf-8")
    arguments = ["pack", str(patterns), "--left", str(language.left), "--right", str(language.right)]
    if language.exception_text:
        exceptions = work / f"{name}.hyp.txt"
        exceptions.write_text("\n".join(language.exception_text) + "\n", encoding="utf-8")
        arguments += ["--exceptions", str(exceptions)]
    if language.letter_lines is not None:
        letters = work / f"{name}.chr.txt"
        letters.write_text("\n".join(language.letter_lines) + "\n", encoding="utf-8")
        arguments += ["--letters", str(letters)]
    table = work / f"{name}.bnhy"
    result = run(tool, *arguments, "--out", str(table))
    expect(result.returncode == 0, f"{name}: bn-hyphen pack ended with {result.returncode}: {result.stderr!r}")
    said = result.stdout.decode("utf-8").splitlines()
    expect(said[0] == f"patterns {len(language.pattern_text)}, exceptions {len(language.exception_text)}",
           f"{name}: pack says {said[0]!r}")
    known = len({form for form in language.forms.values()})
    expect(said[1] == f"letters {known}, as {len(language.forms)} characters", f"{name}: pack says {said[1]!r}")
    expect(said[-1] == f"table {table.stat().st_size} bytes", f"{name}: pack says {said[-1]!r}")
    return table


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tool = sys.argv[1]
    work = Path(sys.argv[2])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    languages = 0
    words_in_all = 0
    places_in_all = 0
    try:
        # A word whose breaks are known: the one in the title of the thesis
        # that set the method out, by the patterns of TeX's that it meets.
        known = work / "known.pat.txt"
        known.write_text("hy3ph he2n hena4 hen5at\n1na n2at 1tio 2io o2n\n", encoding="utf-8")
        table = work / "known.bnhy"
        result = run(tool, "pack", str(known), "--out", str(table))
        expect(result.returncode == 0, f"known: pack ended with {result.returncode}: {result.stderr!r}")
        result = run(tool, "words", str(table), given="hyphenation\nHyphenation\nhyphen\n")
        expect(result.stdout.decode("utf-8") == "hy-phen-ation\nHy-phen-ation\nhy-phen\n",
               f"known: bn-hyphen says {result.stdout!r}")

        for seed in range(150):
            rng = random.Random(seed)
            name = f"language{seed}"
            language = Language(rng)
            table = pack(tool, work, name, language)

            words = language.words(rng, 120) + [""]
            expected = [language.broken(word) for word in words]
            listed = work / f"{name}.words.txt"
            listed.write_text("".join(word + "\n" for word in words), encoding="utf-8")
            # From a file, and one language in ten, from what it is given to read.
            if seed % 10 == 0:
                result = run(tool, "words", str(table), given="".join(word + "\r\n" for word in words))
            else:
                result = run(tool, "words", str(table), str(listed))
            expect(result.returncode == 0, f"{name}: bn-hyphen words ended with {result.returncode}: {result.stderr!r}")
            said = result.stdout.decode("utf-8").split("\n")
            expect(said[-1] == "" and len(said) == len(words) + 1, f"{name}: {len(said) - 1} lines for {len(words)} words")
            for word, want, got in zip(words, expected, said):
                expect(got == want, f"{name}: {word!r} should be {want!r}, and bn-hyphen says {got!r}\n"
                                    f"  patterns {language.pattern_text}\n  exceptions {language.exception_text}\n"
                                    f"  letters {language.letter_lines}, left {language.left}, right {language.right}")

            places = sum(len(want) - len(word) for word, want in zip(words, expected))
            broken = sum(1 for word, want in zip(words, expected) if want != word)
            result = run(tool, "words", str(table), str(listed), "--count")
            first = result.stdout.decode("utf-8").splitlines()[0]
            expect(first == f"words {len(words)}, with a place to break {broken}, places {places}",
                   f"{name}: bn-hyphen words --count says {first!r}")

            result = run(tool, "info", str(table))
            said = result.stdout.decode("utf-8").splitlines()
            expect(result.returncode == 0 and said[0] == f"table {table.stat().st_size} bytes, version 1",
                   f"{name}: bn-hyphen info says {result.stdout!r}")
            expect(said[1] == f"letters before a break {language.left}, after one {language.right}",
                   f"{name}: bn-hyphen info says {said[1]!r}")
            expect(said[2] == f"characters that are letters {len(language.forms)}",
                   f"{name}: bn-hyphen info says {said[2]!r}")

            languages += 1
            words_in_all += len(words)
            places_in_all += places

        # What is not a set of patterns.
        for name, text, complaint in [
            ("two-numbers", "1a\na12b\n", "patterns, line 2: two numbers at one place in a pattern"),
            ("dot-inside", "a.b\n", "patterns, line 1: a `.` that is not at an end of its pattern"),
            ("no-letters", "1a 2\n", "patterns, line 1: a pattern with no letters"),
            ("not-utf8", None, "patterns, line 1: not UTF-8"),
        ]:
            path = work / f"{name}.pat.txt"
            path.write_bytes(b"1a \xff\n" if text is None else text.encode("utf-8"))
            result = run(tool, "pack", str(path), "--out", str(work / f"{name}.bnhy"))
            expect(result.returncode == 1 and result.stderr.decode("utf-8").strip() == f"bn-hyphen: {complaint}",
                   f"{name}: bn-hyphen pack ended with {result.returncode} and said {result.stderr!r}")
            expect(not (work / f"{name}.bnhy").exists(), f"{name}: a table was written all the same")

        # What is not a table: the patterns themselves, and a table cut short.
        result = run(tool, "words", str(known), given="hyphenation\n")
        expect(result.returncode == 1 and result.stderr.decode("utf-8").strip().endswith(": bad table"),
               f"patterns as a table: ended with {result.returncode} and said {result.stderr!r}")
        short = work / "short.bnhy"
        short.write_bytes((work / "known.bnhy").read_bytes()[:-1])
        for command in ("words", "info"):
            result = run(tool, command, str(short), given="hyphenation\n")
            expect(result.returncode == 1 and result.stderr.decode("utf-8").strip().endswith(": bad table"),
                   f"a table cut short, {command}: ended with {result.returncode} and said {result.stderr!r}")

        # And what it is asked that it does not do.
        expect(run(tool).returncode == 1, "with nothing asked of it")
        expect(run(tool, "pack", str(known)).returncode == 1, "pack with nowhere to write")
        expect(run(tool, "pack", str(known), "--out", str(work / "x.bnhy"), "--left", "0").returncode == 1,
               "pack with no letters before a break")
        expect(run(tool, "words", str(work / "no-such-table.bnhy")).returncode == 1, "a table that is not there")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"{languages} made-up languages, {words_in_all} words, {places_in_all} places to break: bn-hyphen agrees "
          f"with looking every piece of every word up among the patterns")
    return 0


if __name__ == "__main__":
    sys.exit(main())
