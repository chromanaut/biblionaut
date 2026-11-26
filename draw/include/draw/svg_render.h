#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "draw/fill.h"
#include "draw/fixed.h"
#include "draw/flatten.h"
#include "draw/stroke.h"
#include "draw/svg_path.h"
#include "draw/svg_sheet.h"
#include "draw/svg_tags.h"
#include "draw/svg_values.h"

namespace bn::draw {

// How reading an SVG file ended.
enum class SvgStatus : std::uint8_t {
  kOk = 0,
  kNotSvg,     // Its first element is not `svg`, or it has none.
  kMalformed,  // Its tags went wrong. What came before that has been drawn.
  kTruncated,  // It ended with elements still open. What there was of it has been drawn.
};

// The name of a status, for diagnostics: "ok", "not svg", "malformed",
// "truncated".
const char* SvgStatusName(SvgStatus status) noexcept;

// What a file held that was not drawn, or was not drawn as it is written.
// A report has one bit for each.
enum class SvgLeftOut : std::uint32_t {
  kText = 1U << 0U,             // `text`: not drawn.
  kImage = 1U << 1U,            // `image`: not drawn.
  kUse = 1U << 2U,              // A `use` of something that is not in `defs` before it: not drawn.
  kStyleSheet = 1U << 3U,       // What a style sheet says that is not read (see SvgSheetReader), or a sheet that
                                // comes after something it would style: that much of it is not used.
  kStroke = 1U << 4U,           // A stroke not drawn as written: one painted with a gradient or a pattern (what
                                // was written to stand in for it is drawn, or nothing); the stroke or the fill of
                                // a shape whose path data is too long to hold (see SvgRenderer); or dashes past
                                // Stroker::kMostDashesPerOutline in one outline (the rest of it is solid).
  kPaintServer = 1U << 5U,      // A paint that names something other than a linear gradient before it, such as a
                                // pattern: what was written to stand in for it is drawn, or nothing. Or a
                                // gradient's stops past kMostStops, which are left out.
  kClip = 1U << 6U,             // `clip-path`: what it would have cut off is drawn.
  kMask = 1U << 7U,             // `mask`: passed over.
  kFilter = 1U << 8U,           // `filter`: passed over.
  kMarker = 1U << 9U,           // A marker at the corners of an outline: not drawn.
  kForeign = 1U << 10U,         // `foreignObject`: not drawn.
  kAnimation = 1U << 11U,       // Something that moves, or a script: passed over.
  kUnknownElement = 1U << 12U,  // An element that SVG does not have: not drawn, nor what is in it.
  kTooDeep = 1U << 13U,         // Elements nested deeper than kDeepest: not drawn.
  kLateTransform = 1U << 14U,   // A transform written after path data too long to hold: that shape is not drawn.
  kBadValue = 1U << 15U,        // An attribute that could not be read, or is too long to be one that is.
  kBadPath = 1U << 16U,         // Path data that went wrong: drawn as far as it was right.
  kHeldNumber = 1U << 17U,      // A number too large, held at the largest that fits.
  kGroupOpacity = 1U << 18U,    // `opacity` on a group: given to each shape in it, which differs where they overlap.
  kViewportClip = 1U << 19U,    // An `svg` inside another, turned (but not by quarters) or slanted: drawn, and not
                                // cut off at its edges.
  kSwitch = 1U << 20U,          // `switch`: everything in it is drawn, not the first that suits.
  kUnknownEntity = 1U << 21U,   // An entity the file declares for itself: taken as nothing.
  kNoRoom = 1U << 22U,          // More in `defs` than there was room to keep: a `use` of the rest is not drawn.
                                // Or names in it one inside another past kDeepestNamed.
  kUseLimit = 1U << 23U,        // More drawn by `use` than kMostReused, or one inside another past kDeepestUse.
};

// What a bit of a report says, in a word or two: "text", "a stroke".
const char* SvgLeftOutName(SvgLeftOut what) noexcept;

// What became of a file.
struct SvgReport {
  std::uint32_t leftOut = 0;    // A bit of SvgLeftOut for each thing met.
  std::uint32_t elements = 0;   // Elements read: one that is used is read again each time.
  std::uint32_t shapes = 0;     // Shapes filled,
  std::uint32_t strokes = 0;    // and stroked,
  std::uint32_t widened = 0;    // of which drawn wider, to the thinnest asked for.
  std::uint32_t commands = 0;   // Commands their outlines had,
  std::uint32_t lines = 0;      // and lines those became.
  std::uint32_t deepest = 0;    // The most groups open at once.
  std::uint32_t kept = 0;       // Bytes of definitions kept,
  std::uint32_t reused = 0;     // and bytes of them gone through again, for `use`.
  std::uint32_t rules = 0;      // Rules of style sheets kept,
  std::uint32_t gradients = 0;  // and linear gradients.
  std::uint32_t looked = 0;     // Times what is kept was looked through, which a call does three times at the most.

  bool Has(SvgLeftOut what) const noexcept { return (leftOut & static_cast<std::uint32_t>(what)) != 0; }
};

// What a file's first `svg` says of its size.
struct SvgSize {
  bool known = false;  // That element's attributes have been read.
  bool hasWidth = false;
  bool hasHeight = false;
  bool hasBox = false;
  SvgLength width;
  SvgLength height;
  SvgBox box;

  // How large the drawing is when nothing else decides it, in pixels. Its
  // `width` and `height` if it has them, in something other than a
  // percentage. One that is missing is taken from the other and the shape
  // of the view box; and with neither, both are the view box's. A drawing
  // that says nothing at all is 300 by 150, as a browser has it. Nothing,
  // if a size it gives is nothing or less.
  void Natural(Fixed& naturalWidth, Fixed& naturalHeight) const noexcept;
};

// Draws an SVG file into a strip of a picture.
//
// It is given the file's bytes, a piece at a time, and a Filler that has
// been set to the strip. The file is read once from its start to its end
// for each strip, and nothing of it is kept from one strip to the next:
// the memory it takes is this object, whatever the file's size, and what
// it is given to keep the file's definitions in (below). A file's longest
// attribute, the outline of an engraving of megabytes, goes through as it
// is read.
//
// What it draws, and how:
//
// - The whole drawing is fitted to the picture: its `viewBox`, where
//   `preserveAspectRatio` puts it; or, with no view box, its width and
//   height stretched to the picture's. Nothing is drawn outside the
//   picture, whatever its `overflow` says, unless PlaceOn() turns it other
//   than by quarters.
// - `svg`, `g`, `a` and `switch` are groups: a `transform`, and paint that
//   what is inside them takes unless it has its own. An `svg` inside
//   another has its own place, size and view box, and what is in it is
//   cut off at its edges unless its `overflow` is "visible" or "auto". A
//   pixel an edge passes through keeps the share of it that is inside.
//   The edges have to be edges of rows and columns of pixels for that: an
//   `svg` that something has turned, other than by quarters, or slanted is
//   drawn whole, and reported.
// - `use` draws something again that `defs` holds: see below.
// - `path`, `rect` (with rounded corners), `circle`, `ellipse`, `polygon`
//   and `polyline` are shapes, filled by `fill`, `fill-rule`,
//   `fill-opacity` and `opacity`, each written as an attribute, in
//   `style`, or by a rule of the file's style sheet. A colour is drawn as
//   a gray.
// - A style sheet, a `<style>` element, is read as SvgSheetReader reads
//   one: rules whose selectors are classes and elements' names. What a
//   rule sets wins over an attribute, and `style` over a rule; between
//   rules, a class wins over a name, and a later rule over an earlier one.
//   A rule styles what comes after the sheet in the file, which is read
//   once from its start for each strip.
// - A fill can be a linear gradient, `fill="url(#name)"`, which is drawn
//   as one: a gray for each pixel from where it lies along the gradient,
//   which is SVG's except for one thing. Over a shape's own box
//   (`gradientUnits="objectBoundingBox"`, which is where a gradient goes
//   unless it says otherwise), the box is the one round the outline's
//   points and its curves' control points, which can be a little larger
//   than the curve. A gradient takes what it does not say from the one
//   its `href` names. It can be anywhere in the file before what it
//   fills.
// - Shapes, and `line`, are stroked as Stroker strokes an outline: by
//   `stroke`, `stroke-width`, `stroke-opacity`, `stroke-linejoin`,
//   `stroke-linecap`, `stroke-miterlimit`, `stroke-dasharray` and
//   `stroke-dashoffset`, in the shape's own coordinates. The stroke goes
//   over the fill. A stroke painted with a gradient is not drawn. No
//   stroke is drawn thinner than SetThinnestStroke() asks.
// - `display` and `visibility` hide things.
// - Everything else is passed over, and Report() says what there was: see
//   SvgLeftOut. A drawing with text or pictures in it comes out without
//   them.
//
// Attributes may come in any order, with limits for path data longer than
// kHeldPathData. Data that short is held until the tag ends, and gone
// through once for the fill and again for the stroke. Longer data is drawn
// as it arrives, with what the tag has said by then, and so:
//
// - If a transform follows it in the same tag, the shape is left out and
//   reported.
// - It is either filled or stroked. It is stroked if by then it has a
//   stroke and no fill, and if its stroke is then changed, by its own tag or
//   by a rule of the style sheet, the stroke is left out. Otherwise it is
//   filled, and a stroke it turns out to have is left out. Either is
//   reported.
//
// What a file defines in one place and uses in another has to be kept
// until it is used, and that is the one thing here whose size the file
// decides. So the memory for it is the caller's (KeepDefinitionsIn()):
// what `defs` holds, the rules of the style sheet, and the gradients.
// What `defs` holds is copied there as it goes by, as it is written, with
// a note of where each element that has an `id` begins. The memory is
// taken in the order the file has things in, whatever pieces the file
// comes in: a rule of a sheet that is inside `defs` is recorded once the
// text as far as the rule's end has been kept, and the rest of the text
// is kept after it. So what fits is the same however the file is cut up.
// A `use` then
// reads its element again from that copy, as if it stood where the `use`
// does: moved by the transform of the `use` and then by its `x` and `y`,
// and painted as the `use` is where it says nothing of its own. A
// `symbol` is drawn only so, and as an `svg` is: with its view box fitted
// to the width and height the `use` gives, which an `svg` that is used
// takes too, and cut off there.
//
// Three limits, each reported and none a failure. An element can be used
// only if it is in `defs` and comes before its use, and is named by "#"
// and its `id`, written out with no entity in it. What does not fit in
// the memory given is not kept, nor is an element with a name inside more
// than kDeepestNamed others that have one. And no more than kMostReused
// bytes are gone through again in one pass, nor one `use` inside another
// deeper than kDeepestUse, so that a small file cannot ask for endless
// work by using things that use things.
//
// Step() does a bounded amount of work, so that whoever calls it can keep
// doing other things between calls: at the most it takes kTokensPerStep
// things from the file's tags or from what it has kept, gives the outline
// in hand kMostCommandsPerStep commands, and fills one shape or one stroke
// (what each line of an outline that is stroked comes to is Stroker's
// bound, and a stroke's dashes are bounded for each outline); of what it
// has kept it reads no more than kReusedPerStep bytes; and it goes through
// the records of what it has kept three times at the most, which are no
// more than the memory given has room for: once to find what a gradient's
// `href` names; twice to style an element and find the gradient it is
// filled with; and for a `use`, which is styled and filled as any element
// is, a third time to find what it names. A piece of a style sheet's text
// is read in the call it comes in.
//
// The file is not trusted. Whatever its bytes, it ends in a status and a
// report. Used by one task. The object is 8,136 bytes on the device: half
// of it what each open group hands on, 124 bytes a group, of which the
// style of a stroke is 52; a thirteenth the stroking; a fifteenth each the
// path data it holds and the reading of a style sheet; a seventeenth the
// reading of a gradient and the ramp a shape is filled with from it; and a
// twentieth the readers of what is being used again.
class SvgRenderer {
 public:
  // The most groups that can be open at once. One more is passed over
  // with everything in it, and reported.
  static constexpr std::size_t kDeepest = 32;
  // A sixteenth of a pixel: how far a line may stand from its curve.
  // Finer than the flattener's own, because the edges here are gray: a
  // line a quarter of a pixel off changes a pixel by up to a quarter.
  static constexpr Fixed kDefaultTolerance = 256;
  static constexpr std::size_t kHeldPathData = 512;
  static constexpr std::uint32_t kTokensPerStep = 16;
  // How many commands of an outline's data one call asks the path reader
  // for. And the most one call can give an outline: those and one more,
  // which is the path reader's own allowance, and two that the end of a
  // tag can complete, the last of a polygon's points and its closing.
  static constexpr std::uint32_t kCommandsPerStep = 16;
  static constexpr std::uint32_t kMostCommandsPerStep = kCommandsPerStep + 3;
  // The most lines one call can give the canvas. Each command becomes
  // kMostLinesPerArc lines at the most. If the outline is stroked, each of
  // those becomes Stroker's kMostLinesPerLine, a command can end a part of
  // the outline, and every dash the outline may have can begin and end
  // in the one call.
  static constexpr std::uint64_t kMostLinesPerStep =
      (std::uint64_t{kMostCommandsPerStep} *
       ((std::uint64_t{PathFlattener::kMostLinesPerArc} * Stroker::kMostLinesPerLine) + Stroker::kMostLinesPerEnd)) +
      (std::uint64_t{Stroker::kMostDashesPerOutline} * (Stroker::kMostLinesPerLine + Stroker::kMostLinesPerEnd));
  // The most bytes of definitions that are gone through again in one pass,
  // all told: a megabyte, which is twelve times what the most of any
  // drawing in the books comes to. And how deep one `use` may be inside
  // what another draws.
  static constexpr std::uint32_t kMostReused = 1048576;
  static constexpr std::size_t kDeepestUse = 4;
  // The longest `id` a `use` can name.
  static constexpr std::size_t kLongestId = 64;
  // How many elements with an `id` can be open at once in `defs`, one
  // inside another. One inside more than that is not kept as something to
  // use, though what it is inside is.
  static constexpr std::size_t kDeepestNamed = 8;
  // The most bytes of what is kept that one call reads.
  static constexpr std::size_t kReusedPerStep = 1024;
  // The most memory for definitions that is used: places in it are kept
  // as numbers of four bytes.
  static constexpr std::size_t kMostKept = 16777216;
  // What keeping one element with an `id` costs, beside its own bytes.
  // A rule of a style sheet and a gradient cost that too, and the bytes of
  // what they say.
  static constexpr std::size_t kBytesPerDefinition = 8;
  // The most stops of a gradient that are kept: the rest are left out, and
  // reported. A gradient in the books has five at the most.
  static constexpr std::size_t kMostStops = GrayRamp::kMostStops;
  // The most bytes of an element's `class` that are looked at, and of what
  // one rule of a style sheet keeps of its selectors and of what it sets.
  // A rule that needs more is left out, and reported.
  static constexpr std::size_t kLongestClasses = 64;
  static constexpr std::size_t kLongestRule = 256;
  // How many times one call can look through the records of what is kept:
  // for a `use` that a style sheet's rules may style and that is filled
  // with a gradient, once for each of those and once for what it names.
  static constexpr std::uint32_t kMostLooksPerStep = 3;

  SvgRenderer() noexcept;
  ~SvgRenderer() = default;
  // It holds pointers into itself, so there is no copying one.
  SvgRenderer(const SvgRenderer&) = delete;
  SvgRenderer& operator=(const SvgRenderer&) = delete;

  // Gives it memory to keep definitions in: what a file's `defs` holds,
  // for `use` to draw. It is the caller's, and has to last as long as the
  // renderer is used; each pass begins it afresh. With none, or with too
  // little, what did not fit is not drawn where it is used, and the report
  // says so. The `defs` of a drawing take the length of what is in them,
  // to the end of the tag that closes them, and kBytesPerDefinition for
  // each element in them that has an `id`. No more than kMostKept bytes
  // of the memory are used. It is given before a pass begins: given in
  // the middle of one, it begins empty, and what the pass has yet to use
  // of what it kept before is not drawn.
  void KeepDefinitionsIn(std::span<std::uint8_t> block) noexcept {
    block_ = block.first(std::min(block.size(), kMostKept));
    keptBytes_ = 0;
    recordBytes_ = 0;
    records_ = 0;
    rules_ = 0;
  }

  // Where the picture goes among the strip's pixels: the drawing, fitted
  // to `width` by `height` (Begin()), is then put through `matrix`. A
  // picture turned to go into a panel's rows, or moved to the middle of a
  // page, is drawn so. An `svg` inside another is cut off at its edges if
  // they still run along rows and columns of pixels, as they do after a
  // quarter turn. Kept from one pass to the next; nothing, until given.
  void PlaceOn(const Matrix& matrix) noexcept { place_ = matrix; }

  // The thinnest a stroke is drawn, in pixels: one that would come out
  // thinner than this, where its shape's matrix makes it thinnest, is drawn
  // that wide, and counted in the report. A stroke far under a pixel comes
  // out a light gray, which a panel of few grays shows broken. Nothing, the
  // default, draws every stroke as wide as it is. Kept from one pass to the
  // next.
  void SetThinnestStroke(Fixed pixels) noexcept { thinnest_ = std::max<Fixed>(pixels, 0); }

  // Begins a pass over a file. The whole drawing is `width` by `height`
  // pixels, and it is drawn into `canvas`: a Filler draws the part of it
  // in its strip there, over what the strip holds, and a SpoolWriter keeps
  // all of it to be drawn later (draw/spool.h).
  //
  // With no canvas nothing is drawn and no outline is read: the pass finds
  // the drawing's size, which is known as soon as its first tag has been
  // read, and what a report can say without drawing.
  void Begin(Canvas* canvas, Fixed width, Fixed height, Fixed tolerance = kDefaultTolerance) noexcept;

  // Takes bytes from the front of `bytes`, and returns how many it took:
  // the rest are for the next call. It may take none and still have
  // worked, on path data it was holding or on something a `use` draws
  // again, so call it until the bytes are gone. Once the file has gone
  // wrong it takes whatever it is given.
  [[nodiscard]] std::size_t Step(std::string_view bytes) noexcept;

  // Says that the file has ended.
  SvgStatus Finish() noexcept;

  SvgStatus Status() const noexcept { return status_; }
  // How many bytes had been read when the tags went wrong.
  std::uint32_t ErrorOffset() const noexcept { return tags_.ErrorOffset(); }
  const SvgSize& Size() const noexcept { return size_; }
  const SvgReport& Report() const noexcept { return report_; }

 private:
  enum class Element : std::uint8_t {
    kIgnored,  // Inside something passed over, or passed over itself.
    kSvg,
    kGroup,
    kPath,
    kRect,
    kCircle,
    kEllipse,
    kPolygon,
    kPolyline,
    kLine,
    kUse,
  };

  // What an attribute is, to the element it is on.
  enum class Attribute : std::uint8_t {
    kIgnored,
    kPathData,  // `d`, or `points`.
    kStyle,
    kTransform,
    kViewBox,
    kFit,
    kLength,    // One of the lengths below: `slot_` says which.
    kProperty,  // Something `style` could also say: `property_` says which.
    kId,        // `id`, of an element that is being kept.
    kHref,      // What a `use` names.
    kClass,     // `class`, for a style sheet's rules.
    kPaint,     // Something a gradient or one of its stops says: `paintSlot_` says what.
  };

  // What a gradient, or a stop of one, says.
  enum class PaintSlot : std::uint8_t { kName, kLink, kX1, kY1, kX2, kY2, kUnits, kTransform, kSpread, kOffset };
  // What is being read that a gradient is made of.
  enum class PaintElement : std::uint8_t { kNone, kGradient, kStop };

  // The lengths an element can have, each in its place in `lengths_`. A
  // `line`'s ends, `x1`, `y1`, `x2` and `y2`, are in the places of `x`,
  // `y`, `width` and `height`, which a percentage is of the same thing in.
  enum Slot : std::uint8_t { kX, kY, kWidth, kHeight, kRadiusX, kRadiusY, kMiddleX, kMiddleY, kRadius, kSlots };

  enum class Property : std::uint8_t {
    kNone,
    kFill,
    kFillRule,
    kFillOpacity,
    kOpacity,
    kColor,
    kDisplay,
    kVisibility,
    kStroke,
    kStrokeWidth,
    kStrokeOpacity,
    kStrokeJoin,
    kStrokeCap,
    kMiterLimit,
    kDashArray,
    kDashOffset,
    kClipPath,
    kMask,
    kFilter,
    kMarker,
    kOverflow,
    kStopColor,
    kStopOpacity,
  };
  static constexpr std::size_t kProperties = static_cast<std::size_t>(Property::kStopOpacity) + 1;

  // What an element hands on to what is inside it.
  struct Level {
    Matrix matrix;         // From its coordinates to pixels.
    Fixed viewWidth = 0;   // What a percentage is of,
    Fixed viewHeight = 0;  // across and down.
    SvgPaint fill;
    std::uint8_t color = 0;  // `color`, as a gray, and how solid it is: what "currentColor" means.
    std::uint8_t colorAlpha = 255;
    std::uint8_t fillOpacity = 255;
    std::uint8_t opacity = 255;  // Every `opacity` from here outwards, multiplied together.
    bool evenOdd = false;
    bool hidden = false;  // `visibility`.
    SvgPaint stroke = {.kind = SvgPaint::Kind::kNone};
    std::uint8_t strokeOpacity = 255;
    StrokeStyle strokeStyle;
    // What is inside is cut off at the edges of a box, in pixels: an `svg`
    // inside another does that, and so everything within it.
    bool boxed = false;
    Point boxFrom;
    Point boxTo;
    // The gradient `fill` is, if it is one: where its record ends in what is
    // kept. Nothing if it is not one.
    std::uint32_t gradient = 0;
  };

  // What a shape is filled with, and within what.
  struct Filling {
    FillRule rule = FillRule::kNonZero;
    std::uint8_t gray = 0;
    std::uint8_t alpha = 0;
    bool boxed = false;
    Point boxFrom;
    Point boxTo;
    // With a gradient: its record, and what it is laid out by.
    std::uint32_t gradient = 0;
    Matrix matrix;
    Fixed viewWidth = 0;
    Fixed viewHeight = 0;
  };

  // A gradient as its tags are read, before it is kept.
  struct Gradient {
    std::array<SvgLength, 4> ends{};  // x1, y1, x2, y2.
    Matrix transform;
    std::array<std::uint16_t, kMostStops> offsets{};  // In 65,535ths of the way.
    std::array<std::uint8_t, kMostStops> grays{};
    std::array<std::uint8_t, kMostStops> alphas{};
    std::array<char, kLongestId> name{};
    std::array<char, kLongestId> link{};  // What its `href` names.
    std::uint8_t nameSize = 0;
    std::uint8_t linkSize = 0;
    std::uint8_t stops = 0;
    std::uint8_t set = 0;  // A bit for each of the ends, its units, transform and spread that it says itself.
    bool userSpace = false;
    GrayRamp::Spread spread = GrayRamp::Spread::kPad;
  };

  // A record of what is kept, as its last eight bytes say: the first four
  // its kind in the top byte and a number in the rest, and the second four
  // another number. A definition is those eight bytes and no more: where
  // its element begins, and where its name is. A rule or a gradient has the
  // number of bytes of what it says below them.
  enum class Record : std::uint8_t { kDefinition, kRule, kGradient };
  struct Kept {
    std::span<const std::uint8_t> said;
    std::uint32_t number = 0;
    std::uint32_t other = 0;
    std::uint32_t top = 0;     // Where the record ends, one past its last byte, which is what it is known by;
    std::uint32_t bottom = 0;  // and where it begins, which is where the one made after it ends.
    Record kind = Record::kDefinition;
  };

  // Something kept that a `use` is drawing again.
  struct Reuse {
    SvgTagReader reader;         // Reads it, from where it begins in what was kept.
    std::uint32_t at = 0;        // How far that reading has got.
    std::uint32_t open = 0;      // How many of its elements are open.
    std::uint32_t levels = 0;    // How many groups were open when it began,
    std::uint32_t skipping = 0;  // and what the file's own reading was passing over.
    bool over = false;           // It has ended, and waits for one it used to end.
  };

  // An element in `defs` that has an `id` and is not closed yet: where it
  // begins among the bytes kept, and where its name does.
  struct OpenId {
    std::uint32_t at = 0;
    std::uint32_t idAt = 0;
    std::uint32_t skipping = 0;  // How deep in `defs` it is.
  };

  // Where lines go when nothing is being drawn.
  class Nowhere final : public LineSink {
   public:
    void MoveTo(Point /*to*/) noexcept override {}
    void LineTo(Point /*to*/) noexcept override {}
    void Close() noexcept override {}
  };

  void Note(SvgLeftOut what) noexcept { report_.leftOut |= static_cast<std::uint32_t>(what); }
  const Level& Parent() const noexcept { return levels_[depth_]; }

  static Property PropertyOf(std::string_view name) noexcept;

  // What the tags say, one thing at a time. TakeValue() returns how much
  // of the piece it used, and Opened() whether it drew a shape or began
  // to.
  void Open(std::string_view name) noexcept;
  void BeginAttribute(std::string_view name) noexcept;
  std::size_t TakeValue(std::string_view piece, bool ofInput) noexcept;
  void EndAttribute() noexcept;
  bool Opened(bool empty) noexcept;
  bool OpenedElement(bool empty) noexcept;
  bool Close() noexcept;
  // Does what one thing from the tags says. `taken` is how many bytes it
  // was read from, and is made less if some of them are handed back.
  // Returns whether that is enough work for one call.
  bool Handle(SvgTagReader& reader, const SvgTagToken& token, std::size_t& taken) noexcept;

  // What is kept. Keep() copies bytes of `defs` as they go by; Define()
  // notes that an element began at `at` among them whose name is written
  // at `idAt`; Find() says where the element began that has the name a
  // `use` gave.
  void BeginKeeping() noexcept;
  void Keep(std::string_view bytes) noexcept;
  void Define(std::uint32_t at, std::uint32_t idAt) noexcept;
  bool Find(std::uint32_t& at) const noexcept;
  // Puts a record below the last, if there is room, and says so.
  bool AddRecord(Record kind, std::uint32_t number, std::uint32_t other, std::span<const std::uint8_t> said) noexcept;
  // The record that ends at `top`: false if none does. The first ends at
  // the memory's end, and each begins where the next one made ends, so
  // going down from there goes through them in the order they were made.
  bool RecordAt(std::uint32_t top, Kept& kept) const noexcept;
  // Where the gradient's record ends that has the name, or nothing.
  std::uint32_t FindGradient(std::string_view name) const noexcept;
  // A gradient as a record says it, and back.
  static std::size_t Pack(const Gradient& gradient, std::span<std::uint8_t> said) noexcept;
  static bool Unpack(std::span<const std::uint8_t> said, Gradient& gradient) noexcept;
  // A `use`: begins the drawing again of what it names. And the drawing
  // again itself, a few things at a call, and its end.
  bool OpenUse(bool empty) noexcept;
  void ReuseSome() noexcept;
  void EndReuse() noexcept;

  // A style sheet: its text, a piece at a time, and what it comes to for
  // the element whose tag is ending. With `keep`, the piece is also kept
  // among what `defs` holds, each part of it as it is read. Whether a
  // token is text to be kept so: a sheet's, in `defs`, as the file has it.
  bool IsSheetTextToKeep(const SvgTagToken& token) const noexcept;
  void ReadSheet(std::string_view piece, bool keep) noexcept;
  void EndRule() noexcept;
  void Style() noexcept;
  // A gradient, and each of its stops, as their tags end; and what one
  // comes to for a shape's pixels. Each says whether it looked through what
  // is kept.
  void SetPaint(std::string_view value) noexcept;
  void AddStop() noexcept;
  bool EndGradient() noexcept;
  bool Ramp(const Filling& filling) noexcept;
  // What a `fill` of `url(...)` names, if it does.
  void TakeFillName(std::string_view value) noexcept;

  std::size_t TakePathData(std::string_view piece, bool ofInput) noexcept;
  void TakeStyle(std::string_view piece) noexcept;
  void EndDeclaration() noexcept;
  void SetProperty(Property property, std::string_view value, bool fromStyle) noexcept;
  void SetStrokeProperty(Property property, std::string_view value, bool inherit) noexcept;
  void SetLength(std::string_view value) noexcept;

  // Each says whether what is inside the element is to be drawn.
  bool OpenRoot() noexcept;
  bool OpenInnerSvg() noexcept;
  // Whether the shape in hand shows: `gray` is then what it is filled
  // with, and `alpha` how solid.
  bool Shows(std::uint8_t& gray, std::uint8_t& alpha) const noexcept;
  // Puts the box an inner `svg` cuts things off at into `pending_`.
  void BoxIn(const SvgBox& space) noexcept;
  // From the coordinates of the element in hand to pixels.
  Matrix ElementMatrix() noexcept;
  bool DrawShape() noexcept;
  // Whether the shape in hand has a stroke that shows, and what it is
  // filled with.
  bool StrokeShows(Filling& filling) const noexcept;
  // An outline goes to the flattener between BeginOutline(), or
  // BeginStroke() for its stroke, and FillOutline(); one that came as path
  // data is ended by EndOutline() first.
  void BeginOutline(const Matrix& matrix) noexcept;
  void BeginStroke(const Matrix& matrix, const StrokeStyle& style) noexcept;
  void StartReading() noexcept;
  void EndOutline() noexcept;
  void FillOutline(bool shown, const Filling& filling) noexcept;
  void Drain() noexcept;
  // The stroke of the shape whose fill the call before filled: whether
  // there is one, and drawing it.
  void StrokeLater(bool stroked, const Filling& stroke) noexcept;
  void StrokeNext() noexcept;
  // Brings the report's counts up to what the outline in hand has come to.
  void Tally() noexcept;
  // The outline of a `rect`, `circle`, `ellipse` or `line`, from its
  // lengths, to the flattener. Each says whether there was anything to
  // outline.
  bool HasOutline() const noexcept;
  void Outline() noexcept;
  void OutlineRect() noexcept;
  void OutlineEllipse(Fixed middleX, Fixed middleY, Fixed radiusX, Fixed radiusY) noexcept;
  Fixed LengthAt(Slot slot, Fixed otherwise) const noexcept;

  SvgTagReader tags_;
  Canvas* canvas_ = nullptr;
  Nowhere nowhere_;
  PathFlattener flattener_;
  SvgPathReader reader_;
  // The gray of each pixel of a shape filled with a gradient, made as it is
  // filled; and a gradient being read.
  GrayRamp ramp_;
  Gradient gradient_;
  Fixed width_ = 0;
  Fixed height_ = 0;
  Fixed tolerance_ = kDefaultTolerance;
  Matrix place_;        // Where the fitted picture goes (PlaceOn()).
  Fixed thinnest_ = 0;  // The thinnest stroke, in pixels (SetThinnestStroke()).
  SvgStatus status_ = SvgStatus::kOk;
  SvgReport report_;
  SvgSize size_;

  // levels_[0] is what a drawing starts from, and each group that is open
  // has the next. `depth_` is how many are open.
  std::array<Level, kDeepest + 1> levels_{};
  std::uint32_t depth_ = 0;
  std::uint32_t skipping_ = 0;  // How deep in something passed over; 0 if not in one.
  bool rootSeen_ = false;

  // What is kept of the file, in the caller's memory: the bytes of `defs`
  // from its start, and from its end backwards a record of each element
  // among them that has an `id`, each rule of a style sheet and each
  // linear gradient (see Kept).
  std::span<std::uint8_t> block_;
  std::uint32_t keptBytes_ = 0;
  std::uint32_t recordBytes_ = 0;
  std::uint32_t records_ = 0;
  std::uint32_t rules_ = 0;
  std::uint32_t keepingFrom_ = 0;  // Where in the file the bytes kept would begin, had they all been in one run.
  bool keeping_ = false;           // Inside `defs`.
  bool blockFull_ = false;
  std::array<OpenId, kDeepestNamed> openIds_{};
  std::uint32_t openIdCount_ = 0;
  // What is being drawn again, one inside another.
  std::array<Reuse, kDeepestUse> reuses_{};
  std::uint32_t reusing_ = 0;

  // A style sheet being read, and the rule in hand: its selectors, each a
  // byte for its kind and one for its length, then the name; then what it
  // sets, each a byte for the property and one for the length of the
  // value, then the value. The first byte is how many selectors there are,
  // and the byte after them how many things it sets.
  SvgSheetReader sheet_;
  std::array<std::uint8_t, kLongestRule> rule_{};
  std::uint32_t ruleSize_ = 0;
  std::uint32_t settingsAt_ = 0;     // Where the count of what it sets is: nothing, until the first.
  std::uint32_t sheetSkipping_ = 0;  // How deep in what is passed over its text is.
  bool openingSheet_ = false;
  bool inSheet_ = false;
  bool shapeSeen_ = false;  // A shape has come before it.
  bool ruleTooLong_ = false;

  // A gradient being read (`gradient_`, above), and a stop of it.
  std::uint32_t gradientSkipping_ = 0;  // How deep in what is passed over its stops are.
  PaintElement paintElement_ = PaintElement::kNone;
  PaintSlot paintSlot_ = PaintSlot::kName;
  bool building_ = false;
  std::uint8_t stopGray_ = 0;
  std::uint16_t stopOffset_ = 0;
  std::uint8_t stopAlpha_ = 255;
  std::uint8_t stopOpacity_ = 255;

  // The element whose tag is being read.
  Element element_ = Element::kIgnored;
  Level pending_;
  SvgFit fit_;
  SvgBox box_;
  Matrix own_;  // Its `transform`.
  bool hasBox_ = false;
  bool hasTransform_ = false;
  bool unreadable_ = false;  // An attribute that says where it is could not be read: it is not drawn.
  bool displayNone_ = false;
  bool lateTransform_ = false;
  bool isInnerSvg_ = false;
  bool isDefs_ = false;
  bool overflows_ = false;  // `overflow` says not to cut off what is inside it.
  // Its `id`, if it is in `defs`: where that is written among the bytes
  // kept, how long it is, and whether it is written there letter for
  // letter, with no entity standing for any of it.
  bool hasId_ = false;
  bool idPlain_ = true;
  std::uint32_t idAt_ = 0;
  std::uint32_t idLength_ = 0;
  std::uint32_t tagAt_ = 0;  // Where the element begins among the bytes kept.
  // What a `use` names, and the size it gives what it draws.
  std::array<char, kLongestId> wanted_{};
  std::uint32_t wantedSize_ = 0;
  bool hasWanted_ = false;
  // Its name and its classes, for a style sheet; and what its `fill` names.
  std::array<char, 32> elementName_{};
  std::uint8_t elementNameSize_ = 0;
  std::array<char, kLongestClasses> classes_{};
  std::uint8_t classesSize_ = 0;
  std::array<char, kLongestId> fillName_{};
  std::uint8_t fillNameSize_ = 0;
  // The element being read is the one a `use` named. It takes its width
  // and height from the `use`, where that has them and it is an `svg`.
  bool firstOfUse_ = false;
  bool useHasWidth_ = false;
  bool useHasHeight_ = false;
  Fixed useWidth_ = 0;
  Fixed useHeight_ = 0;
  std::uint8_t ownOpacity_ = 255;
  std::uint32_t fromStyle_ = 0;   // A bit for each Property that `style` has set.
  std::uint16_t lengthsSet_ = 0;  // A bit for each Slot that was written.
  std::array<Fixed, kSlots> lengths_{};

  // The attribute being read.
  Attribute attribute_ = Attribute::kIgnored;
  Property property_ = Property::kNone;
  Slot slot_ = kX;
  bool valueTooLong_ = false;
  std::uint16_t valueSize_ = 0;
  std::array<char, 192> value_{};
  // Within `style`: the name of the property being read, and whether its
  // colon has gone by.
  std::array<char, 32> name_{};
  std::uint8_t nameSize_ = 0;
  std::uint8_t brackets_ = 0;
  bool nameTooLong_ = false;
  bool inDeclarationValue_ = false;

  // The outline in hand. Its data is held until its tag ends, if it is
  // short enough, and then gone through a few commands a call (draining).
  // If it is too long to hold it is gone through as it arrives
  // (streaming), with what transform has been read by then.
  // The few bytes over are for a piece of the data that cannot be handed
  // back to the tags once the rest is full: what an entity stood for.
  std::array<char, kHeldPathData + 16> held_{};
  std::uint16_t heldSize_ = 0;
  std::uint16_t heldAt_ = 0;
  bool hasPathData_ = false;
  bool streaming_ = false;
  bool draining_ = false;
  bool outlineBad_ = false;     // The outline has been given up, and what is left of its data is not read.
  bool closesOutline_ = false;  // A polygon: its outline is closed when its points end.
  // What an outline whose tag has ended, and whose data is still being
  // gone through, is to be filled with.
  Filling drain_;
  // A stroke: the outline in hand goes through the stroker, `flattener_`
  // having no matrix and giving its lines to `stroker_`, which gives what
  // it makes to `strokeOut_`, which has the matrix. A shape's stroke waits
  // for the call after its fill (`strokeNext_`), with what it is filled
  // with and the matrix. An outline stroked as its data arrives keeps the
  // style it began with, to see that it has not changed.
  Stroker stroker_;
  PathFlattener strokeOut_;
  bool stroking_ = false;
  bool strokeNext_ = false;
  bool widening_ = false;  // The stroke in hand is drawn wider, to the thinnest asked for.
  Filling strokeFilling_;
  Matrix strokeMatrix_;
  StrokeStyle streamStyle_;
  // How much of the outline in hand the report has counted.
  std::uint32_t talliedCommands_ = 0;
  std::uint32_t talliedLines_ = 0;
};

}  // namespace bn::draw
