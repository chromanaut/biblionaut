#!/usr/bin/env python3
"""End-to-end check of tools/queued, run by CTest.

tools/queued makes commands take turns: the heavy checks call it so that
two started at once, from two checkouts, do not run at once. Here it is
given jobs that say when they begin and when they end and that end only
when the test lets them, and what is checked is what it promises:

  - a command's output and its exit code are its own;
  - one job at a time, and those that wait go in the order they came;
  - `--list` says what is running and what waits;
  - a job that is killed gives up its turn, and one that is killed while
    it waits holds nobody up;
  - a turn lasts as long as anything the command started;
  - a command started by one that is having its turn does not wait;
  - queues with different names do not wait for each other;
  - with BN_QUEUE_OFF, or a folder that cannot be used, the command runs.

Nothing here goes by how long anything takes. A job waits for a file the
test writes, and the test waits for what a job or the queue has written,
for as long as a minute before it gives up.

usage: queued.py QUEUED WORK_DIR
"""

from __future__ import annotations

import os
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
        self.environment = {**os.environ, "BN_QUEUE_DIR": str(self.queue)}
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
        second = bench.start("B")
        wait_for(lambda: bench.waiting() == 1, "the second to be waiting")
        third = bench.start("C")
        wait_for(lambda: bench.waiting() == 2, "the third to be waiting")
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
        told = second.stderr.read()
        check("queued: waiting for a turn (heavy). Running now: " in told and " A " in told, f"one that waits says what for ({told!r})")
        check("queued: its turn came after " in told, "and when its turn comes")
        check("1 waiting before this" in third.stderr.read(), "and how many are before it")

        after = subprocess.run(bench.command("--list"), env=bench.environment, capture_output=True, text=True).stdout
        check(after == "heavy: nothing running.\n", f"with all of them over, --list says so ({after!r})")
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
        left = " ".join(f"'{part}'" for part in bench.job_command("A"))
        shell = subprocess.Popen(bench.command("sh", "-c", f"{left} &"), env=bench.environment)
        check(shell.wait(timeout=60) == 0, "the shell ends")
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
