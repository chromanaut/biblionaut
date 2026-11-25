# Biblionaut

EPUB reader firmware for the Xteink X4 (ESP32-C3, 800x480 e-ink, SD card),
written so that everything except the hardware layer also builds and runs on
a desktop.

## Layout

| Directory | Contents |
|---|---|
| `core/` | Shared foundation: arenas, a heap within a fixed block of memory, the timing trace, counters and gauges, the framed diagnostic stream, checksums, the inflater, the heap-failure hook, and the interfaces the portable parts use to reach the outside world (clock, byte source, byte sink, display, buttons, power) |
| `hw/` | Hardware abstraction layer. `hw/include` is the interface. `hw/desktop` implements it for the host; on a device, `hw/esp32c3` holds what the chip provides and `hw/x4` what is specific to the X4 |
| `epub/` | Book parsing: zip, XML, OPF / TOC, CSS, the importer and the intermediate format |
| `draw/` | Fonts and drawing: glyphs, metrics, the display-list format, the band renderer, images |
| `layout/` | Book layout: line breaking, hyphenation, pagination, positions. Emits display lists |
| `reader/` | The reader application. The only part that knows about all the others |
| `app/` | Entry points: `app/desktop` for the host, and `app/firmware`, the ESP-IDF project that builds the one firmware image for all supported devices |
| `tools/` | Host programs (`tools/trace`: the `bn-trace` stream decoder; `tools/zip`: `bn-zip`, which reads a ZIP file as the firmware does; `tools/xml`: `bn-xml`, which does the same for XML; `tools/css`: `bn-css`, which does the same for a style sheet; `tools/epub`: `bn-epub`, which says what a book says of itself; `tools/svg`: `bn-svg`, which draws an SVG file as the firmware does; `tools/png`: `bn-png`, which reads a PNG picture as the firmware does; `tools/jpeg`: `bn-jpeg`, which does the same for a JPEG picture; `tools/hyphen`: `bn-hyphen`, which makes a language's hyphenation table and breaks words with one; `tools/lines`: `bn-lines`, which sets text in lines as the firmware does; `tools/font`: `bn-font`, which makes the fonts image the firmware draws text from) and developer scripts (`bn-symbolize` for crash reports, `check-books.py` to run those tools over a folder of real books, `layout-text.py` to write books' paragraphs to a file for the layout check, `queued` to have the heavy checks take turns) |
| `third_party/` | How the code that is not ours is fetched and built: for each, a CMake file and its settings. The code itself is downloaded when a build is configured and is not in this repository (`THIRD_PARTY.md` lists it) |
| `tests/` | Host tests, and the fuzz targets (`tests/fuzz`) |
| `corpus/` | Real books for the tests to read: 28 EPUB files, in the public domain or openly licensed, which are under their own terms and not this project's licence (`corpus/README.md`) |

Dependency rules, enforced by the build (each part has its own include
directory and can only see the parts it links):

```
reader  ->  layout, draw, epub, hw
layout  ->  draw, epub
epub, layout, draw  ->  core        (never hw)
hw                  ->  core
```

Only code under `hw/` and `app/firmware` may include ESP-IDF or FreeRTOS headers.

The code is C++20 with exceptions and RTTI disabled, so anything that throws
ends in an abort. The portable parts (`core`, `epub`, `draw`, `layout`) are
held to more: they allocate from arenas, not the general heap, and call
nothing that can throw, so every failure reaches their caller as a return
value. `reader` and `hw` may use the general heap and the standard library's
allocating types where that is the simpler choice.

Naming: namespaces are `snake_case`; classes, structs and functions are
`PascalCase`; variables, parameters and members are `camelCase`, with a
trailing underscore on private members; constants are `kPascalCase`.
clang-tidy enforces this.

## Host build

Needs CMake 3.25 or later, Ninja, a C++20 compiler and a C compiler. Five
things are downloaded when CMake configures, each checked against a hash:
expat, the XML parser, which is C and is why a C compiler is needed;
TeX's hyphenation patterns for American English, which the build makes
into a table (see Where a word may be broken); FreeType, which draws the
glyphs for `bn-font` (see Fonts); GoogleTest; and, for the tests of
`bn-font`, the font files of Courier Prime. (The fonts of a fonts image are
downloaded only when one is built: see The presets.) The tests also link zlib, to check the inflater against and to
make their ZIP files with, and so does `bn-font`, to say what a fonts image
would take compressed: macOS has it, and on Linux it is the `zlib1g-dev`
package. Most of the end-to-end
tests are run only if Python 3 is found, and one of them makes more of its
test files if there is a `zip` command. A build with the desktop
application's window downloads a third thing, SDL (see A window on the
desktop, below).

```
cmake --preset host
cmake --build --preset host
ctest --preset host
./build/host/app/desktop/biblionaut-desktop
```

The `host` preset is a debug build with AddressSanitizer and
UndefinedBehaviorSanitizer. `host-release` is optimised, without
sanitizers, and has the same tests (`cmake --preset host-release`, and so
on): it is the nearer of the two to how the firmware is compiled.

How much timing trace is compiled in is set with `BN_TRACE_LEVEL`: `0` for
none, `1` for top-level spans only, `2` (the default) for everything. Pass it
when configuring, for example `cmake --preset host -DBN_TRACE_LEVEL=0` or
`tools/idf -DBN_TRACE_LEVEL=1 build`.

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

## The panel and the buttons

The e-ink panel's controller keeps two full images in its own memory, and
the firmware keeps none: pictures are rendered a band of rows at a time and
written straight to the controller (`core/display.h`), as whole rows or as
a window narrower than them ("Windows of a plane", below). `hw/x4` drives
the X4's panel.

The devices have seven buttons. Six of them share two analogue pins and
cannot raise an interrupt, so `hw/x4` reads them every 5 ms and turns the
readings into events: pressed, released, held (`core/input.h`). An event
carries the time the change was first seen.

Until there are pages to show, the application shows a test pattern and the
buttons step through its four pictures (`reader/pattern_viewer.h`):

| Button | What it does |
|---|---|
| Down, Right | The next picture, by a fast refresh |
| Up, Left | The previous picture, by a fast refresh |
| Confirm | The same picture again, by a half refresh |
| Back | The same picture again, by a full refresh |
| Power | Puts the device to sleep, where it can sleep (see Sleep, below) |

The pattern is in the panel's own orientation, which on the X4 is with the
device turned a quarter-turn anticlockwise from the way a book is read.
The panel says which way that is (`Display::ReadingTurn()`), for whatever
draws a page the way it is read and has to turn it into the panel's rows.

The trace of a press runs from the button to the panel:

```
task 1  hw/button_press button=5          the button going down, first reading to settled
task 0  reader/press_to_refresh button=5  from the button going down to the refresh being started
interrupts  hw/refresh_fast               the panel's own time
task 0  reader/panel_sweep picture=3      the picture after, rendered and sent ahead of the next press
```

The picture a press asks for is usually in the panel already, written
there after the last refresh, so the press has only to start the refresh.

On the desktop a simulation stands in for the panel, working out what a
real one would show after each refresh, including the stale pixels a fast
refresh leaves if the two images are allowed to fall out of step. Buttons
are scripted. Four environment variables control them:

```
BN_DISPLAY_OUT=panel BN_INPUT=4000:down,5000:down,6000:confirm \
  ./build/host/app/desktop/biblionaut-desktop --run-ms 9000
open panel-001.png          # one picture per refresh
```

- `BN_DISPLAY_OUT=prefix` saves what the panel shows at every refresh, as
  `prefix-001.png`, `prefix-002.png` and so on.
- `BN_INPUT` presses buttons: `milliseconds:button`, with an optional third
  field for how long the button is held (`5000:power:1500`). The buttons
  are `back`, `confirm`, `left`, `right`, `up`, `down` and `power`.
- `BN_DISPLAY_SPEED=20` makes the simulated panel twenty times quicker than
  a real one, which takes 3.6 s over a full refresh.
- `BN_DEVICE` names the device simulated. `x4` is the only one, and what
  is simulated without it.

In place of the pattern, the application can show the SVG, PNG and JPEG
files in the card's `drawings` folder (`reader/drawing_viewer.h`): a
drawing drawn as a drawing in a book is, and a picture read (`draw/png.h`,
`draw/jpeg.h`, a JPEG made as small as it can be as it is read and still be
as large as it is shown) and scaled (`draw/scale.h`). Each is fitted to the page the way the device is held to
be read, as large as fits and in the middle of it, turned into the panel's
rows, and its grays dithered to the panel's black and white
(`draw/dither.h`). It is for seeing the renderer's and the picture
reader's work on a panel, and the first place they run in the
application's loop. On the desktop, with `BN_STORAGE` naming the folder
that stands for the card:

```
BN_STORAGE=card BN_DRAWINGS=1 ./build/window/app/desktop/biblionaut-desktop --window
```

| Button | What it does |
|---|---|
| Down, Right | The next drawing or picture, by a half refresh, going round from the last to the first |
| Up, Left | The one before |
| Confirm | The same one again, by a half refresh |
| Back | The same one again, by a full refresh |

A firmware built with `-DBN_DRAWING_VIEWER=ON` does the same with
`/drawings` on its card. Nothing else drives the panel then, and none of
the checks can be asked for with it. With `-DBN_DRAWING_TOUR=ON` as well,
or `BN_DRAWING_TOUR` set on the desktop, it goes round the drawings twice
by itself, the second time from the cache (below), each begun as its last
refresh ends, as if Down had been pressed then, and a line says when the
tour is over: for timing them with nobody at the buttons. One that cannot be shown is passed over in the same way,
once its line has said why.

With `BN_TEXT` naming a fonts image, the desktop application shows text set
in its faces in the same place (see Fonts).

```
tools/idf -DBN_DRAWING_VIEWER=ON -DBN_DRAWING_TOUR=ON build flash
./build/host/tools/trace/bn-trace --seconds 600 --save tour.bin
```

A drawing is drawn a band of 16 of the panel's rows at a time, its file
read once and what it draws kept (below), and each band dithered to the
panel's levels and written to both of the panel's planes, so that the half
or full refresh that shows it leaves both right; the drive voltages go off
as the refresh ends. On a panel with grays, as the X4's, each gray is black
in those planes, and a gray pass after the refresh brings the grays out,
as it does for a picture (below). A turn of
the loop draws for about 20 ms and returns, so a drawing of megabytes takes
many turns and the buttons are heard throughout: a press while a drawing is
being drawn gives it up for the one asked for. For each drawing a line of
log text says what it was, how it ended, how many shapes it filled and
stroked, how long it took from the press to its refresh, and what the
renderer left
out:

```
drawing 2 of 3: lewis-carroll-illustration-7.svg, ok, 17 shapes, 0 strokes, 37 ms
```

That is an engraving of 932 KB from a public book, drawn by the optimised
desktop application. On the X4, nine drawings of every kind take from
0.7 s to 18 s from the press to their refresh the first time, and 0.1 to
0.2 s from the cache; their grays come about 0.3 s after the refresh,
the gray planes taking 79 to 151 ms and the gray pass about 205. No stroke
is drawn thinner than a pixel (`SetThinnestStroke()`, below): one much
thinner would be a light gray, which the panel's levels show as a broken
line.

A drawing is read once, and kept on the card while it is shown, in a
hidden file at the top of the card (`/.drawing-spool`, made again for each
drawing): what it draws for each band of the panel ("Keeping a drawing",
below). Each band is then drawn from what was kept, its own part and no
more. If the card will not keep it, or it would be more than 4 MB, the
drawing is read again for each band instead, as it could always be, and a
line says why. The viewer takes about 109 KB of the heap when it starts:
26 KB for the filler's cells, which also hold what each band has in hand
while the file is read, 24 KB for what a drawing defines, 16 KB for the
links of what is kept, 13 KB for a band of grays, which also gathers what
is kept into writes of up to 8 KB while the file is read, since the card
writes a few kilobytes at once faster than a chunk at a time, 8 KB for the renderer, a
kilobyte for the recorder, 6 KB for what is carried in dithering and a
band of bits, 8 KB of the file read at a time, 3 KB for a band of what
goes to the cache or comes from it, at two bits a pixel, and 4 KB for the
cache to gather such bands into whole sectors of the card.

What is shown is kept as well, in the cache of pictures on the card
(`/.picture-cache`, "Keeping what is shown", below), the size it is
shown and in its levels, two bits a pixel in the X4's four: a drawing as
it lies on the panel, and a
picture as it lies on the page, its rows in the order they came. Shown again, while its file is the same size and was last written
at the same time, it is read from there a band at a time and written to
the panel, and nothing is drawn. What the cache does not have is drawn,
and kept as its bands are written; what it has damaged, or half written
by a press that gave it up, is drawn and kept again. A drawing whose file
could not all be read, or a picture not read whole, is not kept. A line
says what the cache did:

```
drawing 2 cache: kept, 45 KB in 1 ms
drawing 2 of 3: lewis-carroll-illustration-7.svg, from the cache, 3 ms
drawing 2 cache: read 45 KB in 0 ms
```

That is the same engraving, the first time and then gone back to, on the
optimised desktop application.
The trace has a span for each drawing or picture, from the press to the
refresh, and the metrics the time the last took (`drawing_ms`).

Once a drawing's refresh is over, a second line says where its time went,
added up by the viewer, since a long drawing reads the card more times
than a capture can keep a span for: how many bands it was drawn in, how
much of the file was read in all, and how long went in reading it, in
rendering, in dithering, in writing the bands to the panel, in waiting
for the panel to take a band, in the viewer's own turns otherwise
(`other`), in the loop between the viewer's turns, and in the refresh. A
third says how much was kept and read back, how long each took (and in
how many writes to the card) and how
long drawing the bands from it took; or why the drawing was not kept. A
last says how long writing the planes of its gray pass took, and the pass,
or why there was none. A picture's says how long its refresh took, and
then the same of its gray pass.

```
drawing 2 timed: 30 bands, 918 KB read in 0 ms; rendering 19 ms, dithering 8 ms, writing 0 ms, waiting 6 ms, other 0 ms, between turns 0 ms, refreshing 1571 ms
drawing 2 timed: spool 623 KB written in 0 ms (78 writes), 623 KB read in 1 ms; replaying 4 ms
drawing 2 timed: gray planes 1 ms, gray pass 218 ms
```

That is the same engraving as above, 932 KB read once for its size and
once to keep it, on the optimised desktop application, where the panel's
refresh is as long as the X4's.

A picture's rows come from its file in the order the page is read, which
is not the panel's order when the panel is turned to be read, as the X4's
is. Its file is read through once: its rows are read, made the size they
are shown at and dithered, and written to both planes as they come, a
strip at a time. A strip is the rows of the page that fall in one byte of
the panel's rows: on a panel turned a quarter, eight of the page's rows
are a column of the panel one byte wide and the panel's height, written as
one window ("Windows of a plane", above); otherwise they are eight of the
panel's rows. The strips above and below the picture are written white. So
nothing of the panel is kept but the strip being filled. A picture whose
rows would need more than 16 KB of the reader's memory, an interlaced PNG,
a JPEG of a kind that is not read, and a file that is not a picture are
not shown, and a line says why.

A picture is dithered to as many levels as the panel shows
(`Display::Levels()`): on the X4, and the desktop's X4, four, the grays by
a gray pass ("Grays", below). It is shown first by a half or full refresh
in black and white, each of its grays black, and kept in the cache at two
bits a pixel; the drive voltages stay up. Then it is read back from the
cache, its grays are marked in the two planes, strip by strip as before,
and the gray pass brings them out. A picture with no grays has no gray
pass, and one that could not be kept stays in black and white. A press
during the first refresh, for another, is not kept waiting for the grays.

For each picture shown, a line says what it was, the size it was shown
at, and how long the reading, the scaling, the dithering (with the setting
of the panel's bits) and the writing took, and then from the press to its
refresh; and once the panel is done, a second line says how long the
refresh took, and the gray pass: writing its planes, and the pass itself,
or why there was none:

```
picture 3 of 3: c-gutenberg.png, ok, 332x500 palette shown 480x723: reading 1 ms, scaling 0 ms, dithering 6 ms, writing 0 ms; 10 ms
picture 3 timed: refreshing 1577 ms; gray planes 4 ms, gray pass 209 ms
```

That is an illustration from a Project Gutenberg book in `corpus/`, made
larger to fill the page, on the optimised desktop application, where the
refresh and the gray pass take as long as on the X4. On the X4,
an engraving of 600 by 825 pixels in 16 colours from another book in
`corpus/`, made 480 by 660, takes 1.1 s from the press to its refresh:
nearly a third dithering, under a quarter keeping it in the cache at two
bits a pixel (77 KB, below), a fifth reading, a sixth scaling, and 60 ms
writing its 100 strips to both planes. Its grays come 0.4 s after the
refresh: 167 ms reading it back and writing the gray planes, and the gray
pass, with the switch-off after it, 208 ms. A title page of 2,800 by 880
takes 1.0 s, under half of it reading. A colour photograph of 4 megapixels
takes 11.5 s, 9.7 s of it reading its 12 MB of rows. From the
cache, a picture is at its refresh in 0.1 to 0.3 s. What a picture needs
is taken from the heap the first time one is shown, and kept: about 73 KB,
32 KB of it PNG's window, 16 KB the reader's rows, 9 KB the JPEG reader,
6 KB the scaler's sums, 4 KB a buffer to read the file through, 3 KB the
PNG reader, and 2.6 KB for a row at the size shown, the two strips being
filled and two rows of bits.

What the desktop simulates of a device is described once, in a profile
(`hw/desktop/device_profile.h`): the panel's size, how long it takes over
each kind of refresh and over switching off, which way the panel is
turned to be read, how many levels the panel shows, and which buttons the device has, on which edges and
where along them. A button wired to more than one spot, as Up and Down
are where a device has them on both sides, is listed once for each: it
is the same button whichever is pressed, and is lit at all of them. The
simulated panel and the window take everything they need from it. The X4's times are as measured on one, and its
buttons' places along the edges are drawn near enough. A profile does
not say how its controller keeps its two planes: everything that drives a
panel (`core/display.h`) keeps to the rules found on the X4. A panel's
grays are its own business: the drawing code draws in 256 and dithers to
as many as a panel shows, which on the X4 is four, by a gray pass. The
simulated panel keeps a level for
each pixel and shows them in the window; it saves its picture at one bit
a pixel while it shows only black and white, and at two once it shows
grays.

### A window on the desktop

A build configured with `BN_DESKTOP_WINDOW` gives the desktop application
a window. Run with `--window`, it draws an X4 there: the simulated panel as
it would look at each moment, and the device's buttons around it. The
window is made with SDL, which CMake then fetches and builds
(`third_party/sdl`); no other build has it. The `window` preset is such a
build, of the application alone:

```
cmake --preset window
cmake --build --preset window
./build/window/app/desktop/biblionaut-desktop --window
```

The buttons are where the device's profile puts them. On an X4 held for
reading, that is Back, Confirm, Left and Right in a row under the panel,
and Power, Up and Down down the right edge (`hw/desktop/device_layout.h`).
Clicking one presses it, and holding the mouse down holds it. The keys
press them too, those the device has:

| Key | Button |
|---|---|
| Left, Right, Up, Down | The buttons of those names |
| Page Up, Page Down | Up and Down, the two on the side |
| Return | Confirm |
| Escape, Backspace | Back |
| P | Power |

A button is drawn lit while the application has been told that it is
down, by a key, a click or `BN_INPUT`'s script. R turns the device between
the way a book is read, which is how the window opens, and the panel's own
way, a quarter-turn anticlockwise, which is how `BN_DISPLAY_OUT` saves the
picture. The window can be made any size, and the device keeps its shape.

A refresh takes as long as it would on a device, and the window shows it
going. A full refresh shows the old picture black for white, then all
black, then all white, for a third of its time each, and then the new
picture. A half refresh shows black, then white. A fast refresh leaves the
old picture until it is over. That is not the panel's own sequence, only
enough to show which kind of refresh was made and how long it kept the
panel. While it lasts, the window's title names it. What the window shows
once it is over is what the simulation worked out, stale pixels and all.

Everything else is as it is without the window. `BN_INPUT` presses
buttons alongside the keyboard, `BN_DISPLAY_OUT` saves the pictures,
`BN_DISPLAY_SPEED` quickens the panel, and the checks run as they do.
Without `--run-ms`, the application runs until the window is closed. A
press is dated from when SDL saw the key go down, so the trace times it
from there, as it times a press on a device from the button. How quickly
the application answers is the desktop's, which is many times quicker
than an X4's: that takes a device and its trace.

The window build has one more test, `e2e.Window` (`ctest --preset window`).
It runs the application twice with the same scripted presses, with its
window and without, the window on SDL's stand-in for a screen (the `dummy`
video driver, which needs none), and checks that the panel was given the
same pictures both times.

### Sleep

Power puts the device to sleep, where the device can sleep
(`core/power.h`), unless the storage check, the book check or the layout
check is built in: those run again at each press of Power instead (see
Storage and Timing layout on a device, below). The
refresh in progress is allowed to finish, the panel's voltages are
switched off and its controller put to sleep, with the picture left on
it, the trace is sent, and then the device is put to sleep. Waking is a new start: the application begins
again as it does when power comes on, and the stream's hello says it was
started by waking from deep sleep.

No device can sleep yet. On the X4 Power leaves it awake: which of its
pins can wake it, and what of it has to be switched off first for it to
draw little, have not been found out.

The desktop simulates it. Once the device has been put to sleep, the run
is over, as it would be on a device, and the next press of Power, from
the window, the keyboard or `BN_INPUT`, begins a new run, woken from deep
sleep. In between, every other button is passed over, and the window's
title says the device is asleep. What a device would lose in its sleep,
the desktop keeps: the panel's simulation, the presses still to come in
the script, and the counters in the metrics, which go on from where they
were.

```
BN_SERIAL_OUT=capture.bin BN_DISPLAY_SPEED=20 BN_INPUT=400:down,1000:power,2000:power,2600:down \
  ./build/host/app/desktop/biblionaut-desktop --run-ms 3200
./build/host/tools/trace/bn-trace capture.bin
```

Among what it prints:

```
=== boot 1: 0.1.0-dev+4f85335 (stream protocol 3), started by power on ===
[     1.009856] task 0  hw/panel_power_off  12.631 ms
[     1.001023] task 0  reader/press_to_sleep  21.469 ms
=== boot 2: 0.1.0-dev+4f85335 (stream protocol 3), started by waking from deep sleep ===
[     2.006892] task 0  hw/refresh_full power_off_after=0  187.639 ms
```

`reader/press_to_sleep` runs from Power going down to the device being
put to sleep. The second run shows its first picture by a full refresh,
as any start does. The end-to-end test `e2e.PowerSleep` makes this run
and checks the two runs in the trace, and that the panel was given the
first picture, the step, the first picture again on waking, and the step
again.

### The panel's two planes

The panel's controller keeps two images, and a fast refresh works from
both: the new image, and the old one it is to change from. On the X4 the
two change places by themselves after every fast refresh, and go back to
their first places when the panel's drive voltages are switched off.
`core/display.h` sets out the rules, which were found by reading the
planes back out of a device, and what they ask of whatever drives the
panel. The pattern viewer follows them: it writes the next picture ahead
of time, leaves the old image alone while the voltages are on, and writes
both again once they have been switched off. The simulated panel on the
desktop behaves the same way, so a mistake here shows in the tests.

A firmware built with `-DBN_PLANE_CHECK=ON` checks this on a device (on
the desktop, set `BN_PLANE_CHECK`). Whenever the panel comes to rest,
after a refresh and again after the voltages are switched off, both planes
are read back and the trace says what was found:

```
reader/new_plane_holds picture=3
reader/old_plane_holds picture=2
```

1 to 4 is that picture, 8 a blank plane and 9 anything else, in which case
a `new_plane_bands` or `old_plane_bands` line follows with a digit for each
band of 80 rows. The new-image plane should hold the picture after the one
on the panel, and the old-image plane the one on the panel. The metrics
count the readings and how many found otherwise:

```
plane_checks=14 planes_unexpected=0
```

Reading back takes about a second, a band at a time; a press during it is
acted on at once and the reading starts again when the panel is next at
rest. The first time anything is read, the controller's status byte and
display options are recorded too (`hw/panel_status`, `hw/panel_options`).

```
tools/idf -DBN_PLANE_CHECK=ON build flash
./build/host/tools/trace/bn-trace | tee planes.log
tools/idf -DBN_PLANE_CHECK=OFF build flash     # back to the normal firmware
```

### Windows of a plane

Besides whole rows, a plane can be written a window at a time
(`Display::WriteWindow()`): some rows, and in each of them a run of whole
bytes, the rest of the plane left as it was. On a panel turned a quarter,
as the X4's is, a picture's rows run down the panel's columns, so eight of
them, as they come, are a window one byte wide and the panel's height.
Written so, a picture needs no copy of the whole panel.

A firmware built with `-DBN_WINDOW_CHECK=ON` checks on a device that the
controller takes such windows, and times them. At start, before the
application, it fills each plane with a pattern by whole rows, by windows
one byte wide and the panel's height, and by windows of eight rows, each
twice; then it writes eight windows of odd sizes and places over a plane
already filled; and it reads the plane back after each, and says whether
every byte is what it should be. On the X4:

```
window check: a panel of 800 by 480, 100 bytes a row
window check: new plane, whole rows: right, written in 14230 us
window check: new plane, columns of a byte: right, written in 31226 us
window check: new plane, eight rows at a time: right, written in 23048 us
window check: new plane, odd windows over whole rows: right, written in 2622 us
window check: old plane, whole rows: right, written in 14179 us
window check: old plane, columns of a byte: right, written in 31180 us
window check: old plane, eight rows at a time: right, written in 23041 us
window check: old plane, odd windows over whole rows: right, written in 2613 us
window check: windows outside the panel, or not filled exactly, refused
window check: done
```

(The second round of each is left out here.) A window costs 0.15 to
0.17 ms more than its bytes sent as part of whole rows, for the commands
that set it: a plane in 100 columns takes 31 ms against 14 ms in whole
rows. The
times include making the pattern, which is the same in each.

```
tools/idf -DBN_WINDOW_CHECK=ON build flash
./build/host/tools/trace/bn-trace --seconds 20 | grep 'window check'
tools/idf -DBN_WINDOW_CHECK=OFF build flash     # back to the normal firmware
```

### Grays

The X4's panel shows black and white, and two grays between them after a
gray pass (`Display::StartGrayRefresh()`). A picture in four levels is
first shown by a full or half refresh with each of its grays black. Then
the two planes mark the grays, the old-image plane every gray pixel and
the new-image plane the dark ones, and the gray pass drives the marked
pixels from black towards white: a little for a dark gray, more for a
light one. Pixels not marked are left as they are. The pass runs a
waveform the driver gives the controller in place of the panel's own,
with the drive voltages to run it at: the OpenX4 SDK's own gray table
(`THIRD_PARTY.md`). The next full or half refresh loads the panel's
waveform again and shows black and white, leaving nothing of the grays.
A panel that shows no grays refuses the pass. The desktop's X4 takes it
as the X4 does, the marked pixels moved one level towards white or two.

A firmware built with `-DBN_GRAY_CHECK=ON` shows, at start, a picture
made for looking at: four patches of the four levels, a ramp from black
to white dithered to the four, and the same ramp dithered to black and
white. It shows it in black and white, then gives it the gray pass and
holds it for 30 seconds; then shows large squares by a half refresh, in
which any gray left behind would show; then the picture again with the
drive voltages kept up from the half refresh to the gray pass. Each step
is said on the console as it begins, with what it took. On the X4:

```
gray check: a full refresh to white took 3719 ms
gray check: the picture, the drive voltages off between the refreshes
gray check: black and white written in 290 ms
gray check: the half refresh took 1709 ms
gray check: in black and white, every gray black; held 5 s
gray check: the gray planes written in 581 ms
gray check: the gray pass took 288 ms
gray check: LOOK NOW: four patches, black to white; the four-level ramp smoother than the two-level one; held 30 s
gray check: the squares written in 300 ms
gray check: the half refresh took 1709 ms
gray check: LOOK NOW: large squares; any trace of the patches or the ramps is grays not cleared; held 15 s
gray check: the picture, the drive voltages kept up from one to the other
gray check: black and white written in 290 ms
gray check: the half refresh took 1569 ms
gray check: in black and white, every gray black; held 5 s
gray check: the gray planes written in 581 ms
gray check: the gray pass took 208 ms
gray check: LOOK NOW: four patches, black to white; the four-level ramp smoother than the two-level one; held 30 s
gray check: done
```

Looked at on the panel, the four patches were four levels, the
four-level ramp the smoother, nothing of the grays was left in the
squares, and the second showing was as the first. The gray pass takes
208 ms with the drive voltages up, 288 ms when it has to bring them up.
The writing times are mostly the check making its picture a pixel at a
time.

```
tools/idf -DBN_GRAY_CHECK=ON build flash
./build/host/tools/trace/bn-trace --seconds 150 | grep 'gray check'
tools/idf -DBN_GRAY_CHECK=OFF build flash     # back to the normal firmware
```

## Storage

Files are reached through three things in `core`:

- `core/storage.h` is the bare interface a hardware port implements: open,
  read and write at an offset, sync, the handful of calls that work on
  names, and listing a directory. Every call says how it ended
  (`StorageStatus`), and every port accepts the same paths, which are the
  ones an SD card can hold. A file is opened one of three ways: to be read
  (`kRead`); new and empty, to be written (`kCreate`); or as it is, to be
  read and written over and added to (`kUpdate`), which is how a file that
  grows over many sessions is kept without being written out again each
  time. A file has one writer or any number of readers, never both, on
  every port: that is what FatFS allows. Eight things can be open at once
  (`Storage::kMaxOpen`), a directory being listed counting as one, and two
  of them can be directories. `Stat()` and the listing say of a file its size
  and when it was last written (`FileInfo::modified`: FAT's date and time
  on a card, seconds on the desktop), which is how something made from a
  file, such as a picture in the cache, is known to be out of date.
- `core/block_file.h` is what code reads and writes files with. It keeps a
  position and a buffer of its own, asks storage for whole blocks at
  multiples of the block size, and counts what it asks for (`sd_reads`,
  `sd_read_bytes`, `sd_seeks`, `sd_writes`, `sd_write_bytes`, `sd_syncs`).
  It is also a `ByteSource`, so a parser can read a file without knowing it
  is one.
- `core/file_replacement.h` writes a file so that it is never found half
  written, whenever the power goes. An SD card cannot put one file in the
  place of another in one step, so it takes several, and
  `RecoverReplacement()` finishes or undoes whatever was cut short.

On a device the card is on the wires it shares with the panel, driven by
ESP-IDF's SD driver and FatFS, which is called directly
(`hw/esp32c3/sd_storage.cpp`). Only FAT file systems are understood, which
is what cards of up to 32 GB are sold with; a bigger card comes with exFAT
and has to be formatted as FAT32 first.

FAT keeps a file as a chain of pieces, and to go to a place in a file
FatFS follows the chain there, from the file's start if the place is
behind where it is, reading the card's table as it goes: the larger the
file, the longer that takes. On an X4, going from the start of a book of
353 MB to its end took 67 ms. So the driver gives a large file that is
opened to be read a table, which FatFS fills with a map of the file's
pieces and goes by afterwards, reading nothing: a place in that book then
costs what it does in a small one, 3.5 ms with the 4 KB read there. Making
the map follows the chain once, so it is paid for when the file is opened
(62 ms for that book). A file is large, here, if its chain is longer than
one sector of the card's table holds, which is 128 clusters (4 MB where a
cluster is 32 KB): a shorter chain is one read of the table however it is
followed, and such a file is opened as it always was. A table has room for
a file in 63 pieces, and is half a kilobyte; there are four of them, for
the eight files that can be open, since a map is for a large file read
from all over, which is a book, and FatFS cannot make a file longer that
has one, so a file opened to be written is given none. A file in more
pieces, and one opened while all four are in use, goes without a map and
is only slower. The metrics say how many files were given a map
(`sd_maps`), how many were in too many pieces (`sd_maps_refused`), how
many found no table free (`sd_maps_none_free`), the most numbers a file's
map needed (`sd_map_items`, two for each piece and two more), and the size
of the card's clusters (`sd_cluster_bytes`).

On the desktop the card is a folder (`hw/desktop/folder_storage.h`), the
one named by the `BN_STORAGE` environment variable; without it there is no
card. It is made to behave as a card does where that is cheap: the same
paths are refused, only eight files can be open at once (a directory being
listed counts as one, and only two can be), a file open to be written
cannot be opened again nor an open file opened to be written, an open file
cannot be removed or renamed, and a rename will not replace a file.

The application gets storage ready at start and says whether it is
(`storage_ok` in the metrics, `reader/storage_start` in the trace). It
does nothing else with it yet. On a device the trace also says what was
found, and why not if nothing usable was:

```
hw/sd_card megabytes=30436          the card's size
hw/sd_file_system kind=3            1: FAT12, 2: FAT16, 3: FAT32
hw/sd_start status=0  312.5 ms      0: ready, 1: no card, 8: a card that cannot be used
hw/sd_card_error code=263           the card did not answer
hw/sd_mount_error code=13           no file system FatFS knows: exFAT, most likely
```

A firmware built with `-DBN_STORAGE_CHECK=ON` puts the card through its
paces (on the desktop, set `BN_STORAGE_CHECK`): at start, and again at each
press of the Power button, which also looks for a card again if none was
found before. It writes a 256 KB file in 4 KB blocks, reads it back
checking every byte, and reads short pieces from all over it. It opens the
file again as it is, finds that it cannot then be opened a second time,
writes over one block in the middle and adds one at the end, and reads
those and the blocks either side back. It has eight different files open
at once and finds that a ninth is refused. It replaces a small file twice,
and removes what it made (`reader/storage_check.h`). It does a
little each turn, so the panel and the buttons carry on around it. Each
read, write and sync of the card is in the trace with the time it took
(`hw/sd_read`, `hw/sd_write`, `hw/sd_sync`), and the outcome is there and
in the metrics:

```
reader/storage_check passed=1
reader/storage_check_failed step_and_status=307     step 3 (writing), status 7 (no space)
storage_check=1[1..1]
```

```
tools/idf -DBN_STORAGE_CHECK=ON build flash
./build/host/tools/trace/bn-trace | tee card.log
tools/idf -DBN_STORAGE_CHECK=OFF build flash     # back to the normal firmware
```

It can be combined with `-DBN_PLANE_CHECK=ON`, to see whether using the
card disturbs what the panel holds.

## Decompression

`core/inflate.h` undoes DEFLATE (RFC 1951), the compression inside a ZIP
file and so inside an EPUB. `Inflater` is a `ByteSource` that reads another:
it draws compressed bytes from its source as it needs them and gives the
plain bytes out in pieces of whatever size is asked for, so a file is never
whole in memory in either form. It is our own code, the same on the desktop
and the device.

- **Memory**: the caller provides the 32 KB window the format needs. The
  object itself is 2.6 KB on the device. It allocates nothing.
- **Flash**: 4.7 KB, as compiled for the device.
- **Speed**: on an X4, over 34 books, it gives out about 2,740 KB a
  second with the ZIP checksum included (which is a third of that time),
  taking in about 1,360 KB a second, where the SD card delivers about
  980. Three things got it there from the 1,550 KB a second it gave out
  at first. The loop that reads a compressed block keeps the state of
  the input in local variables and not in the object, where the compiler
  had to load and store it again for every symbol, since any byte
  written might have changed it. What is produced is written once, into
  the window, and copied to the caller in one piece when the call ends.
  And a repeat of earlier output, or the bytes of a stored block, are
  copied in runs and not a byte at a time. On the desktop the same
  change doubles its speed on the text of the books in `corpus/`, and
  multiplies it by more than eleven on the data of their PNG pictures,
  which is mostly long runs.
- **Bad input**: whatever bytes it is given, it either produces the data
  they stand for or stops, saying why (`kBadData`, `kTruncated`,
  `kSourceFailed`). It does not check that the data is the right data: that
  is for the checksum a ZIP file carries.
- **One call's work**: a stream cannot make one `Read()` long. DEFLATE
  lets a stream be written so that every byte of output costs a block of
  its own, with its own codes to read and build, or so that block after
  block holds nothing at all; a megabyte of either would keep a call that
  waited to fill its destination busy for as long as the megabyte lasted.
  So a call starts 17 blocks at the most and then comes back with what it
  has, which may be fewer bytes than it was asked for, and a stream with
  more than 16 blocks in a row that produce nothing is refused as bad
  data (a compressor writes such a block where it was told to flush:
  three together at the most). A call asked for `n` bytes therefore takes
  no more than `2n + 5,632` bytes from its source, whatever the stream
  (`Inflater::MostInputFor`).
- **Reading ahead**: it may take up to 260 bytes from its source that come
  after the end of the stream, so the source should end where the stream
  does.

Its tests build streams by hand for each rule of the format and each way of
breaking it, and compare it with zlib on streams zlib compresses: 800 of
them across kinds of data, sizes, compression levels and strategies, and
again with the input and output cut into pieces of many sizes. Since
output is copied in runs within a window that is a ring, there are streams
made for the places where a run has to stop: repeats short and long, from
one byte back to 32,768, each laid across the window's end twice, and
stored blocks of many lengths, some begun with two of their bytes already
taken into the buffer of bits and some with none. They also turn over
every bit of a stream, one at a time, and cut it short at every length, to
see that the inflater always stops. Every call in every one of
those tests is held to what one call may take from its source; and there
are streams made for that alone: a megabyte of blocks with nothing in
them, and blocks of one byte each whose codes are written the longest way
the format has, which bring a call to within a tenth of the bound.

## ZIP files

An EPUB is a ZIP file, and `epub/zip.h` reads one, going by the directory
at the end of the file:

- `ZipArchive` finds that directory and then finds entries in it, by name
  (`Find`) or one after another (`Next`). It is 24 bytes: it keeps no list
  of the entries, however many there are, and reads the directory from the
  file when asked. Each search starts where the last one ended, so asking
  for entries in the file's own order reads the directory once, not once
  for each.
- `ZipEntry` is what the directory says of one entry: 24 bytes that can be
  kept, in memory or in a file, to read the entry later without finding it
  again.
- `ZipEntryReader` gives an entry's bytes as a `ByteSource`, read from the
  file and inflated as they are asked for. It gives out no more than the
  directory says the entry holds, and at the end checks that the data
  stopped there and has the checksum the directory gives. It is 2.7 KB on
  the device and needs the inflater's 32 KB window.
- A name is the bytes the file has for it, and two names are the same if
  their bytes are. A ZIP file has a flag to say that its names are UTF-8,
  which nothing here looks at: a book's names are UTF-8 by the EPUB
  standard, and some books' ZIP files leave the flag off all the same (one
  Japanese book of some 500 tried does). A reader that goes by the flag
  cannot find such a book's chapters.

All three read through a `BlockFile`. Finding entries and reading them can
share one, but each step from an entry back to the directory then reads a
block again: reading every entry of a 3,000-entry file and looking each up
by name took 6,189 reads of storage that way and 242 with a `BlockFile`
each for the directory and the entries.

The file is not trusted: whatever it holds, every call ends with an answer.
What cannot be read is reported as such (`kUnsupported`): ZIP64, a file in
several parts, an encrypted entry, and compression other than DEFLATE.

`bn-zip` runs the same code on the desktop:

```
./build/host/tools/zip/bn-zip list book.epub
./build/host/tools/zip/bn-zip check book.epub
./build/host/tools/zip/bn-zip cat book.epub META-INF/container.xml
```

`list` prints a line for each entry. `check` reads every entry to its end,
looks each one up by name, and says how many reads of the file that took.
`cat` writes one entry out.

The unit tests make their ZIP files with a writer of their own, so that
they can get any one field wrong. Because a writer and a reader by the same
hand could share a mistake, an end-to-end test (`tests/e2e/zip_tool.py`)
also compares `bn-zip`, entry by entry and byte by byte, with Python's
`zipfile` on files made by `zipfile` and, where there is one, by the `zip`
command.

### Reading the books on a card

A firmware built with `-DBN_BOOK_CHECK=ON` reads every book on the SD card
from end to end: at start, and again at each press of the Power button. (On
the desktop, set `BN_BOOK_CHECK`, with `BN_STORAGE` naming the folder that
stands for the card.) It is how to find out whether a device reads books
correctly and how fast. It writes nothing to the card.

It takes for a book every file whose name ends in `.epub` in `/books` and
in each folder in `/books`, one level down and no further. If there is no
`/books`, or no book there, it looks in the top directory and its folders
instead (`reader/book_check.h`). A file or folder whose name begins with a
dot is hidden and passed over, as everything that lists the card does: a
Mac writes a `._` file beside each file it copies to a card. With each book it does two things. First
it opens the book as a reader would: finds the ZIP directory and reads the
book's package, which says what the book is called and where its documents
are (see "What a book says of itself" below); then, with the book
still open, reads its table of contents ("The table of contents" below);
then finds the style sheets that each document of its spine asks for, in
reading order ("What a document asks for" below), opening each document
from where the package says it is and reading its head;
and last, before closing it, reads a byte at each of sixteen places far
apart in the file, to and fro: the start, the end, the middle, and so on
by halves. That is what going to a place in a file costs, which on a card
depends on how large the file is (see "Storage" above). Then it closes the
book, opens it afresh, reads every entry to its end, which checks the
entry's size and checksum, and looks each entry up by name.
Every entry with an XML file's name (one ending in `.xhtml`, `.html`,
`.htm`, `.xml`, `.opf` or `.ncx`) is also parsed as it is read. It works
for about 20 ms a turn, so the panel and the buttons carry on around it,
and a run takes longer by the clock than the times it reports. All of it
is done in steps, each one call into what is being checked: a read from
an entry, a piece of a document parsed, a piece of a package or a table
of contents read, a document opened or a step of finding its sheets. A
turn runs over its 20 ms by however long its last
step takes, so the last line of a run says which step took longest, and
how long: on a device, that is whether those calls are as short as an
application that has a panel and buttons to see to needs them to be.

What it finds goes out as log text: a line when a book is begun, three for
its package, one for its table of contents, two for the sheets its
documents ask for, with one more for each kind of thing left out in
finding them, one for the places, four when it ends, and eleven at the
end for all of them, with those kinds again.

```
book check: the checksum alone, 262144 bytes in 2013 us (127173 KB/s).
book 1: 1832425 bytes, /books/core/The First Book.epub
book 1: package: title "The First Book", author "A. Writer", language "en".
book 1: package: 374 files, 368 documents, cover jpeg, nav, ncx; memory 16288; checksum 7b162ac4.
book 1: package: 2 ms: card 0 ms for 24 reads of 90555 bytes.
book 1: contents: nav, 385 entries, 1 level; checksum 2eeff666; 1 ms: card 0 ms for 2 reads of 8192 bytes.
book 1: styles: 368 documents, 1102 sheets given, 3 read, 0 with rules of their own; checksum c31e8f44.
book 1: styles: table 2288 bytes, 46 rules, names 38 bytes; 11 ms: card 1 ms for 374 reads of 1531904 bytes.
book 1: places: 16 far apart, 16 reads in 14 us, the longest 1 us.
book 1: ok. 377 entries, 4353104 bytes out of 1769871. Directory found in 1 read, 0 ms.
book 1: card 0 ms for 468 reads of 1909179 bytes (2378102 KB/s); inflating 41 ms: 1661195 bytes in (39341 KB/s), 4244428 out (100520 KB/s); all 81 ms.
book 1: opened 4 times in 151 us, the longest 71 us; number 1 in its folder.
book 1: XML: 372 documents, 4240089 bytes, 93155 events, checksum a7e0cc8f; parsing 38 ms (108931 KB/s); most memory 11632.
...
book check: passed. 34 books, 5152 entries, 63899132 bytes out of 39526769.
book check: card 20 ms for 10091 reads of 41148789 bytes (1934265 KB/s); inflating 428 ms: 24130522 bytes in (55011 KB/s), 48502885 out (110574 KB/s); all 807 ms.
book check: XML: 2072 documents, 31585733 bytes, 1059050 events; parsing 277 ms (110995 KB/s); 32821373 bytes of events checksummed.
book check: XML memory: the most a document used 18952 bytes; documents over 8 KB: 2072, over 12 KB: 24, over 16 KB: 5.
book check: packages: 33 read; 27 ms: card 17 ms for 258 reads of 901750 bytes; the longest 2 ms; the most memory 22448. Not read: 1.
book check: contents: 33 read (25 nav), 2666 entries; 7 ms: card 0 ms for 49 reads of 195690 bytes; the longest 1 ms.
book check: styles: 1916 documents of 33 books, 3582 sheets given, 76 read, 4 with rules of their own.
book check: styles: 68 ms: card 22 ms for 1808 reads of 7368383 bytes; the longest 11 ms; the most a table took 19959 bytes, names 117.
book check: files opened 136 times in 4 ms; the longest 97 us.
book check: places: 544 reads in 17 ms; the longest 202 us.
book check: the longest step: 1602 us, parsing an entry.
```

(Those figures are a desktop's, and the book's names are made up.) A
book's file is opened four times, twice to read its package and twice to
read it through; the line about that says how long those took, and where
the book comes in the listing of its folder, since a file is found by
going past the names before it. The line about the places says how many
reads of storage the sixteen came to (fewer in a file so small that one
place is in the block the one before was in), how long they took, and the
longest of them; none of that is in the book's other figures. There are
four times besides. `card` is the time
spent inside storage. `inflating` is the time spent reading out the
compressed entries with storage's share taken off, which is the processor
inflating and checksumming, beside what went into the inflater and what
came out. `parsing` is the time the XML parser took over what it was given,
with the reading and inflating of it taken off; the check's own counting
and checksumming of what the parser found is in that time, and the last
line but one says how many bytes that was. `all` is everything spent on the
book. A KB is 1024 bytes. The checksum is timed by itself first, over
256 KB held in memory, so that its share of `inflating` and of `parsing`
can be worked out. A book or an entry that cannot be read is reported with
`FAILED` and the reason, and the check goes on to the next. A file that
turns out not to be XML is counted (`Not XML: 2.`), and so is one that
wanted more than the 32 KB the parser is given (`Refused for memory: 1.`),
but neither makes the book a failure: the file is read out and checked all
the same.

The package's three lines stand apart from the rest. The first has what
the book calls itself, each name cut short with `...` if it is long. The
second has how many files its manifest lists and how many documents its
spine, whether it has a cover and each kind of table of contents, how much
of the 24 KB the package reader is given it used, and a checksum of
everything the package was found to hold (`epub/package_digest.h`);
then, if there are any, how many documents are not in the book or not in
the manifest, and whether the package file was cut short. The third has
what it cost to open the book and read that: the time, the card's share of
it, and the reads, finding the ZIP directory included. None of that is in
the book's other figures. A book whose package cannot be read has one line
that says why (`package: NOT READ: no container.`), and that does not make
the book a failure either.

The line for the table of contents has which kind was read (the nav, if
the book has one, or the NCX), how many entries it has and how many levels
they go to, a checksum of the entries (`epub/toc_digest.h`), and what
reading it cost, which is counted by itself as the package's is. It is
read a piece at a time, as the entries are. After that come how many
entries point into no document, if any do, and whether the file was cut
short. A book with no table of contents says `contents: none.`, and one
with a file that could not be read as one says why (`contents: NOT READ:
not a table of contents.`); neither makes the book a failure.

The two lines for the styles have what finding the sheets of the spine's
documents came to. The first: how many documents of the spine were gone
through; how many sheets they were given between them, a sheet counted
once for each document that has it; how many sheets were read, each once
for the book; how many documents have `<style>` rules of their own; and
a checksum of what every document was given (`epub/sheets_digest.h`).
After that, if there are any: how many documents could not be opened,
how many the spine names that the book has not got, and how many rules
the table had no room for. The second: what the table of the book's
rules takes of the 32 KB the check gives it, and how many rules are in
it, once the last document's own rules are out of it; what the names of
its sheets take; and what the finding cost, which is counted by itself
as the package's is. Then comes a line for each kind of thing that was
left out, with how many (`styles: left out 12: import of no file of the
book.`). A book whose package could not be read has no spine to go
through, and nothing is said of its styles. What the finding needs is a
second block of memory, of about 42 KB, which the check gets when it is
made: if that cannot be had, the run says so (`styles: NOT READ: no
memory for them.`) and finds no sheets.

`bn-zip check` reads a file the same way, read for read. So the same book
on the desktop should come to the same entries, the same bytes and the same
reads (`sd_reads` and `sd_read_bytes`, in the last line `bn-zip check`
prints) as it does on the device. `bn-xml book` parses the same documents,
and its last line has the count and the checksum of everything found in
them, which should be the book's on the device too. `bn-epub info` opens
the book and reads its package the same way, and prints the checksum, the
memory and the reads that the package's lines should have. `bn-epub toc`
goes on to read the table of contents, and prints its kind, its entries
and their checksum; the reads it reports are the package's and the
table's together. `bn-css sheets --spine` opens the book, reads its
package and its table of contents, and then finds the sheets of the
spine's documents the same way: its `sheets_checksum`, its counts, its
`css_table_bytes` and its `sheets_reads` are what the lines for the
styles should have.

The outcome is also in the metrics (`book_check=1` if every book was sound,
`book_check_books`, `inflate_kb_per_s`, `checksum_kb_per_s`,
`xml_kb_per_s`, `xml_memory` for the most the parser has used, and
`css_table_bytes` for the most the table of one book's rules took) and in
the trace (`reader/book_check`, and `reader/book_check_book` for each
book), where every read of the card is too (`hw/sd_read`), and the reading
of each package with its four parts (`epub/package_read`, and
`epub/package_container`, `package_file`, `package_resolve` and
`package_find_files`), and the finding of each document's sheets
(`epub/document_sheets`, with how many it was given).

```
tools/idf -DBN_BOOK_CHECK=ON build flash
./build/host/tools/trace/bn-trace | tee books.log
tools/idf -DBN_BOOK_CHECK=OFF build flash     # back to the normal firmware
```

It cannot be combined with `-DBN_STORAGE_CHECK=ON`: the two would get in
each other's way on the card. A firmware built with neither has the code
of neither in it.

With `-DBN_BOOK_CHECK_BRIEF=ON` as well (on the desktop,
`BN_BOOK_CHECK_BRIEF` set as well) the check is a brief one: each book is
opened, its package and its table of contents are read and its sixteen
places gone to, and no book is read through, nor are any document's
sheets found, which would be a reading of every document's head. That takes seconds where the
whole takes minutes, and still says whether each book opens, what it
holds, and what opening it and going about in it cost. A book's file is
then opened twice, its last line is `book 1: ok. Not read through.`, and
the run's is `book check: passed. 34 books opened, none read through.`
with nothing after it about entries or XML.

```
tools/idf -DBN_BOOK_CHECK=ON -DBN_BOOK_CHECK_BRIEF=ON build flash
```

## XML

`epub/xml.h` reads XML: the files an EPUB describes itself with, and its
chapters. The parsing is done by [expat](https://libexpat.github.io);
`XmlParser` is what the rest of the code sees of it.

- A document is read from a `ByteSource`, 1 KB at each `Step()`, and never
  held whole, so a chapter can be parsed as it comes out of the ZIP file
  and the inflater. What is found is told to an `XmlHandler` as it is
  found: an element beginning, with its attributes; an element ending;
  text; an entity that was not expanded.
- **Memory**: the parser is given a block of memory when it is made, and
  everything expat needs comes out of that, through a small heap of our
  own that lives inside the block (`core/region_heap.h`). Nothing comes
  from the general heap. That heap checks every piece it is handed back
  by going along its own records to it, so a mistake of the library's
  with a pointer (one given back twice, or kept from an earlier
  document, or pointing into the middle of something) is turned away
  and counted, and cannot make the heap give out memory outside the
  block. A check goes past 17 pieces on average, and the parser asks
  for one about once in every 12 to 25 things it reports, which on the
  desktop is under 1% of its time. A document that needs more than the
  block is
  refused (`kNoMemory`), which in practice means one with a single tag
  or comment several kilobytes long, since a whole one has to be held at
  once, or with elements nested hundreds deep. Text can be any length. On
  an X4, of the 2,072 documents of 34 books chosen to differ from one
  another, all but 8 used less than 12 KB. The most was 17.1 KB, for a
  document that begins with a comment of 5.5 KB.
- **Speed**: on an X4, over the same documents, it took in about 960 KB a
  second with every event counted and checksummed. Book by book that was
  from 640, for one thick with markup, to 1,890, for Chinese prose with
  few tags, and 940 at the median.
- **Bad input**: the document is not trusted. Whatever bytes it holds,
  parsing ends with an answer, and one that is not XML is refused with
  expat's error number and the line it is on. What was found before the
  mistake has been told by then.
- **Entities are not expanded.** The five every XML document has (`&amp;`
  and the rest) and numbered characters (`&#160;`) come out as text. An
  entity of HTML's, such as `&nbsp;`, is told to the handler by name if
  the document names a DTD, as XHTML with a `PUBLIC` doctype does; in a
  document that names none it is a mistake by XML's rules, and parsing
  stops there. An entity a document declares for itself comes out as the
  reference was written. So there is nothing a document can make expand
  without end. In the value of an attribute, which is given whole, there
  is nowhere to tell of a reference by name: one the document does not
  declare is left out of the value, and nothing says so
  (`alt="caf&eacute;"` arrives as `caf`).
- **Stopping, and the handler's manners**: a handler may stop the parser
  from inside one of its own calls, and is then told nothing more, not
  even the end of an element written as one tag (`<br/>`) whose beginning
  it was being told of; the parsing ends as `kStopped` whatever else was
  wrong with the document at that place. A handler may not begin or step
  the parser it is being called by: that is refused (`kInUse`), and the
  document in hand goes on.
- **Names** are given as written, prefix and all (`dc:title`). Namespaces
  are not worked out, which also means a book that uses a prefix it never
  declared can still be read.
- **Encodings**: UTF-8, UTF-16, ISO-8859-1 and US-ASCII; everything is
  given out as UTF-8. Any other is refused (`kUnsupportedEncoding`).

expat is not in this repository: CMake downloads the release named in
`third_party/expat/CMakeLists.txt`, checks its hash, and compiles it with
the settings in `third_party/expat/expat_config.h`, the same for the
desktop and the device. The settings leave out what an EPUB does not need:
entity expansion, parameter entities and external DTDs, and namespace
processing. It is 71 KB of flash once something in the firmware uses it.

`bn-xml` runs the same code on the desktop:

```
./build/host/tools/xml/bn-xml events chapter.xhtml
./build/host/tools/xml/bn-xml book book.epub
./build/host/tools/xml/bn-xml book book.epub --memory 16384
```

`events` prints what the parser finds in one file, an event a line. `book`
parses every XML file in an EPUB as it is read out of the ZIP file, and
prints for each how it ended, a checksum of its events, and the most
memory the parser used; and at the end the count and the checksum of all
of them run together (`epub/xml_digest.h`), which is what the firmware's
book check reports for a book. `--memory` holds the parser to that many bytes, as
a device would.

The unit tests say what the parser should make of documents written for
them, and damage one in every way they can think of. An end-to-end test
(`tests/e2e/xml_tool.py`) compares `bn-xml` with the expat that Python
carries, which is another build and another version driven by other code:
every event of every document has to agree.

## What a book says of itself

`epub/package.h` reads the two files an EPUB describes itself with:
`META-INF/container.xml`, which says where the package file is, and the
package file (the OPF), which lists every file of the book and the order
its documents are read in. It is read a piece at a time, `Package::Begin`
and then `Package::Step` until there is no more (`Package::Read` is both,
for a tool or a test), and gives back:

- the title, the author and the language: the first of each that has
  anything in it, with its spaces tidied;
- the spine, which is the documents in reading order, each with the ZIP
  entry it is in, its kind, and whether the book counts it as part of the
  run of the text (`linear="no"` marks one that is not);
- the entry of the cover picture, of the EPUB 3 table of contents (the
  nav document) and of the EPUB 2 one (the NCX), where the book has them.
  The tables of contents are found here and read by `TocReader` (below).

How it does that:

- **The manifest is kept as numbers, not as text.** A book lists its files
  twice over: the package names each by an id and a path, and the ZIP file
  has an entry for each path. For every file the package lists, 16 bytes
  are kept: a number of 48 bits worked out from its id, another from its
  path (`NameHash`, in `epub/book_path.h`), its kind, and a few notes. A
  document of the spine is 28 bytes. So a book of 800 files and 400
  documents is read in 24 KB, whatever the length of its names.
- **The ZIP directory is gone through once.** When the package has been
  read, every entry of the ZIP file is taken in the file's own order, its
  name turned into the same kind of number, and that looked for among
  the files that are wanted: the documents of the spine, the cover and the
  tables of contents. It stops when it has them all. Looking each file up
  by name instead would read the directory again for every one, since a
  ZIP file's order is seldom the package's.
- **A piece at a time.** How long the whole reading takes is the book's
  to decide, and a book can be made to make it long: a package file of
  megabytes, a ZIP file of tens of thousands of entries, one listed file
  that is not there. How long a step takes is not the book's to decide. A
  step looks at 16 entries of the ZIP directory, or as many as lie in
  4 KB of it; or parses 1 KB of a file; or makes about 16,000
  comparisons among the files of the manifest. With files read 4 KB at a
  time, that is five reads of storage at the most, and usually one or
  none. `container.xml` and the package file are found by going through
  the directory in such steps too, the second from where the first was,
  and not by the ZIP reader's `Find`, which goes on until it has an
  answer. Until the last step the book's documents, cover and tables of
  contents are not to be had: the spine is empty.
- **Paths** in the package are worked out the way a link is
  (`ResolveBookPath`): from the folder the package file is in, with `..`
  and `.` followed (never to above the top of the book), `%20` and the
  like turned back into bytes, and anything after a `#` dropped. A
  reference to somewhere else, such as an address on the internet, is not
  a file of the book and is left out.
- **Memory**: the caller gives a block, and the records are made in it,
  files from one end and documents from the other. Nothing comes from the
  general heap. A book that needs more than the block is refused
  (`kNoMemory`). The parser's memory is the caller's as well
  (`XmlParser`).
- **Speed**: on an X4, opening a book and reading its package took 26 to
  301 ms for each of 33 books, 43 ms at the median; the largest lists 745
  files, and used 22.4 KB of the 24 KB it was given. Nearly half of the
  time is the card, at about 5 ms a read, and most of the rest is parsing
  the package file. Matching the spine to the manifest is a few
  milliseconds at the most. A thirty-fourth book, which lists 2,384
  files, was refused for want of memory after 0.5 s. None of that is one
  long call: over those 34 books the longest single step of the whole
  book check was 24.6 ms, and it was opening a file, which is the card's
  file system finding a name in its folder. Read in one go, as it used to
  be, the same packages took 8% less time in all and up to 463 ms in one
  call.
- **Two names with one number.** Nothing checks a name against the text
  it came from, so two different names in one book that came to the same
  number would be taken for each other. With 48 bits, that is one book in
  some 500 million for a book of a thousand files. `ReadZipEntryName`
  reads an entry's name back out of the ZIP file, for whoever needs to be
  sure.
- **What is put up with**, because real books do it: metadata written
  with capitals or with any prefix (`dc:Title`); an id in the spine that
  the manifest has not got (left out, and counted); a file the package
  lists that is not in the ZIP file (kept, and marked as not there); a
  cover named by EPUB 2's `<meta name="cover">` or by EPUB 3's
  `properties="cover-image"`, the second being believed first; a
  container that names its package with `%20` in the path; a package
  that stops being XML after its spine (`CutShort()`).
- **Bad input**: neither file is trusted. Whatever bytes they hold,
  reading ends with an answer: `kNoContainer`, `kNoPackage`,
  `kNotPackage` for one with no documents in its spine, `kUnreadable` for
  a ZIP file damaged where it had to be read, or `kNoMemory`. After any
  of those the spine is empty. Its tests read every book in steps and
  hold each step to five reads of storage: the books made to be awkward
  (five thousand entries, a megabyte of package file, a directory whose
  records lie 40 KB apart) and the damaged ones alike.

`bn-epub` runs the same code on the desktop:

```
./build/host/tools/epub/bn-epub info book.epub
./build/host/tools/epub/bn-epub info book.epub --memory 4096
```

For a small book with some things wrong with it, it prints:

```
title: Untidy
author: -
language: -
package: deep/er/package.opf
manifest: 5
cover: -
nav: -
ncx: deep/er/toc.ncx
spine: 4
+ xhtml deep/er/cover.xhtml
+ xhtml ?
+ xhtml deep/er/one.xhtml
- xhtml deep/er/cover.xhtml
unknown: 2
checksum: cd21187e
memory: 248
cost: packages_read=1 packages_failed=0 xml_documents=2 xml_failures=0 zip_finds=0 zip_records=16 sd_reads=2 sd_read_bytes=2504 sd_writes=0 sd_write_bytes=0 sd_syncs=0 sd_seeks=0 xml_memory=9536[8752..9536]
```

A `-` for the author, the cover or any of the first lines means the book has
none. Each line of the spine is `+` for a document in the run of the text or
`-` for one set aside, then its kind, then the name of its entry in the ZIP
file, or `?` where the ZIP file has no such file. `unknown` (printed only
when there are any) counts the documents the spine named that the manifest
has not got. `checksum` is of everything above it (`epub/package_digest.h`),
with each file given by where its entry is and not by its name: it is what a
device's book check reports for the same book, so the two can be compared.
`memory` is how many bytes of the block were used, and `cost` is the
counters after the reading: here 16 records of the directory and two reads
of the file. (`zip_finds` counts searches by the ZIP reader's `Find`, which
reading a package does not use: it looks for its two files itself, a piece
at a time.)
`--memory` holds it to a block of that many bytes, as a device would; it
exits with 1, and says why, for a file it cannot read as a book.

The unit tests say what should be made of books written for them, in
either version of EPUB and with their files in any order in the ZIP file,
and of every bit of a package file turned over and every length it can be
cut to. An end-to-end test (`tests/e2e/epub_tool.py`) makes books and
compares `bn-epub` with a reading of them by Python's own XML parser and
ZIP reader, written separately for the test: title, author, cover,
tables of contents and every document of the spine have to agree.

## The table of contents

`epub/toc.h` reads a book's table of contents: its parts by name, each with
the place in the book where the part begins. The package reader finds the
file; `TocReader` reads it.

- **Two kinds.** EPUB 3 has a page of XHTML with a list of links in it (the
  "nav"), and EPUB 2 an XML file made for the purpose (the "NCX"). Many
  books have both, and the two are often not the same: of 138 real books
  with both, 53 had differences between them. A book with both is read from
  its nav unless the NCX is asked for.
- **Entry by entry.** Each entry is given to a `TocHandler` as it is found,
  an entry before the entries under it: what it is called, how far in it
  is, the document it points into, and the place within that document. None
  of it is kept, so a table of contents of any length costs the same
  memory: the reader, which is about 1 KB, and what the XML parser needs.
- **What an entry is.** In an NCX: each `navPoint` in the `navMap`, by the
  text of its label and the `src` of its content, nested as the points
  are. In a nav: within the `nav` element whose `epub:type` is `toc`, each
  link in a list, and each `span` that stands where a link would as a
  heading with no link, nested as the lists are. Page lists, landmarks and
  whatever else the two files hold are passed over.
- **Where an entry points.** Its link is worked out as any link in a book
  is, from the folder the table of contents is in, and looked up in the
  package (`Package::SpineIndexOf`): the answer is the document's place in
  the spine, with what the link has after its `#` as the place within the
  document. An entry that points at nothing, or at something that is no
  document of the spine, has no document.
- **Labels** have every run of spaces and line ends made one space, and the
  text of whatever is inside a link (italics, a line break) is part of
  the label. One longer than 256 bytes is cut at a character and marked.
- **In pieces.** `Begin()` and then `Step()`, which reads 1 KB of the file
  each time, so that a long table of contents can be read a little a turn;
  or `Read()` for all of it at once.
- **Speed**: on an X4, reading the table of contents took 4 to 159 ms for
  each of 33 books, 18 ms at the median, for tables of 1 to 385 entries:
  about 0.5 ms an entry over all of them. Under a fifth of it is the
  card, which gave most tables in one read or two; the rest, inflating
  and parsing the file and placing each entry, went at about 450 KB of
  the file a second. Counted from opening the book, with its package read
  first, the contents were in hand after 36 to 433 ms, 64 ms at the
  median.
- **Bad input**: the file is not trusted. Whatever bytes it holds, reading
  ends with an answer: `kNoToc` for a book with none, `kNotToc` for a file
  with no table of contents in it, `kNoMemory`, `kUnreadable`. A file that
  stops being XML part way gives the entries that came before the mistake,
  and `CutShort()` says that it did.

`bn-epub` prints one:

```
./build/host/tools/epub/bn-epub toc book.epub
./build/host/tools/epub/bn-epub toc book.epub --ncx
```

For a small book with an NCX, in two parts:

```
toc: ncx
0	0	-	Part 1
1	0	s0	Chapter 0: The Tale & Its Telling
1	1	-	Chapter 1: The Tale & Its Telling
1	2	-	Chapter 2: The Tale & Its Telling
1	3	s3	Chapter 3: The Tale & Its Telling
1	4	-	Chapter 4: The Tale & Its Telling
1	5	-	Chapter 5: The Tale & Its Telling
1	6	s6	Chapter 6: The Tale & Its Telling
1	7	-	Chapter 7: The Tale & Its Telling
1	8	-	Chapter 8: The Tale & Its Telling
1	9	s9	Chapter 9: The Tale & Its Telling
0	10	-	Part 2
1	10	-	Chapter 10: The Tale & Its Telling
1	11	-	Chapter 11: The Tale & Its Telling
entries: 14
checksum: 5d9afae8
cost: packages_read=1 packages_failed=0 xml_documents=3 xml_failures=0 zip_finds=0 zip_records=21 sd_reads=2 sd_read_bytes=7338 sd_writes=0 sd_write_bytes=0 sd_syncs=0 sd_seeks=0 xml_memory=10144[8752..10144]
```

The first line says which kind was read. Then a line an entry, with tabs
between: how far in it is, from 0; the document it points into, by its
place in the spine from 0, or `-` for none; the place within the document,
or `-` for its start; and what it is called. Then how many there were, a
checksum of them (`epub/toc_digest.h`), which is what a device's book
check reports for the same book, and the counters after reading the
package and the table. `--nav` and `--ncx`
ask for that kind; it exits with 1, and says why, if there is none to read.

The unit tests say what should be made of tables of contents written for
them, of both kinds, and of every bit of one turned over and every length
it can be cut to. The end-to-end test (`tests/e2e/epub_tool.py`) makes
books whose two tables differ, and compares `bn-epub toc` with a reading of
each by Python's own XML parser, written separately for the test: every
entry has to agree.

## Style sheets

`epub/css.h` reads CSS: a book's style sheets, the `<style>` elements of
its documents and the `style` attributes of their elements. It reads a
sheet and says what is in it. Which rules apply to which element is not
its business: that is the next section's.

- A sheet is read from a `ByteSource`, 1 KB at each `Step()`, and never
  held whole. What is found is told to a `CssHandler` as it is found: a
  rule's beginning, each of its selectors, each thing it sets, its end; an
  `@import`; and everything that was passed over, with what it was.
- **Selectors**: by element, class and id, joined by descent (`div p`),
  as child (`div > p`) or as the element straight after (`h2 + p`), with
  `*` for any element. `:link` is taken as always so, and `::first-letter`
  is read, for drop caps. A selector with anything else in it is dropped:
  any other pseudo-class or pseudo-element, `~`, a namespace before an
  element's name. The others of its rule are kept, and a rule left with
  none is dropped whole. Each selector comes with its rank as CSS has it:
  its counts of ids, of classes and of element names.
- **Attributes**: a part of a selector may ask of an attribute that it is
  there (`[hidden]`), or hold its value against another in any of CSS's
  six ways (`=`, `~=`, `|=`, `^=`, `$=`, `*=`), with the value in quotes or
  as a name, and `i` after it for any case. An attribute's name is given
  as a document writes it, in small letters: `epub|type` in a sheet is
  `epub:type`, since the XML parser gives names with their prefixes and
  does not work namespaces out. So a sheet and a document have to use the
  same prefix. In 520 books the sheets' prefixes are `epub` and `xml` and
  no other, and every document that has such an attribute writes
  `epub:type`. `[*|type]` is the name with any prefix, or none. Each thing
  asked counts in the rank as a class does. A selector may ask 8 things of
  attributes; none in those books asks more than 3.
- **Properties**: `font-style`, `font-weight`, `font-variant`,
  `font-family`, `font-size`, `line-height`, `text-align`, `text-indent`,
  `text-decoration`, `vertical-align`, the four sides of `margin` and of
  `padding`, `display`, `float`, `width`, `height`, and where a page may
  break (`page-break-*` and `break-*`, as one). `margin`, `padding` and
  `font` are taken apart into those. Any other property is dropped, and
  so is a value that is not understood. `!important` is told with the
  declaration.
- **Lengths** are given as a number of thousandths and one of four units:
  pixels, ems, rems and percent. Points, inches and the rest are turned
  into pixels (12pt is 16px), and `ex` into half an em. A length that
  depends on the screen (`vh`, `vw`) is not read.
- **`@media`**: the rules in a block are read if the block is for `all` or
  `screen`, or for "not" some other kind of device (`not amzn-mobi`, which
  six of 335 trade books use, 709 times between them), and has no
  condition in brackets. A
  block for print, for one maker's device, or for a screen of some size is
  passed over. The screen is not asked its size because what is made of a
  book must not depend on which device made it.
- **Other at-rules**: `@import` is told to the handler, if it is for a
  reader of books by the same test. The rest are passed over, with their
  blocks: `@font-face`, `@page`, `@supports`, `@namespace`, `@charset`.
- **Mistakes** stop nothing. Whatever bytes a sheet has, reading goes on
  to its end, dropping the declaration or the rule that cannot be made
  sense of, as CSS's own rules for mistakes have it. In one thing it is
  kinder than they are: a bracket left open ends with its rule, where by
  the rules it would take the rest of the sheet with it. A list of
  selectors over 1,024 bytes, or a declaration over 512, is dropped; the
  longest in the 1,027 sheets of 520 books are 649 and 179.
- **Memory**: about 2 KB of its own, for the selectors or the declaration
  in hand. Nothing from the heap.
- **What is passed over is counted**, by kind, and told to the handler
  with its text, so that what a collection of books loses to all of the
  above can be added up (below).

`bn-css` runs the same code on the desktop:

```
./build/host/tools/css/bn-css rules book.css
./build/host/tools/css/bn-css style 'margin: 1em 0; font-weight: 700 !important'
./build/host/tools/css/bn-css book book.epub --dropped
```

`rules` prints what the reader finds in one sheet, a thing a line. For
this sheet

```
@import url("fonts.css");
body { margin: 0 5%; font-family: Georgia, "Times New Roman", serif }
h2 { text-align: center; page-break-before: always; margin: 2em 0 1em }
p { text-indent: 1.5em; margin: 0; widows: 2 }
h2 + p, p.first, p:first-child { text-indent: 0 }
section[epub|type~="dedication"] > p { text-align: center; font-style: italic }
p.first::first-letter { float: left; font-size: 3em; line-height: 1 }
@media amzn-mobi { p.first::first-letter { float: none } }
a:link { text-decoration: none; color: #036 }
```

it prints

```
I fonts.css
{
S 0,0,1 body
D margin-top: 0px
D margin-right: 5%
D margin-bottom: 0px
D margin-left: 5%
D font-family: georgia, times new roman, serif
}
{
S 0,0,1 h2
D text-align: center
D break-before: always
D margin-top: 2em
D margin-right: 0px
D margin-bottom: 1em
D margin-left: 0px
}
{
S 0,0,1 p
D text-indent: 1.5em
D margin-top: 0px
D margin-right: 0px
D margin-bottom: 0px
D margin-left: 0px
X property: widows
}
{
S 0,0,2 h2 + p
S 0,1,1 p.first
X pseudo-class: :first-child
D text-indent: 0px
}
{
S 0,1,2 section[epub:type~="dedication"] > p
D text-align: center
D font-style: italic
}
{
S 0,1,2 p.first ::first-letter
D float: left
D font-size: 3em
D line-height: 1
}
X media block: amzn-mobi
{
S 0,1,1 a
D text-decoration: none
X property: color
}
```

`{` and `}` are a rule's beginning and end, `S` a selector after its rank,
`D` something the rule sets, `I` an import and `X` something passed over,
with its kind. `style` prints the same for a `style` attribute. `book`
reads every sheet in an EPUB as it comes out of the ZIP file, and prints
for each how it ended, the checksum of what `rules` would print for it,
its rules, the declarations in them and how many things were passed over;
with `--dropped`, also each different thing that was passed over and how
many times. For Standard Ebooks' *Flatland*, which is in `corpus/`:

```
ok 660f898c     38     89     53 epub/css/core.css
ok b1313d47      9     27     18 epub/css/local.css
ok 4bbcc118     15     33     13 epub/css/se.css
css_sheets=3 css_failed=0 css_bytes=10955 css_rules=62 css_declarations=149 css_dropped=84
css_table_bytes=3148 css_table_names=56 css_table_nodes=31 css_table_rules=55 css_table_selectors=95 css_table_left_out=0 css_table_sheets_left_out=0
```

The last line is what keeping all of those rules in one table takes (the
next section).

The unit tests say what the reader should make of sheets written for
them: every property and unit, every kind of selector and of mistake, the
same sheet cut into pieces of every size, and four hundred sheets damaged
at random. An end-to-end test (`tests/e2e/css_tool.py`) compares `bn-css`
with a second reading of the same sheets. Python has no reader of CSS of
its own, so the second reading is written in the test, another way round
(the whole sheet at once, comments out first); every line has to agree,
on sheets the test makes, on 600 of them damaged at random, and, through
`tools/check-books.py`, on the sheets of real books. The two agree on all
1,027 sheets of the 520 books they were tried on. Writing the second one
found a fault in the first (a character written as `\0` was decoded over
the byte after it), and the fuzz target found another in its first two
minutes (a second `::first-letter` after the first was let through).

**What books lose to it.** `tools/check-books.py --css-report` adds up
what was passed over across a folder of books. Over `corpus/` it begins:

```
Style sheets: 1406 rules read, with 3660 declarations; 963 things passed over, in 28 books:
       604  in   28 books  property
       134  in   15 books  pseudo-class
        78  in   26 books  at-rule
        76  in   17 books  rule
        33  in    9 books  pseudo-element
        19  in    9 books  media block
        17  in   17 books  value
         2  in    1 books  sibling selector
  property, the 30 passed over most of 40:
          75  in   15 books  all
          57  in   19 books  color
          39  in   24 books  border
          39  in   19 books  background-color
```

and goes on to name the pseudo-classes, at-rules and media blocks. These
books are from Project Gutenberg, Standard Ebooks and IDPF, whose sheets
lean on selectors more than most: one rule in twenty is dropped for its
selectors, most of them for `:root`, which begins the rules that
Standard Ebooks' sheets keep for one maker's reading application. Before
`+` and attributes were read it was one in eight. In 335 books from trade
publishers it is one in two hundred.

## A book's rules, and what they come to

`epub/css_rules.h` keeps the rules that the reader of CSS tells of, and
says what they come to for each element of a document.

- **`CssRules`** is the table. It is what a `CssReader` is handed as its
  handler, a sheet at a time, and it keeps every sheet of a book in one
  block of memory that the caller gives it: each name once, a selector in
  about 12 bytes and a declaration in about 4. Of a rule that sets a
  property more than once it keeps the declaration that would win. A rule
  there is no room for is left out whole and counted, and the rules
  before it are as they were.
- **`CssCascade`** goes through a document beside its parser. Told of each
  element as it opens and closes, it fills in a `CssStyle`: for each
  property that some rule set, the value that wins, and the same again
  for the element's first letter (`p::first-letter`). What wins is
  decided as CSS has it: `!important` first, then the element's own
  `style` attribute over any rule, then the selector's rank, then the
  later sheet, then what was written later.
- **A document is given the sheets it asks for** and no others, in the
  order it asks for them: every rule is marked with its sheet, and a
  cascade passes over the rules of sheets it was not given. Which sheets
  a document asks for is found by `DocumentSheets` (the next section).
- **A document's own rules** (its `<style>` elements) are added after the
  book's and taken away again: `Here()` marks where the table stands and
  `BackTo()` forgets everything added since.
- **Nothing of the document is kept** but a few bytes for each element
  that is open. A selector can ask anything of the elements an element is
  inside and of the ones before it (`hgroup > h2 + p + p`, `tr + tr td`);
  each different thing asked is a *node*, and for an open element there
  is a bit for each node, twice: which nodes the element is, and which
  the last element to close inside it was. A node for `h2 + p` is "a `p`,
  after an element that is the node `h2`", so one remembered element
  serves a row of `+` of any length. A book with no such selectors, which
  is most trade books, keeps nothing at all. An element deeper than there
  is memory for is still given the rules that ask nothing of the elements
  around it.
- **Values are as the sheets wrote them.** `inherit`, an em and "larger"
  are given back as they are: what an element takes from the one it is
  inside is for whatever keeps the styles of the open elements, which is
  not built.
- **Work is bounded.** A name is looked for among the table's by halving;
  only the selectors kept under an element's id, its classes or its name
  are tried on it, and those kept under none. `CssCascade::MostWorkFor()`
  says the most one element can cost for a table of a given size, and
  the tests and the fuzz target hold every element to it. In real books
  the dearest element costs a few hundred steps.

`bn-css cascade` runs it on a document and the sheets it is to be given:

```
./build/host/tools/css/bn-css cascade chapter.xhtml book.css
./build/host/tools/css/bn-css cascade --in book.epub OEBPS/chapter.xhtml OEBPS/book.css
```

With the sheet of the last section, and this for the document,

```
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<body>
<section epub:type="dedication"><p>For A.</p></section>
<h2>One</h2>
<p>It began.</p>
<p class="first" style="margin-top: 1em">And went <i>on</i>.</p>
</body>
</html>
```

it prints a line for each element that opens (`<`) and closes (`>`), and
between them what is set for it (`P`) and for its first letter (`F`). Here
from the heading on:

```
< h2
P text-align: center
P margin-top: 2em
P margin-right: 0px
P margin-bottom: 1em
P margin-left: 0px
P break-before: always
>
< p
P text-indent: 0px
P margin-top: 0px
P margin-right: 0px
P margin-bottom: 0px
P margin-left: 0px
>
< p
P text-indent: 0px
P margin-top: 1em
P margin-right: 0px
P margin-bottom: 0px
P margin-left: 0px
F font-size: 3em
F line-height: 1
F float: left
< i
>
>
```

The first paragraph has no indent because it comes after the heading
(`h2 + p`), the second because of its class; the second's top margin is
from its own `style`. With `--sum` it prints one line instead, with the
checksum of those lines and what the table and the cascade took:

```
cascade ec12c5fe elements=8 lines=47 table_bytes=347 nodes=2 level_bytes=2 left_out=0 too_deep=0 too_many_classes=0 most_work=23
```

**How much memory.** `bn-css book` puts every sheet of a book into one
table and says what that took, and `tools/check-books.py --css-report`
adds it up over a folder. Over `corpus/`:

```
A table of all a book's rules, in the 28 books with style sheets:
  bytes: 2188 at the median, 4090 for nine in ten, 4498 at the most
  rules kept: 43 at the median, 87 for nine in ten, 105 at the most
  nodes: 7 at the median, 42 for nine in ten, 51 at the most
  books whose table is over 8 KB, 12 KB, 16 KB, 20 KB and 24 KB: 0, 0, 0, 0, 0
  rules left out of a table of 64 KB: 0
  bytes kept for each open element, in the documents the cascade was tried on: 2 at the median, 10 for nine in ten, 14 at the most
  the most work for one element: 41 at the median, 119 for nine in ten, 150 at the most
```

In 335 books from trade publishers a table is 4.1 KB at the median,
11.4 KB for nine in ten and 24.2 KB at the most, about 48 bytes a rule;
eleven of them need more than 16 KB. Their sheets are mostly one class to
a rule, and in the largest the names of the classes are two fifths of
the table.

**How it is tested.** The unit tests give it sheets and documents written
for each thing it has to get right, and beside it a second working-out of
the same rules (`tests/support/css_reference.h`) that keeps the whole
document as a tree and tries every selector on every element; the two
have to agree on every element of four hundred sets of sheets and twelve
hundred documents made at random, with the cascade's memory sometimes cut
to a few bytes. `e2e.CssCascade` compares `bn-css cascade` with a third
working-out, in Python (`tests/e2e/css_cascade_tool.py`), which takes
its rules from the second reading of CSS, so that nothing of the path
from a sheet's bytes to an element's style is checked only against
itself. `tools/check-books.py` runs that comparison on real books: on
the first few documents of each, with the sheets each one asks for, which
the tool finds for itself (the next section). The two agree on all
1,643,899 properties set for 384,305 elements of 2,909 documents in the
520 books they were tried on.

## What a document asks for

`epub/document_sheets.h` finds the style sheets a document is to be read
with. A document is given the rules it asks for and no others, in the
order CSS gives them: `DocumentSheets` reads the document's head, reads
each sheet the head names into the book's table of rules, once for the
whole book, and gives the list that `CssCascade::Begin()` takes.

- **The sheets it links**, in the order of the links. A `link` counts if
  its `rel` has the word `stylesheet` and not the word `alternate`, its
  `type` is `text/css` or is not given, and it has an `href`. The sheet
  is the file of the book that the `href` names from the document's
  folder, whether the book's manifest lists it or not.
- **The sheets those import**, each before the sheet that imports it, as
  CSS has them, and theirs before them: four down from a link at the
  most. An import is found from the folder of the sheet that names it.
- **Its own `<style>` rules**, after all the linked sheets, as one sheet
  that stays in the table only until the next document is begun. An
  element's own `style` attribute comes over all of these, and is the
  cascade's to read.
- **A sheet named twice**, by two links or by two sheets that import it,
  stands in the list once, where it was named last: that is where its
  rules would count if it were read again.
- **Only the head is read.** The document is parsed until its `<body>`
  opens and no further, so a chapter of megabytes costs what its head
  does. A document whose first element is not `html` has no head, and is
  given no sheets.
- **Each sheet is read once for a book.** A book's sheets are known by
  their paths, and a second document that links one opens nothing but
  itself. The files are reached through `epub/book_files.h`: a book's
  files by their paths, one open at a time, which `ZipBookFiles` gives
  for a ZIP file and a test gives from memory.
- **A document can be handed over open.** `Begin()` takes a path, and the
  document is found in the book by it; or a path and the document
  already open, and then nothing is looked for but its sheets, each once
  for the book. Whoever has the book's package has where each document
  of the spine is in the ZIP file, and need not find it by its name. The
  path still says where the document's links are from. The document is
  read only as far as its head goes, and never after a sheet has been
  opened, so the one reader a book's files are opened with can hand it
  over.
- **It is done in steps.** A step opens one file of the book, or has the
  XML parser read 1 KB of the document, or the reader of CSS 1 KB of a
  sheet, or makes one move in working the list out; and never two of
  those. The tests and the fuzz target hold every step to it.

A link's `media`, and a `<style>`'s, go by the rule for `@media` blocks
(Style sheets, above): taken for `all`, for `screen`, or for "not" some
other kind of device, and never with a condition in brackets, so that
what is made of a book does not depend on the device that made it. So a
sheet linked for `amzn-mobi` is not read. Neither is one linked for
`(min-device-width: 0px)`, which one public book, in seven languages, uses
to say "any newer reader" of a second sheet; its first is read.

**Nothing is left out without being counted.** Each thing that is passed
over is counted by its kind, and told to whoever is listening with its
name:

| What is left out | The kind it is counted as |
|---|---|
| A link, or a `<style>`, for some other kind of device | `link for another medium`, `style for another medium` |
| A link or an import of something that is no file of the book (`https://...`), or of a file the book does not have | `link to no file of the book`, `import of no file of the book` |
| A link after the first 8 of a document | `link after the most followed` |
| An import more than 4 sheets down from a link | `import too deep` |
| An import of a sheet that is already on the way to it | `import of a sheet on the way to it` |
| An import after the first 64 of a book | `import after the most remembered` |
| A sheet for which there is no room: the book has 64 known already, or the memory for their names is full, or the table has the 64 it holds | `sheet with no room` |
| A sheet whose file could not be read to its end; what was read of it is kept | `sheet not read to its end` |
| A `<style>` whose text does not fit in what is left of the memory for it | `style too long` |
| An `@import` inside a `<style>`, which is not followed | `import in a style` |
| A head that could not be read through, the document not being XML or its file failing; the sheets named before that are still given. A file that fails just after its head may be counted too: the parser tells of a tag some bytes after reading it, so how the bytes came decides | `head not read through` |

No book of the 548 it has been run on (below) comes near a limit: a
document is given 7 sheets at the most, a book has 18, imports go one
down, and the largest `<style>` in a head is 2.0 KB.

**Where this is not what CSS has.** Three things, each chosen:

- A `<style>` that stands before a link is still read after the linked
  sheets, where CSS would have it before. No document of the 548 books
  has one.
- A `<style>` in the body is not seen, since the head is all that is
  read. One public book, of fixed pages, has 13.
- `<?xml-stylesheet?>` is not read. Among the 548 it is only in the SVG
  pages of one book whose spine is SVG.

And one thing that looks like a difference and is not: text inside a
comment is no `<style>`, to XML, to a browser or to this. Two books made
from Word have `<!--[if gte mso 9]><style>...` in 123 documents.

**How much memory.** The object is about 2 KB. It takes two pieces from
its caller: for the paths of the book's sheets (2 KB is advised: the
longest path in the books is 74 bytes, and a book has 18 sheets at the
most), and for one document's `<style>` text, each with a byte to end it
(4 KB is advised). It reads through the XML parser, the reader of CSS and
the table that its caller has anyway. In the books the names of a book's
sheets take 562 bytes at the most, and 31 at the median.

`bn-css sheets` runs it over every document of a book, those whose names
end in `.xhtml`, `.html` or `.htm`, in the order the ZIP file has them:

```
./build/host/tools/css/bn-css sheets book.epub --dropped
```

For a book of two documents, the first with this head,

```
<link rel="stylesheet" href="../css/book.css"/>
<link rel="stylesheet" href="../css/kindle.css" media="amzn-mobi"/>
<link rel="stylesheet" href="gone.css"/>
<style>p.first { text-indent: 0 }</style>
```

and the second with this,

```
<link rel="alternate stylesheet" href="../css/night.css"/>
<link rel="stylesheet" href="../css/poems.css"/>
```

where `book.css` imports `fonts.css`, and `poems.css` imports
`book.css`, it prints each document (`D`), the sheets it is given in the
order their rules apply (`S`), and with `--dropped` what was left out for
it (`X`); then what it all came to, with a checksum of it; the reads of
the file that the finding took; each kind of thing left out with how
many; and what the table of the book's rules takes:

```
D OEBPS/text/one.xhtml
S OEBPS/css/fonts.css
S OEBPS/css/book.css
S <style>
X link for another medium: ../css/kindle.css
X link to no file of the book: OEBPS/text/gone.css
D OEBPS/text/two.xhtml
S OEBPS/css/fonts.css
S OEBPS/css/book.css
S OEBPS/css/poems.css
sheets_documents=2 sheets_unopened=0 sheets_given=6 sheets_read=3 sheets_names_bytes=75 sheets_left_out=2 sheets_own=1 sheets_absent=0 sheets_checksum=b2af330b
sheets_reads=1 sheets_read_bytes=2478
left_out link_for_another_medium=1 link_to_no_file_of_the_book=1
css_table_bytes=121 css_table_names=4 css_table_nodes=0 css_table_rules=3 css_table_selectors=3 css_table_left_out=0
```

Three sheets were read for the six that were given, and one document has
rules of its own. The night sheet is an alternate, and was never asked
for: it is not among what was left out.

**The spine.** With `--spine` the documents are those of the book's
spine, in reading order, and each is opened from where the book's package
says it is and handed over open: which is how a reader goes through a
book, and what the book check does on a device (Reading the books on a
card, above). The tool reads the package and the table of contents first,
as the check does, so that its reads are the check's reads. For the same
book, whose spine has the second document first:

```
./build/host/tools/css/bn-css sheets book.epub --spine --dropped
```

```
D OEBPS/text/two.xhtml
S OEBPS/css/fonts.css
S OEBPS/css/book.css
S OEBPS/css/poems.css
D OEBPS/text/one.xhtml
S OEBPS/css/fonts.css
S OEBPS/css/book.css
S <style>
X link for another medium: ../css/kindle.css
X link to no file of the book: OEBPS/text/gone.css
sheets_documents=2 sheets_unopened=0 sheets_given=6 sheets_read=3 sheets_names_bytes=75 sheets_left_out=2 sheets_own=1 sheets_absent=0 sheets_checksum=b55acb95
sheets_reads=0 sheets_read_bytes=0
left_out link_for_another_medium=1 link_to_no_file_of_the_book=1
css_table_bytes=121 css_table_names=4 css_table_nodes=0 css_table_rules=3 css_table_selectors=3 css_table_left_out=0
```

Each document is given what it was given before; the order is the
spine's, and so the checksum is another. `sheets_absent` counts the
documents a spine names that the book has not got. (The file is so small
that the finding read nothing: it was all in the two blocks that reading
the package had left in memory.) With `--by-name` as well, each document
of the spine is found in the ZIP file by its name all the same, which
gives the same lists and costs more reads: Real books, below, has how
many.

**The checksum** (`epub/sheets_digest.h`) is how what the documents of a
book were given is said in one number, so that a device's finding can be
set beside the desktop's, or beside another finding altogether. It is the
CRC-32 of these lines, each with its line end: for each document in turn
`D` and its path; `S` and a path for each sheet in its list, `S <style>`
for its own; and `X`, a kind and a count for each kind of thing left out
for it, in the order of the table above (`X link for another medium 1`).
A document that could not be opened is its `D` line and a line `N`. Two
findings with the same checksum gave the same documents, in the same
order, the same sheets in the same order, and left out as many things of
each kind for each.

`bn-css document` finds one document's sheets in the same way, and then
goes through the document with them as `bn-css cascade` does with the
sheets it is handed:

```
./build/host/tools/css/bn-css document --sum book.epub OEBPS/text/one.xhtml
```

```
document e6ca13ad elements=8 lines=22 sheets=3 own_style=1 sheets_left_out=2 table_bytes=118 nodes=0 level_bytes=0 left_out=0 too_deep=0 too_many_classes=0 most_work=11
```

**How it is tested.** The unit tests give it books held in memory
(`tests/support/book_fakes.h`), which count every file opened and every
byte read: that is how a test knows that a sheet was read once, that a
body of 200 KB was not read at all, and that no step did more than a step
may. Beside it goes a plain working-out of the same thing
(`tests/support/sheets_reference.h`), which reads a whole head into
lists, calls itself for each import and reads a sheet again every time it
comes to it; the two have to give every document of 300 books made at
random the same list, leave the same things out, and find as many rules
in its own `<style>` elements, every other document handed over open.
`e2e.SheetsTool`
compares `bn-css sheets` with a third finding, in Python
(`tests/e2e/sheets_tool.py`), which reads the head with Python's expat
and a sheet's imports with the second reader of CSS, on books it makes;
and compares `bn-css document` with the second working-out of the
cascade given the sheets the Python found, so that the path from a
document's bytes to an element's style is checked end to end. It gives
each of its books a package as well, whose spine has the documents in
another order, leaves some out and names one that is not there, and
holds `bn-css sheets --spine` to that order both ways, by place and by
name. The checksum is worked out three times over: by the unit tests
from lines written out by hand, by the fuzz target from what the plain
working-out gives, and by that end-to-end test from the Python's finding
and not from anything the tool printed.
`tools/check-books.py` runs these comparisons on real books (Real books,
below).

The tests were set against 26 faults put into it on purpose, one at a
time: in which links and styles count, in the limits, in the order of the
list, in a ring of imports, in what is kept from one document to the next
and in what a step may do. The unit tests found every one, and the
end-to-end test 21: the other five change nothing that the tool prints
(how often a file is opened, what one step does, a document given up part
way). The fuzz target's kept inputs missed six at first, which showed
three things, all mended: a seed that had never read the sheet it was
written for, a book of eight files having had no way to say that none of
them fails; and two things the target could hold and did not, that no
sheet is opened twice for a book, and that a document's own rules are
those of its `<style>` texts each read to its own end. The kept inputs
now find all 26.

What came after, a document handed over open, a file opened from its
place, the checksum, and the book check's finding of the sheets of a
spine, was set against 30 faults more. 24 were found at first. Four of
the other six had not been planted as they were meant to be; planted so,
three were found, and the fourth, with the two left, showed tests that
could not fail. A document that cannot be opened had only ever been the
first of its spine, where there was nothing of a document before it to
take for its own. The most that a table took had only ever been the last
book's. And the tool's reads were the book check's only by chance, in
books whose tables of contents lie beside their first chapters: the tool
could have left the table of contents unread, which it reads so that its
reads are the check's, and nothing would have said so. Each has a test
now, and all 30 are found.

**On real books.** `tools/check-books.py --css-report` says what the
documents of a folder of books ask for. Over `corpus/`:

```
What documents ask for: 859 documents are given 1676 sheets, 0 of them with rules of their own:
  sheets a book's documents ask for between them: 3 at the median, 4 for nine in ten, 4 at the most
  the most sheets one document is given: 3 at the median, 4 for nine in ten, 4 at the most
  bytes for the names of a book's sheets: 38 at the median, 50 for nine in ten, 50 at the most
  nothing was left out

Through the spines, in reading order: 840 documents. Reads of the file to find their sheets:
  each document opened from where the package says it is: 938 in all; for a book, 16 at the median, 102 for nine in ten, 154 at the most
  each document found in the ZIP file by its name: 1080 in all; for a book, 17 at the median, 115 for nine in ten, 185 at the most
```

In 335 books from trade publishers, 20,588 documents are given 28,410
sheets, and 99 have rules of their own; in 185 public books, 10,660 are
given 19,623, and 55 have. The tool and the finding in Python give every
one of those documents the same list. What is left out in all of them is
of one kind, a link for another medium: 3 in one of the trade books, the
sheet for `amzn-mobi`, and 145 in eight of the public ones, the second
sheet behind `(min-device-width: 0px)` and one for `speech`. No link that
counts is to a file its book does not have, and no head fails to be read
through.

Through the spines it is the same: the 20,640 documents of the trade
books' spines and the 10,559 of the public books', each book's in its
reading order, come to the checksum the finding in Python comes to, book
for book. What it costs to find them is in the last two lines of the
report. With each document opened from where the package says it is, the
trade books take 25,910 reads of 4 KB between them, and 509 for the book
that takes the most; with each found in the ZIP file by its name, 61,435,
and 3,458 for one book. The public books take 12,111 and 22,537. A ZIP
file's directory is searched from where the last search ended, which
costs next to nothing while names are asked for in the directory's own
order; a spine is often in another order, and a sheet read between two
documents moves the search as well. So a reader that has the package
opens a document from its place.

## Real books

The tests make most of the books they read, so that a test can say exactly
what should be found in one. A book made for a test has only what its
writer thought of. `corpus/` holds 28 that nobody here made: 16 from
Project Gutenberg, 7 from Standard Ebooks, and 5 of the International
Digital Publishing Forum's EPUB 3 samples. Between them: eleven languages
in six kinds of writing (Latin, Greek, Cyrillic, Chinese, Arabic and
Hebrew, the last two read from right to left); EPUB 2 and EPUB 3; tables
of contents of both kinds; verse, plays and pictures; a book with 1,647
notes and one with 10,281 formulas in MathML; books with their own
fonts; documents out of the reading order; and a fixed layout whose
pages are SVG files. `corpus/README.md` lists them, with where each came
from and the terms it is passed on under, which are not this project's
licence. They are kept byte for byte as they were fetched.

`tools/check-books.py` runs `bn-zip`, `bn-xml`, `bn-epub`, `bn-css`,
`bn-png` and `bn-jpeg` over every EPUB under a folder, and compares what each says of a
book with a reading of the same book by Python:

```
tools/check-books.py corpus
tools/check-books.py --tools build/host ~/books
```

```
28 books: 1228 entries in their ZIP files, 945 XML documents with 1899986 events, 840 documents in their spines, 79 readings of tables of contents with 3914 entries, 77 style sheets with 1406 rules, 1676 sheets asked for by 859 documents, 42379 properties set for 15299 elements of 141 documents, 80 PNG pictures of 62626058 pixels, 45 JPEG pictures of 46766091 pixels.
Memory for the rows of a book's widest PNG picture, in bytes: 9602 at the median, 11202 for nine in ten, 11202 at the most; over the 18 books that have one.
Memory for the rows of a book's widest JPEG picture read at its whole size (a progressive one at an eighth), in bytes: 22528 at the median, 29952 for nine in ten, 40960 at the most; over the 17 books that have one.
Of the 834 documents of their spines, a book's largest paragraph, in bytes: 3824 at the median, 21717 for nine in ten, 141354 at the most; the most paragraphs in one of its documents: 123 at the median, 1050 for nine in ten, 2108 at the most; the most ids in one: 15 at the median, 123 for nine in ten, 501 at the most.
Items of the spines that are not XHTML by their media type, and were not counted: 6 of the kind svg.
28 files listed in SHA256SUMS: all there and unchanged.
The tools and Python agree about every book.
```

- **The ZIP file**: every entry `bn-zip list` gives is the entry Python's
  `zipfile` finds; `bn-zip check` inflates every entry and finds its
  checksum right; and a few entries are read out and compared byte for
  byte.
- **The XML**: every document `bn-xml book` parses has the events, in
  number and by checksum, that the expat in Python finds in it, and one
  that either takes for XML the other must too.
- **The package**: every line of `bn-epub info` is the line a reading of
  the package file by ElementTree comes to, the checksum among them.
- **The table of contents**: every entry of `bn-epub toc`, for the table
  the book is read by and for each kind it has, is the entry a reading of
  that file by ElementTree comes to, and so is their checksum.
- **The style sheets**: every sheet `bn-css book` reads has the checksum,
  the rules, the declarations and the count of things passed over that a
  second reading of it comes to (Style sheets, above, has what that
  reading is).
- **What a document asks for**: every document `bn-css sheets` goes
  through is given the sheets, in the order, that a second finding in
  Python gives it, and what was left out is the same kind for kind (What
  a document asks for, above, has what that finding is). A book where
  the tool says it had no room for a sheet, or could not read one to its
  end, is passed over and counted: the second finding does not work
  those out. The same is done for the documents of the book's spine, in
  reading order, by the checksum of what they are all given: `bn-css
  sheets --spine` against that checksum worked out from the second
  finding; and the tool has to come to it both ways, with each document
  opened from its place and with each found by its name.
- **What the rules come to**: for the first few documents of a book, up
  to 1,500 elements between them, what `bn-css document` gives each
  element is what a second working-out gives it, by checksum: the tool
  finding the document's sheets itself, the second working-out given
  those the second finding has. The second working-out is slow, which is
  why it is not every document.
- **The pictures**: every PNG `bn-png book` reads has the header, the
  ending, the rows and the checksum of their gray that a second reading of
  it by Python comes to (Pictures, below, has what that reading is); and
  so has every JPEG `bn-jpeg book --reduce 8` reads, at an eighth of its
  size, where the reader's arithmetic and the second reading's are the
  same.

**What it counts and compares with nothing.** No tool reads a document's
text yet, so there is nothing to set these beside; they are counted so
that what real books hold is at hand when something is sized for it. Of
every XHTML document of each book's spine, read once however often the
spine names it, the script counts the paragraphs and the ids:

- **A paragraph** is where `tools/layout-text.py` has one: the text
  between the starts and ends of block elements (the script takes that
  list from there). Its size is that text in UTF-8, with white space in a
  row made one space and a `br` made a line end. What is in `head`,
  `script`, `style`, `svg` or `math` is no part of it, and an element
  with no text, a picture alone, is no paragraph.
- **An id** is the value of any element's `id`, wherever in the document
  it is, or of an `a`'s `name` where it has no `id`. One given twice in
  a document counts once.

The line that begins `Of the` has, book by book, the largest paragraph
and the most paragraphs and ids in one document. An item of a spine that
is not XHTML by its media type is not read: the line after it says how
many there are of each kind (the six in `corpus/` are the pages of the
book laid out in SVG), and another says how many are files the book does
not have, where any are. `--document-report` says more: the spread of
each figure over the documents, how many have a paragraph over each of a
few sizes, and the five books with the most of each, by the document
that has it:

```
tools/check-books.py --document-report corpus
```

```
The documents of the spines: 834 of them, in 27 books:
  bytes of a document: 10769 at the median, 132402 for nine in ten, 2020786 at the most
  paragraphs in a document: 21 at the median, 109 for nine in ten, 2108 at the most
  bytes of a document's largest paragraph: 1195 at the median, 2939 for nine in ten, 141354 at the most
  ids in a document: 1 at the median, 16 for nine in ten, 501 at the most
  bytes of the names of a document's ids: 10 at the median, 162 for nine in ten, 4508 at the most
  bytes of the longest name of an id in a document: 10 at the median, 22 for nine in ten, 47 at the most
  documents with a paragraph over 4, 8, 16, 32 and 64 KB: 46, 29, 18, 5, 3
  the books with the largest paragraphs, in bytes:
      141354  pg24264-images-3.epub: OEBPS/2938539401917988555_24264-0-1.txt.xhtml
       26241  pg14741-images-3.epub: OEBPS/6968912479258425706_14741-h-0.htm.xhtml
       21717  pg7000-images-3.epub: OEBPS/6692091177720485162_7000-h-2.htm.xhtml
       21383  pg4300-images-3.epub: OEBPS/1471490836921852263_4300-h-18.htm.xhtml
       16499  adam-smith_the-wealth-of-nations.epub: epub/text/chapter-5-1.xhtml
  the books with the most paragraphs in one document:
        2108  adam-smith_the-wealth-of-nations.epub: epub/text/chapter-1-11.xhtml
        1438  pg4300-images-3.epub: OEBPS/1471490836921852263_4300-h-15.htm.xhtml
        1050  pg1012-images-3.epub: OEBPS/7872976103240418525_1012-h-2.htm.xhtml
         750  pg17489-images-3.epub: OEBPS/8334475935261823288_17489-h-3.htm.xhtml
         537  william-shakespeare_hamlet.epub: epub/text/act-3.xhtml
  the books with the most ids in one document:
         501  adam-smith_the-wealth-of-nations.epub: epub/text/endnotes-1.xhtml
         142  moby-dick.epub: OPS/toc.xhtml
         123  pg24264-images-3.epub: OEBPS/2938539401917988555_24264-0-5.txt.xhtml
          82  pg22367-images-3.epub: OEBPS/380337140494991823_22367-h-0.htm.xhtml
          67  pg17489-images-3.epub: OEBPS/8334475935261823288_17489-h-0.htm.xhtml
  the books with the largest documents, in bytes:
     2020786  linear-algebra.epub: EPUB/xhtml/fcla-xml-2.30li58.xhtml
      358772  adam-smith_the-wealth-of-nations.epub: epub/text/chapter-1-11.xhtml
      258985  pg24264-images-3.epub: OEBPS/2938539401917988555_24264-0-7.txt.xhtml
      255032  pg4300-images-3.epub: OEBPS/1471490836921852263_4300-h-15.htm.xhtml
      210832  pg26731-images-3.epub: OEBPS/408209634455667199_26731-h-0.htm.xhtml
```

The reading is Python's `html.parser`, which takes what a document has
however badly it is formed; one it gives up on is counted and said.
`e2e.DocumentCount` checks the counting on two books it makes, whose
paragraphs and ids were counted as they were written: a paragraph inside
another's block, a line end, text in the head and in a picture, an id
given twice, a document the spine names twice, a picture in the spine and
an item that is no file. It was tried against 16 faults put into the
counting on purpose, and found each one.

Where they differ it prints a line for each difference, starting
`DIFFERENT`, and ends with 1. It takes the tools from `build/host-release`
if they are built there, or else from `build/host`; `--tools` names another
build, and `--zip`, `--xml`, `--epub`, `--css`, `--png` and `--jpeg` one tool each.

A folder with a file named `SHA256SUMS` in it is checked against that file
as well: every file listed has to be there with that SHA-256, and every
EPUB in the folder has to be listed. That is how a book in `corpus/` that
has been changed, or added without its line, is found.

CTest runs it over `corpus/` as `e2e.Corpus`. It is as useful over any
folder of books on the computer at hand: a few hundred take some seconds
with the `host-release` tools. The check itself was tried against 17
faults made on purpose, in the books and in what the tools print, and
reported each one.

Its readings by Python have to be as careful as the tools are. The first
book it met whose names are UTF-8 without the ZIP file saying so was read
rightly by the tools and wrongly by Python's `zipfile`, which takes such
names for another encoding; the readings now take a name's bytes as UTF-8
whatever the flag says, and `e2e.EpubTool` makes a book of that kind.

## Fuzzing

Each reader of a book's own files, from the inflater to the table of
contents, has a fuzz target in `tests/fuzz`, and so have the heap the XML
parser's memory comes from, the hyphenator and the paragraph setter, which
are given a book's words, each reader of a drawing, the readers of PNG
and JPEG pictures and the scaler, and the reader of the fonts image, which
a partition can hold anything in place of: one function that
is given some bytes, puts them through the reader, and ends the program if
the reader does anything it should not.
Going outside its memory, or into undefined behaviour, is seen by the
sanitizers. The rest a target checks for itself: what the reader's header
promises of it, and that no single call does more than the header says a
call may.

| Target | What it reads its input with | What it holds the reader to, besides staying in its memory |
|---|---|---|
| `inflate` | `core::Inflater`, twice, with the stream and the output cut up two ways | No call takes more from its source than `MostInputFor()` allows. Both readings give the same bytes. They are the bytes zlib gives for the same stream, and a stream is whole only if zlib says so |
| `zip` | `epub::ZipArchive`, `ZipEntryReader` and `ZipBookFiles`: the directory from end to end, and the first few entries looked up and read, and then opened as a book's files are, from their places and by their names | What each call may read from storage (19 reads to open a file, 2 for an entry of the directory, and for a call on an entry what the inflater's bound comes to in blocks). An entry called sound gave as many bytes as the directory says, with its checksum. A file opened from its place has the path its own header gives, as much of it as there is room for, and is opened if and only if that path was read whole and the entry can be opened |
| `xml` | `epub::XmlParser`, with more or less memory, and a handler that may stop it or try to use it from inside | A step asks its source once, for 1 KB. The parser's heap is in order after every step. Text comes in whole characters, and never more of it than twice the bytes read, which is the most a change of encoding makes of them. A handler that stops the parser is told nothing more. The parser reads a good document rightly afterwards |
| `package` | `epub::Package`, a step at a time, twice | No step reads more than five blocks from storage. Nothing is said of the book until the reading is over. A book that was read has documents, each found again by its entry's name. A second reading says what the first said |
| `toc` | `epub::TocReader`, a step at a time, twice, with a handler that may ask for no more | No step reads more than five blocks. Every label is whole characters with its spaces collapsed, within its length; every entry points at a document of the book or at none. A stopped reading says so |
| `css` | `epub::CssReader`, twice, with the sheet cut up two ways; and the same bytes as a `style` attribute | A step asks its source once, for 1 KB. A rule is told whole and in order. A selector has no more parts or classes than there is room for, asks no more of attributes, and has a rank its parts account for. A number is within its bounds, and not below zero where the property cannot be. The reader's counts are the counts of what it told. Both readings tell the same |
| `css_rules` | `epub::CssRules` and `epub::CssCascade`: up to four sheets into a table of one of four sizes, a document gone through with a cascade that has one of four amounts of memory, the table taken back to before its last sheet, and the document gone through again | The table stays inside its memory. If it left no rule out, every element and every first letter is given exactly what a plain working-out of the same rules gives it (`tests/support/css_reference.h`). No element takes more work than `MostWorkFor()` says. A table taken back is the size it was and says of the document what a table never told of the last sheet says; and it takes the sheet again to the same end. |
| `document_sheets` | `epub::DocumentSheets`: a book of up to eight files held in memory, with one of four amounts of memory for the sheets' names, for a document's `<style>` text and for the table, read in pieces of one of four sizes, one file failing part way; the sheets of each document found, every other one handed over open, the first given up part way and begun again, and the first found again at the end | Every document ends with a list. A step opens one file at the most or reads `kMostBytesPerStep` of the book at the most, and never both. A list has no sheet twice, and every sheet in it is in the table and, but for the document's own, is a file of the book. The table has the sheets that were read and no others. What is said to be left out is what the counts come to. Until something is left out for want of room, the list and the counts are those of a plain working-out (`tests/support/sheets_reference.h`), and where the table kept every rule it was given, the document's own rules are as many as its `<style>` texts come to, each read to its own end. A document found again has the same list, and nothing is opened for it but itself. No file but a document is opened twice for the book, the sheet in hand when a document was given up apart. The digest of what the documents were given (`epub/sheets_digest.h`) counts what was found, and is the checksum of the lines the plain working-out comes to |
| `region_heap` | `core::RegionHeap`, given a made-up run of requests and of mistakes | Pieces lie inside the block and share no byte. Each holds what was put in it. Nothing outside the block is written. Every mistake is counted and changes nothing. The heap's records agree after every request |
| `hyphenator` | `layout::Hyphenator`, with a language's table as the input has it, or one made of patterns the input writes out, and the words that follow | No word takes more work than `MostWorkFor()` allows a word of as many characters. A word's places are in order, each at the start of a character, with letters enough directly on each side of it by the target's own reading of the word; a word too long to look at has none. With room for fewer places, the first are given. The same table somewhere else in memory answers the same. With patterns, the places are the ones found by laying every pattern over every place in the word |
| `paragraph` | `layout::ParagraphSetter`, with a paragraph's text in runs of up to seven styles and line ends asked for, on lines from 1 wide to 30,000, measured four ways | A paragraph is done with in a number of calls that depends only on its length. No call does more than `MostWorkFor()` allows, gives the breaker more than one item, asks the hyphenator about more than one word, or has more than a word's worth of bytes measured. The lines hold all the text once and in order. Each is as wide as its text adds up to and has the places between words it says; one is only too wide for a single character that is. The measure is never given a soft hyphen, nor a piece that begins inside a character. The lines are the same when the text is offered all at once |
| `svg_tags` | `draw::SvgTagReader`, twice: whole, and cut up, with the ends of the pieces it gives handed back to it | A call takes a byte or tells something. Things are told in an order tags can have, and `Depth()` is how many elements were opened and not closed. Each tag is in the file where `TagOffset()` says it began. No more bytes are told of than were taken, and what an entity stands for is one character. Once reading has failed nothing more is told. Both readings tell the same things |
| `svg_sheet` | `draw::SvgSheetReader`, twice: the sheet whole, and cut into pieces of one of eight sizes | A call takes a byte or tells something. A rule is told as its selectors, what it sets and its end, in that order. A selector is a class or an element's name made of what a name may be made of; a property's name is in small letters with no space; neither is longer than it may be, nor a value. Both readings tell the same things and say the same of what they dropped |
| `svg_values` | Each reader of what an attribute says (`draw/svg_values.h`), and `ParseSvgTransform` | A reading that refuses the text leaves what it was to fill as it was. Space round the text changes nothing, nor capitals a colour. A list of lengths has some, and no more than it has room for, and what reads as a length reads as a list of one. A view box is fitted where SVG says it goes, worked out again in floating point |
| `svg_path` | `draw::SvgPathReader`, `PathFlattener` and `Filler`: a shape drawn whole, and again in strips with its data cut up | A call gives no more commands than it was asked for and one. A curve is 1,024 lines at the most and an arc 32,768. A line adds to no more cells than `MostCellsPerLine()`. Nothing is written outside the strip, and no cell is left with anything in it. The two pictures are the same, byte for byte |
| `svg` | `draw::SvgRenderer`: a file read with nothing to draw into, drawn whole, and drawn in strips from a file cut up | No call gives an outline more than `kMostCommandsPerStep` commands, gives the filler more than `kMostLinesPerStep` lines, fills more than one shape or stroke, reads more than `kReusedPerStep` bytes of what the file defined, or looks through what it kept more than twice. No more than 32 groups are open, and no more is kept than there is memory for, which is of eight sizes and is filled with something else before each pass. A file that is not a drawing leaves the picture alone. Every strip's reading of the file is the same reading, rules and gradients kept included, and the two pictures are the same, byte for byte; and so is the file kept by a `SpoolWriter` and given back a band at a time, each step of which reads a chunk and fills once at the most |
| `png` | `draw::PngReader`, four ways: for the header alone with no memory, with the room and the pieces the input chooses, with all the room it needs, and against a plain reading of the same file with zlib (`tests/support/png_reference.h`) | No step takes more from its source than `MostInputPerStep()`, and the header alone takes 33 bytes. Rows come in order, each as wide as the picture, and a picture is whole only with all of them. It has no room only when it has less than `RowBytesFor()`, and a picture is interlaced only if it says so. The header alone says what the whole reading says of it. With the room it needs it gives the same rows in any pieces. The rows are the plain reading's, and it ends where and as that does, but for data the inflater refuses as bad that zlib takes |
| `jpeg` | `draw::JpegReader`, four ways: for the header alone with no memory, with the reduction, room and pieces the input chooses, with all the room it needs in pieces of 4 KB, and at an eighth | No step takes more from its source than `MostInputPerStep()`, nor gives more than one row. Rows come in order, each as wide as the picture is read, and a picture is whole only with all of them. The reduction taken is the one asked, or an eighth for a progressive picture, and the sides read are the picture's divided by it. It has no room only when it has less than `RowBytesFor()`, and a header it takes is one it can say the room for. The header alone says what the whole reading says of it, and fails as it does. It gives the same rows in any pieces, and ends at an eighth as it ends at the reduction asked |
| `picture_cache` | `draw::PictureCache`, reading a file the card holds, as the picture it says it is or as another, its checksums mended or not | Reading ends in a status, and no file is left open. Another picture can be kept among the same files, of more rows than the cache has memory for as often as not, and is read back the same; keeping it and reading it back, storage is written and read from where a sector begins and in whole sectors but for the last of the rows, what the header leaves of its sector is nothing, and given up part way it is not found whole. The memory the cache gathers in is one sector to eight, some of them with part of one over. Rows are given only whole, and only as many as the picture has. A picture read through without complaint is the file's own rows, the picture asked for, with the checksum its header gives; and kept again it reads back the same |
| `scale` | `draw::Scaler`, from sizes the input gives (sides of nothing and past the largest among them) and its pixels, sometimes all one gray, sometimes with an old row given out of turn | A side of nothing or too long is refused and nothing is given. `Row()` gives no new row and `Step()` one at the most; the new rows come in order, each as wide as the new picture, as many as it is high. A row out of turn stops it. Every new pixel is within a gray of the working-out in real numbers, and a picture of one gray stays that gray |
| `font` | `draw::FontImage` and `draw::Face`: an image opened twice, its first faces and its last looked into, and each asked for the code points at the edges of its ranges and four the input names | Opening goes the same way twice, and an image that is refused has no faces. A face is found by its own role, style and size. Finding a glyph takes no more steps than halving the face's ranges does. A glyph that is given has a picture of the size its width and height make, wholly inside the image, and is the glyph its number gives. In a face whose ranges are in order and apart, a code point has a glyph exactly when going through the ranges one by one finds it, and the glyph is what the image's bytes say, read the plain way |

The `package` and `toc` targets do not take a ZIP file as their input:
nearly every change to one would be stopped at an entry's checksum, and
the reader behind it never reached. They take the text of the files, and
make a sound ZIP file round them each time (`package` can also be given
the ZIP file itself). The `png` target does the same for a picture: unless
its input says to take the file as it is, it puts every chunk's checksum
right, and may take what the `IDAT` chunks hold as the rows themselves and
compress them before the reader sees them, cut into chunks of the size the
input chooses. Each target's source says how its input is laid out.
The first byte or two are its choices: how much memory the reader has,
how big a piece is asked for at a time, when the handler stops, how many
rows a strip of a picture has.

The targets that draw have a picture of 64 pixels by 48 at the most, and
leave a drawing off once it has come to 200,000 lines, which a few bytes
of arcs can ask for: its pictures are then not compared. The target for a
whole file leaves off as well once 200,000 bytes of what the file defines
have been read again, which a few bytes of `use` can ask for.

The hyphenator's bound is one figure for a whole word, and most tables
leave room to spare under it: work done past what a table says of itself
would not show. So some of its seeds are tables that say the least a table
can (patterns of one letter, states of one edge) and hold more, where
going by what the table holds is over the bound at once.

A target is built two ways.

- **In every ordinary build** it is a program that goes through the inputs
  kept for it in `tests/fuzz/inputs/<target>/`, once, and CTest runs it as
  `fuzz.<Target>`. So the targets are built by both compilers and run with
  the sanitizers and without, on every change, and a fault that an input
  once found stays found. The kept inputs are the seeds, which
  `tests/fuzz/make_seeds.py` makes out of text written for the purpose
  (a stream of each kind, a book of each kind, a table of contents with
  entries in it, a language's table and words for it, paragraphs of text,
  a drawing with something of everything), and any input that ever found
  a fault, cut down to the least that shows it and named `found-` and
  what it found.
- **For fuzzing** it is linked with LLVM's fuzzing engine, libFuzzer,
  which makes up new inputs by changing the ones it has, keeps each one
  that reaches code none reached before, and goes on from those.
  `tools/fuzz.py` builds the targets that way and runs them:

  ```
  tools/fuzz.py                  # every target, a minute each
  tools/fuzz.py --time 3600      # an hour each
  tools/fuzz.py xml zip          # only those two
  ```

  It ends with a line for each target in each build: how many inputs it
  tried, how many of those it kept, and whether anything was found.

  It runs each target in two builds at once: one with AddressSanitizer,
  and one without, because the heap inside a block is laid out one way
  with it and another way without, and the firmware has it the second
  way. What the engine has kept is in `.fuzz/corpus/`, and the next run
  starts from it, so a run gets further the more have gone before. An
  input that makes a target fail is saved in `.fuzz/found/`, and the
  summary says how to see it fail again and how to cut it down
  (`tools/fuzz.py --shrink FILE TARGET`). It needs the LLVM named in
  `tools/llvm-version`: Apple's compiler has no fuzzing engine.

Fuzzing is not one of CI's checks. A minute a target finds only what lies
near the seeds, and it is hours, from a set of inputs kept between runs,
that find the rest. It is run on a developer's computer: for a few minutes
after a change to any of the readers, and for an hour a target before a
set of them is called finished.

What it does not find:

- **How long a call takes.** A target counts what a call reads, not its
  time.
- **A bound that only a large input can reach.** The engine's inputs are
  16 KB at the most, and it takes a ZIP directory of more than five
  blocks, or a megabyte of blocks that produce nothing, to tempt a step
  past its bound. The unit tests make those files and hold the same
  bounds over them. The megabyte that a drawing may have read again of
  what it defines is the same: the target leaves a drawing off long before
  it, and a unit test takes a file up to it.
- `ZipArchive::Find`, which reads as much of a directory as it has to,
  and is not held to anything here.
- **Whether a drawing is drawn rightly.** The targets for SVG hold a
  picture to being the same however it is cut up, not to being the
  picture the file describes. That is for the unit tests and the
  end-to-end tests, which have something to set it beside.
- **A drawing of any size.** The pictures are small, and path data long
  enough to be drawn as it arrives is a good part of the longest input
  the engine makes.
- **What takes several tags that agree with one another**, short of a
  long run: a `use` that names another `use`, a group inside what is
  used, `inherit` for a property. The engine is slow to come to these by
  changing bytes, and what it has not come to it has not tried. The
  seeds for a drawing have one of each such thing that the inputs it had
  kept were found not to reach, by building the targets with clang's
  source coverage and taking them through `.fuzz/corpus/`. With those,
  what the engine's inputs and the seeds together do not reach of the
  renderer and the reader of style sheets is the megabyte above, the
  names the renderer gives things in its reports, and checks that nothing
  written in a file should be able to trip. The seeds alone reach less.

## Where a word may be broken

`layout/hyphenator.h` finds the places a word may be broken across two
lines, by the method TeX uses (Franklin Mark Liang, 1983). A language is a
set of patterns: short runs of letters with a number at each place between
them, such as `hy3ph` or `1tio`. Every pattern that occurs in a word is
laid over it, each place takes the largest number any pattern puts there,
and the word may be broken where that number is odd. So one pattern can
allow a break and a longer one forbid it again.

- **A table for each language**, made from its patterns by `bn-hyphen` and
  read where its bytes lie: in the firmware itself, or wherever else it is
  kept. Nothing is copied out of it and nothing is allocated. The patterns
  are a tree, read a letter at a time, and branches that are alike are
  kept once.
- **American English is built in.** No patterns are in this repository:
  the build downloads TeX's for American English (4,938 patterns, and 14
  words written out with their breaks) and makes them into a table of
  38,049 bytes, which is compiled in as constant data
  (`hyphenation/american_english.h`). On the device it stays in flash and
  is read there. It is only in a program that asks for it: so far the
  layout check (`reader/layout_check.h`), and not yet a normal firmware,
  in which nothing sets text. Its copyright notice is compiled in with it.
  A table made from TeX's patterns for German (36,709) is 234,154 bytes.
- **A word** is given as UTF-8, and the places come back as how many bytes
  of it come before each break. Capitals count as their small letters. A
  character the language has no letter for divides the word, and each side
  is looked at by itself; so do bytes that are not UTF-8. A word of more
  than 63 characters is given no places.
- **Words written out** with their breaks, as a language's exceptions are,
  go into the table as patterns of their own that no other pattern can
  change.
- **A table is not trusted.** Whatever its bytes are, nothing outside it
  is read, and a word is answered within the work the table's own header
  allows: no more starts than the word has letters, no further from each
  than the longest pattern, no more edges at a step than the widest state
  has (`Hyphenator::MostWorkFor`). With the American English table that is
  81,915 edges and numbers for a word of 63 letters. Over the 235,976
  words of a dictionary, the most any one took was 940, and they took 325
  each on average.
- **Flash**: 1.6 KB, as compiled for the device, without a table.

`bn-hyphen` makes a table and breaks words with one:

```
./build/host/tools/hyphen/bn-hyphen pack patterns.txt --out table.bnhy
./build/host/tools/hyphen/bn-hyphen words table.bnhy words.txt
./build/host/tools/hyphen/bn-hyphen info table.bnhy
```

`pack` takes patterns as they are written for TeX, with spaces or line
ends between them, and optionally the language's exceptions
(`--exceptions`), a list of its letters' other forms such as their capitals
(`--letters`), and the fewest letters it leaves before and after a break
(`--left`, `--right`: 2 and 3 unless told). For the nine patterns of TeX's
that the word "hyphenation" meets, it says:

```
patterns 9, exceptions 0
letters 9, as 18 characters
states 13, edges 18 (18 before like branches were kept once), widest 5
longest pattern 5, values 21 bytes
table 199 bytes
```

`words` prints each word of a file, or of what it is given to read, with
a `-` at every place it may be broken: `hy-phen-ation`.

The build makes the American English table the same way, from the
plain-text files that TeX's hyphenation package (hyph-utf8) keeps beside
each language's TeX file: `hyph-en-us.pat.txt` for the patterns and
`hyph-en-us.hyp.txt` for `--exceptions`. The TeX file itself will not do,
as its patterns are inside `\patterns{...}`, which `pack` does not read.
`third_party/hyph-en-us/CMakeLists.txt` names the commit they are taken
from and the hash of each. The table is left in the build, at
`build/host/parts/third_party/hyph-en-us/american_english.bnhy`, for
`bn-lines` or `bn-hyphen words` to be given; a unit test holds what is
compiled in to that file, byte for byte, and to words whose breaks TeX
gives.

Its tests write tables of their own, the plain way and with nothing
shared, and compare the places found with laying every pattern over every
place: 300 made-up sets of patterns, 60 words each. A table with any one
byte changed, and tables of bytes that are no table at all, have to be
answered within the work they state and without a read outside them. The
tables `bn-hyphen` makes are checked the same way, against the plain ones
too. And an end-to-end test (`tests/e2e/hyphen_tool.py`) sets `bn-hyphen`
beside a second way of breaking the same words that shares none of its
code, on 150 made-up languages with letters of one byte to four. Tables
and words that nobody wrote are the fuzz target's (see Fuzzing).

## Breaking paragraphs into lines

`layout/line_breaker.h` chooses where the lines of a paragraph end. It is
the first piece of `layout`, and it knows nothing of text, fonts or pixels.
A paragraph is given to it as a run of items, each with a width in whatever
unit the caller measures in: boxes, which are never broken; spaces, where a
line may end; breaks, which are places inside a word where a line may end
at a price, showing a hyphen if it does; and forced breaks. It gives back
the lines, each as a range of those items.

- **Lines are chosen together.** Filling each line as full as it will go
  leaves some lines tight and others with a great gap. `LineBreaker` makes
  the sum of the squares of the space left on each line as small as it can
  be, so a word is taken down early where that evens out the lines after
  it. The last line of a paragraph is allowed to be short.
- **The first lines can have widths of their own**, up to eight of them,
  and the rest share one. That is how a first line is indented, and how
  room is left beside a large first letter that stands several lines high.
- **Memory**: the caller provides an array of nodes, 32 bytes each, one for
  each place a line could end that is still in play. It allocates nothing.
  256 nodes (8 KB) hold a paragraph of about 250 such places whole.
- **Flash**: 4.1 KB, as compiled for the device.
- **A paragraph longer than its memory.** How even the lines can be made
  depends on the whole paragraph: one more word at the end can shift a
  little of every line before it. So nothing is given out before the end
  unless something makes it. When the nodes run out, the lines of the
  earlier half of them are settled as they would be if the paragraph ended
  there, and the rest is worked out again from that point. A paragraph of
  any length is broken in the same memory, and the lines come out no
  further behind the items than the nodes allow. A caller with room for
  only so many items can ask for lines sooner (`Settle()`).
- **What that costs.** A paragraph that fits its nodes is broken the
  cheapest way there is. For one that does not, the lines settled early
  cannot always be the ones the whole paragraph would have chosen. Over 30
  made-up paragraphs of 1,000 to 4,000 words with 256 nodes they cost 2%
  more than the cheapest breaking on average, and 10% at worst.
- **One call's work**: a paragraph cannot make one call long. Nearly every
  call weighs the nodes within one line's width and does no more. The call
  that does most is one that finds the nodes used up: it settles lines, and
  then works out again the cheapest way to every place after them, each
  place looking back over the nodes before it. Counted in nodes gone over,
  a call with `n` nodes does no more than `n(n + 40) / 2`, whatever the
  paragraph (`LineBreaker::MostWorkFor`): 37,888 with 256 nodes. Settling
  always makes room in one go. If one line's width holds more places than
  there are nodes, a line is made to end at the latest of them.
- **What will not fit.** A stretch of boxes with nowhere to break that is
  wider than its line goes on a line of its own, which says that it is too
  wide. Nothing else is ever wider than its line.
- **Not done**: spaces are not squeezed to get more on a line, two
  hyphenated lines in a row cost no more than two apart, and a last line of
  one short word is not avoided.

Its tests break paragraphs written out by hand for each rule, and compare
it with two other ways of finding the cheapest breaking, written for the
tests. One tries every way there is, on 600 made-up paragraphs short enough
for that. The other works forwards through the whole paragraph, keeping
everything, on paragraphs of up to 60,000 words. With the fewest nodes it
accepts, and with a caller that asks for lines early, the lines still have
to fit and to hold every item once.

Every call in those tests is held to the work one call may do. One
paragraph is made for that alone: a first line that is exactly full, and
after it more places to end a line than there are nodes, all within one
line's width. With 256 nodes its dearest call goes over 33,154 nodes,
which is within an eighth of the bound; the dearest call of a made-up
paragraph of ordinary shape goes over 2,600 to 10,400. Another 400
paragraphs are made to be awkward, with boxes of no width, boxes wider than
any line, spaces of no width and breaks at every price, in 32 to 300
nodes, half of them with a caller that asks for lines early.

## Setting a paragraph's text in lines

`layout/paragraph_setter.h` stands between a paragraph's text and the line
breaker. It takes the text as it comes, in runs that each have a style;
finds the words; asks the hyphenator where each may be broken, and a
measure how wide its pieces are; gives the breaker its items; and says of
each line that comes back which bytes of the paragraph's text are on it.
It knows nothing of fonts. How wide text is, is asked of a `TextMeasure`
that the caller provides, in whatever unit the lines' widths are in.

- **Spaces** are where a line may end. Several in a row are one space as
  wide as all of them. Those at the start of a paragraph, and after a line
  end the text asks for, are left out. A line feed is such a line end.
- **Words** are everything else, a no-break space among it. A word may
  change style part way through and is one word still: the hyphenator is
  asked about the whole of it, and each part is measured in its own style.
- **Where a line may end inside a word**: where the hyphenator says, at a
  price the caller sets, with a hyphen shown. At a soft hyphen, the same
  way; and a word that has one is broken at those and nowhere the
  hyphenator would have chosen. After a hyphen or a dash that is written in
  the word, at no price and with nothing more shown: but not after one the
  word begins with, between two of them, or between a minus and a figure.
  And at a zero width space.
- **What it says of a line**: where its text begins and ends in the
  paragraph, how wide it is, how many places between words it has (which
  is what spreading it out to the full width needs), whether a hyphen is
  to be shown after it, and whether it ends the paragraph or ends where the
  text asked. A line begins with the first character of its first word and
  ends with the last of its last.
- **Nothing runs off the end of a line.** A stretch with nowhere to end a
  line that is wider than the paragraph's narrowest line may be ended
  between any two of its characters, as a last resort, and the lines it
  runs over are then filled as lines of words are. So is a run of bytes too
  long to be taken as one word (252), which is how text in a script written
  without spaces comes out as full lines. A line may also end without a
  hyphen where a word could be broken and the narrowest line has no room
  for one. Only a character that is itself wider than a line makes a line
  too wide.
- **Memory**: the caller's. The breaker's nodes, and a record of 4 bytes
  for each item that is not yet on a line given out: 768 of them (3 KB) go
  with 256 nodes. When the records run out, lines are given out early. The
  setter itself is about 1 KB, most of it the word it has in hand.
- **Flash**: 5.5 KB, as compiled for the device.
- **One call's work**: a paragraph takes many calls, and a call need not
  take all the text it is offered. Each looks at no more than a word's
  worth of bytes (255), asks the hyphenator about one word at the most,
  has the measure go over a word's worth of bytes at the most, and gives
  the breaker one item at the most (`ParagraphSetter::MostWorkFor`). What
  the hyphenator and the breaker then do, they bound themselves.
- **Not done**: anything on a line that is not text, such as a picture
  among the words; the spaces at the start of a line of text that is set
  just as it is written; the rules for ending lines in scripts written
  without spaces between words; and spreading a line out, which is for
  whoever draws it.

`bn-lines` (`tools/lines`) sets a file of paragraphs by this code. There
are no fonts yet to measure text with, so it measures in a made-up face
whose widths it states (`layout/made_up_face.h`: a small letter is 36
quarters of a pixel, an `i` 18, an `m` 54), which lets a reader add a line
up. Text between two asterisks is in a second style, a little wider. With
the table made above from the nine patterns that "hyphenation" meets:

```
$ cat text.txt
Nobody on the quay had a word for hyphenation, and the well-known lamp was *lit* before anyone asked.
$ ./build/host/tools/lines/bn-lines text.txt --table table.bnhy --width 640
0	18	584	3	-	Nobody on the quay
19	36	562	4	h	had a word for hy-
36	54	540	2	-	phenation, and the
55	70	508	1	-	well-known lamp
71	85	426	2	-	was lit before
86	99	432	1	l	anyone asked.
```

Each line is where it begins and ends in the paragraph's text, its width,
its places between words, what is true of it (`h`: a hyphen is shown; `l`:
it is a last line; `w`: it is too wide) and its text. `--figures` prints
what setting the paragraphs took instead, with a checksum of all the lines
(`layout/line_digest.h`: where each begins and ends, its width, its number,
its places between words and what is true of it):

```
$ ./build/host/tools/lines/bn-lines text.txt --table table.bnhy --width 640 --figures
paragraphs 1, bytes of text 99, lines 6, the most in one paragraph 6
lines with a hyphen shown 1, too wide 0
words the hyphenator was asked about 18, places made as a last resort 0
lines given out early 0, in 0 paragraphs; ended for want of records 0
nodes in use at the most 22 of 256
calls 98; the most work in one, by the setter 36 of 1533, by the breaker 17 of 37888
bytes measured 82; checksum of the lines 65aeed32
```

Its tests set paragraphs written out by hand for each rule, and compare it
with a second way of making a paragraph's items, written for the tests:
the whole paragraph at once, every word found, cut and measured before the
breaker is given anything, on 600 made-up paragraphs. Another 400, with
lines as narrow as a letter, little memory, and the text offered a byte or
a few at a time, have to come out as lines that hold all the text once and
add up. Every call is held to what one call may do, on text made to be
awkward as well: one run of 5,000 letters with every letter wider than the
line, 500 soft hyphens end to end, 700 line feeds, a word in a new style
at every other letter.

An end-to-end test (`tests/e2e/lines_tool.py`) sets `bn-lines` beside a
second reading of the same paragraphs in Python that shares none of its
code. It takes each paragraph apart into the stretches that no line ends
inside and the places between them, with a made-up language's hyphenation,
and finds what the cheapest way of ending the lines costs by trying, for
every place, every place a line ending there could have begun. Over 480
paragraphs the lines `bn-lines` prints cost exactly that.

### Timing layout on a device

The breaker, the hyphenator and the setter each state a bound on the work
one call does, in nodes, edges or bytes. A firmware built with
`-DBN_LAYOUT_CHECK=ON` says what that work is in time on the device, and
sets the text of real books there to show that it comes to the same lines
as on the desktop: at start, and again at each press of the Power button.
(On the desktop, set `BN_LAYOUT_CHECK`, with `BN_STORAGE` naming the
folder that stands for the card.) It writes nothing to the card, and goes
with neither the storage check nor the book check.

The text is a file of paragraphs at `/layout/paragraphs.txt`, made from
books on a computer by `tools/layout-text.py`. It reads each book's
documents in the order of its spine, takes a paragraph to be the text of a
block element (`p`, `div`, a heading, a list item, a cell), and writes
them as `bn-lines` reads a file: a `br` is a line end inside a paragraph,
and `i`, `em`, `b`, `strong` and `cite` are the second style. It is a
plain reading of a book's text and not the importer. The text is
hyphenated by the table for American English that is built in, unless
there is a table on the card, at `/layout/hyphenation.bnhy`, as
`bn-hyphen pack` makes them: that one is taken instead, which is how
another language's table, or another version of English's, is timed.

```
tools/layout-text.py ~/Books/card/books --out paragraphs.txt
# copy it to /layout on the card
tools/idf -DBN_LAYOUT_CHECK=ON build flash
./build/host/tools/trace/bn-trace | tee layout.log
tools/idf -DBN_LAYOUT_CHECK=OFF build flash     # back to the normal firmware
```

What it does, in order (`reader/layout_check.h`):

1. Times the made-up face alone over 64 KB of text in memory, as the book
   check times its checksum: the face is what the text is measured in.
2. Breaks the paragraph that brings a call of the breaker's closest to its
   bound, with 64, 256 and 1,024 nodes, timing every call: the longest is
   the bound in time.
3. Reads the table on the card, if there is one, and otherwise takes the
   one that is built in.
4. Sets text made to be awkward for the setter (5,000 letters with no
   space, soft hyphens end to end, line feeds and hyphens and spaces by the
   thousand) at two widths, timing every call; and has the hyphenator find
   the places in eight long words.
5. Sets the text on the card three times: hyphenated, then not, then
   hyphenated again with every call timed, which finds the longest call
   and costs a reading of the clock round each. The lines are 1,760
   quarter pixels wide (440 pixels), with 256 nodes, 768 records and a
   hyphen at 12,100, which are `bn-lines`'s defaults.

What it finds goes out as log text. Over the text of the 28 books in
`corpus/`, on a desktop (`tools/layout-text.py corpus`, and no table on
the card, so the one built in):

```
layout check: the made-up face alone, 65536 bytes in 509 us (125736 KB/s).
layout check: breaker, 64 nodes: the dearest call 8 us, 2146 of 3328 work; 388 calls in 33 us.
layout check: breaker, 256 nodes: the dearest call 78 us, 33154 of 37888 work; 1540 calls in 338 us.
layout check: breaker, 1024 nodes: the dearest call 1112 us, 525826 of 544768 work; 6148 calls in 4533 us.
layout check: no table at /layout/hyphenation.bnhy (not found): the built-in table, 38049 bytes; no break before 2 letters or after 3 letters.
layout check: setter, awkward text: the dearest call 32 us, the most work 504 of 1533, the breaker's 11361 of 37888; 35666 calls in 5730 us.
layout check: hyphenator, 8 long words: the dearest 146 us for 64 (2281 ns a word), 2083 of 81915 work.
layout check: text: 17350039 bytes in /layout/paragraphs.txt.
layout check: hyphenated: 50313 paragraphs, 17233557 bytes, 329876 lines (30747 with a hyphen, 0 too wide); checksum 8ee4f78b.
layout check: hyphenated: setting 1272 ms (13230 KB/s), 115 us for 30 lines; the face about 114 ms of it; card 6 ms.
layout check: hyphenated: the longest paragraph 2027 us, 141354 bytes in 1585 lines; the most work in a call: setter 494, breaker 3242, hyphenator 1638.
layout check: hyphenated: 2435592 words asked about, 104 last resort, 76811 lines early in 2397, 0 cut short; 256 nodes; 14704031 bytes measured.
layout check: hyphenated: 13872034 calls.
...
layout check: hyphenated, every call timed: 13872034 calls, the longest 57 us.
layout check: passed.
layout check: the longest step: 1146 us, breaking the dearest paragraph.
```

The lines for each setting say what it came to, with the checksum of its
lines; how long setting it took, and so a page of 30 lines, beside the
card's time in reading it and the face's share, worked out from the bytes
measured at the rate found first; the paragraph that took longest, from
its start; the counts `bn-lines --figures` prints; and how many calls it
took. `bn-lines` prints the same paragraphs, bytes,
lines, hyphens, words asked about, bytes measured and checksum for the
same file and table with its defaults, so a device's lines can be shown to
be the desktop's. The run passes if the text could be set every way and
the first setting and the last, which differ only in being timed call by
call, came to the same lines. A table on the card that cannot be used is
said, and the built-in one taken; a missing text fails the run, after
everything that does not need it.

It is also in the metrics (`layout_check=1` if it passed,
`layout_kb_per_s` for the first setting, and `layout_longest_call_us`) and
in the trace (`reader/layout_check`, and `reader/layout_check_pass` for each
setting).

What it does not say: what a real face's measuring costs, which waits for
the fonts; nor what reading a paragraph out of a book costs, which waits for
the format books will be imported into. `e2e.LayoutCheck` runs the check
in the desktop application against `bn-lines` on text made from books the
test writes, with a table on the card and with the built-in one, and holds
`tools/layout-text.py` to a book whose paragraphs it writes out by hand.

## Choosing the lines of a page

`layout/page_chooser.h` decides which lines go on a page. Breaking a
paragraph into lines does not depend on where a page ends, so a chapter is
one run of lines whatever pages they fall on, a page is known by its first
line, and choosing pages is a matter of how many lines fit. Like the line
breaker it knows nothing of text. A line is a height, the gap that belongs
between it and the line before, and whether a page may end between the two.

There are three ways to come to a page, and something for each:

- **Forward**, a page is filled down from its first line (`PageFromTop`).
  Lines are offered in order until one is refused, and the next page
  begins with the line after the last one on this.
- **Back**, to a page that has not been laid out before, it is filled up
  from its last line: the one above the first line of the page being left
  (`PageFromBottom`). The lines above a page are only known by breaking
  their paragraphs from the start, so they are offered a block at a time,
  the nearest block first and the lines of each in their own order. A long
  paragraph is not held: only the lines that could still be the page's
  first are remembered, in entries the caller provides, 12 bytes each. 128
  of them (1.5 KB) do for a page of 128 lines.
- **Into the middle** of a chapter, at some line, the page begins with that
  line if a page may, and otherwise with the nearest line above it that
  one may begin with (`PageHolding`).

The rules are the same whichever way a page is come to:

- **Gaps.** The gap before a line is there while the line before is on the
  same page, and is left out above the first line of a page. The exception
  is a line that starts a page whatever comes before it, such as the first
  of a chapter: there it is the space the book wants above that line.
- **Lines kept together.** A line can be kept with the one before it, as a
  heading is with the paragraph under it, and the lines beside a large
  first letter are with each other. No page ends or begins between them.
  Filled down, a page goes back to the latest place it may end. Filled up,
  it begins with the first line below that may begin one. If the lines kept
  together are more than a page holds, it ends, or begins, where it is
  full.
- **A line that starts a page** always begins one. A page filled up stops
  there, with room to spare below if that is how it falls, and says that
  it did: whoever wants such a page full fills it down from that line.
- **A line taller than the page** has a page to itself.
- **The two directions agree.** A page filled up to where a page filled
  down ended holds that page's lines, and perhaps more above them; and a
  page filled down from where one filled up begins reaches at least as far
  as that one's end. The one exception is a page that had to begin or end
  between lines kept together, for want of anywhere better.
- **One call's work**: a call to `PageFromTop` or `PageHolding` does a
  fixed amount, and a call to `PageFromBottom` goes over no more entries
  than it was given.
- **Flash**: 1.7 KB, as compiled for the device.

Its tests fill pages written out by hand for each rule, and compare all
three with adding the heights up over the whole run: 300 made-up runs of
up to 600 lines, with lines of no height and lines taller than the page,
gaps, lines kept together and lines that start a page, cut into blocks of
1 to 300 lines, and 40 places tried in each. A page is filled up from the
end of a block of 20,000 lines in the entries an ordinary page gets, and
blocks whose heights add up to more than 32 bits hold are counted rightly.

## Fonts

The firmware does not read font files. Every face it draws text in, at
every size it draws it, is drawn beforehand on the computer that builds,
and kept in one run of bytes, the fonts image, which the firmware reads
where it lies: on the device, in the flash partition called `fonts` (see A
partition for fonts), mapped into the processor's address space; on the
desktop, in a file read into memory. So far the application uses the image
for one thing, showing a file of text to try faces with (Trying faces on
the panel, below); the rest of this is the image, the program that makes
it, the reader of it and the measure that sets text by it.

A face is one role (serif, sans, monospace, or the fallback for what the
others lack), one style (regular, italic, bold, bold italic) and one size
in pixels. It has its metrics, a table from code points to glyphs, a record
for each glyph and the glyphs' pictures. A glyph's picture is two bits a
pixel: four levels of ink, from paper to full. Its advance, how far it
moves the pen, is in quarters of a pixel and is the outline's own, not
rounded to the pixels its picture was fitted to: so the widths of a line's
characters add up to what the line is wide at any size. Glyph 0 of a face
is what is drawn for a character it has no glyph for. `draw/font.h`
describes the layout byte by byte. There is no kerning yet, and no
ligatures; bytes are kept for kerning in a face's record and a glyph's.

### Making the image

`bn-font` (`tools/font`) makes the image from font files, drawing each
glyph with FreeType, which the host build downloads (`third_party/freetype`)
and which is no part of the firmware. (A build configured with
`-DBN_FONT_TOOL=OFF` has no `bn-font` and downloads neither FreeType nor
the font its tests use; the firmware's build of its own host tools is
one.) Which files, and at which sizes, is said by a preset, a text file (the
build has some of its own: The presets, below):

```
sizes 26 30 34
mono regular  CourierPrime-Regular.ttf
mono bold     CourierPrime-Bold.ttf
```

A line names a face's role (`serif`, `sans`, `mono`, `fallback`), its style
(`regular`, `italic`, `bold`, `bold-italic`) and its font file, which is the
rest of the line and is found from the preset's folder unless it begins
with `/`. `sizes` are the sizes every face is drawn at, in pixels to the
em. `characters U+0020-U+007E U+00A0 ...` chooses the characters that are
drawn; without it they are Latin with its extensions, the phonetic
alphabet, the marks set over letters, Greek, Cyrillic, punctuation,
currency and letterlike symbols, number forms, arrows, mathematical
operators and the Latin ligatures. A character the font has no glyph for is
left out and counted. Anything from a `#` to the end of a line is passed
over.

```
$ bn-font build courier.preset --out courier.bnfn
mono     regular     26 px:  381 glyphs (2074 lacking), pictures   20833 bytes; ascent 21, descent 9, line 29, x-height 12
mono     regular     30 px:  381 glyphs (2074 lacking), pictures   27877 bytes; ascent 24, descent 11, line 34, x-height 14
mono     regular     34 px:  381 glyphs (2074 lacking), pictures   35732 bytes; ascent 27, descent 12, line 38, x-height 16
mono     bold        26 px:  381 glyphs (2074 lacking), pictures   22159 bytes; ascent 21, descent 9, line 29, x-height 12
mono     bold        30 px:  381 glyphs (2074 lacking), pictures   29667 bytes; ascent 24, descent 11, line 34, x-height 14
mono     bold        34 px:  381 glyphs (2074 lacking), pictures   37570 bytes; ascent 27, descent 12, line 38, x-height 16
faces 6, pictures 173838 bytes, image 204534 bytes
```

Glyphs are drawn with FreeType's light hinting, which moves an outline up
and down to the pixels and not across, and its 256 grays are made the four
levels. A serif or a sans that is given a regular and not all of the other
three styles is built, with a warning for each: the reader of the image
gives the nearest style a role has. The fonts a preset names are whoever
builds's own business: no font is in this repository, and a preset that
names fonts from the builder's computer need not be either. The pictures
are not compressed; what they would take if they were is for `bn-font
sizes` to say (below).

`bn-font info` says what an image holds, reading it by the code the
firmware reads it with; with `--glyphs` and some characters, what each face
has for them: the glyph's number, its advance in quarters of a pixel, its
picture's size and where its top left corner lies from the pen on the
baseline, and its levels of ink added up.

```
$ bn-font info courier.bnfn --glyphs U+0041 U+0067 U+0020 U+03B1
image 204534 bytes, faces 6
mono     regular     26 px:  381 glyphs; ascent 21, descent 9, line 29, x-height 12
  U+0041: glyph 34, advance 62, 16 by 15 at 0, 15, ink 249
  U+0067: glyph 72, advance 62, 14 by 17 at 1, 12, ink 288
  U+0020: glyph 1, advance 62, 0 by 0 at 0, 0, ink 0
  U+03B1: no glyph
mono     regular     30 px:  381 glyphs; ascent 24, descent 11, line 34, x-height 14
  U+0041: glyph 34, advance 72, 18 by 18 at 0, 18, ink 340
  U+0067: glyph 72, advance 72, 17 by 20 at 1, 14, ink 385
```

(and so on for the other four faces).

`bn-font sizes` says what an image's pictures would take if they were kept
in other ways than the image keeps them, which is as they are: for
choosing a way, and how many sizes a partition has room for, from the
fonts themselves. It changes nothing, and the firmware reads none of these
forms. For the faces of each size, and for all of them:

- **run-length, a glyph**: each glyph by itself as runs of pixels of one
  level, a byte to a run: the level in two bits and the length, 64 at the
  most, in six;
- **DEFLATE, a glyph**: each glyph by itself, compressed by zlib at its
  best;
- **the best, a glyph**: each glyph in whichever of those, or as it is, is
  smallest for it, and the last line says how many glyphs that was for
  each;
- **DEFLATE, groups** of 16, 64 and 256 glyphs in the order a face has
  them, and **a face** as one: compressed together, so that to draw one
  glyph its group has to be decompressed.

```
$ bn-font sizes courier.bnfn
image 204534 bytes: tables 30696, pictures 173838
26 px: 2 faces, 762 glyphs
  as they are                42992 bytes  100%
  run-length, a glyph        63138 bytes  147%
  DEFLATE, a glyph           39957 bytes   93%
  the best, a glyph          38914 bytes   91%
  DEFLATE, groups of 16      24473 bytes   57%
  DEFLATE, groups of 64      21986 bytes   51%
  DEFLATE, groups of 256     19218 bytes   45%
  DEFLATE, a face            17647 bytes   41%
```

(and the same for 30 px and 34 px)

```
all: 6 faces, 2286 glyphs
  as they are               173838 bytes  100%
  run-length, a glyph       219738 bytes  126%
  DEFLATE, a glyph          146463 bytes   84%
  the best, a glyph         144369 bytes   83%
  DEFLATE, groups of 16      86779 bytes   50%
  DEFLATE, groups of 64      78341 bytes   45%
  DEFLATE, groups of 256     68083 bytes   39%
  DEFLATE, a face            62114 bytes   36%
the best, a glyph: 691 as they are, 58 run-length, 1537 DEFLATE
```

Letters drawn in grays are poor in runs: an edge is a pixel or two of each
level, and a byte to a run comes to more than the picture as it is. A
glyph by itself is too little for DEFLATE to find much in, and what it
saves grows with the size; together, glyphs compress to half and less.
The compressed sizes are zlib's, and another version of zlib may give a
few bytes more or fewer.

### The presets

A fonts image can be made without bringing any fonts: the build has
presets, each a serif and a sans in four styles and a monospace face in
two, whose font files it downloads from their makers' repositories at a
commit it names, each checked against a hash (`third_party/fonts`). They
are downloaded when an image is asked for, not when a build is configured:

```
cmake --build --preset host --target fonts      # writes build/host/fonts/fonts.bnfn
```

Which typefaces is chosen when the build is configured:

| | |
|---|---|
| `-DBN_FONTS=noto` | Noto Serif and Noto Sans. This is what an image is made of unless something else is asked for |
| `-DBN_FONTS=plex` | IBM Plex Serif and IBM Plex Sans |
| `-DBN_FONTS=source` | Source Serif 4 and Source Sans 3 |
| `-DBN_FONTS=/a/preset/of/ones/own` | Whatever that preset names: its own font files, at its own sizes |
| `-DBN_FONTS_MONO=courier-prime` | Courier Prime as the monospace face. This is so unless something else is asked for |
| `-DBN_FONTS_MONO=family` | The family's own: Noto Sans Mono, IBM Plex Mono or Source Code Pro |

A preset here is three files of `tools/font/presets` read as one:
`ladder.preset`, which has the sizes; the family's faces; and a monospace
face. `bn-font build` takes any number of presets and reads them so, and
with `--fonts FOLDER` finds their files from that folder, which is how the
build points them at what it downloaded. The sizes are five, 22, 26, 30,
34 and 38 pixels to the em, each about an eighth larger than the one
before, 30 being the size text is first read in: on the X4's panel, about
7, 8.5, 10, 11.5 and 12.5 points as printed. Plex and Source have fewer of
the characters an image is made with than Noto has (about 900 and about
860 to 1,400 a face, against about 1,980), so their images have Noto Sans
besides, as the fallback face, for what they lack. As built, with the
12 MB of the `fonts` partition to fit in:

| Preset | Monospace face | Faces | Image |
|---|---|---|---|
| `noto` | Courier Prime | 50 | 9.64 MB |
| `noto` | Noto Sans Mono | 50 | 11.47 MB |
| `plex` | Courier Prime | 55 | 5.55 MB |
| `plex` | IBM Plex Mono | 55 | 6.04 MB |
| `source` | Courier Prime | 55 | 6.03 MB |
| `source` | Source Code Pro | 55 | 6.67 MB |

```
$ cmake --build --preset host-release --target fonts
...
mono     bold        34 px:  381 glyphs (2074 lacking), pictures   37570 bytes; ascent 27, descent 12, line 38, x-height 16
mono     bold        38 px:  381 glyphs (2074 lacking), pictures   47138 bytes; ascent 30, descent 13, line 43, x-height 18
faces 50, pictures 8620836 bytes, image 9639730 bytes
```

All of these typefaces are under the SIL Open Font License; what that asks
of a firmware given out with an image made of them is in `THIRD_PARTY.md`
(Fonts in a firmware). The tests download none of them: they read each
preset with either monospace face, and `e2e.FontTool` holds every font
file a preset names to being one the build has a commit and a hash for.

### Reading the image

`draw::FontImage` (`draw/font.h`) opens an image and gives its faces: by
number, by role, style and size, or the nearest style a role has at a size
(bold italic gives way to bold, then to italic, and any of them to
regular). A `draw::Face` gives a glyph for a code point, with its picture
as a span of the image's own bytes; nothing is copied, and nothing is
allocated.

The image is not trusted, though the firmware's own build made it: a
partition can hold anything.

- **Opening** looks at the header and at each face's record, 1,024 faces at
  the most, and refuses an image in which any face's tables run outside
  it. It reads only as many bytes as the image says it is long, so a
  partition larger than its image is no trouble.
- **Finding a glyph** halves the face's ranges of code points: 33 steps at
  the most (`Face::kMostSteps`), whatever the tables hold, and `LastSteps()`
  says how many. A glyph whose picture would run outside its face's
  pictures is not given.
- **RAM**: none but the two small objects. **Flash**: 1.2 KB for the
  reader and 1.1 KB for the measure below, as compiled for the device,
  besides the image itself.

`layout::FaceMeasure` (`layout/face_measure.h`) is the `TextMeasure` that
sets text in such faces. Each of the paragraph setter's styles is given a
face. A character is drawn with its style's face if that has a glyph for
it, with the fallback face if that has, and otherwise with its style's
face's glyph 0; a soft hyphen and a zero width space are not drawn and take
no room; a byte that is not UTF-8 is drawn as U+FFFD. A piece of a word is
as wide as its glyphs' advances added up, so what is drawn from the same
glyphs lines up with what was measured. The hyphen shown at a line's end is
the style's U+2010, or its hyphen-minus. Reading UTF-8 a character at a
time, strictly, is `core/utf8.h`, which the hyphenator shares.

The reader's tests write their images by hand from the description of the
layout (`tests/support/font_image.h`): what is found and what is not, an
image cut short at every byte, and one with every byte changed three ways,
from which nothing may be given that lies outside it. The builder's tests,
and `e2e.FontTool`, try `bn-font` on a real font, Courier Prime, which the
build downloads for them (`third_party/courier-prime`): the test takes the
image apart itself, and the font file as far as its tables of characters
and widths, and holds the image to having a glyph for exactly the
characters the font has, each moving the pen as far as the font's width
for it comes to at that size; `bn-font info --glyphs` to saying of
every glyph what the image's bytes say; and `bn-font sizes` to the sizes
the test works out again, the runs written out a second time and the
compressing done by Python's zlib. Nothing draws a glyph a second way:
that a picture is its letter is for whoever looks at it. The reader has a
fuzz target as well (`font`, see Fuzzing).

### Trying faces on the panel

In place of the test pattern, the application can show text set in a fonts
image's faces (`reader/text_viewer.h`): the card's
`/layout/paragraphs.txt`, the file the layout check sets, a page at a time.
It is for seeing faces and sizes on a panel, and the first place the
hyphenator, the paragraph setter and the fonts work together in the
application's loop. On the desktop, with `BN_TEXT` naming an image `bn-font`
made and `BN_STORAGE` the folder that stands for the card:

```
BN_STORAGE=card BN_TEXT=courier.bnfn ./build/window/app/desktop/biblionaut-desktop --window
```

| Button | What it does |
|---|---|
| Right, Down | The next page, by a half refresh |
| Left, Up | The page before, of those gone through |
| Confirm | The next of the image's sizes, from where the page shown begins |
| Back | The sans in place of the serif, or the serif again, from the same place |

The text is read as `bn-lines` reads it: an empty line ends a paragraph, a
line end inside one is a line the text ends, and what is between two
asterisks is in italic. A paragraph is set by the paragraph setter with the
image's faces for its measure and the built-in table of American English,
its lines spread to the full width but the last, with 24 pixels left round
the text and a third of a line between paragraphs.

No picture of the page is kept. Setting a page leaves a list of its glyphs,
each with where its picture goes (12 bytes a glyph, 3,072 of them at the
most), and the panel's rows are made from the list a band of 16 at a time,
turned the way the device is held to be read, with the darker ink where
two glyphs meet. On a panel that shows four levels, as the X4's does, a
page is shown first with every pixel that has any ink black, by a full
refresh the first time and a half refresh after; then the bands are made
again as the marks of the pixels with one level of ink and with two, and
the gray pass (see Grays) makes those light and dark gray, the drive
voltages kept up between the two. A page with no gray in it has no gray
pass. On a panel of two levels, a pixel with two or three levels of ink of
the three is black.

Nothing in it waits. A turn of the loop works for about 20 ms: a paragraph
is read 256 bytes at a step and given to the setter a call at a step, its
lines are placed one at a step and the bands written one at a step, so a
paragraph of any length leaves the buttons answering. What a button asks
for is done once the page under way is set and the panel is free: a page
whose rows are still being written is given up for the one asked for, and
one already shown gives up its grays. For each page shown, a line says
what it was and where the time went:

```
text: page 1, serif 30 px, 21 lines, 368 glyphs: set 2 ms, planes 6 ms, refresh 3587 ms, gray planes 6 ms, gray pass 212 ms
text: page 2, serif 30 px, 21 lines, 381 glyphs: set 1 ms, planes 7 ms, refresh 1585 ms, gray planes 6 ms, gray pass 216 ms; the end of the text
text: page 1, serif 22 px, 29 lines, 709 glyphs: set 3 ms, planes 7 ms, refresh 1579 ms, gray planes 7 ms, gray pass 217 ms
text: page 1, sans 22 px, 29 lines, 709 glyphs: set 3 ms, planes 8 ms, refresh 1587 ms, gray planes 9 ms, gray pass 216 ms
```

(From the desktop, in a build with the sanitizers: the refreshes are the
simulated panel's, and the rest is no measure of a device.) It takes about
54 KB from the heap for the X4's panel, most of it the list of glyphs, and
keeps the paragraph being set, 64 KB of text at the most. No firmware has
it yet: the fonts image is not yet in a device's flash.

Its tests give it images written by hand, on panels turned each of the
four ways, and hold every pixel the panel shows to the level of its ink,
worked out in the test; and `e2e.TextViewer` runs the desktop application
on an image `bn-font` makes of Courier Prime and adds up the ink in the
pictures the simulated panel saves: two pages of text hold the ink that
`bn-font info` gives for each of its letters, and a hyphen's for each word
broken, and nothing else.

## Drawings

A book can hold a drawing as SVG, and `draw/` draws one. It reads the
file's tags, works out where each shape goes, reads the shape's outline,
which SVG writes as path data (the `d` of a `<path>`), turns that into
straight lines in pixels, and fills what they enclose with a gray, a strip
of the picture at a time. That is shapes and what they are filled with,
whether a shape is written where it is drawn or defined once and used in
many places, and a line of some width along an outline. Text and a
picture inside a drawing are not drawn yet: a drawing comes out without
them, and with a report of what it had.

This section takes the parts from the inside out: outlines, then filling
one and stroking one, then a whole file.

The device's processor has no floating point, so all of it is done in whole
numbers (`draw/fixed.h`). A position counts 4096ths of a unit of the
drawing or of a pixel, a factor of scale 1,048,576ths, a sine 2^30ths, and
an angle a part of a turn. Sines, cosines and the angle of a point are
found by shifts and additions. Nothing can overflow: a number too large for
its kind is held at the largest that fits, and whoever asked is told.

- `SvgPathReader` (`draw/svg_path.h`) reads path data and tells a
  `PathSink` what it draws. The data is given in pieces of any size, cut
  anywhere, so a path never has to be whole in memory: one in a book is
  over 3 MB. What the sink hears is simpler than what was written. Every
  distance has become a place, a command followed by several sets of
  numbers has become several commands, and the short forms of a curve have
  been given the control point they leave out.
- `ParseSvgTransform` (`draw/svg_transform.h`) reads a `transform`: a list
  of `matrix`, `translate`, `scale`, `rotate`, `skewX` and `skewY`, as one
  matrix.
- `PathFlattener` (`draw/flatten.h`) is a `PathSink` that puts each point
  through a matrix and cuts each curve into lines, as many as it takes for
  none to stand further from the curve than a tolerance, which is a
  quarter of a pixel unless it is told otherwise. It gives each line to a
  `LineSink` as it is made and keeps none. Told which part of a picture is
  being drawn (`KeepTo()`), it gives a curve that lies wholly above, below
  or to one side of that part as a single line, which changes nothing that
  is drawn there.
- **Arcs** are drawn as SVG defines them, whatever the matrix does to
  them: radii too short to reach from one end to the other are made just
  long enough, and with no radius an arc is a line. One so shallow that it
  stays within the tolerance of a straight line is drawn as that line,
  which is what makes a radius of any size harmless: two public books
  have arcs whose radius is seventeen digits long.
- **Memory**: the reader is 168 bytes and the flattener 128, on the device,
  and cutting a curve takes about half a kilobyte of stack. Nothing is
  allocated, and nothing grows with the path.
- **Flash**: 18 KB, as compiled for the device.
- **Speed**: not yet measured on a device.
- **Bad input**: the data is not trusted. Whatever bytes it holds, reading
  ends with a status. Where path data goes wrong, everything before the
  command with the mistake in it has been drawn and nothing after it is,
  which is what SVG asks for; `ErrorOffset()` says where.
- **What a call can cost**: the data cannot make a call long. The reader
  looks at each byte once, and its `Step()` comes back once it has told the
  sink as many commands as it was asked for, or one more. One command is
  one line, or a curve of 1,024 lines at the most, or an arc of 32,768,
  whatever its numbers. So whoever reads a path a piece at a time decides
  how much one piece may draw.
- **Limits**: a position is held to a 4096th and to 524,287 either side of
  nothing, and a factor of scale to 2,047.
- **How exact**: a line is within the tolerance of its curve. A matrix's
  factors are exact to a part in a million, so the far edge of a drawing
  32,000 units wide lands within a fiftieth of a pixel. Where a path is
  written as distances from one point to the next, they are added up far
  more finely than a position is held, so thousands of small steps still
  end where they should.

`bn-svg` runs the same code on the desktop. For path data in a file, it
prints each command with its numbers as places, and under each the lines it
became, in pixels:

```
./build/host/tools/svg/bn-svg path outline.d --transform 'scale(2)'
```

```
M 10.0000 20.0000
 m 20.0000 40.0000
L 40.0000 20.0000
 l 80.0000 40.0000
A 15.0000 15.0000 0.0000 0 1 10.0000 20.0000
 l 79.4180 45.8796
 l 77.7161 51.4802
 l 74.9592 56.6436
 l 71.2129 61.2129
 l 66.6436 64.9592
 l 61.4800 67.7161
 l 55.8792 69.4177
 l 49.9990 69.9995
 l 44.1191 69.4175
 l 38.5188 67.7156
 l 33.3552 64.9587
 l 28.7861 61.2124
 l 25.0396 56.6431
 l 22.2830 51.4797
 l 20.5811 45.8794
 l 20.0000 40.0000
Z
 z
4 commands, 17 lines, 27 bytes
```

That is `M10 20h30a15 15 0 0 1-30 0z`, a line and half a circle. `--tolerance`
sets how far a line may stand from its curve, `--pieces` gives the reader
the data a few bytes at a time, and `--count` prints only the last line.

The unit tests set the whole-number working beside floating point: sines
and angles against the C library's, and the lines drawn for curves and for
arcs of every shape against the curve itself, worked out from the formula
that defines it. An end-to-end test (`tests/e2e/svg_tool.py`) sets `bn-svg`
beside a second reading of path data that shares none of its code, written
in Python with regular expressions: over paths made up at random in every
way SVG lets one be written, the commands have to be the same and the lines
within the tolerance of the true curves.

### Fewer grays

`Ditherer` (`draw/dither.h`) turns rows of 256 grays into as many levels as
a panel shows, from two up, in place. Each pixel becomes the nearest level
to its gray and what was carried to it, and what is left over is carried on
as Floyd and Steinberg did it: seven sixteenths to the next in the row, and
three, five and one to the three below. So a patch comes out, on the whole,
as light as it was, and a gray between two levels as a mixture of them.
Rows are given from the top, as strips make them: what is carried down from
the last row of one strip goes into the first of the next. The memory it
carries in is the caller's, two rows of 16-bit numbers. The unit tests hold
it to patches as light as their grays, a ramp as light as it was column by
column, levels already levels left alone, and only levels coming out.

### Filling an outline

`Filler` (`draw/fill.h`) is a `LineSink`, so the flattener's lines go
straight to it. Given the lines of a shape, it lays the shape in a gray
over a strip of a picture, which is held a byte a pixel. Edges are smooth:
a pixel the outline passes through takes as much of the gray as the shape
covers of it.

- **How**: no outline is kept. Each line, as it arrives, is added into a
  row of cells, one for each pixel: what the line adds to the cover of the
  pixels it passes through, and of every pixel to the right of it. Adding
  the cells up along a row then gives how many times the outline has gone
  round each pixel, in 256ths, and the fill rule turns that into cover.
  Both of SVG's rules are there, `nonzero` and `evenodd`.
- **Memory**: set by the strip and not by the drawing. Beside the strip
  itself, there are two bytes of cells for each pixel and four for each
  row: 30,848 bytes for 32 rows of 480 pixels. An outline of a hundred
  thousand lines needs no more than one of three. The object is 68 bytes
  on the device.
- **Strips**: a picture taller than the memory to hand is drawn as several
  strips, with the drawing gone through once for each. It comes out the
  same, byte for byte, however it is cut.
- **One shape over another**: a shape is laid over whatever the strip holds
  already, and with an opacity short of the whole, what is under it shows
  through.
- **What lies outside**: lines may lie anywhere. What is above or below
  the strip is passed over, what is right of it changes nothing, and what
  is left of it counts for its height alone.
- **Within a box**: `Fill()` can be given a box of the picture to keep to.
  Nothing outside it is drawn, and a pixel that an edge of the box passes
  through takes the share of its cover that the box has of the pixel. That
  is how what is inside an `svg` is cut off at the edges of the `svg`.
- **A gray that changes**: `Fill()` can be given a `GrayRamp` in place of
  one gray: how far along the ramp the middle of the picture's first pixel
  is, how much further each pixel to the right is and each pixel down,
  and up to eight stops, each a place along it with a gray and how solid
  it is there. Between two stops the gray goes evenly from the one to the
  other, two stops in one place make a step, and beyond the ends there is
  the end's gray, or the ramp again to and fro, or again from its start.
  That is a linear gradient. Each pixel costs a few additions, a look
  through the stops and a multiplication. The ramp is 208 bytes.
- **What a call can cost**: wherever a line runs, it adds to no more cells
  than `MostCellsPerLine()`, which is little over the strip's width and
  four for each of its rows. `Fill()` looks at each pixel of the strip once
  at the most.
- **Limits**: a strip is at most 16,384 pixels wide and 16,384 rows high.
  Cover is added up for a pixel as a whole. That is exact where one edge of
  a shape passes through the pixel, or a corner, or the edge between two
  parts that overlap, and only near where an outline crosses itself within
  the pixel or runs twice along the same edge: a shape drawn twice over in
  one outline comes out with a heavier edge. An outline that goes round a
  pixel more than 127 times the same way is beyond what a cell counts.
- **How exact**: lines are placed to a 256th of a pixel, and a pixel's
  cover comes out within three parts in 255 of the area of the shape that
  lies in it. What is left is the flattener's tolerance. One engraving in
  a public book is a single outline of 2.4 MB and 102,891 commands. Drawn
  480 pixels wide, it was set beside the same drawn eight times the size by
  `rsvg-convert` and averaged down. At a tolerance of a quarter of a pixel
  the worst pixel is 57 parts in 255 out and the average 2.0; at an
  eighth, 29 and 1.1; at a sixteenth, 13 and 0.5; at a fiftieth, 5 and
  0.3. What `rsvg-convert` draws at that size is itself 34 and 1.8 out.
  The same was done for 389 paths taken at random from public books, each
  drawn 160 pixels across: at a fiftieth the worst pixel of any of them is
  8 parts in 255 out, and at a quarter 73, with half of the paths at 61 or
  more. So a drawing whose edges are to be gray wants a finer tolerance
  than the quarter of a pixel that is the flattener's own.
- **Flash**: 7 KB, as compiled for the device.
- **Speed**: not yet measured on a device. At a quarter of a pixel that
  engraving is 139,129 lines, which add to cells 495,500 times. Drawn in
  strips, the whole outline is read again for each, and a curve outside a
  strip still costs a line: in 22 strips of 33 rows it comes to 1,957,022
  lines.

`bn-svg fill` draws path data into a picture, in strips of as many rows as
it is told, and says what that took:

```
./build/host/tools/svg/bn-svg fill outline.d --size 100x80 --transform 'scale(2)' --rows 16 --out outline.pgm
```

```
100 by 80 in 5 strips of 16 rows: 4 commands, 49 lines and 200 cells added to in all, 3264 bytes of cells
```

That is the outline above again, and the picture is a PGM file. `--rule`
chooses `nonzero` or `evenodd`, `--gray` and `--opacity` what the shape is
filled with and how much of what is under it shows through.

The unit tests set what is drawn beside the cover each pixel should have,
found two other ways: for shapes with no dents, by cutting the shape to the
pixel and measuring what is left, and for shapes that cross themselves, by
trying a grid of points in each pixel. They also hold it to the same
picture however it is cut into strips, with or without `KeepTo()`. The
end-to-end test fills shapes of lines, curves and arcs made up at random,
and sets `bn-svg`'s pictures beside a second way of filling them, in
Python: the outline traced finely, and lines run through each row of pixels
to see where they are inside it.

### Stroking an outline

`Stroker` (`draw/stroke.h`) draws a line of some width along an outline.
It is a `LineSink`, as the filler is: a flattener with no matrix hands it
the outline's lines in the coordinates they were written in, and what it
makes, the outline of the stroke, goes on through a second flattener, with
the outline's matrix, to the filler, which fills it by the nonzero rule.
So the stroke is as wide as SVG says in the outline's own coordinates, and
a matrix that stretches one way more than the other makes it wider one way
than the other, as SVG has it.

- **How**: each line of the outline becomes a piece as wide as the stroke;
  each corner between two lines, a piece that fills the corner's outer
  side as `stroke-linejoin` says (a miter, as far as `stroke-miterlimit`
  lets it reach and a bevel past that; round; or a bevel); and each open
  end a piece as `stroke-linecap` says (butt, round or square). A part of
  an outline that has no length is a dot, if its ends are round or square.
  Every piece goes round the same way, so that where pieces overlap the
  nonzero rule fills them as one. `stroke-dasharray` and
  `stroke-dashoffset` cut the stroke into dashes, each with its own ends,
  and a corner within a dash is a corner. A round edge is cut into sides
  of at most a sixty-fourth of a turn: for a stroke 100 pixels wide, they
  stand 0.06 of a pixel inside the circle at the most.
- **Where pieces meet**: the filler counts a pixel twice where the edges
  of two pieces that overlap cross it (its limit, above). So the pieces of
  two lines that meet end along the line that halves the corner, and share
  that edge, and the corner's own piece shares its edges with them both:
  along an outline nothing overlaps, and every pixel is counted once. A
  stroke 12 pixels wide round a circle 40 across, given as the 64 lines a
  curve comes to, is within 0.012 of the cover each pixel should have;
  pieces that overlapped at every corner would make its inner edge up to
  0.35 too dark. Only where a corner is too sharp for the lines beside it,
  so that the halving line would reach past the middle of either, do the
  pieces end square and overlap on its inner side; there, and where a
  stroke crosses itself, the pixels where edges cross come out darker,
  and never lighter.
- **A dashed part that is closed** is drawn as if it were open where it
  begins and ends, with ends and no corner there.
- **Memory**: nothing of the outline is kept but the first and last lines
  of the part in hand, until it is known how they begin and end. The
  object is 336 bytes on the device, and its code 8 KB.
- **What a call can cost**: a line given to it becomes no more than 40
  lines (`kMostLinesPerLine`), and the end of a part of the outline or of a
  dash no more than 78 (`kMostLinesPerEnd`). No more than 4,096 dashes are
  begun in one outline (`kMostDashesPerOutline`): past them the stroke is
  solid, which it says, so that a pattern of dashes too short to see
  cannot ask for endless work.

The unit tests draw strokes through the filler and set them beside where
each stroke should be, found another way: with round corners and ends,
every point no further from the outline than half the width, tried at 256
points in each pixel; with the others, the shape of each corner and end.
Outlines made at random, which cross themselves and turn sharply between
short lines, come out nowhere lighter than they should by more than that
way of finding them can tell.

### A whole file

`SvgRenderer` (`draw/svg_render.h`) draws an SVG file into a strip of a
picture. It is given the file's bytes a piece at a time, and a `Filler` set
to the strip. The file is read from its start to its end once for each
strip. Nothing of it is kept from one strip to the next, and within a
strip only what it defines for later use.

- **Tags**: `SvgTagReader` (`draw/svg_tags.h`) reads as much of XML as a
  drawing needs. expat, which reads a book's other XML, holds a whole tag
  at once, and a drawing can have a tag of megabytes: the outline of an
  engraving is one attribute, and so is a picture inside a cover. So this
  reader gives an attribute's value out in pieces as it arrives and keeps
  none of it. A piece is at most 512 bytes (`kLongestPiece`), however much
  the reader is given at once: the renderer takes the front of a piece and
  gives the rest back, and without that limit the rest would be looked at
  again at every call. Read 8 KB at a time on the X4, the largest engraving
  took 9.6 s to render without it and 7.0 s with it. It reads elements and attributes, the five entities,
  characters by number and text marked as CDATA, and passes over comments,
  processing instructions and a document type. It does not check that an
  element is closed by its own name, and an entity that a file declares
  for itself stands for nothing.
- **Style sheets** (`draw/svg_sheet.h`): `SvgSheetReader` reads the text
  of a `<style>` element as it arrives, in pieces cut anywhere, and tells
  of each rule: its selectors, then each declaration, then its end. Of
  every sheet inside a drawing in the books, every selector is a class
  (`.st0`) or an element's name (`path`), so those are what it reads; a
  rule with any other selector is passed over whole, as CSS passes over a
  rule with one it does not understand, and so is an `@` rule. Comments,
  strings, brackets and the `<!--` a sheet is written inside are read as
  CSS has them. It holds one rule's selectors and one declaration, and is
  284 bytes on the device.
- **What an attribute says** (`draw/svg_values.h`): a length in any of
  SVG's units, and a list of them, a colour (`#rgb`, `#rrggbb`, `rgb()`
  and twenty names), a paint, an opacity, a view box, and how a view box
  is fitted. A colour is
  drawn as the gray it looks: 0.299 of its red, 0.587 of its green and
  0.114 of its blue.
- **What it draws**: `svg`, `g`, `a` and `switch` as groups, each with a
  `transform` and with paint that what is inside it takes unless it has
  its own. `path`, `rect` with rounded corners, `circle`, `ellipse`,
  `polygon` and `polyline` as shapes, filled as `fill`, `fill-rule`,
  `fill-opacity` and `opacity` say, each written as an attribute, in
  `style` or by a rule of the drawing's style sheet. A rule wins over an
  attribute and `style` over a rule; between rules a class wins over an
  element's name, and a later rule over an earlier. A sheet styles what
  comes after it in the file, which is read once from its start for each
  strip, and says so if it comes after a shape. Shapes and `line` are
  stroked as `Stroker` strokes an outline (above), as `stroke`,
  `stroke-width`, `stroke-opacity`, `stroke-linejoin`, `stroke-linecap`,
  `stroke-miterlimit`, `stroke-dasharray` and `stroke-dashoffset` say, said
  in any of the same ways, and the stroke goes over the fill.
  `display` and `visibility` hide things. An `svg` inside another
  has its own place, size and view box, and what is in it is cut off at
  its edges unless its `overflow` says it may show beyond them. The whole
  drawing is fitted to the picture by its `viewBox` and
  `preserveAspectRatio`, and nothing is drawn outside the picture,
  whatever its `overflow` says: placed in a larger one, as the viewer
  places it on the page, it covers its own part and no more.
- **What a file defines**: `use` draws again something that `defs` holds,
  moved by the transform of the `use` and then by its `x` and `y`, and
  painted as the `use` is where it says nothing of its own. A `symbol` is
  drawn only so, as an `svg` of the size the `use` gives. This is how a
  formula is written: the outline of each letter once in `defs`, and a
  `use` wherever the letter stands. To be used, an outline has to be kept
  until then, and that is the one thing here whose size the file decides,
  so the memory for it is the caller's (`KeepDefinitionsIn()`). What
  `defs` holds is copied there as it goes by, just as it is written, with
  eight bytes more for each element that has an `id`. A `use` reads its
  element again from the copy, with a reader of tags of its own, as if it
  stood where the `use` does. 2,277 files in the public books define
  something. The largest keeps 18,430 bytes; 24 KB is room for all of
  them, 16 KB for all but 2, 12 KB for all but 15, 8 KB for all but 113
  and 4 KB for all but 766. The most that one of them reads again for a
  strip is 82,257 bytes. There are three limits, each reported and none
  of them a failure. Only what is in `defs`, before its use, can be used,
  by the name `#` and its `id`. What does not fit in the memory is not
  drawn where it is used, nor is a named thing inside more than eight
  others that have names. And no more than a megabyte is read again for a
  strip, nor one `use` inside another more than four deep, so that a few
  kilobytes of things that use things cannot ask for hours of work.
- **Gradients**: a `fill` of `url(#name)` that names a `linearGradient`
  earlier in the file, in `defs` or not, is drawn as one, a gray for each
  pixel from where its middle lies along the gradient. Its ends are
  anywhere and its transform any, over the shape's own box or in the
  space the shape is drawn in, and it takes what it does not say for
  itself from the gradient its `href` names. It is kept in the memory for
  definitions, as a record of 71 bytes, its name, and four bytes for each
  stop. In SVG the box of a
  shape is the box round the shape itself; here it is the box round its
  outline's points and its curves' control points, which can be a little
  larger where a curve does not reach its control points. A gradient whose
  `href` names one later in the file takes nothing from it.
- **What it leaves out**, each of which the report names (`SvgReport`):
  text, pictures, radial gradients and patterns (and a stroke painted with
  any gradient), what a style sheet says that it does not read, clip
  paths, masks, filters and markers. Two things are drawn nearly:
  `opacity` on a group or a `use` is given to each shape in it, which
  differs where they overlap, and an `svg` inside another that something
  has turned, other than by quarter turns, or slanted is not cut off at its
  edges, which are then no edges of pixels.
- **The order of attributes** is free, with limits for long path data. An
  outline is drawn as its data arrives, so a `transform` written after a
  shape's own path data would come too late. Data of up to 512 bytes is
  therefore held until its tag ends, and gone through once for the fill
  and again for the stroke. In the public books a transform follows its
  path data on 33 paths, the longest of them 403 bytes. If longer data is
  followed by a transform, the shape is left out and reported. And longer
  data is either filled or stroked as it arrives: stroked if by then the
  shape has a stroke and no fill, and filled otherwise. A stroke the shape
  turns out to have, or a fill, or a stroke changed after its data has
  begun, is left out and reported. Of the books' longer data, 4 outlines
  in the public books and 413 in the library are stroked and not filled,
  and are drawn; 3 and 95 are both, and are filled without their strokes.
- **Where it goes**: the drawing is fitted to the size it is given, and
  `PlaceOn()` then puts it through a matrix into the strips' pixels: to
  the middle of a page, or turned into the rows of a panel turned in its
  device. An `svg` inside another is cut off at its edges after a quarter
  turn as before it. A picture turned by any quarter is the picture turned,
  within two parts in 255, the gradients and the cutting off included.
- **Size**: a drawing's first tag says how large it is, and `Size()` has
  that as soon as the tag has been read, so whoever shows the drawing can
  choose the picture's size before any of it is drawn.
- **How fine**: curves are cut to a sixteenth of a pixel, not the
  flattener's own quarter, because the edges are gray ("How exact",
  above).
- **Memory**: the renderer is 8,136 bytes on the device. Half of it is
  what each of up to 32 open groups hands on to what is inside it, 124
  bytes a group, of which the style of a stroke is 52; a thirteenth the
  stroking, the stroker and what it draws through; a fifteenth each the
  path data it can hold and the reading of a style sheet and a rule of
  it; a seventeenth the reading of a gradient and the ramp a shape is
  filled with; and a twentieth the four readers of tags, 80 bytes each,
  that read things used one inside another. Nothing is
  allocated, and nothing grows with the file but what it defines, its
  sheet's rules and its gradients, in the memory given for that.
- **Flash**: as compiled for the device, the renderer is 32 KB, the
  stroker 8 KB and the reader of style sheets 3 KB. Laying a gradient out
  is 4.7 KB of the renderer.
- **Thinnest stroke**: `SetThinnestStroke()` says how thin a stroke may be
  drawn, in pixels. One that would come out thinner, across the way its
  shape's matrix makes things smallest, is drawn that wide; the report
  counts it (`widened`). Under a matrix that stretches one way more than
  the other, that widens it the other way more than it needs. With
  nothing asked for, the default, every stroke is as wide as it is, and a
  stroke under a pixel comes out a light gray, which a panel of two or
  four grays shows as a broken line. Drawn 480 pixels wide, as a page of
  the X4 holds one, 190 of the public books' 195 strokes and 7,040 of the
  library's 7,143 are under a pixel.
- **Speed**: not yet measured on a device. What is used is read again for
  every use and every strip, tags and path data both, and that is most of
  what a drawing costs. A stroked outline's curves are cut into lines in
  every strip, where a filled one's are cut only in the strips they
  cross, and each line comes to some ten lines of the stroke: the stroked
  drawing in the books that takes longest, with 2,632 strokes, comes to
  5.8 million lines and 540 ms on the desktop, 480 pixels wide in strips
  of 16 rows, and 25 ms in one strip. Without its strokes it takes 300 ms
  in those strips, and 21 ms in one; and the lines its strokes give the
  filler cost a tenth of the rest.
- **What a call can cost**: `Step()` takes sixteen things from the tags at
  the most, the file's or those of something it kept, and of what it kept
  it reads no more than 1,024 bytes. It gives the outline in hand nineteen
  commands and fills one shape or one stroke, whatever the file holds;
  what those commands come to in lines is bounded
  (`kMostLinesPerStep`), a stroke's dashes included. A shape's stroke is
  drawn in the call after its fill. It looks through
  the records of what is kept twice at the most: to find what a `use` or
  a gradient's `href` names, or to style an element and find the gradient
  it is filled with. A piece of a sheet's text is read in the call it
  comes in.
- **Bad input**: the file is not trusted. Whatever its bytes, it ends in a
  status (`ok`, `not svg`, `malformed`, `truncated`), and what came before
  a fault has been drawn. Groups nested deeper than 32 are passed over
  with what is in them, and reported.

`bn-svg draw` draws a file into a picture, in strips of as many rows as it
is told, and says what that took and what was left out:

```
./build/host/tools/svg/bn-svg draw drawing.svg --width 240 --rows 32 --out drawing.pgm
```

```
a drawing of 60 by 40, as 240 by 160 in 5 strips of 32 rows: 6 elements, 3 shapes filled, 0 stroked, 20 commands, 267 lines in all, 15488 bytes of cells
left out, or not drawn as written: text
```

That is this file, in which a group moves and paints a rounded rectangle
and a circle, a path has its paint in `style`, and there is a label:

```
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 60 40">
  <g fill="#666" transform="translate(4 4)">
    <rect width="24" height="16" rx="4"/>
    <circle cx="40" cy="8" r="8" fill="black"/>
  </g>
  <path d="M6 30h48l-24 8z" style="fill: #999"/>
  <text x="6" y="28">A label</text>
</svg>
```

With `--width` or `--height` the picture keeps the drawing's shape, and
with neither it is the size the drawing says it is. `--pieces` gives the
file to the renderer a few bytes at a time.

A drawing with a style sheet or gradients is told how many of each it
kept. In this one a sheet fills the sky with a gradient and the hill with
a gray, and every `path` with four fifths of its gray:

```
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 60 40">
  <style>
    .sky { fill: url(#dusk) }
    .hill { fill: #555 }
    path { fill-opacity: .8 }
  </style>
  <linearGradient id="dusk" x2="0" y2="1">
    <stop offset="0" stop-color="#222"/>
    <stop offset="1" stop-color="#ddd"/>
  </linearGradient>
  <rect class="sky" width="60" height="40"/>
  <path class="hill" d="M0 40c10-14 22-14 32-4s18-10 28-6v10z"/>
</svg>
```

```
a drawing of 60 by 40, as 240 by 160 in 5 strips of 32 rows: 7 elements, 2 shapes filled, 0 stroked, 10 commands, 162 lines in all, 15488 bytes of cells
style: 3 rules of a style sheet and 1 linear gradients kept
```

All but 57 of its 38,400 pixels are within one gray of what `rsvg-convert`
draws, and the worst, on the edge of the hill, is 7 parts in 255 out.

Strokes are counted beside the shapes filled. Here a river has round ends,
a road is dashed, a roof has round corners, and a wheel is filled and
stroked:

```
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 60 40">
  <path d="M2 30c12-10 22 6 34-4s16-14 22-12" fill="none" stroke="#999"
        stroke-width="3" stroke-linecap="round"/>
  <path d="M4 8h52" stroke="#000" stroke-dasharray="4 2"/>
  <polygon points="10,24 18,16 26,24" fill="#ddd" stroke="#333"
           stroke-width="1.5" stroke-linejoin="round"/>
  <circle cx="46" cy="28" r="5" fill="#fff" stroke="#000" stroke-width="2"/>
</svg>
```

```
a drawing of 60 by 40, as 240 by 160 in 5 strips of 32 rows: 5 elements, 3 shapes filled, 4 stroked, 27 commands, 5921 lines in all, 15488 bytes of cells
```

The road's path data has no inside, and its fill covers nothing. Of the
38,400 pixels, 37,423 are the same as `rsvg-convert`'s, all but 7 are
within 8 parts in 255, and the worst is 11 out.

`--thinnest` says how thin a stroke may be drawn, in pixels, and a line
says how many were drawn wider. Drawn 240 pixels wide, this page's rules,
a tenth of a unit, are 0.4 of a pixel, and its frame 4 pixels:

```
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 60 40">
  <path d="M4 10h52M4 20h52M4 30h52" stroke="#000" stroke-width=".1"/>
  <rect x="20" y="5" width="20" height="30" fill="none" stroke="#000"/>
</svg>
```

```
./build/host/tools/svg/bn-svg draw ruled.svg --width 240 --rows 32 --thinnest 1 --out ruled.pgm
```

```
a drawing of 60 by 40, as 240 by 160 in 5 strips of 32 rows: 3 elements, 1 shapes filled, 2 stroked, 17 commands, 220 lines in all, 15488 bytes of cells
strokes: 1 of 2 drawn wider, as thin as they may be
```

A file that defines things and uses them is also told how much of it was
kept, and how much of that was read again for a strip. This one defines a
leaf, and a sprig of two leaves, draws the sprig twice, and cuts a circle
off at the edges of an `svg`:

```
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 60 40">
  <defs>
    <path id="leaf" d="M0 0c4-6 10-6 14 0c-4 6-10 6-14 0z"/>
    <g id="sprig">
      <use href="#leaf"/>
      <use href="#leaf" transform="translate(0 8) rotate(-20)"/>
    </g>
  </defs>
  <use href="#sprig" x="6" y="10"/>
  <use href="#sprig" x="30" y="10" fill="#999"/>
  <svg x="4" y="26" width="52" height="8">
    <circle cx="26" cy="4" r="30" fill="#ccc"/>
  </svg>
</svg>
```

```
a drawing of 60 by 40, as 240 by 160 in 5 strips of 32 rows: 20 elements, 5 shapes filled, 0 stroked, 22 commands, 734 lines in all, 15488 bytes of cells
definitions: 190 bytes kept of 32768, and 452 gone through again in a strip
```

`--definitions` says how much memory there is for what is kept. With 100
bytes there is room for the leaf and not for the sprig, so neither use of
the sprig is drawn:

```
a drawing of 60 by 40, as 240 by 160 in 5 strips of 32 rows: 10 elements, 1 shapes filled, 0 stroked, 6 commands, 260 lines in all, 15488 bytes of cells
definitions: 92 bytes kept of 100, and 0 gone through again in a strip
left out, or not drawn as written: use of what is not defined, definitions there was no room for
```

Every SVG file in the public books, 2,420 of them, ends in `ok`, and 2,381
are drawn in full, two of them with an arc of enormous radius drawn as the
line it nearly is. 2,273 of those are the formulas of one book, which
place their letters with `use`. 31 files have a style sheet, with up to
37 rules, and 3 have linear gradients, and every rule and gradient in
them is read; but each of those files also has something that is not
drawn, so none of them is drawn in full for it. Six files have strokes,
189 of them, and two of those are drawn in full. Of the 39 not drawn in
full, 20 are covers, each a picture with lettering over it. Of the other
19, 10 have text, 5 a picture, 3 a stroke that is not drawn (on a long
outline that is also filled, or painted with a gradient), 3 a clip path,
2 markers and 1 a filter. In the library, which has
strokes in 14 of its 40 drawings, 7,239 of them, 34 drawings are drawn in
full; 95 of the strokes are on long outlines that are also filled.

The 2,381 were also drawn beside `rsvg-convert`: at their own size, or,
for the 2,245 that say they are more than 700 pixels wide, 700 wide, with
that size written into a copy of the file so that both programs fit the
drawing to it by the same rule. The worst pixel of a file is 20 parts in
255 from `rsvg-convert`'s at the median and under 27 for nine files in
ten, which is about as far as `rsvg-convert` is from the same drawing
drawn larger and averaged down. Three files have pixels more than 48 parts
out. One has the arc, and two pixels. One formula has a single pixel, at
the tip of the hollow of a letter, where the letter's outline overlaps
itself within the pixel (the filler's limit, above). And one formula says
it is 636.3 pixels by 851.8, which the two programs make a picture of a
quarter of a pixel apart: given a whole number of pixels, its worst pixel
is 25. The two with strokes are within 25 at their worst pixel. In the
library, the drawings with strokes that are drawn in full are within 48 at
every pixel but two, in one small drawing: at the inner corners where a
stroke down and a stroke across, parts of one outline, overlap, the
pixel their edges cross is black where `rsvg-convert` has 68 and 51 (the
filler's limit, above).

The unit tests draw files small enough that every pixel can be said by
hand, or set beside another drawing that has to come out the same: a
circle beside the four arcs that make one, a path beside the same data
given straight to the flattener and the filler. They give the reader of
tags and the renderer 26,000 files of the bytes that mean something to
them put together at random, and hold each call to its bound. What is
used is set beside the same thing written where it is used, which has to
come out the same byte for byte. The memory for definitions is cut down to
a byte less than a file needs, and to less, and a file of five kilobytes
that asks for tens of megabytes to be read again is stopped at the limit.
An end-to-end test (`tests/e2e/svg_draw.py`) makes drawings up at random,
with groups inside groups, every shape, transforms, paint as attributes,
as `style` and by the rules of a style sheet, things hidden, things
defined and used, `svg` elements that cut off what is in them, linear
gradients of every kind, and strokes of many widths with round corners
and ends, and writes each out as a file in one of the many ways it can be
written. The picture it should make is worked out in Python from the
drawing and not from the file: matrices multiplied out, outlines traced
finely, what is used drawn where it is used, the rule that wins for each
shape, a gradient's gray at the middle of each pixel found by taking it
back through the matrices into the gradient's own space, a stroke as
every point no further from the outline than half its width in the
shape's own space, and each shape laid over the last. A unit test does the same
for one gradient under a slanting, stretching transform. `bn-svg`
has to agree with it, in any strips and any pieces, and to say it left out
what the drawing was given that it does not draw. The reader of tags, the
readers of what attributes say, path data and the renderer each have a
fuzz target ("Fuzzing", above), and the renderer's target places its picture by each
of the four turns. Another end-to-end test
(`tests/e2e/drawing_viewer.py`) runs the desktop application over a folder
of drawings and pictures (a PNG, a JPEG and a progressive JPEG) and goes
from one to the next and round, and sets each picture the simulated panel
shows beside `bn-svg`'s drawing of the same file, or `bn-png`'s or
`bn-jpeg`'s reading of it, at the size it should be on the page, turned
onto the panel: in blocks of 16
by 16 pixels, each as dark as it should be to within a fifth, and all of
them to within a hundredth on the whole.

### Keeping a drawing

A drawing read again for each strip costs its whole file each time: on
the X4, the largest drawing in the public books, 3.5 MB, took 354 s in 30
bands, about 5 s of reading the card and 5 s of rendering a pass. So a
drawing can be read once and kept, and each strip drawn from what was
kept. Kept so, with the loop pausing only when there is nothing to do
("The watchdog", below), what is kept written 8 KB at a time, and the file
read 8 KB at a time, it takes 17 s, from the press to the refresh:

| On the X4, 2026-11-17 | Read each band | Kept | The loop not pausing | Written 8 KB at a time | Read 8 KB at a time |
|---|---|---|---|---|---|
| The largest engraving, 3.5 MB | 354 s | 31.1 s | 25.6 to 27.2 s | 20.1 s | 17.4 s |
| An engraving of 1.8 MB | | 17.2 s | 14.4 to 14.9 s | 11.4 s | 10.0 s |
| An engraving of 17 shapes, 932 KB | | 11.2 s | 8.3 to 8.5 s | 6.5 s | 5.7 s |
| Drawings of 4 to 52 KB: of strokes, a formula, the samples below | | 1.0 to 1.8 s | 0.6 to 1.2 s | 0.6 to 1.1 s | 0.5 to 1.0 s |

Of the 17 s: reading the file once, 3.3 s (1.0 MB/s, against 0.66 MB/s
read a kilobyte at a time); rendering into what is kept, 7.0 s; writing
what is kept, 1.85 MB in 232 writes, 2.2 s (0.84 MB/s, against 0.25 MB/s
written a chunk of 512 bytes at a time); reading it back, a chunk at a
time, 2.7 s; drawing the bands from it, 1.2 s; dithering, 0.3 s; waiting
for the panel, 0.5 s; and 0.03 s between the viewer's turns. The refresh
after them takes 3.7 s, or 1.7 s for a half refresh.

The X4's card reads faster the more it is asked for at once, 2 MB read
straight through: 0.73 MB/s at 512 bytes a read, 0.89 MB/s at a
kilobyte, 1.38 MB/s at 4 KB, 1.52 MB/s at 8 KB and 1.64 MB/s at 32 KB.
Reads of 512 bytes scattered over the file came to the same rate as reads
of 512 bytes one after another: what costs is how many reads there are.
While a drawing is kept, the file's reads take turns with the writes of
what is kept, and come to less than these rates.

What a renderer draws into is a `Canvas` (`draw/fill.h`): the lines of
each shape's outline, in pixels, and then the shape's filling. A `Filler`
is one, and draws into its strip as the lines come. `SpoolWriter`
(`draw/spool.h`) is another, which keeps them:

- **What it keeps**: each line placed as a filler places it, at a 256th of
  a pixel, with the line that closes an outline left open; and each line
  for every band of the picture whose rows it crosses, since a filler does
  nothing with a line that lies wholly above or below its strip. Each
  filling is kept for every band that has lines it uses up. A band's lines
  and fillings, given back in the order they came to a filler set to that
  band (`BeginBand()`, `ReplayStep()`), draw it byte for byte as drawing
  into it would.
- **Where**: each band has a chunk of 512 bytes in hand, in memory the
  caller gives; a full chunk is added to a store, the caller's
  (`SpoolStore`: a file on a card, or memory), and a table of links, two
  bytes for each chunk the store may hold, chains each band's chunks in
  order. A line that goes on from where the band's last ended takes 5
  bytes, or 9 if it is long; one that does not, 17; a filling 4 to 94.
- **What it comes to**: drawn 480 pixels wide in bands of 16 rows, the
  3.5 MB engraving keeps 315,112 lines in 1.85 MB, the 1.8 MB one 1.1 MB,
  and the Alice engraving 0.6 MB; the drawings of shapes and strokes keep
  tens of kilobytes. About 5.9 bytes a line.
- **What a call costs**: `ReplayStep()` reads one chunk at the most, and
  fills once at the most.
- **Bad input**: the links given can be too few (`kNoRoom`), the store can
  fail (`kStoreFailed`), and what is read back can be damaged
  (`kBadRecord`); each is a status, and a damaged chunk is never read past
  its end. The object is about a kilobyte.

`bn-svg draw --spool` reads the file once into a recording in memory, and
draws each strip from it:

```
./build/host/tools/svg/bn-svg draw drawing.svg --width 240 --rows 32 --spool --out drawing.pgm
```

```
spool: ok, 5 chunks of 512 bytes, 140 lines and 8 fillings kept
a drawing of 60 by 40, as 240 by 160 in 5 strips of 32 rows: 6 elements, 3 shapes filled, 0 stroked, 20 commands, 138 lines in all, 15488 bytes of cells
left out, or not drawn as written: text
```

The unit tests set drawings kept and given back in bands of 1, 7 and 16
rows and of the whole picture beside the same drawn straight into those
bands, byte for byte: fillings of every kind, boxes, gradients, strokes and
dashes, what lies beyond the picture, what is used again, and drawings made
at random; and give back a recording damaged at random. The renderer's
fuzz target does the same for every file it is given. The nine drawings
timed on the X4 come out the same, byte for byte, with and without
`--spool`.

### Keeping what is shown

A drawing made once can be shown again without being made again:
`draw::PictureCache` (`draw/picture_cache.h`) keeps it on the card, as it
was shown. A picture read and scaled is kept the same way.

- **What is kept**: the picture at the size it is shown and in the
  panel's levels, a pixel's level packed into 1, 2, 4 or 8 bits, row after
  row. It is found by a key: the path of the file it was made from, the
  file's size and when it was last written, its own width and height, the
  levels, and how it lies on the panel. A file changed since is not the
  same key.
- **Where**: a folder of 64 files, each holding one picture, so the cache
  never holds more than 64. The key chooses a picture's home file, and it
  may be kept in any of the 8 files from there on. Of those it takes the
  place of what was kept before as the same picture, else of the first
  that holds nothing whole, else of the picture kept longest ago. Looking
  for a picture, or for a place to keep one, stops at the first of its
  places that has no file, since nothing is kept past one.
- **Each file**: a header, which says what the picture is and when it
  was kept, and carries a checksum of itself and one of the rows; then
  the rows. The header is a sector of the card, 512 bytes, of which it
  uses 44. The
  rows are written first and the header last, so a file left half written
  by a press, or by the power going, has no header. Files are not synced:
  what the card had not written when the power went shows in the rows'
  checksum, which is checked as the last row is read.
- **Whole sectors**: a card is read and written in sectors, and a write
  that begins or ends part way through one costs it a read of that sector
  and a write of all of it. So the rows are gathered, in memory the
  caller gives (`GatherIn()`; the viewer gives 4 KB), and go to the card
  as many whole sectors at a time as that holds, each write begun where a
  sector begins; only the last write of a picture's rows ends part way
  through one. They are read back the same way. The X4's driver then
  moves every sector straight between the card and that memory. Given a
  band of rows at a time, it had to go by way of a block of its own from
  the heap, of up to 3 KB, for 84 of the reads and 28 of the writes that
  showing the card's 29 pictures took.
- **Bad input**: whatever a file holds, reading it ends in a status:
  `kMissing`, `kOther` (another picture, or this one before its file
  changed), `kDamaged`, `kStoreFailed`; and no row is given that is not
  whole. The viewer draws again what it finds damaged, and keeps it again.

On the X4's panel held as it is read, the viewer keeps a drawing as it
lies on the panel, so a page's 480 by 800 is 800 by 480 in the cache,
47 KB at a bit a pixel, and a drawing narrower than the page keeps only
its own part. It keeps a picture as it lies on the page, its rows in the
order they come from the file, which is the order its strips are written
in again: so the key of a picture says it is not turned, whichever way
the panel is.

With one file for each picture, chosen by its key, two pictures were
likely to choose the same one: on the X4, going round the 18 drawings and
pictures of the card set twice, two pairs did, so four of the 18 were made
again the second time, each having taken the place of the other. In sets
of two files, three pictures shared one, and each of them took the place
of the next. A simulation of tours of files chosen at random puts it in
numbers, as the share of tours in which something is made again the
second time:

| Files toured | One file each | Sets of two | 8 files from a home |
|---|---|---|---|
| 18 | 93% | 47% | 0% |
| 30 | 100% | 95% | 4% |
| 45 | 100% | 100% | 56% |

Looking at a file costs about 3.4 ms on the X4's card, so looking at all 8
places, as when the picture is not there and they are all full, costs
about 27 ms. On the X4, going round the card set a second time, each of
the 29 came from the cache, in 75 to 325 ms from the press to the
refresh, which takes about 1.6 s after it: 11 to 91 KB, read in 21 to
127 ms. The largest engraving, 17.9 s and a full refresh of 3.6 s the
first time, came back in 1.7 s; the colour photograph, 11.5 s and 1.6 s
the first time, in 1.9 s. Keeping each took 114 to 289 ms: 268 KB a
second over the 29, and 705 KB a second reading them back. Written and
read a band of rows at a time, wherever in a sector a band began and
ended, the same 29 were kept at 206 KB a second and read back at 619.
The card has its own pauses, which neither way is spared: one write of
4 KB in a later round took 160 ms, and a picture of 42 KB 1.2 s to keep.

The unit tests keep pictures in every number of bits and read them back in
pieces, look for them with each part of the key changed, fill more files
than there are, keep as many pictures with one home as it has places and
then more, keep one where a file holds nothing whole, and damage the files
in each place a byte can be damaged.

## Pictures

`PngReader` (`draw/png.h`) reads a PNG picture. It is the first of the
pieces that will turn a book's pictures into what a panel shows: a reader
of each format gives rows of 8-bit gray, the scaler (below) makes them the
size the picture is shown at, and the ditherer (`draw/dither.h`) brings
them down to the grays a panel has. `JpegReader` (`draw/jpeg.h`, below) is
the second. The drawing viewer shows the PNG and JPEG files on its card
this way ("The panel and the buttons", above). GIF is not read yet. The pieces hand rows on to one another through one interface,
`RowSink` (`draw/row_sink.h`).

A picture is read from a `ByteSource`, such as an entry of the ZIP file, a
piece at a time, and never held whole: first its header, then its rows,
top first, each given to a `RowSink` as soon as it is whole. All it keeps
of a picture is the row being read and the one above it, which PNG's
filters refer back to.

- **What it reads**: every kind of picture PNG has. Gray at 1, 2, 4, 8 and
  16 bits a sample; colour (RGB), gray with alpha and colour with alpha
  (RGBA) at 8 and 16; colours from a palette at 1, 2, 4 and 8.
  Transparency by an alpha sample, by a palette's alphas, or by one gray
  or colour that stands for none (`tRNS`).
- **What it gives**: a byte a pixel, from black (0) to white (255), as the
  picture looks on white paper. A colour is the gray it looks, by the same
  rule as in a drawing (`draw/gray.h`: 0.299 of its red, 0.587 of its green
  and 0.114 of its blue), and a pixel that is not wholly solid is that much
  of its gray laid over white. A sample of 16 bits is taken by its high
  byte, and one of fewer than 8 is stretched so that its largest value is
  white. A palette index past the end of the palette is black.
- **What it leaves out**: interlaced pictures, whose rows are not whole
  until the last of seven passes over the picture: the header is read, and
  the rows refused (`kInterlaced`). Anything about colour itself (`gAMA`,
  `sRGB`, `iCCP`, `cHRM`), since what comes out is gray for paper; the
  colour a picture asks to be put behind it (`bKGD`), since it is laid on
  the page; text; and every other chunk a reader may pass over. An
  animated picture is read as its first picture, which is what its `IDAT`
  chunks hold. In the books looked at, 2,823 PNG files in all, none is
  interlaced and none is animated.
- **Bad input**: whatever bytes it is given, it ends in a status:
  `kNotPng`, `kBadHeader`, `kBadChecksum`, `kBadChunk`, `kUnknownChunk`,
  `kNoPalette`, `kBadData`, `kTruncated`, `kNoRoom` or `kSourceFailed`. Every
  chunk's checksum is checked. A wrong one ends the reading if the picture
  needs the chunk (`IHDR`, `PLTE`, `IDAT`), and the chunk is passed over,
  and counted, if not. A chunk's checksum comes after its bytes, so the rows
  made from a chunk of data have been given by the time its checksum is
  found wrong. The checksum zlib puts at the end of the data is not
  checked: the inflater has read it by then, as it reads ahead, and its
  bytes have all been through a chunk's checksum.
- **Memory**: the inflater's 32 KB window and room for two rows, both the
  caller's. A row needs the longer of its bytes, with the filter byte in
  front of them, and its gray, which is a byte a pixel
  (`PngReader::RowBytesFor()`). The rows of the widest picture in the
  library need 10,202 bytes; in the public books 29,426; 16 KB holds all
  but 73 of the public books' 2,435. The object itself is 3,048 bytes on
  the device, most of it the inflater. Reading only the header needs no
  window and no rows, and reads 33 bytes of the file.
- **One call's work**: `Step()` reads PNG's signature, or the start of a
  chunk, or up to 1 KB of one; or it asks the inflater once for up to
  1 KB of a row, and if that finishes the row, undoes its filter, turns
  it to gray and gives it out. What the inflater takes is bounded by its
  own rule. Between the bytes it takes lie the ends and starts of chunks:
  a picture's data may be cut into chunks of any length, down to a byte,
  and two empty ones in a row are passed over (no encoder writes one, and
  none of the books' pictures has one; three in a row are refused as bad
  data). So the most a call takes, whatever the file, is 284,196 bytes, for
  data a byte to a chunk with two empty chunks after each
  (`PngReader::MostInputPerStep()`). Encoders write chunks of 8 KB or more
  but the last, and a call then takes about what the inflater does.

`bn-png` (`tools/png`) runs the reader on a file, or on every PNG in an
EPUB:

```
./build/host/tools/png/bn-png read picture.png --out picture.pgm
./build/host/tools/png/bn-png read picture.png --width 480 --out small.pgm
./build/host/tools/png/bn-png header picture.png
./build/host/tools/png/bn-png book book.epub
```

`read` prints how the reading ended, the CRC-32 of the rows of gray it
gave, what the header says (width, height, kind, bits a sample), how many
rows it gave, the memory its rows need, how many chunks it passed over, and
how many of those had a wrong checksum; with `--out`, it writes the rows as
a PGM file to be looked at. `--rows` gives it less memory for rows, and
`--pieces` the file in smaller pieces. `--width` and `--height` put the
rows through the scaler, as the firmware will to show the picture at
another size; with one of them, the other keeps the picture's shape.
`header` reads no more than the header, as a book's importer would. For
the logo of a book from Standard Ebooks:

```
ok 0dd98f59 342 218 gray+alpha 8 218 1370 0 0
ok 342 218 gray+alpha 8 0 33
```

and with a kilobyte for its rows:

```
no-room 00000000 342 218 gray+alpha 8 0 1370 0 0
```

and made the panel's width, when the checksum and the rows are those of the
new picture:

```
ok 63611f11 342 218 gray+alpha 8 306 1370 0 0 scaled 480 306
```

`book` prints the same line for every PNG in the book, with its name, and
adds them up. For *Pride and Prejudice*, in `corpus/`:

```
ok 0dd98f59 342 218 gray+alpha 8 218 1370 0 0 epub/images/logo-2x.png
ok 3cffcd3b 220 140 gray+alpha 8 140 882 0 0 epub/images/logo.png
ok 4e9b3d89 2800 880 gray+alpha 8 880 11202 0 0 epub/images/titlepage-2x.png
ok c2934a08 1400 440 gray+alpha 8 440 5602 0 0 epub/images/titlepage.png
png_files=4 png_failed=0 png_bytes=97941 png_pixels=3185356 png_most_room=11202
```

The unit tests make pictures with zlib, of every kind and depth with every
filter, and set what the reader gives beside the gray each pixel should
come to, worked out a pixel at a time from its samples
(`tests/support/png_builder.h`); then spoil them in each way the reader
looks for, and hold every step to its bound, among them a picture whose
data is a byte to a chunk. An end-to-end test (`tests/e2e/png_tool.py`)
compares `bn-png` with a second reading of the same files in Python, with
Python's zlib: 400 pictures made up at random, every line the same, and
600 spoiled. Through `tools/check-books.py` the two agree on every PNG of
the books they were tried on: 80 in `corpus/`, 308 in the library and
2,435 in the public books, 1.7 billion pixels in all. The reader has a
fuzz target ("Fuzzing", above).

The tests were set against 24 faults put into the reader on purpose, one
at a time: in each filter, each kind of picture, transparency, the
checksums, the order of chunks, the bound on a step and the room for
rows. The unit tests found 23. Two of those they found only once a test
was written for them, after the fuzz target's inputs had found them first:
data that went on into the chunk after its last, and a signature checked
by its first two bytes. The 24th changes nothing: which of two bytes
Paeth's filter takes when they are equally near, which only happens when
they are the same.

### JPEG

`JpegReader` (`draw/jpeg.h`) reads a JPEG picture in the same way: from a
`ByteSource`, a piece at a time, its header first and then its rows, top
first, each given to a `RowSink`. What it keeps of a picture is one row of
its blocks: the 8 or 16 rows of pixels that are made at once.

- **What it reads**: baseline and extended pictures coded with Huffman
  codes, 8 bits a sample, gray (one component) or colour as YCbCr (three),
  sampled in any way the standard allows, in one scan or several; restart
  markers; and progressive pictures by their first scan (below).
- **What it gives**: a byte a pixel, from black (0) to white (255). A
  JPEG's gray is its first component, Y, which is 0.299 of red, 0.587 of
  green and 0.114 of blue, the rule `draw/gray.h` has for every picture. So
  only Y is turned back from frequencies into pixels: the colour is decoded
  only as far as is needed to find where Y's next block begins.
- **Smaller as it is read**: at a half, a quarter or an eighth of its size
  each way (`JpegReduction`), for less work and less memory. Each pixel is
  then the average of those it stands for in the block at its whole size,
  worked out from the block's frequencies in one transform; at an eighth
  that is the block's average alone. `ReductionFor()` gives the most a
  picture can be made smaller as it is read and still be as large as it is
  shown, and the scaler does the rest. Shown on the X4's page, 480 by 800,
  the rows of every JPEG in the library and the public books fit in 16 KB
  (15,360 bytes at the most); at their whole size the widest need 71,424.
- **Progressive pictures**: a progressive picture is coded in scans over
  the whole of it, each adding to every block, and none of its pixels is
  whole until the last; keeping every block's coefficients between scans
  would take about two bytes a pixel. Its first scan holds each block's
  average, which is the picture at an eighth of its size. So a progressive
  picture is read at an eighth, whatever is asked, from that scan alone,
  and made larger by the scaler: soft. The low bit of an average that a
  later scan would add is taken as nothing. libjpeg's `jpegtran` rewrites
  a progressive file as baseline without changing a coefficient. Of the
  608 progressive JPEGs in the library and the public books, every one has
  such a first scan; only 27 are covers.
- **What it leaves out**: arithmetic coding, lossless and hierarchical
  JPEG, samples of 12 or 16 bits, four components (CMYK), colour coded as
  RGB, a picture whose Y is sampled less than its colour, and one whose
  height comes after its first scan. Each has its header read and is
  refused (`kUnsupported`). The orientation an Exif block gives is not
  looked at. Of the 10,008 JPEGs in the library and the public books, the
  two that are CMYK are the only ones refused, and every Exif orientation
  is upright.
- **Bad input**: whatever bytes it is given, it ends in a status:
  `kNotJpeg`, `kBadMarker`, `kBadHeader`, `kBadTable`, `kBadScan`,
  `kUnsupported`, `kBadData`, `kTruncated`, `kNoRoom` or `kSourceFailed`.
  Bytes between segments that are not a marker are passed over and
  counted, as other readers do. Inside a scan, a code that no table has, a
  coefficient past a block's end, or a restart marker other than the one
  due is bad data; coded data that runs into the end of the file, or into
  the picture's end marker, is cut short. The rows of the blocks finished
  before either have been given.
- **Memory**: room for one row of blocks, the caller's
  (`JpegReader::RowBytesFor()`), and the object itself, which holds eight
  tables of Huffman codes, four of quantization and a block's numbers:
  9,052 bytes on the device. Reading only the header needs no room;
  the source is read 512 bytes at a time, and a header is in the first 512
  bytes unless segments of metadata come before it.
- **One call's work**: `Step()` reads JPEG's first marker; or passes over
  up to 1 KB between segments, finding a marker; or reads one table, a
  frame or scan header, or up to 1 KB of a segment that is passed over; or
  passes over up to 1 KB of a scan that is not read; or decodes up to eight
  of a scan's units of blocks and turns Y's blocks among them into pixels;
  or gives one row. So the most a call takes, whatever the file, is 34,096
  bytes: eight units of ten blocks, each of whose coefficients has the
  longest code and the most bits there can be, every byte of it 0xFF and
  so followed by a 0, and what is read ahead
  (`JpegReader::MostInputPerStep()`).

`bn-jpeg` (`tools/jpeg`) runs the reader on a file, or on every JPEG in an
EPUB:

```
./build/host/tools/jpeg/bn-jpeg read picture.jpg --out picture.pgm
./build/host/tools/jpeg/bn-jpeg read picture.jpg --width 480 --out small.pgm
./build/host/tools/jpeg/bn-jpeg read picture.jpg --reduce 8
./build/host/tools/jpeg/bn-jpeg header picture.jpg
./build/host/tools/jpeg/bn-jpeg book book.epub
```

`read` prints how the reading ended, the CRC-32 of the rows of gray it
gave, what the header says (width, height, kind), how many times smaller
it was read, how many rows it gave, the memory its rows need, and how many
segments and scans it passed over and bytes between segments that were not
a marker; with `--out`, it writes the rows as a PGM file to be looked at.
`--reduce` reads it smaller; `--width` and `--height` put the rows through
the scaler, read at the reduction the firmware would take for that size.
`--rows` and `--pieces` are as `bn-png`'s. For the cover of *Pride and
Prejudice* from Standard Ebooks, in `corpus/`:

```
ok 34521b35 1400 2100 4:2:0 1 2100 22528 1 0 0
ok 1400 2100 4:2:0 512
```

and made the panel's width, read at a half on the way:

```
ok 1194a0f2 1400 2100 4:2:0 2 720 5632 1 0 0 scaled 480 720
```

and the same cover made progressive by `jpegtran`, read at an eighth from
its first scan:

```
ok 0a856cdf 1400 2100 progressive-4:2:0 8 720 352 1 0 0 scaled 480 720
```

`book` prints the same line for every JPEG in the book, with its name, and
adds them up. For *Moby Dick* from the IDPF's samples, in `corpus/`:

```
ok 65840f70 646 902 4:4:4 1 902 5184 5 0 0 OPS/images/9780316000000.jpg
ok 64de40f2 840 1380 gray 1 1380 6720 4 0 0 OPS/images/Moby-Dick_FE_title_page.jpg
jpeg_files=2 jpeg_failed=0 jpeg_bytes=453855 jpeg_pixels=1741892 jpeg_most_room=6720
```

The unit tests make pictures from the coefficients of their blocks, of
every kind the reader reads, and set what it gives beside what those
coefficients come to in real numbers: T.81's inverse transform a pixel at
a time, and at a half and a quarter the average of the whole block's
pixels (`tests/support/jpeg_builder.h`); within a gray, and at an eighth
exactly. Then pictures of each kind it refuses, and pictures spoiled in
each way it looks for, every step held to its bound. An end-to-end test
(`tests/e2e/jpeg_tool.py`) compares `bn-jpeg` with a second reading of the
same files in Python, of pictures made up at random by an encoder of its
own: 160 of every kind the reader reads, each the same line for line and
byte for byte at an eighth and within a gray at the other sizes; 12 of the
kinds it refuses; and 300 spoiled. Through `tools/check-books.py` the two
agree at an eighth, byte for byte, on every JPEG of the books they were
tried on: 45 in `corpus/`, 9,097 in the library and 911 in the public
books, 6.4 billion pixels in all.
Set beside libjpeg (`djpeg -grayscale -dct float`, and `-scale` for each
reduction) on 400 JPEGs of the library and the public books, 100 of them
progressive, it is within a gray at every size, and the same at an eighth;
a progressive picture's first scan is within two grays of libjpeg's
eighth, for the bit a later scan would add. The reader has a fuzz target
("Fuzzing", above).

The tests were set against 18 faults put into the reader on purpose, one
at a time: in the transform and its tables, restarts, a progressive
picture's first scan, the sides of a picture read smaller, the tables of
codes, what is counted and passed over, the kinds refused, how a unit's
blocks are placed, and the units decoded a step. The unit tests, the fuzz
target's inputs and the end-to-end test found 15. A 16th, a step that
decoded a unit more than it says, they found only once a test was written
for it: the bound on what a step takes is for the worst a file can be, far
past what any picture is. The other two move pixels by less than a gray,
which a comparison of 8-bit pixels cannot tell from rounding: the first
direction of the transform not rounded, and one number of the table for a
half 10 in 4,096 off.

### Scaling

`Scaler` (`draw/scale.h`) makes a picture larger or smaller a row at a time.
It is a `RowSink`, so a reader of pictures gives it its rows as it reads
them, and it gives the rows of the picture at the new size to another
`RowSink`. It keeps nothing of the picture but what the rows still to come
need: two rows of the new width at the most.

- **Smaller**: each new pixel is the average of the old pixels it covers,
  each counted by how much of it is covered. No old pixel is left out, so a
  line one pixel wide in a large picture is still there in a small one,
  lighter for being spread.
- **Larger**: each new pixel is a blend of the two old pixels nearest its
  middle, by how near each is (bilinear). Past the middle of the first or
  last old pixel, that pixel is used as it is.
- **Each direction by itself**, so a picture can be made narrower and
  taller at once; the same size is the picture as it is.
- **The grays as they are stored** are averaged, not the light they stand
  for: a picture's grays are already spaced so that their steps look about
  even, and averaging them as they are keeps the thin dark lines of
  diagrams and formulas darker.
- **Whole numbers**, rounded three times, each to the nearest with a half
  up: a blend's weight to 4096ths of the way, a row made the new width to
  256ths of a gray, and each new pixel to a gray. Every new pixel is within
  a gray of the same working-out in real numbers. The sums stay inside 32
  bits for sides of up to 65,535.
- **No division for each pixel**: each number that every pixel of a row
  is divided by (the old width, the units of a new row, twice the new size
  for a blend's weight) is made once into a multiplier and two shifts
  (`draw/divide.h`), by Torbjörn Granlund and Peter L. Montgomery's method,
  which gives the quotient division gives for every 32-bit number. The old
  row is walked once, by a loop for each way of scaling, with nothing
  decided again for each pixel. On the X4 that made scaling twice as fast,
  every pixel coming out as before.
- **Memory**: the caller's, nine bytes for each pixel of the new width
  (4,320 for the panel's 480).
- **One call's work**: `Row()` goes over the old row once and the new width
  once, and gives nothing out; `Step()` makes one new row and gives it out.
  One old row can be needed by several new ones when a picture is made
  taller, so all the new rows an old row has made ready are given out
  before the next old row is taken; one given before then stops the
  scaling (`kOutOfTurn`). After each step of the reader, the caller calls
  `Step()` until it gives nothing.

Its unit tests set it beside a working-out of the same picture in real
numbers, at 300 sizes made up at random and at the sizes a book's pictures
meet, and hold it within a gray of it; and beside its three roundings
worked out from exact fractions, at 400 more, where every pixel has to be
the same. A picture of one gray stays that gray at any size, the same size
is the picture byte for byte, and the largest sides keep their sums inside
32 bits. The end-to-end test scales 82 of its pictures with `bn-png` and
holds every pixel within a gray of a working-out in Python. The scaler has
a fuzz target ("Fuzzing", above). The divisions' multipliers are held to
division itself: for every divisor up to 70,000 at the numbers where a
quotient changes, for every number up to 2^22 and a step through the rest
by eight divisors, for every power of two and the numbers either side of
it, and for a million divisions at random.

Set against 21 faults put into it on purpose, one at a time, the unit tests
found 19. Three of those, a rounding left out on the way, moved pixels by
less than a 256th of a gray, which the comparison with real numbers cannot
see; the comparison with exact fractions was written for them. The other two
change no pixel: a step between old pixels taken one blend later, with its
weight then the whole way, and a test at the first old pixel that picks the
same pixel either way.

## Device build

The firmware builds with ESP-IDF. A setup script installs the pinned version
and its compiler into `.esp/` inside this repository (a download of a few
gigabytes, ignored by git); nothing is installed elsewhere.

```
tools/setup-idf.sh          # once
tools/idf build
tools/idf flash monitor
```

`tools/idf` runs `idf.py` on `app/firmware` with that local install, and
passes its arguments through.

The build also needs the computer's own C and C++ compilers, the ones the
host build uses. The table of American English's hyphenation patterns is
made as the firmware is built, by `bn-hyphen`, which is a program for the
computer and not for the device: the firmware build compiles it with the
host build's CMake files, in a build of its own under
`app/firmware/build/host-tools` that makes nothing else. It takes the
patterns the firmware build downloaded, and downloads nothing itself.

The firmware is compiled for speed (`-O2`), with ESP-IDF's assertions left
on: `app/firmware/sdkconfig.defaults` sets both. Every figure in this
document that was measured on a device is from such a build, and the
level matters. On an X4 reading the same 34 books, a firmware at ESP-IDF's
own default (`-Og`, for debugging) inflated at two thirds of the speed
and parsed XML an eighth slower; one compiled for size (`-Os`) was hardly
faster than that, and took three and a half times as long to draw a test
pattern. The `-O2` firmware is 22 KB larger than the first and 74 KB
larger than the second, of the megabyte it has.

Three more settings there leave some of ESP-IDF's own code in flash that
it would otherwise copy into RAM, and on this chip RAM that holds code is
RAM the heap does not have. The driver for the chip's flash is taken from
the chip's ROM; the ring buffer's functions for interrupt handlers and the
C library's locks stay in flash. ESP-IDF keeps such code in RAM so that it
can run while the chip's flash is being written, which this firmware never
does. On an X4 they leave the heap 13 KB more (298 KB free with the test
pattern showing, against 285 KB), and the largest block that can be taken
16 KB larger. Reading the same 34 books took as long with them as without
(108.5 s and 108.6 s), and so did the card's own check and writing to the
panel; getting a picture out of the cache and ready to show took 2% longer
(207 ms against 204 ms). A fourth setting, which moves the heap's own
functions, would leave 17 KB more again; but ESP-IDF then moves the SPI
driver's interrupt handler out of RAM with them, and the card's driver
uses that for every block of data. With it the card read 6% slower and
the 34 books took 4% longer, so it is left as it was.

ESP-IDF keeps the settings a build was first made with in
`app/firmware/sdkconfig`, which it writes itself. After a change to
`sdkconfig.defaults`, delete that file and build again, or the change is
not taken up.

Flashing replaces whatever firmware is on the device, including its
bootloader and partition table.

### A partition for fonts

The firmware's fonts are to live in a partition of the flash of their own,
read where they lie through the processor's mapping of flash into its
address space. A firmware built with `-DBN_PARTITION_CHECK=ON` checks that
on a device. It has a partition table of its own
(`app/firmware/partitions_fonts.csv`: a 4 MB app partition and a 12 MB
data partition, `fonts`), which `app/firmware/sdkconfig.partition_check`
chooses, so it is built in a directory of its own, where its settings are
kept apart from the normal build's:

```
tools/idf -B app/firmware/build-partition-check -DBN_PARTITION_CHECK=ON build flash
./build/host-release/tools/trace/bn-trace --seconds 20 | tee partition.log
tools/idf flash      # back to the normal firmware and ESP-IDF's own table
```

Flashing it writes the table and fills `fonts` with 12 MB of a pattern in
which every four bytes are a number worked out from where they are
(`app/firmware/partition_pattern.py`). At start, before the application,
it says how large the flash chip is, what partitions there are, how much
of `fonts` can be mapped at once, how long mapping a window takes, how
fast the whole partition is read and whether every word of it is right,
and what a read of 64 bytes costs where the cache has it and where it does
not (`app/firmware/main/partition_check.h`). On an X4:

```
partition check: flash: the chip says 16384 KB (ESP_OK); the build takes it to be 16384 KB (ESP_OK).
partition check: partition nvs: type 1, subtype 0x02, at 0x009000, 20 KB.
partition check: partition phy_init: type 1, subtype 0x01, at 0x00e000, 4 KB.
partition check: partition factory: type 0, subtype 0x00, at 0x010000, 4032 KB.
partition check: partition fonts: type 1, subtype 0x40, at 0x400000, 12288 KB.
partition check: pages of address space free for data: 123 (7872 KB).
partition check: the whole partition at once, 12288 KB: ESP_ERR_NO_MEM.
partition check: the most mapped at once: 123 pages (7872 KB).
partition check: a window of 64 KB: mapped in 16 us and unmapped in 4 us, on average of 100 (0 failed).
partition check: a window of 1024 KB: mapped in 79 us and unmapped in 7 us, on average of 100 (0 failed).
partition check: read all 12288 KB in 824 ms (14902 KB/s), through windows of 1024 KB.
partition check: every word is what it should be (3145728 words).
partition check: scattered over 4096 KB: 20000 reads of 64 bytes in 87542 us (4377 ns a read).
partition check: within 8 KB: 20000 reads of 64 bytes in 17346 us (867 ns a read).
partition check: passed.
```

The processor has 8 MB of address space for flash, in 128 pages of 64 KB,
shared by the app's code and constant data and whatever else is mapped:
beside an app of 280 KB, 123 pages were free, so a partition larger than
that is mapped a part at a time. Mapping a window of 1 MB takes five times
as long as one of 64 KB, and is still under a tenth of a millisecond. The
mapping is done by the chip's ROM, where the firmware takes its driver for
the flash from (above). Done by ESP-IDF's own driver it left 124 pages
free and took 26 µs for a window of either size, and reading through it
was as fast (822 ms for the 12 MB).

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

## Checks

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

Fuzzing itself (`tools/fuzz.py`, under Fuzzing above) is not among them: it
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

## Licence

MIT; see `LICENSE`. Third-party components are listed in `THIRD_PARTY.md`.
The books in `corpus/` are not part of that: each is under the terms it
came with, which `corpus/README.md` gives.
