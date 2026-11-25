# The panel and the buttons

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

In place of the pattern, the application can show the drawings and
pictures on the card ([Drawings and pictures on the panel](drawing-viewer.md)),
or text set in a fonts image's faces ([Trying faces on the
panel](text-viewer.md)).

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

## A window on the desktop

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

## Sleep

Power puts the device to sleep, where the device can sleep
(`core/power.h`), unless the storage check, the book check or the layout
check is built in: those run again at each press of Power instead (see
[Storage](storage.md) and [Timing layout on a device](layout-check.md)). The
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

## The panel's two planes

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

## Windows of a plane

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

## Grays

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
