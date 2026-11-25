# Checks

CI runs all of these on every push to `main` and every pull request
(`.github/workflows/ci.yml`). To run them locally:

| Check | Command |
|---|---|
| Unit tests, with AddressSanitizer and UndefinedBehaviorSanitizer | `cmake --build --preset host && ctest --preset host` |
| The same tests in the optimised build, without sanitizers | `cmake --build --preset host-release && ctest --preset host-release` |
| Both as CI's Linux jobs run them: GCC 14 on Ubuntu, in Docker | `tools/linux-check.sh` (arguments go to `ctest`) |
| The fuzz targets, over the inputs kept for them | Part of each of the three above; `ctest --preset host -R '^fuzz'` for those alone |
| The desktop application with its window, and its end-to-end test (CI runs it on macOS) | `cmake --preset window && cmake --build --preset window && ctest --preset window` |
| Formatting | `tools/check-format.sh` (`--fix` to rewrite files) |
| Constructs the portable parts may not use (`std::string`, `std::vector`, `std::function`, `std::shared_ptr`, iostreams, C stdio streams) | `tools/check-banned.py` |
| clang-tidy (rules in `.clang-tidy`) on everything the host build compiles | `tools/check-tidy.sh` |
| clang-tidy on what the firmware build compiles: the device code, and the portable parts as built for the device | `tools/check-firmware-tidy.py` (after a firmware build) |
| Firmware builds | `tools/idf build` |
| The portable parts (`core`, `epub`, `draw`, `layout`) call nothing that can throw | `tools/check-firmware-symbols.sh` (after a firmware build) |

The formatting check and both clang-tidy runs need the LLVM major version
named in `tools/llvm-version` (`brew install llvm@<version>` on macOS).

Four of these take every processor for minutes: the Linux run, the two
clang-tidy runs, and fuzzing. Two started together, from two checkouts of
the repository on one machine, each take twice as long, and with several
people or programs at work on one machine that happens all the time. So
each of the four waits for its turn before it starts: `tools/queued` runs
one command at a time on a machine, in the order they came, whichever
checkout each was started from, and `tools/linux-check.sh`,
`tools/check-tidy.sh`, `tools/check-firmware-tidy.py` and `tools/fuzz.py`
go through it. One that has to wait says what it is waiting for, and
`tools/queued --list` says what is running and what waits:

```
heavy: running: tools/check-tidy.sh, in ~/biblionaut, since 20:05:18, process 2653.
heavy: waiting: tools/linux-check.sh, in ~/biblionaut, since 20:05:22, process 2910.
heavy: waiting: tools/fuzz.py --time 420 inflate zip png, in ~/biblionaut-two, since 20:05:33, process 6030.
```

A turn is a lock on a file in `~/.cache/biblionaut/queue`, and the command
that is running holds it itself: `tools/queued` does not start the command
and wait for it, it becomes it. So a turn ends when the command ends,
however that is, and with whatever the command started; one that is killed
leaves nothing to clear up, and one that is killed while it waits holds
nobody up. `tools/queued COMMAND` gives any other command a turn in the
same queue, and `--name` another queue, which a run on a device with one
cable can use (`tools/queued --help`). Setting `BN_QUEUE_OFF` runs a
command at once. CI is not changed by any of this: each of its jobs has a
machine to itself, where there is nothing to wait for.

CI gives each host build a job of its own, so that they run at the same
time, and runs the tests one to a core, as `tools/linux-check.sh` does. A local run does the same with `-j`: on a
Mac with ten cores, `ctest --preset host -j 8` takes under a minute, and
the same tests one at a time take four. So that tests can run side by side, each writes only files no other
test writes, in a directory or under names of its own.

Fuzzing itself (`tools/fuzz.py`, [Fuzzing](fuzzing.md)) is not among them: it
is run on a developer's computer, for minutes or hours, and CI only takes
the targets through the inputs they have kept.

Two clang-tidy runs, because no single build compiles everything. The host
run sees the portable parts, the desktop port with its window, the tools and the tests, and
leaves the Clang Static Analyzer out for the tests (`tests/.clang-tidy` says
why). The firmware run sees the device code under `hw/` and
`app/firmware`, which nothing else checks, by translating the firmware's
GCC compile commands into ones clang can use.

The Linux run is worth doing before pushing changes to how memory is
handled. Its compiler and C library catch faults the macOS build cannot see,
such as a null pointer passed to `memset` with a length of zero, and under
the sanitizers a finding fails the test.

On an arm64 computer, such as a Mac with Apple silicon, the Linux run leaves
out one of AddressSanitizer's checks: for use of a function's stack after
it has returned. GCC's runtime for arm64 Linux is slow enough at it to turn
seconds of tests into minutes (`tests/CMakeLists.txt` has the figures). CI's
Linux jobs run on x86-64, where it is not slow, and make that check.
