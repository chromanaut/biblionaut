#!/usr/bin/env python3
"""End-to-end check of tools/queued, run by CTest.

tools/queued makes commands take turns: the heavy checks call it so that
two started at once, from two checkouts, do not run at once. Here it is
given jobs that say when they begin and when they end and that end only
when the test lets them, and what is checked is what it promises:

  - a command's output and its exit code are its own;
  - one job at a time, and those that wait go in the order they came;
  - jobs that say how many processors they use share the machine, as far
    as it goes, and still go in the order they came: one that wants all
    of it waits for those before it and holds back those behind;
  - a tools/queued that knows only of whole turns, as an older checkout
    has, waits for those that share and is waited for by them;
  - each turn is written down when it ends, however it ends;
  - a command that has been short goes ahead of what waits, while
    something that is not short is running, and not otherwise;
  - `--list` says what is running and what waits, and `--watch` says it
    again each time it changes;
  - a job that is killed gives up its turn, and one that is killed while
    it waits holds nobody up;
  - a turn lasts as long as anything the command started;
  - a command started by one that is having its turn does not wait;
  - queues with different names do not wait for each other;
  - with BN_QUEUE_OFF, or a folder that cannot be used, the command runs.

Nothing here goes by how long anything takes. A job waits for a file the
test writes, and the test waits for what a job or the queue has written,
for as long as a minute before it gives up. Once it waits a second and a
half for nothing: for `--watch` to have had the time to say again what
it has said, which it must not. A machine too slow for that to show a
fault makes the check pass, not fail.

usage: queued.py QUEUED WORK_DIR
"""

from __future__ import annotations

import fcntl
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

JOB = """\
import pathlib, sys, time
log, name, go = sys.argv[1:4]
with open(log, "a") as out:
    out.write(f"begin {name}\\n")
while not pathlib.Path(go).exists():
    time.sleep(0.02)
with open(log, "a") as out:
    out.write(f"end {name}\\n")
"""

failures = 0


def check(condition: bool, what: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAILED: {what}")


def wait_for(condition, what: str, seconds: float = 60.0) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(0.02)
    check(False, f"waited {seconds:.0f} s for {what}")
    return False


class Bench:
    """A folder for one test: the queue's own, the jobs' log, and the files
    that let jobs end."""

    def __init__(self, queued: Path, work: Path, name: str) -> None:
        self.queued = queued
        self.folder = work / name
        if self.folder.exists():
            shutil.rmtree(self.folder)
        self.folder.mkdir(parents=True)
        self.queue = self.folder / "queue"
        self.log = self.folder / "log"
        self.log.write_text("")
        self.job = self.folder / "job.py"
        self.job.write_text(JOB)
        # A machine of four processors, whatever this one has; and nothing
        # counts as short, so that the order is the order they came in.
        self.environment = {**os.environ, "BN_QUEUE_DIR": str(self.queue), "BN_QUEUE_CORES": "4", "BN_QUEUE_BRIEF": "0"}
        self.environment.pop("BN_QUEUE_HELD", None)
        self.environment.pop("BN_QUEUE_OFF", None)
        self.started: list[subprocess.Popen] = []

    def command(self, *arguments: str) -> list[str]:
        return [sys.executable, str(self.queued), *arguments]

    def job_command(self, name: str) -> list[str]:
        return [sys.executable, str(self.job), str(self.log), name, str(self.folder / f"go-{name}")]

    def start(self, name: str, *options: str, **more) -> subprocess.Popen:
        """Starts a job through the queue."""
        process = subprocess.Popen(
            self.command(*options, *self.job_command(name)),
            env=more.get("env", self.environment),
            stderr=subprocess.PIPE,
            text=True,
        )
        self.started.append(process)
        return process

    def start_heard(self, name: str, *options: str, **more) -> Path:
        """Starts a job through the queue, and returns the file that has
        what the queue says of it, which can be read while it waits."""
        heard = self.folder / f"said-of-{name}"
        with open(heard, "w") as out:
            self.started.append(
                subprocess.Popen(
                    self.command(*options, *self.job_command(name)),
                    env=more.get("env", self.environment),
                    cwd=more.get("cwd"),
                    stderr=out,
                )
            )
        return heard

    def begun_times(self, name: str) -> int:
        return self.said().count(f"begin {name}")

    def run_through(self, name: str, *options: str) -> None:
        """Runs a job through the queue from its beginning to its end, and
        waits for the turn to be written down; then it can be run again."""
        written = len(self.turns())
        process = self.start(name, *options)
        self.let_end(name)
        check(process.wait(timeout=60) == 0, f"the job {name} ends well")
        wait_for(lambda: len(self.turns()) == written + 1, f"the turn of {name} to be written down")
        (self.folder / f"go-{name}").unlink()

    def let_end(self, name: str) -> None:
        (self.folder / f"go-{name}").write_text("")

    def lines(self) -> list[str]:
        return self.log.read_text().split()

    def said(self) -> list[str]:
        text = self.log.read_text().splitlines()
        return text

    def begun(self, name: str) -> bool:
        return f"begin {name}" in self.said()

    def ended(self, name: str) -> bool:
        return f"end {name}" in self.said()

    def waiting(self, queue: str = "heavy") -> int:
        try:
            return sum(1 for path in (self.queue / f"{queue}.waiting").iterdir() if not path.name.startswith("."))
        except OSError:
            return 0

    def listed(self) -> list[str]:
        return subprocess.run(self.command("--list"), env=self.environment, capture_output=True, text=True).stdout.splitlines()

    def turns(self) -> list[list[str]]:
        """The turns written down so far, each as its fields."""
        try:
            return [line.split("\t") for line in (self.queue / "turns.log").read_text().splitlines()]
        except OSError:
            return []

    def finish(self) -> None:
        for process in self.started:
            if process.poll() is None:
                process.kill()
            process.wait()
            if process.stderr is not None:
                process.stderr.close()


def passes_a_command_through(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "through")
    result = subprocess.run(
        bench.command("sh", "-c", "echo out; echo err >&2; exit 7"), env=bench.environment, capture_output=True, text=True
    )
    check(result.returncode == 7, f"the command's exit code is passed on (got {result.returncode})")
    check(result.stdout == "out\n", f"its output is passed on (got {result.stdout!r})")
    check(result.stderr == "err\n", f"and nothing is added when there was no wait (got {result.stderr!r})")

    missing = subprocess.run(bench.command("no-such-command-anywhere"), env=bench.environment, capture_output=True, text=True)
    check(missing.returncode == 127, f"a command that is not there comes to 127 (got {missing.returncode})")
    check("no-such-command-anywhere" in missing.stderr, "and is named")

    nothing = subprocess.run(bench.command(), env=bench.environment, capture_output=True, text=True)
    check(nothing.returncode == 2 and "usage" in nothing.stderr, "no command at all is a mistake in how it was called")


def takes_turns_in_order(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "order")
    try:
        bench.start("A")
        wait_for(lambda: bench.begun("A"), "the first job to begin")
        second = bench.start_heard("B")
        wait_for(lambda: bench.waiting() == 1, "the second to be waiting")
        third = bench.start_heard("C")
        wait_for(lambda: bench.waiting() == 2, "the third to be waiting")
        wait_for(lambda: "Running now: " in second.read_text() and "Running now: " in third.read_text(), "each to say that it is waiting")
        check(bench.said() == ["begin A"], f"only the first has begun (log: {bench.said()})")

        listed = subprocess.run(bench.command("--list"), env=bench.environment, capture_output=True, text=True).stdout
        rows = listed.splitlines()
        check(len(rows) == 3, f"--list has a line for each of the three (got {listed!r})")
        if len(rows) == 3:
            check(rows[0].startswith("heavy: running: ") and " A " in rows[0], f"the first as running ({rows[0]!r})")
            check(rows[1].startswith("heavy: waiting: ") and " B " in rows[1], f"the second as waiting ({rows[1]!r})")
            check(rows[2].startswith("heavy: waiting: ") and " C " in rows[2], f"and the third after it ({rows[2]!r})")

        bench.let_end("A")
        wait_for(lambda: bench.begun("B"), "the second to begin once the first ended")
        check(bench.said() == ["begin A", "end A", "begin B"], f"the second began after the first ended, and the third has not ({bench.said()})")
        bench.let_end("B")
        wait_for(lambda: bench.begun("C"), "the third to begin once the second ended")
        bench.let_end("C")
        for process in bench.started:
            check(process.wait(timeout=60) == 0, "each job ends well")
        check(
            bench.said() == ["begin A", "end A", "begin B", "end B", "begin C", "end C"],
            f"one at a time, in the order they came ({bench.said()})",
        )
        told = second.read_text()
        check("queued: waiting for a turn (heavy). Running now: " in told and " A " in told, f"one that waits says what for ({told!r})")
        check("queued: its turn came after " in told, "and when its turn comes")
        check("1 waiting before this" in third.read_text(), "and how many are before it")

        after = subprocess.run(bench.command("--list"), env=bench.environment, capture_output=True, text=True).stdout
        check(after == "heavy: nothing running.\n", f"with all of them over, --list says so ({after!r})")
    finally:
        bench.finish()


def watching_says_each_change(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "watch")
    watched = bench.folder / "watched"
    with open(watched, "w") as out:
        watcher = subprocess.Popen(bench.command("--watch"), env=bench.environment, stdout=out, stderr=subprocess.PIPE, text=True)
    bench.started.append(watcher)

    def pictures(over: bool = False) -> list[list[str]]:
        # Written to a file, each is the time, a line for each thing in a
        # queue, a line for each queue of what today's turns waited, and an
        # empty line. The rows, or with `over` the lines for today.
        said = [picture.splitlines()[1:] for picture in watched.read_text().split("\n\n") if picture.strip()]
        return [[line for line in lines if line.startswith("today, ") == over] for lines in said]

    def last(over: bool = False) -> list[str]:
        return pictures(over)[-1] if pictures(over) else []

    try:
        wait_for(lambda: len(last()) == 1 and last()[0].startswith("Nothing has been queued in "), "it to say that nothing has been queued")
        bench.start("A")
        wait_for(lambda: bench.begun("A"), "the first job to begin")
        bench.start("B", "--cores", "2")
        wait_for(lambda: bench.waiting() == 1, "the second to be waiting")
        wait_for(lambda: len(last()) == 2 and " A " in last()[0] and " B " in last()[1], "it to show one running and one waiting")
        rows = last()
        if len(rows) == 2:
            here = Path.cwd().name
            check(rows[0].startswith("heavy  running  ") and f"  {here}  " in rows[0], f"the first as running, and where it was started ({rows[0]!r})")
            check(rows[1].startswith("       waiting  ") and f"  {here}  " in rows[1], f"the second under it, as waiting ({rows[1]!r})")
            check(" .../job.py " in rows[0] and str(bench.folder) not in rows[0], f"with a long path cut to its last name ({rows[0]!r})")
            check(re.search(r"running  all cores +\d+ (s|min|h)\b", rows[0]) is not None, f"with all the processors, and for how long ({rows[0]!r})")
            check(re.search(r"waiting  2 cores +\d+ (s|min|h)\b", rows[1]) is not None, f"and the second with the two it asked for ({rows[1]!r})")
        # It looks twice a second, and nothing has changed.
        said = len(pictures())
        time.sleep(1.5)
        check(len(pictures()) == said, f"with nothing changed it says nothing more ({pictures()[said:]})")

        bench.let_end("A")
        wait_for(lambda: bench.begun("B"), "the second to begin")
        wait_for(lambda: len(last()) == 1 and last()[0].startswith("heavy  running  ") and " B " in last()[0], "it to show the second running alone")
        bench.let_end("B")
        wait_for(lambda: last() == ["heavy  nothing running"], "it to show nothing running")
        wait_for(lambda: len(last(over=True)) == 1 and last(over=True)[0].startswith("today, heavy: 2 turns over, which waited "),
                 "it to say what the two turns of today waited")
        watcher.send_signal(signal.SIGINT)
        check(watcher.wait(timeout=60) == 0, "stopped as Ctrl-C stops it, it ends well")
        check(watcher.stderr.read() == "", "having said nothing else")
    finally:
        bench.finish()


def those_that_say_how_many_processors_share_the_machine(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "cores")
    try:
        # Two of the four processors each: both at once.
        bench.start("A", "--cores", "2")
        bench.start("B", "--cores", "2")
        wait_for(lambda: bench.begun("A") and bench.begun("B"), "two jobs of two processors each to be running at once")
        check(bench.waiting() == 0, "neither waits")
        # One more is one too many.
        third = bench.start_heard("C", "--cores", "1")
        wait_for(lambda: bench.waiting() == 1, "a third to wait, the four being taken")
        wait_for(lambda: "Running now: " in third.read_text(), "it to say that it is waiting")
        rows = bench.listed()
        check(len(rows) == 3, f"--list has a line for each ({rows})")
        if len(rows) == 3:
            check(all(row.startswith("heavy: running: ") and row.endswith(", 2 cores.") for row in rows[:2]), f"two as running, each with its two ({rows[:2]})")
            check(" A " in rows[0] + rows[1] and " B " in rows[0] + rows[1], f"which are the two ({rows[:2]})")
            check(rows[2].startswith("heavy: waiting: ") and " C " in rows[2] and rows[2].endswith(", 1 core."), f"and the third as waiting for its one ({rows[2]!r})")
        check(not bench.begun("C"), "the third has not begun")
        bench.let_end("A")
        wait_for(lambda: bench.begun("C"), "the third to begin when one of the two ended")
        check(not bench.ended("B"), "while the other of the two goes on")
        bench.let_end("B")
        bench.let_end("C")
        for process in bench.started:
            check(process.wait(timeout=60) == 0, "each job ends well")
        said = third.read_text()
        check("queued: waiting for a turn (heavy). Running now: " in said and " A " in said and " B " in said, f"one that waits names all that are running ({said!r})")
        wait_for(lambda: bench.listed() == ["heavy: nothing running."], "--list to say that nothing is running")

        # As many as the machine has, or more, is all of it; and it has to be a number of them.
        bench.start("D", "--cores", "1")
        wait_for(lambda: bench.begun("D"), "a job of one processor to begin")
        bench.start("E", "--cores", "9")
        wait_for(lambda: bench.waiting() == 1, "one that asks for more than there are to wait for the whole machine")
        check(not bench.begun("E"), "though three of the four are free")
        waits = [row for row in bench.listed() if row.startswith("heavy: waiting: ")]
        check(len(waits) == 1 and " cores" not in waits[0], f"and it is listed as wanting all of it ({waits})")
        bench.let_end("D")
        wait_for(lambda: bench.begun("E"), "it to begin when the other ended")
        bench.let_end("E")
        for wrong in ("0", "two", "-1"):
            refused = subprocess.run(bench.command("--cores", wrong, "true"), env=bench.environment, capture_output=True, text=True)
            check(refused.returncode == 2 and "--cores" in refused.stderr, f"--cores {wrong} is refused ({refused.returncode}, {refused.stderr!r})")
    finally:
        bench.finish()


def the_order_holds_for_those_that_share(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "fair")
    try:
        bench.start("A", "--cores", "1")
        wait_for(lambda: bench.begun("A"), "a job of one processor to begin")
        bench.start("W")
        wait_for(lambda: bench.waiting() == 1, "one that wants the whole machine to wait for it")
        bench.start("C", "--cores", "1")
        wait_for(lambda: bench.waiting() == 2, "a small one that came after to wait as well")
        check(bench.said() == ["begin A"], f"neither has begun, though three processors are free ({bench.said()})")
        bench.let_end("A")
        wait_for(lambda: bench.begun("W"), "the one that wants the whole machine to begin when it is free")
        check(not bench.begun("C"), "and the small one behind it still waits")
        bench.let_end("W")
        wait_for(lambda: bench.begun("C"), "the small one to begin after it")
        bench.let_end("C")
        for process in bench.started:
            check(process.wait(timeout=60) == 0, "each job ends well")
        check(bench.said() == ["begin A", "end A", "begin W", "end W", "begin C", "end C"], f"in the order they came ({bench.said()})")
    finally:
        bench.finish()


def works_beside_one_that_knows_only_whole_turns(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "older")
    bench.queue.mkdir()
    lock = bench.queue / "heavy.lock"

    def has_it_all() -> int | None:
        # What such a tools/queued does to take its turn: the one lock, for
        # itself alone, with what it is written in it.
        descriptor = os.open(lock, os.O_RDWR | os.O_CREAT, 0o600)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(descriptor)
            return None
        os.ftruncate(descriptor, 0)
        os.pwrite(descriptor, f"an older command, in /somewhere, since 00:00:00, process {os.getpid()}\n".encode(), 0)
        return descriptor

    try:
        older = has_it_all()
        check(older is not None, "the lock is free to begin with")
        first = bench.start("A", "--cores", "1")
        wait_for(lambda: bench.waiting() == 1, "one that would share to wait for it")
        rows = bench.listed()
        check(len(rows) == 2 and rows[0].startswith("heavy: running: an older command, ") and " A " in rows[1], f"--list says what it is waiting for ({rows})")
        check(not bench.begun("A"), "it has not begun")
        os.close(older)
        wait_for(lambda: bench.begun("A"), "it to begin when the older one ended")

        check(has_it_all() is None, "and an older one cannot have its turn while one that shares is running")
        told = lock.read_text()
        check("share the machine" in told, f"and is told why, where it looks for what is running ({told!r})")
        bench.let_end("A")
        check(first.wait(timeout=60) == 0, "the job ends well")
        again: list[int] = []
        wait_for(lambda: again.append(has_it_all() or -1) or again[-1] >= 0, "an older one to have its turn once that ended")
        os.close(again[-1])
    finally:
        bench.finish()


def writes_each_turn_down(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "log")
    try:
        bench.start("A", "--cores", "2")
        wait_for(lambda: bench.begun("A"), "the first job to begin")
        killed = bench.start("B")
        wait_for(lambda: bench.waiting() == 1, "the second to be waiting")
        check(bench.turns() == [], f"nothing is written of a turn that is not over ({bench.turns()})")
        bench.let_end("A")
        wait_for(lambda: len(bench.turns()) == 1, "the first turn to be written down when it ended")
        wait_for(lambda: bench.begun("B"), "the second to begin")
        killed.send_signal(signal.SIGKILL)
        wait_for(lambda: len(bench.turns()) == 2, "the second to be written down, though it was killed")
        turns = bench.turns()
        if len(turns) == 2:
            check(all(len(turn) == 8 for turn in turns), f"each has its eight things ({turns})")
            check(all(re.fullmatch(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d", turn[0]) for turn in turns), f"when it began ({[turn[0] for turn in turns]})")
            check([turn[1:3] for turn in turns] == [["heavy", "2"], ["heavy", "4"]], f"its queue and its processors, all four where it asked for none ({[turn[1:3] for turn in turns]})")
            check(all(turn[3].isdigit() and turn[4].isdigit() for turn in turns), f"how long it waited and ran ({[turn[3:5] for turn in turns]})")
            check(all(turn[5].endswith(Path.cwd().name) for turn in turns), f"where it was started ({[turn[5] for turn in turns]})")
            check(turns[0][6].endswith(" A " + str(bench.folder / "go-A")) and " B " in turns[1][6], f"and the command ({[turn[6] for turn in turns]})")
            check(all(re.fullmatch(r"\d+\.\d", turn[7]) for turn in turns), f"and the machine's load as it ended ({[turn[7] for turn in turns]})")
        wait_for(lambda: not any((bench.queue / "heavy.running").iterdir()), "nothing to be left that says a turn is going on")

        # What it leaves behind to do the writing holds no turn, and nobody's output.
        through = subprocess.run(bench.command("sh", "-c", "echo out"), env=bench.environment, capture_output=True, text=True, timeout=60)
        check(through.stdout == "out\n" and through.stderr == "", f"a command's output ends when the command does ({through.stdout!r}, {through.stderr!r})")
        wait_for(lambda: len(bench.turns()) == 3, "that turn to be written down too")
        off = subprocess.run(bench.command("true"), env={**bench.environment, "BN_QUEUE_OFF": "1"}, capture_output=True, text=True, timeout=60)
        check(off.returncode == 0, "a command that takes no turn runs")
        check(len(bench.turns()) == 3, "and is not written down")
    finally:
        bench.finish()


def what_has_been_short_goes_ahead_while_something_long_runs(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "brief")
    # Short is anything under ten minutes here: a job the test lets end at
    # once is short however slow the machine is.
    bench.environment["BN_QUEUE_BRIEF"] = "600"
    try:
        # Two commands the queue has seen be short, twice each: one that asks
        # for nothing, which is the whole machine, and one that asks for a
        # processor.
        for _ in range(2):
            bench.run_through("S")
            bench.run_through("T", "--cores", "1")
        # And one it has seen be short once, which is not enough.
        bench.run_through("U", "--cores", "1")
        # Something it has not seen before has two of the four processors,
        # and something that wants all four waits for it.
        bench.start("L", "--cores", "2")
        wait_for(lambda: bench.begun("L"), "a job the queue has not seen before to begin")
        waiter = bench.start_heard("W")
        wait_for(lambda: "Running now: " in waiter.read_text(), "one that wants the whole machine to wait for it")

        # With no time counted as short, the short one waits its turn.
        strict = bench.start_heard("S", env={**bench.environment, "BN_QUEUE_BRIEF": "0"})
        wait_for(lambda: "1 waiting before this" in strict.read_text(), "the short one to wait, where nothing counts as short")
        check(bench.begun_times("S") == 2, "it has not begun")
        bench.started[-1].send_signal(signal.SIGKILL)
        bench.started[-1].wait()
        # Nor does it go ahead from another folder, where the same words are
        # another command; nor does the one seen to be short only once.
        for count, (name, more) in enumerate((("S", {"cwd": bench.folder}), ("U", {}))):
            other = bench.start_heard(name, *(["--cores", "1"] if name == "U" else []), **more)
            wait_for(lambda: f"{1 + count} waiting before this" in other.read_text(), f"{name} to wait, not known to be short")
        check(bench.begun_times("S") == 2 and bench.begun_times("U") == 1, "neither has begun")
        for process in bench.started[-2:]:
            process.send_signal(signal.SIGKILL)
            process.wait()

        # As things are, it goes at once, with the two processors there are.
        bench.start("S")
        wait_for(lambda: bench.begun_times("S") == 3, "the short one to begin, ahead of the one that waits")
        check(not bench.begun("W"), "the one that wants the whole machine has not begun")
        rows = [row for row in bench.listed() if " S " in row]
        check(len(rows) == 1 and rows[0].startswith("heavy: running: ") and rows[0].endswith(", brief, 2 cores."), f"it is listed as short, with the two it has ({rows})")
        # With every processor taken, the other short one has to wait for
        # one, and goes when there is one, with the one it asked for.
        other = bench.start_heard("T", "--cores", "1")
        wait_for(lambda: "Running now: " in other.read_text(), "the other short one to wait, with no processor free")
        check(bench.begun_times("T") == 2, "it has not begun")
        bench.let_end("S")
        wait_for(lambda: len(bench.turns()) == 6, "the first short one's turn to be written down")
        check(bench.turns()[-1][2] == "2", f"with the two processors it had ({bench.turns()[-1][:5]})")
        wait_for(lambda: bench.begun_times("T") == 3, "the other short one to begin when there is a processor")
        rows = [row for row in bench.listed() if " T " in row]
        check(len(rows) == 1 and rows[0].endswith(", brief, 1 core."), f"with the one processor it asked for, though two are free ({rows})")

        # A command the queue has not seen be short waits, processors or no.
        unknown = bench.start_heard("N", "--cores", "1")
        wait_for(lambda: "1 waiting before this" in unknown.read_text(), "one it has not seen before to wait behind")
        check(not bench.begun("N"), "it has not begun, though a processor is free")

        # The long one ends. Nothing but a short one is running now, so no
        # other short one may begin before what has waited.
        bench.let_end("L")
        wait_for(lambda: bench.ended("L"), "the long one to end")
        (bench.folder / "go-S").unlink()
        late = bench.start_heard("S")
        wait_for(lambda: "2 waiting before this" in late.read_text(), "a short one that comes now to wait")
        check(bench.begun_times("S") == 3, "it has not begun, though three processors are free")
        check(not bench.begun("W"), "nor has the one that wants the whole machine, with a short one still running")
        bench.let_end("T")
        wait_for(lambda: bench.begun("W"), "the one that wants the whole machine to begin when the short one ends")
        check(not bench.begun("N") and bench.begun_times("S") == 3, "and before those that came after it")
        bench.let_end("W")
        wait_for(lambda: bench.begun("N") and bench.begun_times("S") == 4, "the rest to begin after it")
        bench.let_end("N")
        bench.let_end("S")
        for process in bench.started:
            if process.returncode is None:
                check(process.wait(timeout=60) == 0, "each job ends well")
        wait_for(lambda: len(bench.turns()) == 11, "every turn to be written down")

        # A command whose last turn was long is short no more. The last
        # turn written down for S, as if it had run for hours:
        turns = bench.turns()
        last = next(turn for turn in reversed(turns) if " S " in turn[6])
        with open(bench.queue / "turns.log", "a") as log:
            log.write("\t".join([*last[:4], "9999", *last[5:]]) + "\n")
        (bench.folder / "go-S").unlink()
        bench.start("M", "--cores", "2")
        wait_for(lambda: bench.begun("M"), "another job the queue has not seen before to begin")
        again = bench.start_heard("X")
        wait_for(lambda: "Running now: " in again.read_text(), "another that wants the whole machine to wait")
        once_long = bench.start_heard("S")
        wait_for(lambda: "1 waiting before this" in once_long.read_text(), "the one that was long to wait its turn")
        check(bench.begun_times("S") == 4, "it has not begun")
        bench.let_end("M")
        wait_for(lambda: bench.begun("X"), "what it waited behind to begin")
        bench.let_end("X")
        wait_for(lambda: bench.begun_times("S") == 5, "and then it")
        bench.let_end("S")
    finally:
        bench.finish()


def a_killed_job_gives_up_its_turn(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "killed")
    try:
        first = bench.start("A")
        wait_for(lambda: bench.begun("A"), "the first job to begin")
        bench.start("B")
        wait_for(lambda: bench.waiting() == 1, "the second to be waiting")
        first.send_signal(signal.SIGKILL)
        wait_for(lambda: bench.begun("B"), "the second to begin when the first was killed")
        check(not bench.ended("A"), "the first never ended")
        bench.let_end("B")
    finally:
        bench.finish()


def a_killed_waiter_holds_nobody_up(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "waiter")
    try:
        bench.start("A")
        wait_for(lambda: bench.begun("A"), "the first job to begin")
        second = bench.start("B")
        wait_for(lambda: bench.waiting() == 1, "the second to be waiting")
        bench.start("C")
        wait_for(lambda: bench.waiting() == 2, "the third to be waiting")
        second.send_signal(signal.SIGKILL)
        second.wait()
        bench.let_end("A")
        wait_for(lambda: bench.begun("C"), "the third to begin, the second having been killed while it waited")
        check(not bench.begun("B"), "the second never began")
        bench.let_end("C")
    finally:
        bench.finish()


def a_turn_lasts_as_long_as_what_the_command_started(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "children")
    try:
        # A shell that starts a job and ends at once, leaving the job going.
        # Whoever reads the shell's output has all of it when the shell
        # ends: nothing the queue leaves behind keeps it open.
        left = " ".join(f"'{part}'" for part in bench.job_command("A"))
        try:
            shell = subprocess.run(
                bench.command("sh", "-c", f"{left} > /dev/null 2>&1 < /dev/null & echo started"),
                env=bench.environment, capture_output=True, text=True, timeout=60,
            )
            check(shell.returncode == 0 and shell.stdout == "started\n", f"the shell ends, and its output with it ({shell.stdout!r})")
        except subprocess.TimeoutExpired:
            check(False, "the shell's output ended when the shell did")
        check(not bench.ended("A"), "while what it started goes on")
        wait_for(lambda: bench.begun("A"), "what the shell started to begin")
        bench.start("B")
        wait_for(lambda: bench.waiting() == 1, "the next to be waiting, though the shell has ended")
        check(not bench.begun("B"), "the next has not begun while what the shell started goes on")
        bench.let_end("A")
        wait_for(lambda: bench.begun("B"), "the next to begin when that ended")
        bench.let_end("B")
    finally:
        bench.finish()


def what_a_job_starts_does_not_wait(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "nested")
    try:
        inner = bench.command(*bench.job_command("A"))
        outer = subprocess.Popen(bench.command(*inner), env=bench.environment)
        bench.started.append(outer)
        wait_for(lambda: bench.begun("A"), "a job started through the queue by one having its turn to begin")
        check(bench.waiting() == 0, "without waiting for a turn of its own")

        # Another queue is another matter: it does not wait for this one,
        # and this one does not wait for it.
        other = subprocess.run(
            bench.command("--name", "x4", "sh", "-c", "echo free"), env=bench.environment, capture_output=True, text=True, timeout=60
        )
        check(other.stdout == "free\n" and other.stderr == "", f"a queue of another name runs at once ({other.stdout!r}, {other.stderr!r})")

        # Nor does anything that is told not to queue.
        off = subprocess.run(
            bench.command("sh", "-c", "echo now"), env={**bench.environment, "BN_QUEUE_OFF": "1"}, capture_output=True, text=True, timeout=60
        )
        check(off.stdout == "now\n" and off.stderr == "", "with BN_QUEUE_OFF a command runs at once")
        bench.let_end("A")
        check(outer.wait(timeout=60) == 0, "the job ends well")

        bad = subprocess.run(bench.command("--name", "a/b", "true"), env=bench.environment, capture_output=True, text=True)
        check(bad.returncode == 2, "a name that could not be a file's is refused")
    finally:
        bench.finish()


def runs_without_a_folder(queued: Path, work: Path) -> None:
    bench = Bench(queued, work, "nofolder")
    # A folder that cannot be made: a file is in the way.
    in_the_way = bench.folder / "file"
    in_the_way.write_text("")
    result = subprocess.run(
        bench.command("sh", "-c", "echo ran"),
        env={**bench.environment, "BN_QUEUE_DIR": str(in_the_way / "queue")},
        capture_output=True,
        text=True,
        timeout=60,
    )
    check(result.returncode == 0 and result.stdout == "ran\n", "the command runs all the same")
    check("Running without waiting for a turn." in result.stderr, f"and says that it did not wait ({result.stderr!r})")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    queued = Path(sys.argv[1]).resolve()
    work = Path(sys.argv[2]).resolve()
    for test in (
        passes_a_command_through,
        takes_turns_in_order,
        those_that_say_how_many_processors_share_the_machine,
        the_order_holds_for_those_that_share,
        works_beside_one_that_knows_only_whole_turns,
        writes_each_turn_down,
        what_has_been_short_goes_ahead_while_something_long_runs,
        watching_says_each_change,
        a_killed_job_gives_up_its_turn,
        a_killed_waiter_holds_nobody_up,
        a_turn_lasts_as_long_as_what_the_command_started,
        what_a_job_starts_does_not_wait,
        runs_without_a_folder,
    ):
        before = failures
        test(queued, work)
        print(f"{'ok' if failures == before else 'FAILED'}: {test.__name__.replace('_', ' ')}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
