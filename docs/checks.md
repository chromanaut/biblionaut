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

The Linux run keeps its two builds in Docker volumes, so that only the
first run builds everything. They are named for the checkout's directory
(`biblionaut-linux-build` and `biblionaut-linux-build-release` for a
checkout in `biblionaut`), and a run says which they are; remove them with
`docker volume rm` when a checkout is done with. Each checkout has its
own because a build is only good for the tree it was made from: ninja
builds again what is older than its source, and a file one checkout last
changed a week ago is older than what was built from another checkout
yesterday, so given that build nothing would be built and the tests run
would be the other checkout's. A volume therefore also records which
checkout it was built from, and is emptied before it is used for any
other.

Four of these take every processor for minutes: the Linux run, the two
clang-tidy runs, and fuzzing. Two started together, from two checkouts of
the repository on one machine, each take twice as long, and with several
people or programs at work on one machine that happens all the time. So
each of the four waits for its turn before it starts: `tools/queued` gives
the machine to one command at a time, in the order they came, whichever
checkout each was started from, and `tools/linux-check.sh`,
`tools/check-tidy.sh`, `tools/check-firmware-tidy.py` and `tools/fuzz.py`
go through it.

A command that keeps to a few processors does not need the machine to
itself. It says how many it uses, `tools/queued --cores 2 COMMAND`, and
shares the machine with others that say so, for as long as what they ask
for comes to no more than the machine has; one that says nothing has all
of it. The order is still the order they came: nothing begins until
everything that came before it has begun, so a command that wants the
whole machine is not kept waiting by small ones that come after it, and
they wait behind it though there are processors free.

A command that is in no hurry and is not being timed holds the others up
for nothing: a few minutes of fuzzing on two processors keep whatever
wants the whole machine waiting, and everything behind that. Such a
command is run in the background, `tools/queued --background COMMAND`. It
has its turn in a queue of its own, `background`, which nothing in the
other waits for, and it is run at the lowest priority the machine has. On
a Mac that is the one that keeps a process to the efficiency cores
(`taskpolicy -b`), and the queue has as many processors to share out as
there are of those; elsewhere it is `nice`'s lowest, and a quarter of the
processors, one at the least (`BN_QUEUE_BACKGROUND_CORES` says another
number). `--cores` means there what it means anywhere. The price is that
the command gets much less done in a minute, about a ninth on a Mac whose
other cores are busy, and reads and writes files slowly: it is given more
minutes, or does less. `tools/fuzz.py` takes two turns: the whole machine
to build its targets, which is soon over, and then one in the background
for its runs, a processor for each that goes on at once.

There is one exception to the order. Most commands that come this way are
over in seconds, a build of the file just changed, and left to the order
most of the waiting there is would be theirs, behind something that takes
a quarter of an hour and whatever is waiting for that to end. So a command
that has been short does not wait behind them: while something that is not
short is running, it goes as soon as a processor is free, with as many as
are free up to what it asked for, and `--list` and `--watch` say `brief`
of it. A command has been short if the last three turns written down for
it, the same words in the same folder, of which there have to be two, each
ran under half a minute (`BN_QUEUE_BRIEF` seconds where that is set, and
with 0 none counts). What came first is held up by this no longer than
that: once nothing but short commands is running, no more of them begin
until those that came first have.

One that has to wait says what it is waiting for, and
`tools/queued --list` says what is running and what waits:

```
background: running: tools/fuzz.py --built --time 420 inflate, in ~/biblionaut-two, since 14:38:45, process 9962, 2 cores.
heavy: running: build/host-release/tests/draw_tests, in ~/biblionaut, since 14:40:45, process 10004, 1 core.
heavy: waiting: tools/check-tidy.sh, in ~/biblionaut, since 14:41:00, process 12654.
heavy: waiting: python3 trial.py, in ~/biblionaut-two, since 14:41:10, process 12817, 2 cores.
```

`tools/queued --watch` keeps saying it, until it is stopped with Ctrl-C. On
a terminal it draws the queues again twice a second: for each, what has a
turn, with how many processors and for how long, then each that waits and
for how long, with the folder of the checkout it was started in and its
command; and under them what the turns that began today and are over
waited. A long path in a command is cut to its last name, and a line to
the terminal's width:

```
14:41:33  the queues in ~/.cache/biblionaut/queue  (Ctrl-C to stop)

background  running  2 cores    2 min 48 s  biblionaut-two  tools/fuzz.py --built --time 420 inflate
heavy       running  1 core           47 s  biblionaut      .../draw_tests
            waiting  all cores        33 s  biblionaut      tools/check-tidy.sh
            waiting  2 cores          23 s  biblionaut-two  python3 trial.py

today, heavy: 1 turn over, which waited 0 s on average and 0 s at the most
```

Written to a file or a pipe and not a terminal, it says the same once each
time something changes, under the time it changed.

A turn is locks on files in `~/.cache/biblionaut/queue`, and the command
that is running holds them itself: `tools/queued` does not start the
command and wait for it, it becomes it. So a turn ends when the command
ends, however that is, and with whatever the command started; one that is
killed leaves nothing to clear up, and one that is killed while it waits
holds nobody up. One lock is the whole machine: a command that wants all
of it has that lock to itself, and those that share have it together,
each with a lock of its own for every processor it asked for. A
`tools/queued` that knows nothing of sharing, in a checkout not brought up
to date, takes the one lock for itself, so it waits for those that share
and they for it. `--list` and `--watch` find out what is taken by trying
the locks for a moment, so a command that finds one taken looks once more
before it says that it is waiting.

Each turn is written down when it ends, a line in `turns.log` in the same
folder: when it began, the queue, how many processors it had, the seconds
it waited and the seconds it ran, where it was started, the command, and
the machine's load as it ended, with a tab between each:

```
2026-11-27 14:38:40	heavy	10	0	125	~/biblionaut	tools/linux-check.sh	12.7
2026-11-27 14:38:45	background	2	0	169	~/biblionaut-two	tools/fuzz.py --built --time 420 inflate	13.9
2026-11-27 14:40:45	heavy	1	117	50	~/biblionaut	build/host-release/tests/draw_tests	13.9
```

The load is how many processes were running or ready to run, on average
over the minute before, whatever started them: set beside the processors
a long turn had, it says whether the turn used them, and it says what
else the machine was doing. These were taken while other work had the
machine busy.

`tools/queued` has become the command by then, so the line is written by a
process it leaves behind for that, which holds no part of the turn and is
given the turn's own file when everything else has let go of it.

`tools/queued COMMAND` gives any other command a turn in the same queue,
and `--name` another queue, which a run on a device with one cable can
use (`tools/queued --help`). Setting `BN_QUEUE_OFF` runs a command at
once and as it is, and `BN_QUEUE_CORES` says how many processors there
are to share out, where that is not all the machine has. A command is
told how many its turn has, in `BN_QUEUE_TURN_CORES`, and can run that
many things at once, as `tools/fuzz.py` does. CI is not changed by any of
this: each of its jobs has a machine to itself, where there is nothing to
wait for.

CI gives each host build a job of its own, so that they run at the same
time, and runs the tests one to a core, as `tools/linux-check.sh` does. A
local run is much the same: the `host` and `host-release` test presets run
eight tests at a time, where ctest left to itself runs one (`-j N` on the
command line for another number). On a Mac with ten cores
`ctest --preset host` takes about a minute, and the same tests one at a
time take six. So that tests can run side by side, each writes only files
no other test writes, in a directory or under names of its own.

Fuzzing itself (`tools/fuzz.py`, [Fuzzing](fuzzing.md)) is not among them:
these checks only take the targets through the inputs they have kept. It
has a workflow of its own (`.github/workflows/fuzz.yml`), for a pull
request that changes a reader and for main every night, which no pull
request waits for; and it is run on a developer's computer for a minute
after such a change.

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
