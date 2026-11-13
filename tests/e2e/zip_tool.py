#!/usr/bin/env python3
"""End-to-end check of bn-zip, and through it of the ZIP reader, run by CTest.

The unit tests make their ZIP files with a writer of their own, which could
share a misunderstanding of the format with the reader. The ZIP files here
are made by code that has nothing to do with either: Python's zipfile
module, and the `zip` command where the computer has one. What bn-zip says
of each file is compared with what zipfile says of it, entry by entry and
byte by byte.

usage: zip_tool.py BN_ZIP WORK_DIR
"""

from __future__ import annotations

import random
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

WORDS = ["the ", "reader ", "turned ", "a ", "page, ", "and ", "then ", "another; ", "night ", "fell ", "quietly.\n"]


def text(size: int, seed: int) -> bytes:
    """Bytes that compress well."""
    rng = random.Random(seed)
    out = bytearray()
    while len(out) < size:
        out += rng.choice(WORDS).encode()
    return bytes(out[:size])


def noise(size: int, seed: int) -> bytes:
    """Bytes that do not compress at all."""
    return random.Random(seed).getrandbits(8 * size).to_bytes(size, "little") if size else b""


class WriteOnly:
    """A file that can only be written from start to end, like a pipe.

    Given one of these, zipfile cannot go back to fill in an entry's sizes,
    so it puts them after the entry's data instead.
    """

    def __init__(self, file):
        self.file = file

    def write(self, data):
        return self.file.write(data)

    def flush(self):
        self.file.flush()


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def run(tool: str, *arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run([tool, *arguments], capture_output=True, check=False)


def method_name(method: int) -> str:
    return {0: "stored", 8: "deflated"}.get(method, f"method-{method}")


def name_bytes(info: zipfile.ZipInfo) -> bytes:
    """An entry's name as the bytes in the file."""
    return info.filename.encode("utf-8" if info.flag_bits & 0x800 else "cp437")


def unescaped(name: bytes) -> bytes:
    return re.sub(rb"\\x([0-9A-F]{2})", lambda match: bytes([int(match.group(1), 16)]), name)


def listed(tool: str, path: Path) -> list[tuple]:
    """What `bn-zip list` says: (checksum, size, size in the file, method, flags, name) for each entry."""
    result = run(tool, "list", str(path))
    expect(result.returncode == 0, f"{path.name}: list ended with {result.returncode}: {result.stderr!r}")
    entries = []
    for line in result.stdout.splitlines():
        crc, size, compressed, method, flags, name = line.split(None, 5)
        entries.append((int(crc, 16), int(size), int(compressed), method.decode(), int(flags, 16), unescaped(name)))
    return entries


def compare(tool: str, path: Path, cat_limit: int = 40) -> int:
    """Checks everything bn-zip says of the ZIP file at `path`. Returns how many entries it has."""
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        expected = [
            (info.CRC, info.file_size, info.compress_size, method_name(info.compress_type), info.flag_bits,
             name_bytes(info))
            for info in infos
        ]
        got = listed(tool, path)
        expect(len(got) == len(expected), f"{path.name}: {len(got)} entries listed, {len(expected)} expected")
        for index, (mine, theirs) in enumerate(zip(got, expected)):
            expect(mine == theirs, f"{path.name}: entry {index}: listed {mine}, expected {theirs}")

        result = run(tool, "check", str(path))
        expect(result.returncode == 0, f"{path.name}: check ended with {result.returncode}: {result.stdout!r}")
        expect(f"{len(infos)} entries, 0 failed".encode() in result.stdout, f"{path.name}: check said {result.stdout!r}")

        # Entries read out whole: the first few, the last, and the biggest.
        chosen = list(infos[:cat_limit]) + infos[-1:] + sorted(infos, key=lambda info: info.file_size)[-2:]
        for info in chosen:
            result = run(tool, "cat", str(path), name_bytes(info).decode("utf-8", "surrogateescape"))
            expect(result.returncode == 0, f"{path.name}: cat {info.filename!r} ended with {result.returncode}")
            expect(result.stdout == archive.read(info), f"{path.name}: cat {info.filename!r} gave the wrong bytes")
    return len(infos)


def make_book(path: Path) -> None:
    """Laid out as an EPUB is, with something of every ordinary kind."""
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
        archive.writestr("META-INF/container.xml", text(400, 1), zipfile.ZIP_DEFLATED)
        for level in (1, 6, 9):
            archive.writestr(f"OEBPS/chapter{level}.xhtml", text(40_000 + level, level), zipfile.ZIP_DEFLATED,
                             compresslevel=level)
        archive.writestr("OEBPS/empty.css", b"", zipfile.ZIP_DEFLATED)
        archive.writestr("OEBPS/also-empty.css", b"")
        archive.writestr(zipfile.ZipInfo("OEBPS/images/"), b"")
        archive.writestr("OEBPS/images/cover.jpg", noise(30_000, 2))
        archive.writestr("OEBPS/images/noise-deflated.png", noise(100_000, 3), zipfile.ZIP_DEFLATED)
        archive.writestr("OEBPS/long.xhtml", text(400_000, 4), zipfile.ZIP_DEFLATED)
        archive.writestr("OEBPS/café/naïve — 文.xhtml", text(3_000, 5), zipfile.ZIP_DEFLATED)
        archive.writestr("OEBPS/" + "n" * 240 + ".xhtml", text(100, 6), zipfile.ZIP_DEFLATED)
        archive.comment = b"made by zipfile"


def make_streamed(path: Path) -> None:
    """Written to something that cannot be gone back over: the sizes follow each entry's data."""
    with open(path, "wb") as file, zipfile.ZipFile(WriteOnly(file), "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("first.txt", text(20_000, 7))
        archive.writestr("second.bin", noise(5_000, 8), zipfile.ZIP_STORED)
        archive.writestr("empty", b"")
        with archive.open("written-in-pieces.txt", "w") as entry:
            for piece in range(50):
                entry.write(text(1_000, 100 + piece))


def make_wide_headers(path: Path) -> None:
    """Entries whose own headers are in the ZIP64 form though nothing in the file needs it."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        with archive.open(zipfile.ZipInfo("wide.txt"), "w", force_zip64=True) as entry:
            entry.write(text(10_000, 9))
        archive.writestr("ordinary.txt", text(2_000, 10))


def make_many(path: Path) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for index in range(3_000):
            archive.writestr(f"OEBPS/text/part-{index:05d}.xhtml", text(40 + (index % 90), index))


def make_long_comment(path: Path) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("only.txt", text(5_000, 11))
        archive.comment = text(40_000, 12)


def make_with_zip_command(work: Path, path: Path) -> bool:
    """Made by the `zip` command, which gives each entry extra fields of its own."""
    if shutil.which("zip") is None:
        return False
    source = work / "for-zip"
    (source / "OEBPS" / "images").mkdir(parents=True)
    (source / "mimetype").write_bytes(b"application/epub+zip")
    (source / "OEBPS" / "chapter.xhtml").write_bytes(text(50_000, 13))
    (source / "OEBPS" / "images" / "cover.jpg").write_bytes(noise(20_000, 14))
    (source / "OEBPS" / "empty.css").write_bytes(b"")
    # The way an EPUB is made by hand: the mimetype first and not compressed, then the rest.
    subprocess.run(["zip", "-q", "-X", "-0", str(path), "mimetype"], cwd=source, check=True)
    subprocess.run(["zip", "-q", "-r", "-9", str(path), "OEBPS"], cwd=source, check=True)
    return True


def check_unsupported(tool: str, work: Path) -> None:
    """Entries compressed in ways an EPUB may not use are refused, one by one."""
    methods = []
    for name, method in (("bz2", zipfile.ZIP_BZIP2), ("lzma", zipfile.ZIP_LZMA)):
        try:
            __import__(name)
            methods.append((name, method))
        except ImportError:
            pass
    path = work / "other-methods.zip"
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("good.txt", text(3_000, 15), zipfile.ZIP_DEFLATED)
        for name, method in methods:
            archive.writestr(f"{name}.txt", text(3_000, 16), method)
        archive.writestr("also-good.txt", text(3_000, 17))
    listing = {entry[5]: entry[3] for entry in listed(tool, path)}
    expect(listing[b"good.txt"] == "deflated", "good.txt should be listed as deflated")
    result = run(tool, "check", str(path))
    expect(result.returncode == (1 if methods else 0), f"other-methods.zip: check ended with {result.returncode}")
    expect(f"{len(methods) + 2} entries, {len(methods)} failed".encode() in result.stdout,
           f"other-methods.zip: check said {result.stdout!r}")
    for name, _ in methods:
        expect(f"FAILED  unsupported: {name}.txt".encode() in result.stdout, f"{name}.txt should be refused")
        result_cat = run(tool, "cat", str(path), f"{name}.txt")
        expect(result_cat.returncode == 1 and result_cat.stdout == b"", f"cat {name}.txt should fail with no output")
    expect(run(tool, "cat", str(path), "also-good.txt").stdout == text(3_000, 17), "also-good.txt should still read")


def check_refusals(tool: str, work: Path, book: Path) -> None:
    """Files that are not ZIP files, and a ZIP file that has been damaged."""
    plain = work / "plain.txt"
    plain.write_bytes(text(10_000, 18))
    result = run(tool, "list", str(plain))
    expect(result.returncode == 1 and b"not a zip file" in result.stderr, f"plain.txt: {result.stderr!r}")

    whole = book.read_bytes()
    cut = work / "cut.zip"
    cut.write_bytes(whole[:-30])
    expect(run(tool, "check", str(cut)).returncode == 1, "a ZIP file cut short should be refused")

    expect(run(tool, "list", str(work / "no-such-file.zip")).returncode == 1, "a missing file should be refused")

    result = run(tool, "cat", str(book), "no/such/entry")
    expect(result.returncode == 1 and b"not found" in result.stderr, f"a missing entry: {result.stderr!r}")
    expect(run(tool, "cat", str(book), "MIMETYPE").returncode == 1, "names are matched exactly")

    # One byte changed in the middle of an entry's data: that entry fails, and only that one.
    with zipfile.ZipFile(book) as archive:
        info = archive.getinfo("OEBPS/long.xhtml")
        count = len(archive.infolist())
    damaged = bytearray(whole)
    damaged[info.header_offset + 30 + len(name_bytes(info)) + info.compress_size // 2] ^= 0x55
    spoiled = work / "spoiled.zip"
    spoiled.write_bytes(damaged)
    result = run(tool, "check", str(spoiled))
    expect(result.returncode == 1, "a damaged entry should fail the check")
    expect(f"{count} entries, 1 failed".encode() in result.stdout, f"spoiled.zip: check said {result.stdout!r}")
    expect(b"OEBPS/long.xhtml" in result.stdout, "the damaged entry should be named")
    expect(run(tool, "cat", str(spoiled), "OEBPS/long.xhtml").returncode == 1, "and reading it out should fail")
    expect(run(tool, "cat", str(spoiled), "mimetype").stdout == b"application/epub+zip", "the others still read")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tool = str(Path(sys.argv[1]).resolve())
    work = Path(sys.argv[2]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    try:
        makers = [
            ("book.zip", make_book),
            ("streamed.zip", make_streamed),
            ("wide-headers.zip", make_wide_headers),
            ("many.zip", make_many),
            ("long-comment.zip", make_long_comment),
        ]
        entries = 0
        for name, make in makers:
            make(work / name)
            entries += compare(tool, work / name)
        files = len(makers)
        used_zip = make_with_zip_command(work, work / "zip-command.zip")
        if used_zip:
            entries += compare(tool, work / "zip-command.zip")
            files += 1
        check_unsupported(tool, work)
        check_refusals(tool, work, work / "book.zip")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"{files} ZIP files, {entries} entries: bn-zip agrees with zipfile about all of them"
          + ("" if used_zip else " (no zip command here, so none was made with it)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
