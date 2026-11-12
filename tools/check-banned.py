#!/usr/bin/env python3
"""Fails if the portable parts use constructs that are not allowed in them.

The portable parts (core, epub, draw, layout) run on a device with about
200 KB of heap and no exceptions. They allocate from fixed-size arenas, not
the general heap, and they reach files only through the interfaces in core.
The compiler cannot enforce that, so this script does:

  - std::string, std::vector, std::function, std::shared_ptr: each allocates
    from the general heap, at sizes that are decided at run time.
  - iostreams: heap, locales and static initialisers; large in flash.
  - C stdio streams (fopen, printf, FILE* and friends): file access belongs
    to the storage interface, and the device C library's buffered stream
    output has a heap-corrupting bug. Formatting into a caller's buffer
    (snprintf) is fine and is not flagged.

Comments and string literals are ignored, so code may still talk about these
names. The reader application, the hardware layer, tools and tests are not
checked.

Usage: tools/check-banned.py [ROOT]    (ROOT defaults to the repository)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

PARTS = ("core", "epub", "draw", "layout")
SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp", ".inl"}

HEAP = "allocates from the general heap; use an arena or a fixed-capacity container"
STREAMS = "iostreams are not available to the portable parts"
STDIO = "C stdio streams are not available to the portable parts; use the storage interface"

RULES: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"#\s*include\s*<string>"), f"<string>: std::string {HEAP}"),
    (re.compile(r"\bstd::(?:basic_|w|u8|u16|u32)?string\b(?!_view)"), f"std::string {HEAP}"),
    (re.compile(r"#\s*include\s*<vector>"), f"<vector>: std::vector {HEAP}"),
    (re.compile(r"\bstd::vector\b"), f"std::vector {HEAP}"),
    (re.compile(r"\bstd::function\b"), f"std::function {HEAP}"),
    (re.compile(r"\bstd::(?:shared_ptr|make_shared|allocate_shared|weak_ptr)\b"), f"std::shared_ptr {HEAP}"),
    (re.compile(r"#\s*include\s*<(?:iostream|fstream|sstream|istream|ostream|iomanip)>"), STREAMS),
    (re.compile(r"\bFILE\s*\*"), STDIO),
    (
        re.compile(
            r"\b(?:fopen|freopen|fdopen|fclose|fread|fwrite|fgetc|fgets|fputc|fputs|getc|putc|getchar|putchar|"
            r"puts|printf|fprintf|vprintf|vfprintf|scanf|fscanf|fflush|fseek|ftell|rewind)\s*\("
        ),
        STDIO,
    ),
]


def strip_comments_and_literals(text: str) -> str:
    """Blanks out comments and string/char literals, keeping line structure."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif ch == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("\n" * text.count("\n", i, end))
            i = end
        elif ch in "\"'":
            quote = ch
            out.append(quote)
            i += 1
            while i < n and text[i] != quote and text[i] != "\n":
                i += 2 if text[i] == "\\" else 1
            if i < n and text[i] == quote:
                out.append(quote)
                i += 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def check_file(path: Path, root: Path) -> list[str]:
    findings = []
    code = strip_comments_and_literals(path.read_text(encoding="utf-8", errors="replace"))
    for number, line in enumerate(code.splitlines(), start=1):
        for pattern, message in RULES:
            if pattern.search(line):
                findings.append(f"{path.relative_to(root)}:{number}: {message}")
    return findings


def main() -> int:
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
    findings: list[str] = []
    checked = 0
    for part in PARTS:
        for path in sorted((root / part).rglob("*")):
            if path.is_file() and path.suffix in SUFFIXES:
                checked += 1
                findings.extend(check_file(path, root))
    for finding in findings:
        print(finding)
    if findings:
        print(f"\n{len(findings)} banned construct(s) in the portable parts.", file=sys.stderr)
        return 1
    print(f"{checked} files checked, no banned constructs.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
