#!/usr/bin/env python3
"""Fuzzes: runs the fuzz targets with a fuzzing engine and says what it found.

A fuzz target (tests/fuzz) puts whatever bytes it is given through one
reader of untrusted input and ends the program if the reader goes wrong.
In an ordinary build the targets only go through the inputs kept in the
repository. This builds them with LLVM's fuzzing engine, libFuzzer, which
makes up new inputs, keeps each one that reaches code no input reached
before, and goes on from those for as long as it is given.

    tools/fuzz.py                  every target, a minute each
    tools/fuzz.py --time 3600      an hour each
    tools/fuzz.py xml zip          only those two
    tools/fuzz.py --list           the names of the targets

Each target is run in two builds, side by side:

    fuzz          with AddressSanitizer and UndefinedBehaviorSanitizer
    fuzz-noasan   with UndefinedBehaviorSanitizer only

The second is there because the heap inside a block (core/region_heap.h),
which the XML parser's memory comes from, is laid out one way with
AddressSanitizer and another way without, and the firmware has it the
second way. There the targets' own checks are what there is to go by.

What it leaves behind, in .fuzz/ at the top of the repository (which git
ignores):

    .fuzz/corpus/<target>/   the inputs the engine has kept, which the next
                             run starts from: the longer this has been
                             built up, the further a run gets
    .fuzz/found/<target>/    each input that made a target fail, under
                             the name of the build it failed in
    .fuzz/logs/              what each run printed

For an input that was found, the last lines of the summary say how to see
the failure again, and how to cut the input down to the least that still
fails. Once the fault is mended, that small input goes into
tests/fuzz/inputs/<target>/ under a name beginning found-, where the tests
take it from then on.

It needs the LLVM whose major version is in tools/llvm-version, which the
format and tidy checks need too: Apple's compiler has no fuzzing engine.
Set CLANG to use a specific clang.

When to run it: after a change to any of the readers, for a few minutes;
and for an hour a target before the work on a set of readers is called
finished.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / ".fuzz"
INPUTS = ROOT / "tests" / "fuzz" / "inputs"

# Each target, and the longest input the engine is to make for it, in
# bytes. Long enough to hold what the target is for (a ZIP file with a few
# entries, a document of several of the parser's pieces, a drawing with
# path data too long to be held); the longer the inputs, the fewer of them
# a run gets through.
TARGETS = {
    "inflate": 8192,
    "zip": 16384,
    "xml": 16384,
    "package": 16384,
    "toc": 16384,
    "css": 16384,
    "css_rules": 8192,
    "region_heap": 3072,
    "hyphenator": 4096,
    "paragraph": 4096,
    "svg_tags": 4096,
    "svg_sheet": 4096,
    "svg_values": 256,
    "svg_path": 2048,
    "svg": 8192,
}

# Pieces of XML, of CSS and of SVG that the readers look for, which the
# engine would be a long time coming to by changing bytes. For the targets
# that read each.
XML_PIECES = ROOT / "tests" / "fuzz" / "xml.dict"
CSS_PIECES = ROOT / "tests" / "fuzz" / "css.dict"
CSS_RULES_PIECES = ROOT / "tests" / "fuzz" / "css_rules.dict"
SVG_PIECES = ROOT / "tests" / "fuzz" / "svg.dict"
DICTIONARIES = {
    "xml": XML_PIECES,
    "package": XML_PIECES,
    "toc": XML_PIECES,
    "css": CSS_PIECES,
    "css_rules": CSS_RULES_PIECES,
    "svg_tags": SVG_PIECES,
    "svg_sheet": SVG_PIECES,
    "svg_values": SVG_PIECES,
    "svg_path": SVG_PIECES,
    "svg": SVG_PIECES,
}

BUILDS = ("fuzz", "fuzz-noasan")

# An input that keeps a target busy this long, in seconds, is reported as
# one that does not end.
INPUT_TIMEOUT = 10


def find_clang() -> Path:
    """The clang of the pinned LLVM version, which is not Apple's."""
    major = (ROOT / "tools" / "llvm-version").read_text().strip()
    candidates = [
        os.environ.get("CLANG", ""),
        f"clang-{major}",
        f"/opt/homebrew/opt/llvm@{major}/bin/clang",
        "/opt/homebrew/opt/llvm/bin/clang",
        f"/usr/local/opt/llvm@{major}/bin/clang",
        f"/usr/lib/llvm-{major}/bin/clang",
        "clang",
    ]
    seen = []
    for candidate in candidates:
        path = shutil.which(candidate) if candidate else None
        if not path:
            continue
        version = subprocess.run([path, "--version"], capture_output=True, text=True, check=False).stdout
        first = version.splitlines()[0] if version else ""
        if "Apple" not in first and re.search(rf"version {major}\.", first):
            return Path(path)
        seen.append(f"{candidate} ({first})")
    found = f" Found: {'; '.join(seen)}." if seen else ""
    sys.exit(f"error: LLVM's clang {major} is required (tools/llvm-version).{found}")


def build(clang: Path) -> None:
    """Configures and builds the fuzz targets, in both builds."""
    for preset in BUILDS:
        # The C++ compiler is beside the C one, under the same name with ++.
        configure = [
            "cmake", "--preset", preset,
            f"-DCMAKE_C_COMPILER={clang}",
            f"-DCMAKE_CXX_COMPILER={clang.with_name(clang.name.replace('clang', 'clang++', 1))}",
        ]
        for command in (configure, ["cmake", "--build", "--preset", preset]):
            done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
            if done.returncode != 0:
                sys.stderr.write(done.stdout + done.stderr)
                sys.exit(f"error: `{' '.join(command)}` failed.")


def program(preset: str, target: str) -> Path:
    return ROOT / "build" / preset / "tests" / "fuzz" / f"fuzz_{target}"


def run_one(preset: str, target: str, seconds: int) -> dict:
    """Runs one target in one build for `seconds`, and reads what it printed."""
    corpus = WORK / "corpus" / target
    found = WORK / "found" / target / preset
    log = WORK / "logs" / f"{target}-{preset}.log"
    for directory in (corpus, found, log.parent):
        directory.mkdir(parents=True, exist_ok=True)
    before = set(found.iterdir())
    command = [
        str(program(preset, target)),
        f"-max_total_time={seconds}",
        f"-timeout={INPUT_TIMEOUT}",
        f"-max_len={TARGETS[target]}",
        f"-artifact_prefix={found}/",
        "-print_final_stats=1",
    ]
    if target in DICTIONARIES:
        command.append(f"-dict={DICTIONARIES[target]}")
    # What the engine keeps goes into the first directory. The repository's
    # inputs are read and left as they are.
    command += [str(corpus), str(INPUTS / target)]
    started = time.monotonic()
    with log.open("w") as out:
        done = subprocess.run(command, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, check=False)
    text = log.read_text(errors="replace")

    def stat(name: str) -> int:
        match = re.search(rf"stat::{name}:\s+(\d+)", text)
        return int(match.group(1)) if match else 0

    return {
        "preset": preset,
        "target": target,
        "code": done.returncode,
        "seconds": time.monotonic() - started,
        "runs": stat("number_of_executed_units"),
        "kept": stat("new_units_added"),
        "found": sorted(set(found.iterdir()) - before),
        "log": log,
    }


def shrink(target: str, path: Path, seconds: int) -> None:
    """Cuts an input that makes `target` fail down to a small one that still does."""
    preset = BUILDS[0]
    if not program(preset, target).exists():
        build(find_clang())
    small = path.with_name(path.name + ".small")
    command = [
        str(program(preset, target)),
        "-minimize_crash=1",
        f"-max_total_time={seconds}",
        f"-timeout={INPUT_TIMEOUT}",
        f"-exact_artifact_path={small}",
        str(path),
    ]
    subprocess.run(command, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    if small.exists():
        print(f"{small} ({small.stat().st_size} bytes, from {path.stat().st_size})")
    else:
        sys.exit(f"error: {path} does not make fuzz_{target} fail in the {preset} build.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("targets", nargs="*", metavar="TARGET", help="the targets to run (default: all)")
    parser.add_argument("--time", type=int, default=60, metavar="SECONDS", help="how long each run lasts (default 60)")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2), metavar="N",
                        help="how many runs go on at once (default: all but two of the processors)")
    parser.add_argument("--list", action="store_true", help="print the names of the targets and stop")
    parser.add_argument("--shrink", metavar="FILE", type=Path,
                        help="cut down an input that makes the one TARGET named fail, to FILE.small")
    arguments = parser.parse_args()

    if arguments.list:
        print("\n".join(TARGETS))
        return 0
    unknown = [target for target in arguments.targets if target not in TARGETS]
    if unknown:
        sys.exit(f"error: no such target: {', '.join(unknown)}. There are: {', '.join(TARGETS)}.")
    if arguments.shrink:
        if len(arguments.targets) != 1:
            sys.exit("error: --shrink wants the one target the input is for.")
        shrink(arguments.targets[0], arguments.shrink, arguments.time)
        return 0
    targets = arguments.targets or list(TARGETS)

    clang = find_clang()
    print(f"Building the fuzz targets with {clang} ...", flush=True)
    build(clang)

    runs = [(preset, target) for target in targets for preset in BUILDS]
    print(f"Fuzzing {len(targets)} targets in {len(BUILDS)} builds, {arguments.time} s each, "
          f"{min(arguments.jobs, len(runs))} at once ...", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=arguments.jobs) as pool:
        results = list(pool.map(lambda run: run_one(run[0], run[1], arguments.time), runs))

    print()
    print(f"{'target':<12} {'build':<12} {'inputs tried':>13} {'kept':>6}  result")
    failed = []
    for result in results:
        if result["found"]:
            outcome = f"FOUND {len(result['found'])}"
            failed.append(result)
        elif result["code"] != 0:
            outcome = f"the run itself failed (see {result['log'].relative_to(ROOT)})"
            failed.append(result)
        else:
            outcome = "nothing found"
        print(f"{result['target']:<12} {result['preset']:<12} {result['runs']:>13,} {result['kept']:>6}  {outcome}")

    for result in failed:
        for path in result["found"]:
            replay = program("host", result["target"]).relative_to(ROOT)
            print(f"\n{path.relative_to(ROOT)}")
            print(f"  to see it fail:   {replay} {path.relative_to(ROOT)}")
            print(f"  to cut it down:   tools/fuzz.py --shrink {path.relative_to(ROOT)} {result['target']}")
    if not failed:
        print(f"\nNothing found. The inputs kept are in {(WORK / 'corpus').relative_to(ROOT)}/.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
