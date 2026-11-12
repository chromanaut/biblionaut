#!/usr/bin/env python3
"""Runs clang-tidy on our code as the firmware build compiles it.

The host build's clang-tidy run (`cmake --build --preset tidy`) covers the
portable parts, the desktop port, the tools and the tests, compiled for the
machine it runs on. This covers what that leaves out, by the same rules
(.clang-tidy):

  - the device code, which only the firmware build compiles: hw/esp32c3, the
    per-device directories next to it, and the firmware's entry point under
    app/firmware;
  - the portable parts once more, this time as the firmware has them: for a
    32-bit processor, with the device's C and C++ libraries.

It needs a configured firmware build, for the list of how each file is
compiled, and the clang-tidy version named in tools/llvm-version:

    tools/idf build && tools/check-firmware-tidy.py

The firmware is compiled by GCC for RISC-V; clang-tidy is clang. So each
compile command is translated first: the options only GCC knows are dropped,
clang is told the processor, and it is pointed at the C and C++ library
headers that GCC uses (GCC is asked where they are). ESP-IDF's and the
toolchain's headers are marked as system headers, so findings are reported
for our code only.

Set CLANG_TIDY to use a specific clang-tidy.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD = ROOT / "app" / "firmware" / "build"

# Where the code that only the firmware build compiles lives.
DEVICE_SOURCE_DIRS = (ROOT / "hw", ROOT / "app" / "firmware" / "main")
# The desktop port is under hw/ too, but the host build compiles and checks it.
HOST_ONLY_DIRS = (ROOT / "hw" / "desktop",)

# Sources that an ordinary firmware build leaves out, and the definitions
# they are compiled with when they are in. They are checked as if they were
# in, using the options of a neighbouring file.
OPTIONAL_SOURCES = {
    ROOT / "app" / "firmware" / "main" / "crash_test.cpp": ["-DBN_CRASH_TEST=0", '-DBN_CRASH_TEST_NAME="fault"'],
}

# Options GCC takes and clang does not.
GCC_ONLY_OPTIONS = {"-fno-shrink-wrap", "-fstrict-volatile-bitfields", "-fno-tree-switch-conversion"}
GCC_ONLY_PREFIXES = ("-specs=", "--specs=", "-mtune=")
# The firmware build's own choices about which compiler warnings are errors.
# clang-tidy's rules decide that here.
WARNING_POLICY_PREFIXES = ("-Werror", "-Wno-error")

# Rules that do not fit device code, taken off for it (and only for it: the
# portable parts keep them).
#
# bugprone-invalid-enum-default-initialization: ESP-IDF's configuration
# structures are made to be zero-initialised and then filled in. Zero is how
# a field says "use the default", including for enumerations that have no
# member equal to zero, which is exactly what this rule objects to.
DEVICE_CODE_EXEMPTIONS = "-bugprone-invalid-enum-default-initialization"

# Read before each source file, to make the C library's headers acceptable
# to clang. They define __noreturn as [[noreturn]] for C++ and then use it
# after a function's parameters, where GCC lets it pass and clang calls it
# an error. The attribute's other spelling means the same and is accepted
# in either place.
PRELUDE_NAME = "bn_firmware_tidy_prelude.h"
PRELUDE = """\
#include <sys/cdefs.h>
#undef __noreturn
#define __noreturn __attribute__((__noreturn__))
"""


def find_clang_tidy() -> str:
    major = (ROOT / "tools" / "llvm-version").read_text().strip()
    candidates = [
        os.environ.get("CLANG_TIDY", ""),
        f"clang-tidy-{major}",
        f"/opt/homebrew/opt/llvm@{major}/bin/clang-tidy",
        "/opt/homebrew/opt/llvm/bin/clang-tidy",
        f"/usr/local/opt/llvm@{major}/bin/clang-tidy",
        "clang-tidy",
    ]
    found = []
    for candidate in candidates:
        path = shutil.which(candidate) if candidate else None
        if path is None:
            continue
        version = subprocess.run([path, "--version"], capture_output=True, text=True, check=False).stdout
        match = re.search(r"version (\d+)\.", version)
        if match and match.group(1) == major:
            return path
        found.append(f"{candidate} ({match.group(1) if match else 'unknown version'})")
    detail = f" Found: {', '.join(found)}." if found else ""
    sys.exit(f"error: clang-tidy {major} is required (tools/llvm-version).{detail}")


def arguments_of(entry: dict) -> list[str]:
    """The compile command of one entry, with any @file of options read in."""
    arguments = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
    expanded = []
    for argument in arguments:
        if argument.startswith("@"):
            expanded.extend(shlex.split(Path(argument[1:]).read_text()))
        else:
            expanded.append(argument)
    return expanded


def library_include_dirs(compiler: str, arguments: list[str], cache: dict) -> list[str]:
    """Where GCC finds the C and C++ library headers, for the options a file is compiled with."""
    # Only these options change the answer.
    relevant = tuple(a for a in arguments if a.startswith(("-march=", "-mabi=", "-specs=", "--specs=", "-fno-rtti")))
    key = (compiler, relevant)
    if key not in cache:
        result = subprocess.run(
            [compiler, *relevant, "-E", "-x", "c++", "-v", os.devnull], capture_output=True, text=True, check=False
        )
        dirs: list[str] = []
        listing = False
        for line in result.stderr.splitlines():
            if "search starts here" in line:
                listing = True
            elif "End of search list" in line:
                break
            elif listing and line.startswith(" "):
                path = os.path.normpath(line.strip())
                # GCC's own built-in headers are left out: clang brings its own.
                if "/lib/gcc/" not in path and path not in dirs:
                    dirs.append(path)
        if not dirs:
            sys.exit(f"error: could not ask {compiler} where its library headers are:\n{result.stderr}")
        cache[key] = dirs
    return cache[key]


def translate(entry: dict, build: Path, cache: dict, extra: list[str] | None = None, source: Path | None = None) -> dict:
    """One compile command, as clang-tidy can use it."""
    arguments = arguments_of(entry)
    compiler = arguments[0]
    original_source = entry["file"]
    # clang would otherwise add the C++ library it was installed with; the
    # firmware's is GCC's, added below.
    translated = ["clang++", "--target=riscv32-esp-elf", "-nostdinc++"]
    skip_next = False
    for argument in arguments[1:]:
        if skip_next:
            skip_next = False
        elif argument == "-o":
            skip_next = True
        elif argument in GCC_ONLY_OPTIONS or argument.startswith(GCC_ONLY_PREFIXES + WARNING_POLICY_PREFIXES):
            pass
        elif argument.startswith("-I"):
            # Headers that are not ours (ESP-IDF's, and what the build
            # generated) become system headers.
            include = Path(argument[2:])
            translated.extend([argument] if is_ours(include, build) else ["-isystem", str(include)])
        elif source is not None and argument == original_source:
            translated.append(str(source))
        else:
            translated.append(argument)
    for include in library_include_dirs(compiler, arguments, cache):
        translated.extend(["-isystem", include])
    # The warning options left in name some warnings clang does not have.
    translated.extend(["-Wno-unknown-warning-option", "-Wno-unused-command-line-argument"])
    translated.extend(extra or [])
    return {"directory": entry["directory"], "file": str(source or original_source), "arguments": translated}


def is_ours(path: Path, build: Path) -> bool:
    """Whether a file or directory is this project's own: in the repository, and neither ESP-IDF nor build output."""
    return path.is_relative_to(ROOT) and not path.is_relative_to(ROOT / ".esp") and not path.is_relative_to(build)


def is_device_source(path: Path) -> bool:
    return any(path.is_relative_to(d) for d in DEVICE_SOURCE_DIRS) and not any(
        path.is_relative_to(d) for d in HOST_ONLY_DIRS
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Runs clang-tidy on the code that only the firmware build compiles.")
    parser.add_argument("build", nargs="?", type=Path, default=DEFAULT_BUILD, help="the firmware build directory")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1, help="files to check at once")
    options = parser.parse_args()

    build = options.build.resolve()
    database = build / "compile_commands.json"
    if not database.is_file():
        sys.exit(f"error: {database} does not exist. Run tools/idf build first.")
    clang_tidy = find_clang_tidy()

    entries = {Path(entry["file"]).resolve(): entry for entry in json.loads(database.read_text())}
    cache: dict = {}
    # Every source of ours that goes into the firmware.
    translated = [translate(entry, build, cache) for path, entry in sorted(entries.items()) if is_ours(path, build)]
    for source, definitions in OPTIONAL_SOURCES.items():
        if source in entries or not source.is_file():
            continue
        neighbours = [entry for path, entry in sorted(entries.items()) if path.parent == source.parent]
        if not neighbours:
            sys.exit(f"error: nothing in {database} is compiled from {source.parent}, to take {source.name}'s options from")
        translated.append(translate(neighbours[0], build, cache, definitions, source))
    device_count = sum(1 for entry in translated if is_device_source(Path(entry["file"])))
    if device_count == 0:
        sys.exit(f"error: {database} lists no device sources. Is it the firmware's build directory?")

    with tempfile.TemporaryDirectory(prefix="bn-firmware-tidy-") as directory:
        # The prelude is found through a system include directory, so nothing
        # in it is reported.
        (Path(directory) / PRELUDE_NAME).write_text(PRELUDE)
        for entry in translated:
            entry["arguments"].extend(["-isystem", directory, "-include", PRELUDE_NAME])
        (Path(directory) / "compile_commands.json").write_text(json.dumps(translated))

        def check(entry: dict) -> tuple[str, int, str]:
            command = [clang_tidy, "-p", directory, "--quiet"]
            if is_device_source(Path(entry["file"])):
                command.append(f"--checks={DEVICE_CODE_EXEMPTIONS}")
            result = subprocess.run([*command, entry["file"]], capture_output=True, text=True, check=False)
            # Findings are on standard output; standard error has only counts,
            # unless the file could not be read at all.
            output = result.stdout if result.stdout.strip() else (result.stderr if result.returncode != 0 else "")
            return entry["file"], result.returncode, output

        failed = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(options.jobs, 1)) as pool:
            for source, status, output in pool.map(check, translated):
                if status != 0 or output.strip():
                    failed += 1
                    print(output.replace(str(ROOT) + os.sep, "").rstrip())

    summary = f"{len(translated)} firmware source files ({device_count} of them device code)"
    if failed:
        print(f"\nclang-tidy found problems in {failed} of the {summary}.", file=sys.stderr)
        return 1
    print(f"{summary} checked by clang-tidy, no findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
