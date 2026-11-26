# Drawings

A book can hold a drawing as SVG, and `draw/` draws one. It reads the
file's tags, works out where each shape goes, reads the shape's outline,
which SVG writes as path data (the `d` of a `<path>`), turns that into
straight lines in pixels, and fills what they enclose with a gray, a strip
of the picture at a time. That is shapes and what they are filled with,
whether a shape is written where it is drawn or defined once and used in
many places, and a line of some width along an outline. Text and a
picture inside a drawing are not drawn yet: a drawing comes out without
them, and with a report of what it had.

This document takes the parts from the inside out: outlines, then filling
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

## Filling an outline

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
  is how what is inside an `svg` is cut off at the edges of the `svg`. A
  pixel wholly inside the box is drawn as if there were no box, with
  nothing worked out for it: every drawing has a box the size of its
  picture, so that is nearly every pixel drawn.
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

## Stroking an outline

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

## A whole file

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
  eight bytes more for each element that has an `id`. The memory is taken
  in the order the file has things in, so what fits in it does not depend
  on the pieces the file is read in: a rule of a style sheet that is
  inside `defs`, which is kept in the same memory, is kept once the
  sheet's text as far as the rule's end has been copied, and the rest of
  the text is copied after it. A `use` reads its
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
  the records of what is kept three times at the most: once to find what
  a gradient's `href` names, twice to style an element and find the
  gradient it is filled with, and a third time, for a `use`, to find what
  it names. A piece of a sheet's text is read in the call it
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

The unit tests draw files small enough that every pixel can be said by
hand, or set beside another drawing that has to come out the same: a
circle beside the four arcs that make one, a path beside the same data
given straight to the flattener and the filler. They give the reader of
tags and the renderer 26,000 files of the bytes that mean something to
them put together at random, and hold each call to its bound. What is
used is set beside the same thing written where it is used, which has to
come out the same byte for byte. The memory for definitions is cut down to
a byte less than a file needs, and to less; with a style sheet inside
`defs`, to where a rule of it just fits and to a byte less, in pieces of
seven sizes, which have to keep the same; and a file of five kilobytes
that asks for tens of megabytes to be read again is stopped at the limit.
An end-to-end test (`tests/e2e/svg_draw.py`) makes drawings up at random,
with groups inside groups, every shape, transforms, paint as attributes,
as `style` and by the rules of a style sheet, which in some is inside
`defs`, things hidden, things
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
fuzz target ([Fuzzing](fuzzing.md)), and the renderer's target places its picture by each
of the four turns. Another end-to-end test
(`tests/e2e/drawing_viewer.py`) runs the desktop application over a folder
of drawings and pictures (a PNG, a JPEG and a progressive JPEG) and goes
from one to the next and round, and sets each picture the simulated panel
shows beside `bn-svg`'s drawing of the same file, or `bn-png`'s or
`bn-jpeg`'s reading of it, at the size it should be on the page, turned
onto the panel: in blocks of 16
by 16 pixels, each as dark as it should be to within a fifth, and all of
them to within a hundredth on the whole.
