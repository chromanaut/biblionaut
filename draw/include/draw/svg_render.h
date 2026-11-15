#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "draw/fill.h"
#include "draw/fixed.h"
#include "draw/flatten.h"
#include "draw/svg_path.h"
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
  kUse = 1U << 2U,              // `use`: not drawn.
  kStyleSheet = 1U << 3U,       // `style`: not read, so nothing is painted as it says.
  kStroke = 1U << 4U,           // A line along an outline: not drawn.
  kPaintServer = 1U << 5U,      // A gradient or a pattern: not drawn, or what was written to stand in for it.
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
  kViewportClip = 1U << 19U,    // An `svg` inside another: drawn, and not cut off at its edges.
  kSwitch = 1U << 20U,          // `switch`: everything in it is drawn, not the first that suits.
  kUnknownEntity = 1U << 21U,   // An entity the file declares for itself: taken as nothing.
};

// What a bit of a report says, in a word or two: "text", "a stroke".
const char* SvgLeftOutName(SvgLeftOut what) noexcept;

// What became of a file.
struct SvgReport {
  std::uint32_t leftOut = 0;   // A bit of SvgLeftOut for each thing met.
  std::uint32_t elements = 0;  // Elements there were.
  std::uint32_t shapes = 0;    // Shapes filled.
  std::uint32_t commands = 0;  // Commands their outlines had,
  std::uint32_t lines = 0;     // and lines those became.
  std::uint32_t deepest = 0;   // The most groups open at once.

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
// the memory it takes is this object, whatever the file's size. A file's
// longest attribute, the outline of an engraving of megabytes, goes
// through as it is read.
//
// What it draws, and how:
//
// - The whole drawing is fitted to the picture: its `viewBox`, where
//   `preserveAspectRatio` puts it; or, with no view box, its width and
//   height stretched to the picture's.
// - `svg`, `g`, `a` and `switch` are groups: a `transform`, and paint that
//   what is inside them takes unless it has its own. An `svg` inside
//   another has its own place, size and view box.
// - `path`, `rect` (with rounded corners), `circle`, `ellipse`, `polygon`
//   and `polyline` are shapes, filled by `fill`, `fill-rule`,
//   `fill-opacity` and `opacity`, each written as an attribute or in
//   `style`, where `style` wins. A colour is drawn as a gray.
// - `display` and `visibility` hide things.
// - Everything else is passed over, and Report() says what there was: see
//   SvgLeftOut. A drawing with strokes, text or pictures in it comes out
//   without them.
//
// Attributes may come in any order, with one limit. A `transform` written
// after its own shape's path data is used if the data is no longer than
// kHeldPathData, since data that short is held until the tag ends. Longer
// data is drawn as it arrives, and if a transform then follows it in the
// same tag, the shape is left out and reported.
//
// Step() does a bounded amount of work, so that whoever calls it can keep
// doing other things between calls: at the most it takes kTokensPerStep
// things from the file's tags, gives the outline in hand
// kMostCommandsPerStep commands, and fills one shape.
//
// The file is not trusted. Whatever its bytes, it ends in a status and a
// report. Used by one task. The object is 2,840 bytes on the device, half
// of it what each open group hands on and a fifth the path data it holds.
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

  SvgRenderer() noexcept;
  ~SvgRenderer() = default;
  // It holds pointers into itself, so there is no copying one.
  SvgRenderer(const SvgRenderer&) = delete;
  SvgRenderer& operator=(const SvgRenderer&) = delete;

  // Begins a pass over a file. The whole drawing is `width` by `height`
  // pixels, and the part of it in `filler`'s strip is drawn there, over
  // what the strip holds.
  //
  // With no filler nothing is drawn and no outline is read: the pass finds
  // the drawing's size, which is known as soon as its first tag has been
  // read, and what a report can say without drawing.
  void Begin(Filler* filler, Fixed width, Fixed height, Fixed tolerance = kDefaultTolerance) noexcept;

  // Takes bytes from the front of `bytes`, and returns how many it took:
  // the rest are for the next call. It may take none and still have
  // worked, on path data it was holding, so call it until the bytes are
  // gone. Once the file has gone wrong it takes whatever it is given.
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
  };

  // The lengths an element can have, each in its place in `lengths_`.
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
    kClipPath,
    kMask,
    kFilter,
    kMarker,
  };

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
    bool hidden = false;      // `visibility`.
    bool stroked = false;     // `stroke` is something,
    bool strokeless = false;  // though with no width.
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
  void Close() noexcept;

  std::size_t TakePathData(std::string_view piece, bool ofInput) noexcept;
  void TakeStyle(std::string_view piece) noexcept;
  void EndDeclaration() noexcept;
  void SetProperty(Property property, std::string_view value, bool fromStyle) noexcept;
  void SetLength(std::string_view value) noexcept;

  // Each says whether what is inside the element is to be drawn.
  bool OpenRoot() noexcept;
  bool OpenInnerSvg() noexcept;
  // Whether the shape in hand shows: `gray` is then what it is filled
  // with, and `alpha` how solid.
  bool Shows(std::uint8_t& gray, std::uint8_t& alpha) const noexcept;
  // From the coordinates of the element in hand to pixels.
  Matrix ElementMatrix() noexcept;
  bool DrawShape() noexcept;
  // An outline goes to the flattener between BeginOutline() and
  // FillOutline(); one that came as path data is ended by EndOutline()
  // first.
  void BeginOutline(const Matrix& matrix) noexcept;
  void EndOutline() noexcept;
  void FillOutline(bool shown, FillRule rule, std::uint8_t gray, std::uint8_t alpha) noexcept;
  void Drain() noexcept;
  // Brings the report's counts up to what the outline in hand has come to.
  void Tally() noexcept;
  // Each says whether there was anything to outline.
  bool DrawRect() noexcept;
  bool DrawEllipse(Fixed middleX, Fixed middleY, Fixed radiusX, Fixed radiusY) noexcept;
  Fixed LengthAt(Slot slot, Fixed otherwise) const noexcept;

  SvgTagReader tags_;
  Filler* filler_ = nullptr;
  Nowhere nowhere_;
  PathFlattener flattener_;
  SvgPathReader reader_;
  Fixed width_ = 0;
  Fixed height_ = 0;
  Fixed tolerance_ = kDefaultTolerance;
  SvgStatus status_ = SvgStatus::kOk;
  SvgReport report_;
  SvgSize size_;

  // levels_[0] is what a drawing starts from, and each group that is open
  // has the next. `depth_` is how many are open.
  std::array<Level, kDeepest + 1> levels_{};
  std::uint32_t depth_ = 0;
  std::uint32_t skipping_ = 0;  // How deep in something passed over; 0 if not in one.
  bool rootSeen_ = false;

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
  std::uint8_t ownOpacity_ = 255;
  std::uint16_t fromStyle_ = 0;   // A bit for each Property that `style` has set.
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
  FillRule drainRule_ = FillRule::kNonZero;
  std::uint8_t drainGray_ = 0;
  std::uint8_t drainAlpha_ = 0;
  // How much of the outline in hand the report has counted.
  std::uint32_t talliedCommands_ = 0;
  std::uint32_t talliedLines_ = 0;
};

}  // namespace bn::draw
