# Diagnostics

## Diagnostic stream

The application sends trace records, log text and metrics over its serial
line as binary frames (`core/frame.h`): the USB port on a device. On the
desktop the same stream goes to the file named by `BN_SERIAL_OUT`:

```
BN_SERIAL_OUT=run.bin ./build/host/app/desktop/biblionaut-desktop --run-ms 5000
```

Frames are delimited by zero bytes, so they can share the line with plain
text such as boot messages. In a serial monitor the frames show up as short
bursts of unreadable bytes between the readable lines.

`bn-trace` (built with the host build) decodes the stream. It prints every
event as a line of text and can write a timeline to open at
[ui.perfetto.dev](https://ui.perfetto.dev) or `chrome://tracing`:

```
# A capture file from a desktop run
./build/host/tools/trace/bn-trace run.bin --out run.json

# The plugged-in device, live, until Ctrl-C
./build/host/tools/trace/bn-trace --out device.json --save device.bin
```

With no input named, `bn-trace` finds the USB serial device itself; name
the port (for example `/dev/cu.usbmodem101`) if more than one is plugged in.
It restarts the device first, so the capture begins with the boot messages
and the hello that names the firmware. `--no-reset` attaches without
restarting, and then shows whatever the device had buffered since something
last read it.

The hello also says why the device started, as its hardware has it
(`core/start.h`): power coming on, the reset pin, a host on the USB port
(which is what `bn-trace` restarting it is), the firmware itself, a crash,
a watchdog, the supply falling too low, or waking from deep sleep. So a
run that was cut short is not taken for a fresh start:

```
=== boot 1: 0.1.0-dev+5e81f9e (stream protocol 3), started by power on ===
```

A capture from before the hello said so (protocol 2) is still read, and
its line stops at the bracket.

In the timeline each boot of the device is a process and each task a thread;
spans are slices, log lines are markers, and each counter and gauge is its
own graph. `--save` keeps the raw bytes so a capture can be decoded again
later.

Trace ids are turned back into names from the lists in each part's
`trace_ids.h`, which the tool is built from. A capture from newer firmware
than the tool shows ids it does not know as numbers.

## Crash reports

When the firmware stops unexpectedly (a processor fault, `abort()`, a failed
assertion, running out of memory, a stack overflow, the watchdog), ESP-IDF
prints what it knows on the console and restarts the device. Before that happens the
firmware saves a report of its own: the reason, the task, the registers, the
top of the stack and the last 32 trace records. The report sits in memory
that survives a restart, and every later start sends it on the diagnostic
stream, until the device loses power or crashes again. So after a crash,
plug the device in and run `bn-trace`: the report is the first thing after
the boot.

The report gives addresses. `tools/bn-symbolize` turns them into functions
and source lines:

```
./build/host/tools/trace/bn-trace | tools/bn-symbolize
./build/host/tools/trace/bn-trace capture.bin | tools/bn-symbolize
tools/bn-symbolize 0x4200614a
```

Addresses only mean something for the exact build that crashed. Every
firmware build keeps a copy of its ELF file in `.elf-archive/` (ignored by
git, the newest 40 are kept), named by its SHA-256; a report carries the
start of that hash, and `bn-symbolize` uses it to pick the right file. If a
report comes from a build that is not in the archive, its addresses are left
alone; `--elf FILE` names a file to use regardless.

The watchdog is what turns a hang into a crash. The application is one
loop, and each turn of it reports to ESP-IDF's task watchdog. If no turn
does for five seconds, because something a turn called has not come back,
the firmware stops as it does for any other fatal error, and the report
says `watchdog: a watched task did not report in time`, with the
registers and the stack of whatever was running: the call that did not
come back. (The last four registers of such a report, `mstatus` to
`mtval`, are zero: they are of the machine's state at a fault, and
nothing saves them when the watchdog breaks in.) (Left as ESP-IDF sets it up, the watchdog watches only the idle
task, and prints a warning on the console every five seconds while the
device stays hung.) The settings are in `app/firmware/sdkconfig.defaults`.

The loop pauses before each turn for as long as the application asks
(`App::PauseUs()`): 10 ms, which is as long as a press can wait before it
is acted on, unless there is work waiting that can be done now, such as a
drawing being read or drawn (`Screen::WorkWaiting()`). Then it does not
pause, but for once at least every 250 ms, so that the idle task, which
the watchdog also watches, has its turn. The desktop's loop does the same.
The 10 ms pauses had come to about 4 s of the 31 s the largest drawing
took to be drawn and kept, and to half of what a small one took.

To see a report without waiting for a bug, build a firmware that crashes on
purpose ten seconds after it starts:

```
tools/idf -DBN_CRASH_TEST=fault build flash     # or abort, oom, stack, hang
./build/host/tools/trace/bn-trace | tools/bn-symbolize
tools/idf -DBN_CRASH_TEST= build flash          # back to the normal firmware
```

`hang` does not crash by itself: its loop stops coming round, and the
watchdog ends it five seconds later. None of them crashes again on the
restart that follows a crash. On the desktop,
`BN_FAKE_CRASH=1` makes the application start with an invented report, which
exercises everything after the crash itself.
