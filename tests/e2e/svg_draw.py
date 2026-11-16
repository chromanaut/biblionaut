#!/usr/bin/env python3
"""End-to-end check of `bn-svg draw`, and through it of how a whole SVG file is drawn, run by CTest.

The unit tests say what the renderer should make of files written for them.
Here drawings are made up at random: groups inside groups, every kind of
shape, transforms, paint said as attributes and as `style`, things hidden,
an `svg` inside another that cuts off what is in it, things defined in
`defs` and drawn by `use`, and a few things bn-svg does not draw. Each is
written out as a file in one of the many ways the same drawing can be
written, and bn-svg draws it.

The picture it should make is worked out here from the drawing itself and
not from the file, so nothing of the file is read twice: where each shape
goes by multiplying out its matrices, its outline traced finely, the pixels
inside found by running lines through each row, what a `use` names drawn
again where the `use` stands, and one shape laid over another by the
opacity it has. That shares no code with bn-svg. The two
pictures have to agree, the picture has to be the same however it is cut
into strips and however the file is cut into pieces, and what bn-svg says
it left out has to be what the drawing was given that it does not draw.

usage: svg_draw.py BN_SVG WORK_DIR
"""

from __future__ import annotations

import math
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # The script beside this one is read as a module, and leaves nothing behind.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from svg_tool import Failure, counted, expect, multiplied, outlines_of  # noqa: E402

IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


# ---- Drawings made up at random ----


def number(value: float) -> str:
    """A multiple of a quarter, written the short way."""
    text = repr(float(value))
    return text[:-2] if text.endswith(".0") else text


def a_gray(rng: random.Random) -> tuple[str, float]:
    """A colour as it is written, and the gray it should be drawn in."""
    if rng.random() < 0.2:
        red, green, blue = rng.randrange(256), rng.randrange(256), rng.randrange(256)
        gray = 0.299 * red + 0.587 * green + 0.114 * blue
        return rng.choice([f"#{red:02x}{green:02x}{blue:02x}", f"rgb({red}, {green},{blue})"]), gray
    level = rng.choice([0x00, 0x33, 0x66, 0x80, 0x99, 0xCC, 0xEE])
    ways = [f"#{level:02x}{level:02x}{level:02x}", f"rgb({level},{level},{level})", f"#{level:02X}{level:02X}{level:02X}"]
    if level % 0x11 == 0:
        ways.append(f"#{level >> 4:x}{level >> 4:x}{level >> 4:x}")
    if level == 0:
        ways.append("black")
    return rng.choice(ways), float(level)


def a_transform(rng: random.Random) -> tuple[str, tuple]:
    """A transform as SVG writes one, gentle enough to leave a shape in the picture, and the matrix it comes to."""
    text, matrix = [], IDENTITY
    for _ in range(rng.randrange(1, 3)):
        kind = rng.randrange(5)
        if kind == 0:
            x, y = rng.randrange(-24, 25) / 4, rng.randrange(-24, 25) / 4
            text.append(f"translate({number(x)} {number(y)})")
            one = (1.0, 0.0, 0.0, 1.0, x, y)
        elif kind == 1:
            x, y = rng.choice([0.5, 0.75, 1.25, 1.5]), rng.choice([0.5, 0.75, 1.25, 1.5])
            text.append(f"scale({x},{y})")
            one = (x, 0.0, 0.0, y, 0.0, 0.0)
        elif kind == 2:
            degrees, x, y = rng.choice([-30, -10, 15, 45]), rng.randrange(10, 40), rng.randrange(5, 30)
            text.append(f"rotate({degrees} {x} {y})")
            cos, sin = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
            one = (cos, sin, -sin, cos, x - cos * x + sin * y, y - sin * x - cos * y)
        elif kind == 3:
            degrees = rng.choice([-15, 10, 20])
            text.append(f"skewX({degrees})")
            one = (1.0, 0.0, math.tan(math.radians(degrees)), 1.0, 0.0, 0.0)
        else:
            one = (rng.choice([0.75, 1.0, 1.25]), rng.choice([-0.25, 0.0, 0.25]), rng.choice([-0.25, 0.0, 0.25]),
                   rng.choice([0.75, 1.0, 1.25]), float(rng.randrange(-5, 6)), float(rng.randrange(-5, 6)))
            text.append("matrix(" + " ".join(number(value) for value in one) + ")")
        matrix = multiplied(matrix, one)
    return " ".join(text), matrix


def paint_of(rng: random.Random, group: bool) -> dict:
    """What an element says of how it is painted. Whatever is missing, it takes from what it is in."""
    paint: dict = {}
    if rng.random() < (0.3 if group else 0.6):
        paint["fill"] = ("none", None) if rng.random() < 0.1 else a_gray(rng)
    if rng.random() < 0.25:
        paint["fill-opacity"] = rng.choice([0.25, 0.5, 0.75])
    if rng.random() < 0.2:
        # Never so slight that a few of them, one inside another, come to nothing at all.
        paint["opacity"] = rng.choice([0.5, 0.75])
    if rng.random() < 0.3:
        paint["fill-rule"] = rng.choice(["evenodd", "nonzero"])
    if rng.random() < 0.08:
        paint["hidden"] = rng.choice(["display", "visibility"])
    return paint


def a_shape(rng: random.Random, across: float, down: float) -> dict:
    """A shape somewhere in a space `across` by `down`: what it is written as, and its outlines there."""
    def x() -> float:
        return rng.randrange(0, int(across * 4)) / 4

    def y() -> float:
        return rng.randrange(0, int(down * 4)) / 4

    kind = rng.choice(["rect", "rect", "round", "circle", "ellipse", "polygon", "polyline", "path", "path"])
    attributes: dict[str, str] = {}
    if kind in ("rect", "round"):
        left, top = x() * 0.7, y() * 0.7
        width, height = 2 + rng.randrange(0, int(across * 2)) / 4, 2 + rng.randrange(0, int(down * 2)) / 4
        left, top = round(left * 4) / 4, round(top * 4) / 4
        attributes = {"x": number(left), "y": number(top), "width": number(width), "height": number(height)}
        if rng.random() < 0.2:
            # As parts of the space it is in: a quarter of a percent is exact.
            attributes["width"] = number(100 * width / across) + "%" if (400 * width / across).is_integer() else number(width)
        data = f"M{left} {top}H{left + width}V{top + height}H{left}Z"
        if kind == "round":
            rx = min(rng.choice([0.5, 1.0, 2.0, 3.0]), width / 2)
            ry = min(rng.choice([0.5, 1.0, 2.0, 3.0]), height / 2)
            attributes["rx"], attributes["ry"] = number(rx), number(ry)
            right, bottom = left + width, top + height
            data = (f"M{left + rx} {top}H{right - rx}A{rx} {ry} 0 0 1 {right} {top + ry}V{bottom - ry}"
                    f"A{rx} {ry} 0 0 1 {right - rx} {bottom}H{left + rx}A{rx} {ry} 0 0 1 {left} {bottom - ry}V{top + ry}"
                    f"A{rx} {ry} 0 0 1 {left + rx} {top}Z")
        name, outlines = "rect", ("data", data)
    elif kind in ("circle", "ellipse"):
        middle = (x(), y())
        radii = (1 + rng.randrange(0, int(across)) / 4, 1 + rng.randrange(0, int(down)) / 4)
        if kind == "circle":
            radii = (radii[0], radii[0])
            attributes = {"cx": number(middle[0]), "cy": number(middle[1]), "r": number(radii[0])}
        else:
            attributes = {"cx": number(middle[0]), "cy": number(middle[1]), "rx": number(radii[0]), "ry": number(radii[1])}
        points = [(middle[0] + radii[0] * math.cos(2 * math.pi * step / 720),
                   middle[1] + radii[1] * math.sin(2 * math.pi * step / 720)) for step in range(720)]
        name, outlines = kind, ("points", [points])
    elif kind in ("polygon", "polyline"):
        points = [(x(), y()) for _ in range(rng.randrange(3, 7))]
        separator = rng.choice([" ", ",", ", "])
        attributes = {"points": rng.choice([" ", "\n  "]).join(f"{number(px)}{separator}{number(py)}" for px, py in points)}
        name, outlines = kind, ("points", [points])
    else:
        data = f"M{number(x())} {number(y())}"
        for _ in range(rng.randrange(2, 6)):
            letter = rng.choice("LLCQSTAHV")
            if letter == "A":
                data += (f"A{number(rng.randrange(4, 60) / 4)} {number(rng.randrange(4, 60) / 4)} {rng.choice([0, 30, -45])} "
                         f"{rng.randrange(2)} {rng.randrange(2)} {number(x())} {number(y())}")
            elif letter in "HV":
                data += letter + number(x() if letter == "H" else y())
            else:
                pairs = {"L": 1, "C": 3, "Q": 2, "S": 2, "T": 1}[letter]
                data += letter + " ".join(f"{number(x())} {number(y())}" for _ in range(pairs))
        data += rng.choice(["Z", "z", ""])
        if rng.random() < 0.3:
            data += f"M{number(x())} {number(y())}l4 0 0 4-4 0z"
        attributes = {"d": data}
        name, outlines = "path", ("data", data)
    shape = {"name": name, "attributes": attributes, "outlines": outlines, "paint": paint_of(rng, False)}
    if rng.random() < 0.3:
        shape["transform"] = a_transform(rng)
    if rng.random() < 0.05:
        shape["stroke"] = True
    return shape


def a_use(rng: random.Random, definitions: list) -> dict:
    """A `use` of one of the things a drawing defines: which, where it is put, and what the use says of its paint."""
    target = rng.choice(definitions)
    use: dict = {"name": "use", "paint": paint_of(rng, True), "target": target,
                 "attributes": {rng.choice(["href", "xlink:href"]): "#" + target["attributes"]["id"]}}
    if rng.random() < 0.5:
        use["at"] = (rng.randrange(-20, 21) / 4, rng.randrange(-20, 21) / 4)
        use["attributes"].update({"x": number(use["at"][0]), "y": number(use["at"][1])})
    if rng.random() < 0.3:
        use["transform"] = a_transform(rng)
    return use


def a_group(rng: random.Random, across: float, down: float, depth: int, definitions: list | None = None,
            inner: bool = False) -> dict:
    """A group, or an `svg` with a place and a view box of its own, and what is in it. With `inner`, an `svg`
    for certain, with something in it that lies beyond its edges."""
    group: dict = {"name": "g", "paint": paint_of(rng, True), "children": []}
    inner_across, inner_down = across, down
    if inner or rng.random() < 0.25:
        # An svg inside: where it is, how large, and perhaps its own coordinates and how they are fitted.
        place = (rng.randrange(0, int(across / 2)), rng.randrange(0, int(down / 2)), rng.randrange(8, max(int(across), 9)),
                 rng.randrange(8, max(int(down), 9)))
        group.update({"name": "svg", "place": place})
        if rng.random() < 0.4:
            # What is in it is cut off at its edges unless this says it may show beyond them.
            group["overflow"] = rng.choice(["visible", "auto", "hidden", "scroll"])
        inner_across, inner_down = float(place[2]), float(place[3])
        if rng.random() < 0.7:
            group["box"] = (rng.choice([0, -4, 10]), rng.choice([0, 5]), rng.randrange(10, 60), rng.randrange(10, 60))
            group["fit"] = a_fit(rng)
            inner_across, inner_down = float(group["box"][2]), float(group["box"][3])
    elif rng.random() < 0.5:
        group["transform"] = a_transform(rng)
    for _ in range(rng.randrange(1, 4)):
        if definitions and rng.random() < 0.3:
            group["children"].append(a_use(rng, definitions))
        elif depth < 3 and rng.random() < 0.25:
            group["children"].append(a_group(rng, inner_across, inner_down, depth + 1, definitions))
        else:
            group["children"].append(a_shape(rng, inner_across, inner_down))
    if group["name"] == "svg" and (inner or rng.random() < 0.7):
        # Something half as large again as the space, and moved, so that some of it lies beyond the edges.
        x, y = -rng.randrange(0, int(inner_across)) / 4, -rng.randrange(0, int(inner_down)) / 4
        group["children"].append({"name": "g", "paint": {}, "children": [a_shape(rng, inner_across, inner_down)],
                                  "transform": (f"translate({number(x)} {number(y)}) scale(1.5)", (1.5, 0.0, 0.0, 1.5, x, y))})
    return group


def a_fit(rng: random.Random) -> tuple[str, str, str] | None:
    """How a view box is fitted: where it goes across and down, and "meet", "slice" or "none". Or nothing said."""
    if rng.random() < 0.3:
        return None
    if rng.random() < 0.2:
        return ("Mid", "Mid", "none")
    return (rng.choice(["Min", "Mid", "Max"]), rng.choice(["Min", "Mid", "Max"]), rng.choice(["meet", "meet", "slice"]))


def a_drawing(rng: random.Random) -> dict:
    """A whole drawing: its own size or view box, and what is in it."""
    drawing: dict = {"name": "svg", "paint": {}, "children": [], "root": True}
    across, down = float(rng.randrange(30, 80)), float(rng.randrange(20, 60))
    if rng.random() < 0.7:
        drawing["box"] = (rng.choice([0, 0, -10, 25]), rng.choice([0, 0, 5]), across, down)
        drawing["fit"] = a_fit(rng)
        if rng.random() < 0.5:
            drawing["size"] = (rng.choice(["100", "50%", "3in"]), rng.choice(["80", "100%", "40pt"]))
    else:
        drawing["size"] = (number(across), number(down))
    drawing["across"], drawing["down"] = across, down
    # Things defined, to be drawn only where they are used: shapes, groups, and groups that use what was
    # defined before them. They are written first, in one `defs` or two.
    definitions: list[dict] = []
    if rng.random() < 0.7:
        names = rng.sample(["a", "glyph0", "MJMAIN-31", "E1-MJMATHI-78", "b_2", "leaf.1", "a1"], 3)
        for name in names[:rng.randrange(1, 4)]:
            if definitions and rng.random() < 0.3:
                thing: dict = {"name": "g", "paint": paint_of(rng, True),
                               "children": [a_use(rng, list(definitions)), a_shape(rng, across, down)]}
            elif rng.random() < 0.3:
                thing = a_group(rng, across, down, 2)
            else:
                thing = a_shape(rng, across, down)
            thing.setdefault("attributes", {})["id"] = name
            definitions.append(thing)
        split = rng.randrange(len(definitions) + 1)
        for some in (definitions[:split], definitions[split:]):
            if some:
                drawing["children"].append({"name": "defs", "paint": {}, "children": some})
    for _ in range(rng.randrange(2, 5)):
        kind = rng.random()
        if definitions and kind < 0.3:
            drawing["children"].append(a_use(rng, definitions))
        elif kind < 0.4:
            # An svg that says what is in it may show beyond its edges.
            showing = a_group(rng, across, down, 1, definitions, True)
            showing["overflow"] = rng.choice(["visible", "auto"])
            drawing["children"].append(showing)
        elif kind < 0.5:
            # An svg in a group that turns it, which is drawn whole.
            degrees = rng.choice([-30, 15, 45])
            cos, sin = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
            drawing["children"].append({"name": "g", "paint": {}, "transform": (f"rotate({degrees})", (cos, sin, -sin, cos, 0.0, 0.0)),
                                        "children": [a_group(rng, across, down, 2, definitions, True)]})
        elif kind < 0.7:
            drawing["children"].append(a_group(rng, across, down, 1, definitions))
        else:
            drawing["children"].append(a_shape(rng, across, down))
    # Things that are not drawn, and should be said to have been left out or should not matter at all.
    drawing["extras"] = [extra for extra in (
        ("<text x='5' y='9'>Words</text>", "text"),
        ("<use href='#nothing' x='3'/>", "use of what is not defined"),
        ("<image href='picture.png' width='9' height='9'/>", "a picture"),
        ("<defs><rect width='999' height='999'/><path d='M0 0h999v999z'/></defs>", None),
        ("<title>A drawing</title><desc>Made up</desc>", None),
        ("<!-- a comment, with <rect width='999' height='999'/> in it -->", None),
    ) if rng.random() < 0.25]
    return drawing


# ---- A drawing written out as a file ----


def written(rng: random.Random, element: dict, indent: str = "") -> str:
    """One of the many ways an element and what is in it can be written."""
    attributes: list[tuple[str, str]] = list(element.get("attributes", {}).items())
    if element.get("root"):
        attributes.append(("xmlns", "http://www.w3.org/2000/svg"))
        attributes.append(("xmlns:xlink", "http://www.w3.org/1999/xlink"))
        if "size" in element:
            attributes += [("width", element["size"][0]), ("height", element["size"][1])]
    if "place" in element:
        attributes += list(zip(("x", "y", "width", "height"), (number(value) for value in element["place"])))
    if "overflow" in element:
        attributes.append(("overflow", element["overflow"]))
    if "box" in element:
        attributes.append(("viewBox", rng.choice([" ", ",", ", "]).join(number(value) for value in element["box"])))
        if element.get("fit"):
            across, down, mode = element["fit"]
            text = "none" if mode == "none" else f"x{across}Y{down}" + ("" if mode == "meet" and rng.random() < 0.5 else " " + mode)
            attributes.append(("preserveAspectRatio", text))
    if "transform" in element:
        attributes.append(("transform", element["transform"][0]))
    if element.get("stroke"):
        attributes.append(("stroke", "#000"))

    # Paint is said as attributes, in `style`, or some of each; and `style` wins over an attribute that
    # says otherwise.
    paint = element["paint"]
    said: dict[str, str] = {}
    if "fill" in paint:
        said["fill"] = paint["fill"][0]
    for name in ("fill-opacity", "opacity", "fill-rule"):
        if name in paint:
            said[name] = str(paint[name])
    if paint.get("hidden") == "display":
        said["display"] = "none"
    elif paint.get("hidden") == "visibility":
        said["visibility"] = "hidden"
    style = []
    for name, value in said.items():
        way = rng.randrange(4)
        if way == 0:
            style.append(f"{name}:{value}")
        elif way == 1:
            style.append(f" {name} : {value} ")
            attributes.append((name, "#123456" if name == "fill" else "inherit"))
        else:
            attributes.append((name, value))
    if style:
        attributes.append(("style", ";".join(style) + rng.choice(["", ";"])))
    rng.shuffle(attributes)

    text = indent + "<" + element["name"]
    for name, value in attributes:
        quote = rng.choice("'\"")
        if name == "d" and rng.random() < 0.2:
            value = value.replace(" ", "&#32;", 1)
        text += rng.choice([" ", "  ", "\n" + indent + "    "]) + name + rng.choice(["=", " = "]) + quote + value + quote
    children = element.get("children")
    if children is None:
        return text + rng.choice(["/>", " />", f"></{element['name']}>"]) + "\n"
    text += ">\n"
    for child in children:
        text += written(rng, child, indent + "  ")
    for extra, _ in element.get("extras", []):
        text += indent + "  " + extra + "\n"
    return text + indent + f"</{element['name']}>\n"


# ---- The picture a drawing should make ----


def fitted(box, space, fit) -> tuple:
    """The matrix that puts a view box in a space, as SVG says `preserveAspectRatio` does."""
    across, down, mode = fit or ("Mid", "Mid", "meet")
    scale_x, scale_y = space[2] / box[2], space[3] / box[3]
    if mode != "none":
        scale_x = scale_y = min(scale_x, scale_y) if mode == "meet" else max(scale_x, scale_y)
    part = {"Min": 0.0, "Mid": 0.5, "Max": 1.0}
    return (scale_x, 0.0, 0.0, scale_y, space[0] - box[0] * scale_x + part[across] * (space[2] - box[2] * scale_x),
            space[1] - box[1] * scale_y + part[down] * (space[3] - box[3] * scale_y))


def moved(point, matrix):
    return (matrix[0] * point[0] + matrix[2] * point[1] + matrix[4], matrix[1] * point[0] + matrix[3] * point[1] + matrix[5])


class Picture:
    def __init__(self, width: int, height: int):
        self.width, self.height = width, height
        self.gray = [[255.0] * width for _ in range(height)]
        self.sure = [[True] * width for _ in range(height)]
        self.left_out: set[str] = set()
        self.shapes = 0
        self.uses = 0
        self.cut = 0  # Pixels that something was cut off in, wholly or partly,
        self.uncut = 0  # and those something showed in beyond the edges of an svg that lets it.
        self.turned = 0  # Inner svgs drawn whole because they are turned.
        # Whether bn-svg's numbers could fairly say something else of this drawing than these do.
        self.doubtful = False

    def draw(self, element: dict, matrix: tuple, handed: dict, box: tuple | None = None,
             beyond: tuple | None = None) -> None:
        """Lays an element, and what is in it, over the picture. `handed` is what it takes from what it is in,
        and `box` the edges it is cut off at, in pixels, if it is inside an `svg` that cuts things off.
        `beyond` is the edges of an `svg` it is in that cuts nothing off, kept only to count what shows there."""
        paint = element["paint"]
        if paint.get("hidden") == "display" or element["name"] == "defs":
            return
        state = dict(handed)
        if "fill" in paint:
            state["fill"] = paint["fill"][1]
        if "fill-opacity" in paint:
            state["fill-opacity"] = paint["fill-opacity"]
        if "fill-rule" in paint:
            state["fill-rule"] = paint["fill-rule"]
        if paint.get("hidden") == "visibility":
            state["hidden"] = True
        # Opacity is the element's own. bn-svg gives a group's to each shape in it, and says so.
        state["opacity"] = handed["opacity"] * paint.get("opacity", 1.0)

        if element["name"] == "use":
            # What it names, as if that stood here: moved by the use's transform and then to its place, and
            # painted as the use is where it says nothing itself.
            if "transform" in element:
                matrix = multiplied(matrix, element["transform"][1])
            if "at" in element:
                matrix = multiplied(matrix, (1.0, 0.0, 0.0, 1.0, element["at"][0], element["at"][1]))
            if "opacity" in paint:
                self.left_out.add("opacity on a group")
            self.uses += 1
            self.draw(element["target"], matrix, state, box, beyond)
            return

        if "children" in element:
            if "place" in element:
                place = tuple(float(value) for value in element["place"])
                if element.get("overflow") in ("visible", "auto"):
                    if matrix[1] == 0.0 and matrix[2] == 0.0:
                        one = moved((place[0], place[1]), matrix)
                        other = moved((place[0] + place[2], place[1] + place[3]), matrix)
                        beyond = (min(one[0], other[0]), min(one[1], other[1]), max(one[0], other[0]), max(one[1], other[1]))
                else:
                    if matrix[1] == 0.0 and matrix[2] == 0.0:
                        # Its edges are edges of pixels' rows and columns: what is in it is cut off there,
                        # and at the edges of whatever it is in.
                        one = moved((place[0], place[1]), matrix)
                        other = moved((place[0] + place[2], place[1] + place[3]), matrix)
                        edges = (min(one[0], other[0]), min(one[1], other[1]), max(one[0], other[0]), max(one[1], other[1]))
                        box = edges if box is None else (max(box[0], edges[0]), max(box[1], edges[1]),
                                                         min(box[2], edges[2]), min(box[3], edges[3]))
                    else:
                        # Turned or slanted, it is drawn whole, and bn-svg says so. Turns that all but undo
                        # each other could come out as no turn at all in its arithmetic.
                        self.left_out.add("the edges of an inner svg that is turned")
                        self.doubtful = self.doubtful or (abs(matrix[1]) < 1e-6 and abs(matrix[2]) < 1e-6)
                        self.turned += 1
                inner = fitted(element["box"], place, element.get("fit")) if "box" in element else (1.0, 0.0, 0.0, 1.0, place[0], place[1])
                matrix = multiplied(matrix, inner)
            elif "transform" in element:
                matrix = multiplied(matrix, element["transform"][1])
            if "opacity" in paint:
                self.left_out.add("opacity on a group")
            for child in element["children"]:
                self.draw(child, matrix, state, box, beyond)
            return

        if "transform" in element:
            matrix = multiplied(matrix, element["transform"][1])
        if element.get("stroke") and not state["hidden"]:
            self.left_out.add("a stroke")
        if state["hidden"] or state["fill"] is None:
            return
        kind, what = element["outlines"]
        outlines = outlines_of(what, matrix) if kind == "data" else [[moved(point, matrix) for point in points] for points in what]
        cover, plain = counted(outlines, self.width, self.height, state["fill-rule"])
        solid = state["fill-opacity"] * state["opacity"]
        self.shapes += 1
        for row in range(self.height):
            for pixel in range(self.width):
                # How much of the pixel is inside the edges it is cut off at.
                share = 1.0
                if box is not None:
                    share = (max(0.0, min(pixel + 1.0, box[2]) - max(float(pixel), box[0])) *
                             max(0.0, min(row + 1.0, box[3]) - max(float(row), box[1])))
                if share < 1.0 and (cover[row][pixel] > 0 or not plain[row][pixel]):
                    self.cut += 1
                if beyond is not None and cover[row][pixel] > 0.5 and not (
                        beyond[0] <= pixel and pixel + 1 <= beyond[2] and beyond[1] <= row and row + 1 <= beyond[3]):
                    self.uncut += 1
                if share == 0.0:
                    continue
                if not plain[row][pixel]:
                    self.sure[row][pixel] = False
                elif cover[row][pixel] > 0:
                    if share < 1.0 and cover[row][pixel] < 0.999:
                        # An edge of the shape and an edge it is cut off at, in one pixel: how much of the
                        # shape is on the inner side of the cut is not known from how much is in the pixel.
                        self.sure[row][pixel] = False
                    else:
                        self.gray[row][pixel] += (state["fill"] - self.gray[row][pixel]) * cover[row][pixel] * share * solid


def picture_of(drawing: dict, width: int, height: int) -> Picture:
    picture = Picture(width, height)
    space = (0.0, 0.0, float(width), float(height))
    if "box" in drawing:
        matrix = fitted(drawing["box"], space, drawing.get("fit"))
    else:
        matrix = (width / drawing["across"], 0.0, 0.0, height / drawing["down"], 0.0, 0.0)
    picture.draw(drawing, matrix, {"fill": 0.0, "fill-opacity": 1.0, "opacity": 1.0, "fill-rule": "nonzero", "hidden": False})
    picture.left_out |= {name for _, name in drawing["extras"] if name}
    return picture


# ---- bn-svg ----

SUMMARY = re.compile(rb"a drawing of (\S+) by (\S+), as (\d+) by (\d+) in (\d+) strips? of (\d+) rows: (\d+) elements, "
                     rb"(\d+) shapes filled, (\d+) commands, (\d+) lines in all, (\d+) bytes of cells")


def draw(tool: str, work: Path, file: str | bytes, *options: str):
    """Has bn-svg draw a file. What it ended with and said, its summary, what it left out, and the picture's pixels."""
    (work / "drawing.svg").write_bytes(file.encode("utf-8") if isinstance(file, str) else file)
    out = work / "picture.pgm"
    out.unlink(missing_ok=True)
    result = subprocess.run([tool, "draw", str(work / "drawing.svg"), "--out", str(out), *options], capture_output=True,
                            check=False)
    summary = SUMMARY.search(result.stderr)
    left = re.search(rb"left out, or not drawn as written: (.*)", result.stderr)
    left_out = set(left.group(1).decode().split(", ")) if left else set()
    pixels = None
    if out.exists() and summary:
        written_out = out.read_bytes()
        header = b"P5\n%d %d\n255\n" % (int(summary.group(3)), int(summary.group(4)))
        expect(written_out.startswith(header) and len(written_out) == len(header) + int(summary.group(3)) * int(summary.group(4)),
               f"what bn-svg wrote is not the picture it said it drew: {result.stderr!r}")
        pixels = written_out[len(header):]
    return result, summary, left_out, pixels


def check_drawing(tool: str, work: Path, rng: random.Random, number_of: int) -> tuple:
    """Checks one drawing. Returns how many of its pixels were compared, how many were left out, the worst, and
    how much of what the test is for the drawing had: uses, pixels with something cut off, pixels with something
    showing beyond the edges of an `svg` that lets it, and `svg` elements that are turned."""
    drawing = a_drawing(rng)
    file = ("<?xml version='1.0' encoding='UTF-8'?>\n" if number_of % 2 else "") + written(rng, drawing)
    width, height = rng.randrange(30, 49), rng.randrange(24, 37)
    # Curves are cut finely, so that what is compared is where things are put and how they are painted.
    options = ["--width", str(width), "--height", str(height), "--tolerance", "0.02"]
    result, summary, left_out, drawn = draw(tool, work, file, *options)
    expect(result.returncode == 0 and drawn is not None, f"{file}\nbn-svg ended with {result.returncode}: {result.stderr!r}")
    for more in (["--rows", "5"], ["--pieces", "7"], ["--rows", "1", "--pieces", "1"]):
        _, _, left_again, again = draw(tool, work, file, *options, *more)
        expect(again == drawn and left_again == left_out, f"{file}\ndrawn differently with {more}")

    picture = picture_of(drawing, width, height)
    if picture.doubtful:
        return 0, 0, 0.0, (0, 0, 0, 0)
    expect(left_out == picture.left_out, f"{file}\nbn-svg says it left out {sorted(left_out)}; it was given {sorted(picture.left_out)}")
    expect(int(summary.group(8)) == picture.shapes, f"{file}\nbn-svg filled {int(summary.group(8))} shapes of {picture.shapes}")
    compared, unsure, worst, total = 0, 0, 0.0, 0.0
    for row in range(height):
        for pixel in range(width):
            if not picture.sure[row][pixel]:
                unsure += 1
                continue
            apart = abs(drawn[row * width + pixel] - picture.gray[row][pixel]) / 255
            expect(apart <= 0.06, f"{file}\nthe pixel at {pixel},{row} is {drawn[row * width + pixel]}, and should be "
                                  f"{picture.gray[row][pixel]:.1f}")
            compared += 1
            worst = max(worst, apart)
            total += apart
    expect(total / max(compared, 1) <= 0.004, f"{file}\n{total / max(compared, 1):.4f} of the way from black to white out on average")
    return compared, unsure, worst, (picture.uses, picture.cut, picture.uncut, picture.turned)


def check_the_tool(tool: str, work: Path) -> None:
    """What `bn-svg draw` does with a size, with a file that goes wrong, and with what it cannot do."""
    square = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 30 20'><rect x='3' y='2' width='6' height='4' fill='#808080'/></svg>"

    def gray_at(pixels: bytes, width: int, x: int, y: int) -> int:
        return pixels[y * width + x]

    # Left to itself the picture is the size the drawing says it is; given a width or a height, it keeps its shape.
    for options, size in (([], (30, 20)), (["--width", "60"], (60, 40)), (["--height", "10"], (15, 10)),
                          (["--width", "60", "--height", "60"], (60, 60))):
        result, summary, left_out, pixels = draw(tool, work, square, *options)
        expect(result.returncode == 0 and summary and (int(summary.group(3)), int(summary.group(4))) == size and not left_out,
               f"a drawing of 30 by 20 with {options}: {result.stderr!r}")
        scale = size[0] / 30
        middle_x, middle_y = int(6 * scale), int(4 * scale) + (10 if size == (60, 60) else 0)
        expect(gray_at(pixels, size[0], middle_x, middle_y) == 128 and gray_at(pixels, size[0], 0, 0) == 255,
               f"a drawing of 30 by 20 with {options}: the square is not where it should be")
    result, summary, _, _ = draw(tool, work, "<svg width='2.5in' height='10.25'/>")
    expect(result.returncode == 0 and summary.group(1, 2, 3, 4) == (b"240", b"10.25", b"240", b"11"),
           f"a drawing of two and a half inches: {result.stderr!r}")
    result, summary, _, _ = draw(tool, work, square, "--rows", "7")
    expect(summary.group(5, 6) == (b"3", b"7") and int(summary.group(11)) == (30 + 2) * 7 * 2, f"strips of 7 rows: {result.stderr!r}")

    # A file that is not a drawing, and files that go wrong part of the way through: what came before is drawn.
    for file, said in ((b"", b"not svg"), (b"<html><svg/></html>", b"not svg"), (b"\x89PNG\r\n", b"not svg")):
        result, _, _, _ = draw(tool, work, file)
        expect(result.returncode == 1 and said in result.stderr, f"{file!r}: ended with {result.returncode}: {result.stderr!r}")
    over = "<rect width='30' height='20'/>"
    result, _, _, pixels = draw(tool, work, square.replace("</svg>", over + "<<</svg>"))
    wrong_at = square.index("</svg>") + len(over) + 1  # The second `<`, which cannot follow the first.
    expect(result.returncode == 1 and f"malformed at byte {wrong_at}\n".encode() in result.stderr
           and gray_at(pixels, 30, 5, 3) == 0 and gray_at(pixels, 30, 20, 15) == 0, f"tags that go wrong: {result.stderr!r}")
    result, _, _, pixels = draw(tool, work, square.replace("</svg>", "<g><rect x='20' width='5' height='5'/>"))
    expect(result.returncode == 1 and b"truncated" in result.stderr and gray_at(pixels, 30, 5, 3) == 128
           and gray_at(pixels, 30, 22, 2) == 0, f"a file that stops short: {result.stderr!r}")
    result, _, _, _ = draw(tool, work, "<svg width='0' height='5'/>")
    expect(result.returncode == 1 and b"no size" in result.stderr, f"a drawing of no size: {result.stderr!r}")
    result, _, _, _ = draw(tool, work, "<svg width='20000' height='5'/>")
    expect(result.returncode == 1 and b"too large" in result.stderr, f"a drawing too large: {result.stderr!r}")

    # What a file defines and uses is kept in the memory it is given for that, and it says how much. With too
    # little, what did not fit is not drawn, and it says that.
    defined = square.replace("</svg>", "<defs><rect id='dot' width='4' height='4'/></defs><use href='#dot' x='20' y='10'/></svg>")
    result, _, left_out, pixels = draw(tool, work, defined)
    kept = len("<rect id='dot' width='4' height='4'/></defs>")
    expect(result.returncode == 0 and not left_out and gray_at(pixels, 30, 22, 12) == 0
           and f"definitions: {kept} bytes kept of 32768, and {kept - len('</defs>')} gone through again in a strip\n".encode()
           in result.stderr, f"something defined and used: {result.stderr!r}")
    result, _, left_out, pixels = draw(tool, work, defined, "--definitions", "30")
    expect(result.returncode == 0 and left_out == {"definitions there was no room for", "use of what is not defined"}
           and gray_at(pixels, 30, 22, 12) == 255 and gray_at(pixels, 30, 5, 3) == 128
           and b"definitions: 30 bytes kept of 30, and 0 gone through again in a strip\n" in result.stderr,
           f"too little room for what is defined: {result.stderr!r}")
    result, _, _, pixels = draw(tool, work, defined, "--definitions", str(kept + 8), "--rows", "3", "--pieces", "5")
    expect(result.returncode == 0 and gray_at(pixels, 30, 22, 12) == 0, f"just room enough for what is defined: {result.stderr!r}")

    # What it leaves out is said, and the rest is drawn.
    result, _, left_out, pixels = draw(tool, work, square.replace("</svg>", "<text>Words</text><path d='M20 0h5v5' stroke='#000' fill='url(#g)'/></svg>"))
    expect(result.returncode == 0 and left_out == {"text", "a stroke", "a gradient or a pattern"} and gray_at(pixels, 30, 5, 3) == 128,
           f"things left out: {result.stderr!r}")

    # What it will not do.
    (work / "drawing.svg").write_text(square)
    for options in (["--width", "0"], ["--height", "x"], ["--width", "99999"], ["--rows", "0"], ["--size", "10x10"],
                    ["--transform", "scale(2)"], ["--count"], ["--tolerance", "0"], ["--pieces", "0"], ["--out"],
                    ["--definitions", "x"], ["--definitions", "99999999"], ["--definitions"]):
        result = subprocess.run([tool, "draw", str(work / "drawing.svg"), *options], capture_output=True, check=False)
        expect(result.returncode == 2 and b"usage:" in result.stderr, f"draw {options}: ended with {result.returncode}")
    result = subprocess.run([tool, "draw", str(work / "not-there.svg")], capture_output=True, check=False)
    expect(result.returncode == 1 and b"cannot read" in result.stderr, f"a missing file: {result.stderr!r}")
    result = subprocess.run([tool, "draw", str(work / "drawing.svg"), "--out", str(work / "no" / "p.pgm")],
                            capture_output=True, check=False)
    expect(result.returncode == 1 and b"cannot write" in result.stderr, f"nowhere to write: {result.stderr!r}")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    tool = sys.argv[1]
    work = Path(sys.argv[2])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    try:
        check_the_tool(tool, work)
        rng = random.Random(20261116)
        drawings, compared, unsure, worst = 0, 0, 0, 0.0
        uses, cut, uncut, turned = 0, 0, 0, 0
        for number_of in range(36):
            one, left, apart, had = check_drawing(tool, work, rng, number_of)
            drawings, compared, unsure, worst = drawings + (1 if one else 0), compared + one, unsure + left, max(worst, apart)
            uses, cut, uncut, turned = uses + had[0], cut + had[1], uncut + had[2], turned + had[3]
        expect(unsure * 5 < compared, f"{unsure} pixels of {compared + unsure} could not be compared: the test is at fault")
        expect(drawings >= 30 and uses >= 30 and cut >= 300 and uncut >= 100 and turned >= 2,
               f"{drawings} drawings with {uses} uses, {cut} pixels cut off, {uncut} showing beyond an svg's edges and "
               f"{turned} svg elements turned: too few for the test to mean much")
    except Failure as failure:
        print(f"FAILED: {failure}", file=sys.stderr)
        return 1
    print(f"{drawings} drawings, {compared} pixels, {uses} uses, {cut} pixels with something cut off: what bn-svg draws "
          f"agrees with the picture each drawing should make to within {worst:.3f} of the way from black to white, and is "
          f"the same in any strips and pieces")
    return 0


if __name__ == "__main__":
    sys.exit(main())
