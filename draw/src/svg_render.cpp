#include "draw/svg_render.h"

#include <algorithm>

#include "draw/svg_transform.h"

namespace bn::draw {

namespace {

using Kind = SvgTagToken::Kind;

// A part of 255 of a part of 255.
constexpr std::uint8_t Both(std::uint8_t one, std::uint8_t other) noexcept {
  return static_cast<std::uint8_t>(((std::uint32_t{one} * other) + 127U) / 255U);
}

constexpr std::uint16_t BitOf(unsigned index) noexcept { return static_cast<std::uint16_t>(1U << index); }

// What becomes of an element, by its name.
enum class Fate : std::uint8_t {
  kSvg,
  kGroup,
  kPath,
  kRect,
  kCircle,
  kEllipse,
  kPolygon,
  kPolyline,
  kLine,
  kPassedOver,  // Not drawn, nor anything in it; and reported, if `note` is not nothing.
};

struct ElementName {
  std::string_view name;
  Fate fate;
  std::uint32_t note;
};

constexpr std::uint32_t Noted(SvgLeftOut what) noexcept { return static_cast<std::uint32_t>(what); }

// The elements of SVG. Those that only hold what others refer to, or say
// something about the file, are passed over without a word: nothing in
// them is drawn where it stands.
constexpr ElementName kElements[] = {
    {.name = "svg", .fate = Fate::kSvg, .note = 0},
    {.name = "g", .fate = Fate::kGroup, .note = 0},
    {.name = "path", .fate = Fate::kPath, .note = 0},
    {.name = "rect", .fate = Fate::kRect, .note = 0},
    {.name = "circle", .fate = Fate::kCircle, .note = 0},
    {.name = "ellipse", .fate = Fate::kEllipse, .note = 0},
    {.name = "polygon", .fate = Fate::kPolygon, .note = 0},
    {.name = "polyline", .fate = Fate::kPolyline, .note = 0},
    {.name = "line", .fate = Fate::kLine, .note = 0},
    {.name = "a", .fate = Fate::kGroup, .note = 0},
    {.name = "switch", .fate = Fate::kGroup, .note = Noted(SvgLeftOut::kSwitch)},
    {.name = "use", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kUse)},
    {.name = "image", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kImage)},
    {.name = "text", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kText)},
    {.name = "style", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kStyleSheet)},
    {.name = "foreignObject", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kForeign)},
    {.name = "script", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kAnimation)},
    {.name = "animate", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kAnimation)},
    {.name = "animateColor", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kAnimation)},
    {.name = "animateMotion", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kAnimation)},
    {.name = "animateTransform", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kAnimation)},
    {.name = "set", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kAnimation)},
    {.name = "defs", .fate = Fate::kPassedOver, .note = 0},
    {.name = "symbol", .fate = Fate::kPassedOver, .note = 0},
    {.name = "clipPath", .fate = Fate::kPassedOver, .note = 0},
    {.name = "mask", .fate = Fate::kPassedOver, .note = 0},
    {.name = "marker", .fate = Fate::kPassedOver, .note = 0},
    {.name = "pattern", .fate = Fate::kPassedOver, .note = 0},
    {.name = "linearGradient", .fate = Fate::kPassedOver, .note = 0},
    {.name = "radialGradient", .fate = Fate::kPassedOver, .note = 0},
    {.name = "filter", .fate = Fate::kPassedOver, .note = 0},
    {.name = "title", .fate = Fate::kPassedOver, .note = 0},
    {.name = "desc", .fate = Fate::kPassedOver, .note = 0},
    {.name = "metadata", .fate = Fate::kPassedOver, .note = 0},
    {.name = "font", .fate = Fate::kPassedOver, .note = 0},
    {.name = "font-face", .fate = Fate::kPassedOver, .note = 0},
    {.name = "cursor", .fate = Fate::kPassedOver, .note = 0},
    {.name = "view", .fate = Fate::kPassedOver, .note = 0},
    {.name = "color-profile", .fate = Fate::kPassedOver, .note = 0},
};

// An element's name without the "svg:" some files write before it.
std::string_view Plain(std::string_view name) noexcept {
  constexpr std::string_view kPrefix = "svg:";
  if (name.size() > kPrefix.size() && name.starts_with(kPrefix)) {
    name.remove_prefix(kPrefix.size());
  }
  return name;
}

// `value` without an "!important" at its end, which changes nothing here.
std::string_view WithoutImportant(std::string_view value) noexcept {
  constexpr std::string_view kImportant = "!important";
  value = WithoutSpace(value);
  if (value.size() >= kImportant.size() &&
      IsWord({value.data() + (value.size() - kImportant.size()), kImportant.size()}, kImportant)) {
    return WithoutSpace({value.data(), value.size() - kImportant.size()});
  }
  return value;
}

Fixed Sum(Fixed one, Fixed other, bool& clamped) noexcept { return Clamp32(std::int64_t{one} + other, clamped); }

}  // namespace

const char* SvgStatusName(SvgStatus status) noexcept {
  switch (status) {
    case SvgStatus::kOk:
      return "ok";
    case SvgStatus::kNotSvg:
      return "not svg";
    case SvgStatus::kMalformed:
      return "malformed";
    case SvgStatus::kTruncated:
      return "truncated";
  }
  return "unknown";
}

const char* SvgLeftOutName(SvgLeftOut what) noexcept {
  switch (what) {
    case SvgLeftOut::kText:
      return "text";
    case SvgLeftOut::kImage:
      return "a picture";
    case SvgLeftOut::kUse:
      return "use";
    case SvgLeftOut::kStyleSheet:
      return "a style sheet";
    case SvgLeftOut::kStroke:
      return "a stroke";
    case SvgLeftOut::kPaintServer:
      return "a gradient or a pattern";
    case SvgLeftOut::kClip:
      return "a clip path";
    case SvgLeftOut::kMask:
      return "a mask";
    case SvgLeftOut::kFilter:
      return "a filter";
    case SvgLeftOut::kMarker:
      return "a marker";
    case SvgLeftOut::kForeign:
      return "a foreign object";
    case SvgLeftOut::kAnimation:
      return "animation or a script";
    case SvgLeftOut::kUnknownElement:
      return "an unknown element";
    case SvgLeftOut::kTooDeep:
      return "nesting too deep";
    case SvgLeftOut::kLateTransform:
      return "a transform after long path data";
    case SvgLeftOut::kBadValue:
      return "an attribute that could not be read";
    case SvgLeftOut::kBadPath:
      return "path data that goes wrong";
    case SvgLeftOut::kHeldNumber:
      return "a number too large";
    case SvgLeftOut::kGroupOpacity:
      return "opacity on a group";
    case SvgLeftOut::kViewportClip:
      return "the edges of an inner svg";
    case SvgLeftOut::kSwitch:
      return "a switch";
    case SvgLeftOut::kUnknownEntity:
      return "an unknown entity";
  }
  return "unknown";
}

void SvgSize::Natural(Fixed& naturalWidth, Fixed& naturalHeight) const noexcept {
  constexpr Fixed kUsualWidth = 300 * kFixedOne;
  constexpr Fixed kUsualHeight = 150 * kFixedOne;
  bool clamped = false;
  const bool wide = hasWidth && width.unit != SvgUnit::kPercent;
  const bool high = hasHeight && height.unit != SvgUnit::kPercent;
  const bool boxed = hasBox && box.width > 0 && box.height > 0;
  Fixed across = wide ? LengthIn(width, 0, clamped) : 0;
  Fixed down = high ? LengthIn(height, 0, clamped) : 0;
  if ((wide && across <= 0) || (high && down <= 0)) {
    naturalWidth = 0;
    naturalHeight = 0;
    return;
  }
  if (!wide && !high) {
    across = boxed ? box.width : kUsualWidth;
    down = boxed ? box.height : kUsualHeight;
  } else if (!wide) {
    across = boxed ? Clamp32((std::int64_t{down} * box.width) / box.height, clamped) : kUsualWidth;
  } else if (!high) {
    down = boxed ? Clamp32((std::int64_t{across} * box.height) / box.width, clamped) : kUsualHeight;
  }
  naturalWidth = across;
  naturalHeight = down;
}

SvgRenderer::SvgRenderer() noexcept : flattener_(nowhere_, Matrix()), reader_(flattener_) {}

void SvgRenderer::Begin(Filler* filler, Fixed width, Fixed height, Fixed tolerance) noexcept {
  tags_ = SvgTagReader();
  filler_ = (filler != nullptr && filler->Rows() > 0) ? filler : nullptr;
  width_ = width;
  height_ = height;
  tolerance_ = tolerance;
  status_ = SvgStatus::kOk;
  report_ = SvgReport();
  size_ = SvgSize();
  levels_[0] = Level();
  levels_[0].viewWidth = width;
  levels_[0].viewHeight = height;
  depth_ = 0;
  skipping_ = 0;
  rootSeen_ = false;
  element_ = Element::kIgnored;
  attribute_ = Attribute::kIgnored;
  streaming_ = false;
  draining_ = false;
  heldSize_ = 0;
  heldAt_ = 0;
}

std::size_t SvgRenderer::Step(std::string_view bytes) noexcept {
  if (status_ != SvgStatus::kOk) {
    return bytes.size();
  }
  if (draining_) {
    Drain();
    return 0;
  }
  std::size_t taken = 0;
  for (std::uint32_t tokens = 0; tokens < kTokensPerStep && taken < bytes.size(); ++tokens) {
    SvgTagToken token;
    std::size_t used = tags_.Next({bytes.data() + taken, bytes.size() - taken}, token);
    // Whether enough has been done for one call.
    bool enough = false;
    switch (token.kind) {
      case Kind::kNone:
      case Kind::kText:
        break;
      case Kind::kOpen:
        Open(token.text);
        break;
      case Kind::kAttribute:
        BeginAttribute(token.text);
        break;
      case Kind::kValue: {
        const std::size_t wanted = TakeValue(token.text, token.ofInput);
        if (wanted < token.text.size()) {
          tags_.GiveBack(token.text.size() - wanted);
          used -= token.text.size() - wanted;
          enough = true;
        } else if (streaming_) {
          enough = true;
        }
        break;
      }
      case Kind::kOpened:
        enough = Opened(token.empty);
        break;
      case Kind::kClose:
        Close();
        break;
    }
    taken += used;
    if (tags_.Status() != SvgTagStatus::kOk) {
      status_ = SvgStatus::kMalformed;
    }
    if (status_ != SvgStatus::kOk) {
      return bytes.size();
    }
    if (enough || draining_) {
      break;
    }
  }
  return taken;
}

SvgStatus SvgRenderer::Finish() noexcept {
  // A shape the file ended in the middle of is not drawn.
  if (filler_ != nullptr) {
    filler_->Fill(FillRule::kNonZero, 0, 0);
  }
  draining_ = false;
  streaming_ = false;
  if (tags_.UnknownEntities() > 0) {
    Note(SvgLeftOut::kUnknownEntity);
  }
  if (status_ == SvgStatus::kOk) {
    const SvgTagStatus tags = tags_.Finish();
    if (!rootSeen_) {
      status_ = SvgStatus::kNotSvg;
    } else if (tags == SvgTagStatus::kTruncated) {
      status_ = SvgStatus::kTruncated;
    } else if (tags != SvgTagStatus::kOk) {
      status_ = SvgStatus::kMalformed;
    }
  }
  return status_;
}

// ---- What the tags say ----

void SvgRenderer::Open(std::string_view name) noexcept {
  ++report_.elements;
  element_ = Element::kIgnored;
  attribute_ = Attribute::kIgnored;
  fit_ = SvgFit();
  own_ = Matrix();
  hasBox_ = false;
  box_ = SvgBox();
  hasTransform_ = false;
  unreadable_ = false;
  displayNone_ = false;
  lateTransform_ = false;
  isInnerSvg_ = false;
  ownOpacity_ = 255;
  fromStyle_ = 0;
  lengthsSet_ = 0;
  heldSize_ = 0;
  heldAt_ = 0;
  hasPathData_ = false;
  streaming_ = false;
  outlineBad_ = false;
  closesOutline_ = false;
  name = Plain(name);
  if (skipping_ > 0) {
    // Something that moves is reported wherever it is: inside a shape, it
    // is the shape that would have moved.
    for (const ElementName& known : kElements) {
      if (name == known.name && known.note == Noted(SvgLeftOut::kAnimation)) {
        report_.leftOut |= known.note;
      }
    }
    return;
  }

  if (!rootSeen_) {
    rootSeen_ = true;
    if (name != "svg") {
      status_ = SvgStatus::kNotSvg;
      return;
    }
    element_ = Element::kSvg;
    pending_ = Parent();
    return;
  }
  if (depth_ == 0) {
    return;  // After the drawing's own element has ended: not part of it.
  }

  Fate fate = Fate::kPassedOver;
  // An element from some other vocabulary, which has a prefix, is passed
  // over without a word. One with a plain name that SVG does not have is
  // reported.
  std::uint32_t note = name.find(':') == std::string_view::npos ? Noted(SvgLeftOut::kUnknownElement) : 0;
  for (const ElementName& known : kElements) {
    if (name == known.name) {
      fate = known.fate;
      note = known.note;
      break;
    }
  }
  report_.leftOut |= note;
  switch (fate) {
    case Fate::kSvg:
      element_ = Element::kSvg;
      isInnerSvg_ = true;
      break;
    case Fate::kGroup:
      element_ = Element::kGroup;
      break;
    case Fate::kPath:
      element_ = Element::kPath;
      break;
    case Fate::kRect:
      element_ = Element::kRect;
      break;
    case Fate::kCircle:
      element_ = Element::kCircle;
      break;
    case Fate::kEllipse:
      element_ = Element::kEllipse;
      break;
    case Fate::kPolygon:
      element_ = Element::kPolygon;
      break;
    case Fate::kPolyline:
      element_ = Element::kPolyline;
      break;
    case Fate::kLine:
      element_ = Element::kLine;
      break;
    case Fate::kPassedOver:
      return;
  }
  pending_ = Parent();
}

void SvgRenderer::BeginAttribute(std::string_view name) noexcept {
  EndAttribute();
  valueSize_ = 0;
  valueTooLong_ = false;
  if (element_ == Element::kIgnored) {
    return;
  }

  const bool outlined = element_ == Element::kPath || element_ == Element::kPolygon || element_ == Element::kPolyline;
  if (outlined && name == (element_ == Element::kPath ? "d" : "points")) {
    // A second one in the same tag, which XML does not allow, is ignored.
    if (!hasPathData_) {
      hasPathData_ = true;
      attribute_ = Attribute::kPathData;
    }
    return;
  }
  if (name == "style") {
    attribute_ = Attribute::kStyle;
    nameSize_ = 0;
    nameTooLong_ = false;
    inDeclarationValue_ = false;
    brackets_ = 0;
    return;
  }
  if (name == "transform") {
    // An `svg` has no transform of its own in the SVG the books are
    // written in.
    if (element_ != Element::kSvg) {
      attribute_ = Attribute::kTransform;
    }
    return;
  }
  if (element_ == Element::kSvg && name == "viewBox") {
    attribute_ = Attribute::kViewBox;
    return;
  }
  if (element_ == Element::kSvg && name == "preserveAspectRatio") {
    attribute_ = Attribute::kFit;
    return;
  }

  // The lengths that say where a shape is.
  struct LengthName {
    std::string_view name;
    Slot slot;
    bool ofBoxes;   // `svg` and `rect` have it,
    bool ofRounds;  // or `circle` and `ellipse`.
  };
  constexpr LengthName kLengths[] = {
      {.name = "x", .slot = kX, .ofBoxes = true, .ofRounds = false},
      {.name = "y", .slot = kY, .ofBoxes = true, .ofRounds = false},
      {.name = "width", .slot = kWidth, .ofBoxes = true, .ofRounds = false},
      {.name = "height", .slot = kHeight, .ofBoxes = true, .ofRounds = false},
      {.name = "rx", .slot = kRadiusX, .ofBoxes = true, .ofRounds = true},
      {.name = "ry", .slot = kRadiusY, .ofBoxes = true, .ofRounds = true},
      {.name = "cx", .slot = kMiddleX, .ofBoxes = false, .ofRounds = true},
      {.name = "cy", .slot = kMiddleY, .ofBoxes = false, .ofRounds = true},
      {.name = "r", .slot = kRadius, .ofBoxes = false, .ofRounds = true},
  };
  const bool box = element_ == Element::kSvg || element_ == Element::kRect;
  const bool round = element_ == Element::kCircle || element_ == Element::kEllipse;
  for (const LengthName& length : kLengths) {
    if (name == length.name && ((box && length.ofBoxes) || (round && length.ofRounds))) {
      attribute_ = Attribute::kLength;
      slot_ = length.slot;
      return;
    }
  }

  property_ = PropertyOf(name);
  if (property_ != Property::kNone) {
    attribute_ = Attribute::kProperty;
  }
}

SvgRenderer::Property SvgRenderer::PropertyOf(std::string_view name) noexcept {
  struct PropertyName {
    std::string_view name;
    Property property;
  };
  constexpr PropertyName kProperties[] = {
      {.name = "fill", .property = Property::kFill},
      {.name = "fill-rule", .property = Property::kFillRule},
      {.name = "fill-opacity", .property = Property::kFillOpacity},
      {.name = "opacity", .property = Property::kOpacity},
      {.name = "color", .property = Property::kColor},
      {.name = "display", .property = Property::kDisplay},
      {.name = "visibility", .property = Property::kVisibility},
      {.name = "stroke", .property = Property::kStroke},
      {.name = "stroke-width", .property = Property::kStrokeWidth},
      {.name = "clip-path", .property = Property::kClipPath},
      {.name = "mask", .property = Property::kMask},
      {.name = "filter", .property = Property::kFilter},
      {.name = "marker", .property = Property::kMarker},
      {.name = "marker-start", .property = Property::kMarker},
      {.name = "marker-mid", .property = Property::kMarker},
      {.name = "marker-end", .property = Property::kMarker},
  };
  for (const PropertyName& known : kProperties) {
    if (name == known.name) {
      return known.property;
    }
  }
  return Property::kNone;
}

std::size_t SvgRenderer::TakeValue(std::string_view piece, bool ofInput) noexcept {
  switch (attribute_) {
    case Attribute::kIgnored:
      break;
    case Attribute::kPathData:
      return TakePathData(piece, ofInput);
    case Attribute::kStyle:
      TakeStyle(piece);
      break;
    case Attribute::kTransform:
    case Attribute::kViewBox:
    case Attribute::kFit:
    case Attribute::kLength:
    case Attribute::kProperty:
      // Short enough to hold whole, or not one of these at all.
      if (piece.size() > value_.size() - valueSize_) {
        valueTooLong_ = true;
      } else {
        std::copy(piece.begin(), piece.end(), value_.begin() + valueSize_);
        valueSize_ = static_cast<std::uint16_t>(valueSize_ + piece.size());
      }
      break;
  }
  return piece.size();
}

void SvgRenderer::EndAttribute() noexcept {
  const Attribute attribute = attribute_;
  attribute_ = Attribute::kIgnored;
  if (attribute == Attribute::kIgnored || attribute == Attribute::kPathData) {
    return;
  }
  if (attribute == Attribute::kStyle) {
    EndDeclaration();
    return;
  }
  if (valueTooLong_) {
    Note(SvgLeftOut::kBadValue);
    if (attribute == Attribute::kTransform) {
      unreadable_ = true;
    }
    return;
  }

  const std::string_view value(value_.data(), valueSize_);
  bool clamped = false;
  switch (attribute) {
    case Attribute::kTransform:
      // SVG has an element whose transform cannot be read not drawn.
      if (ParseSvgTransform(value, own_, clamped)) {
        hasTransform_ = true;
        // If its outline is already being drawn, it is too late.
        lateTransform_ = streaming_;
      } else {
        unreadable_ = true;
        Note(SvgLeftOut::kBadValue);
      }
      break;
    case Attribute::kViewBox:
      if (ParseSvgViewBox(value, box_, clamped)) {
        hasBox_ = true;
      } else {
        Note(SvgLeftOut::kBadValue);
      }
      break;
    case Attribute::kFit:
      if (!ParseSvgFit(value, fit_)) {
        Note(SvgLeftOut::kBadValue);
      }
      break;
    case Attribute::kLength:
      SetLength(value);
      break;
    case Attribute::kProperty:
      SetProperty(property_, value, false);
      break;
    case Attribute::kIgnored:
    case Attribute::kPathData:
    case Attribute::kStyle:
      break;
  }
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
  }
}

void SvgRenderer::SetLength(std::string_view value) noexcept {
  SvgLength length;
  bool clamped = false;
  if (!ParseSvgLength(value, length, clamped)) {
    Note(SvgLeftOut::kBadValue);
    return;
  }
  if (element_ == Element::kSvg && !isInnerSvg_) {
    // The drawing's own size, which is for whoever asks for it: a
    // percentage of it is of something only they know.
    if (slot_ == kWidth) {
      size_.hasWidth = true;
      size_.width = length;
    } else if (slot_ == kHeight) {
      size_.hasHeight = true;
      size_.height = length;
    }
    return;
  }

  // What a percentage is of: the width, the height, or for a radius that
  // is neither, the diagonal over the square root of two.
  const Level& parent = Parent();
  Fixed whole = 0;
  switch (slot_) {
    case kX:
    case kWidth:
    case kRadiusX:
    case kMiddleX:
      whole = parent.viewWidth;
      break;
    case kY:
    case kHeight:
    case kRadiusY:
    case kMiddleY:
      whole = parent.viewHeight;
      break;
    case kRadius: {
      const auto across = static_cast<std::uint64_t>(std::max<Fixed>(parent.viewWidth, 0));
      const auto down = static_cast<std::uint64_t>(std::max<Fixed>(parent.viewHeight, 0));
      whole = static_cast<Fixed>(SquareRoot(((across * across) + (down * down)) / 2));
      break;
    }
    case kSlots:
      break;
  }
  lengths_[slot_] = LengthIn(length, whole, clamped);
  lengthsSet_ |= BitOf(slot_);
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
  }
}

void SvgRenderer::SetProperty(Property property, std::string_view value, bool fromStyle) noexcept {
  // What `style` says wins over the same thing said as an attribute,
  // whichever comes first in the tag.
  const std::uint16_t bit = BitOf(static_cast<unsigned>(property));
  if (fromStyle) {
    fromStyle_ |= bit;
  } else if ((fromStyle_ & bit) != 0) {
    return;
  }
  value = WithoutImportant(value);
  const Level& parent = Parent();
  const bool inherit = IsWord(value, "inherit");
  const bool none = IsWord(value, "none");
  switch (property) {
    case Property::kNone:
      break;
    case Property::kFill: {
      SvgPaint paint;
      if (!ParseSvgPaint(value, paint)) {
        Note(SvgLeftOut::kBadValue);
      } else {
        pending_.fill = paint.kind == SvgPaint::Kind::kInherit ? parent.fill : paint;
      }
      break;
    }
    case Property::kFillRule:
      if (inherit) {
        pending_.evenOdd = parent.evenOdd;
      } else if (IsWord(value, "evenodd")) {
        pending_.evenOdd = true;
      } else if (IsWord(value, "nonzero")) {
        pending_.evenOdd = false;
      } else {
        Note(SvgLeftOut::kBadValue);
      }
      break;
    case Property::kFillOpacity:
      if (inherit) {
        pending_.fillOpacity = parent.fillOpacity;
      } else if (!ParseSvgOpacity(value, pending_.fillOpacity)) {
        Note(SvgLeftOut::kBadValue);
      }
      break;
    case Property::kOpacity:
      if (!inherit && !ParseSvgOpacity(value, ownOpacity_)) {
        Note(SvgLeftOut::kBadValue);
      }
      break;
    case Property::kColor:
      if (inherit) {
        pending_.color = parent.color;
        pending_.colorAlpha = parent.colorAlpha;
      } else if (!ParseSvgColour(value, pending_.color, pending_.colorAlpha)) {
        Note(SvgLeftOut::kBadValue);
      }
      break;
    case Property::kDisplay:
      displayNone_ = none;
      break;
    case Property::kVisibility:
      if (inherit) {
        pending_.hidden = parent.hidden;
      } else {
        pending_.hidden = IsWord(value, "hidden") || IsWord(value, "collapse");
      }
      break;
    case Property::kStroke: {
      SvgPaint paint;
      if (!ParseSvgPaint(value, paint)) {
        Note(SvgLeftOut::kBadValue);
      } else if (paint.kind == SvgPaint::Kind::kInherit) {
        pending_.stroked = parent.stroked;
      } else {
        pending_.stroked = paint.kind != SvgPaint::Kind::kNone || paint.server;
      }
      break;
    }
    case Property::kStrokeWidth: {
      SvgLength width;
      bool clamped = false;
      if (inherit) {
        pending_.strokeless = parent.strokeless;
      } else if (ParseSvgLength(value, width, clamped)) {
        pending_.strokeless = width.value == 0;
      } else {
        Note(SvgLeftOut::kBadValue);
      }
      break;
    }
    case Property::kClipPath:
      if (!none) {
        Note(SvgLeftOut::kClip);
      }
      break;
    case Property::kMask:
      if (!none) {
        Note(SvgLeftOut::kMask);
      }
      break;
    case Property::kFilter:
      if (!none) {
        Note(SvgLeftOut::kFilter);
      }
      break;
    case Property::kMarker:
      if (!none) {
        Note(SvgLeftOut::kMarker);
      }
      break;
  }
}

// `style` is a list of "name: value", with a semicolon between one and the
// next. It is read as it arrives: the name of the one in hand is in
// `name_`, and its value collects in `value_`.
void SvgRenderer::TakeStyle(std::string_view piece) noexcept {
  for (const char c : piece) {
    if (!inDeclarationValue_) {
      if (c == ':') {
        inDeclarationValue_ = true;
        valueSize_ = 0;
        valueTooLong_ = false;
        brackets_ = 0;
      } else if (c == ';') {
        nameSize_ = 0;
        nameTooLong_ = false;
      } else if (!IsSvgSpace(c)) {
        if (nameSize_ < name_.size()) {
          name_[nameSize_] = c;
          ++nameSize_;
        } else {
          nameTooLong_ = true;
        }
      }
    } else if (c == ';' && brackets_ == 0) {
      EndDeclaration();
    } else {
      // A semicolon inside brackets, as in the address of a picture, does
      // not end the value.
      if (c == '(' && brackets_ < UINT8_MAX) {
        ++brackets_;
      } else if (c == ')' && brackets_ > 0) {
        --brackets_;
      }
      if (valueSize_ < value_.size()) {
        value_[valueSize_] = c;
        ++valueSize_;
      } else {
        valueTooLong_ = true;
      }
    }
  }
}

void SvgRenderer::EndDeclaration() noexcept {
  if (inDeclarationValue_ && !nameTooLong_) {
    const Property property = PropertyOf({name_.data(), nameSize_});
    if (property != Property::kNone && valueTooLong_) {
      Note(SvgLeftOut::kBadValue);
    } else if (property != Property::kNone) {
      SetProperty(property, {value_.data(), valueSize_}, true);
    }
  }
  nameSize_ = 0;
  nameTooLong_ = false;
  inDeclarationValue_ = false;
  valueSize_ = 0;
  valueTooLong_ = false;
  brackets_ = 0;
}

bool SvgRenderer::Opened(bool empty) noexcept {
  EndAttribute();
  if (skipping_ > 0) {
    if (!empty) {
      ++skipping_;
    }
    return false;
  }
  if (element_ == Element::kIgnored) {
    if (!empty) {
      skipping_ = 1;
    }
    return false;
  }

  if (element_ != Element::kSvg && element_ != Element::kGroup) {
    const bool drew = DrawShape();
    // Whatever a shape has inside it, a title perhaps, is not drawn.
    if (!empty) {
      skipping_ = 1;
    }
    return drew;
  }

  bool shows = !displayNone_ && !unreadable_;
  if (element_ == Element::kGroup) {
    pending_.matrix = ElementMatrix();
  } else if (isInnerSvg_) {
    shows = OpenInnerSvg() && shows;
  } else {
    shows = OpenRoot() && shows;
  }
  pending_.opacity = Both(Parent().opacity, ownOpacity_);
  if (empty) {
    return false;
  }
  if (!shows) {
    skipping_ = 1;
    return false;
  }
  if (ownOpacity_ != 255) {
    Note(SvgLeftOut::kGroupOpacity);
  }
  if (depth_ == kDeepest) {
    Note(SvgLeftOut::kTooDeep);
    skipping_ = 1;
    return false;
  }
  ++depth_;
  levels_[depth_] = pending_;
  report_.deepest = std::max(report_.deepest, depth_);
  return false;
}

void SvgRenderer::Close() noexcept {
  if (skipping_ > 0) {
    --skipping_;
  } else if (depth_ > 0) {
    --depth_;
  }
}

// ---- Where things are ----

Matrix SvgRenderer::ElementMatrix() noexcept {
  if (!hasTransform_) {
    return Parent().matrix;
  }
  bool clamped = false;
  const Matrix matrix = Multiply(Parent().matrix, own_, clamped);
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
  }
  return matrix;
}

bool SvgRenderer::OpenRoot() noexcept {
  size_.known = true;
  size_.hasBox = hasBox_;
  size_.box = hasBox_ ? box_ : SvgBox();
  Fixed naturalWidth = 0;
  Fixed naturalHeight = 0;
  size_.Natural(naturalWidth, naturalHeight);

  // With nowhere to draw there is nothing to fit the drawing to.
  const bool placed = width_ > 0 && height_ > 0;
  bool clamped = false;
  pending_.matrix = Matrix();
  if (hasBox_) {
    // A view box with no width or no height says to draw nothing.
    if (box_.width <= 0 || box_.height <= 0) {
      return false;
    }
    if (placed) {
      pending_.matrix = FitBox(box_, {.x = 0, .y = 0, .width = width_, .height = height_}, fit_, clamped);
    }
    pending_.viewWidth = box_.width;
    pending_.viewHeight = box_.height;
  } else {
    if (naturalWidth <= 0 || naturalHeight <= 0) {
      return false;
    }
    // With no view box, the drawing's own width and height are stretched
    // to the picture's.
    if (placed) {
      pending_.matrix.a = Clamp32((std::int64_t{width_} * kCoefficientOne) / naturalWidth, clamped);
      pending_.matrix.d = Clamp32((std::int64_t{height_} * kCoefficientOne) / naturalHeight, clamped);
    }
    pending_.viewWidth = naturalWidth;
    pending_.viewHeight = naturalHeight;
  }
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
  }
  return true;
}

bool SvgRenderer::OpenInnerSvg() noexcept {
  Note(SvgLeftOut::kViewportClip);
  const Level& parent = Parent();
  const SvgBox space = {.x = LengthAt(kX, 0),
                        .y = LengthAt(kY, 0),
                        .width = LengthAt(kWidth, parent.viewWidth),
                        .height = LengthAt(kHeight, parent.viewHeight)};
  if (space.width <= 0 || space.height <= 0) {
    return false;
  }
  bool clamped = false;
  Matrix place = Translation(space.x, space.y);
  if (hasBox_) {
    if (box_.width <= 0 || box_.height <= 0) {
      return false;
    }
    place = FitBox(box_, space, fit_, clamped);
    pending_.viewWidth = box_.width;
    pending_.viewHeight = box_.height;
  } else {
    pending_.viewWidth = space.width;
    pending_.viewHeight = space.height;
  }
  pending_.matrix = Multiply(parent.matrix, place, clamped);
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
  }
  return true;
}

Fixed SvgRenderer::LengthAt(Slot slot, Fixed otherwise) const noexcept {
  return (lengthsSet_ & BitOf(slot)) != 0 ? lengths_[slot] : otherwise;
}

// ---- Shapes ----

bool SvgRenderer::Shows(std::uint8_t& gray, std::uint8_t& alpha) const noexcept {
  if (filler_ == nullptr || displayNone_ || unreadable_ || lateTransform_ || outlineBad_ || pending_.hidden) {
    return false;
  }
  std::uint8_t solid = pending_.fill.alpha;
  gray = pending_.fill.gray;
  if (pending_.fill.kind == SvgPaint::Kind::kCurrent) {
    gray = pending_.color;
    solid = pending_.colorAlpha;
  } else if (pending_.fill.kind != SvgPaint::Kind::kGray) {
    return false;
  }
  alpha = Both(Both(solid, pending_.fillOpacity), Both(Parent().opacity, ownOpacity_));
  return alpha != 0;
}

bool SvgRenderer::DrawShape() noexcept {
  if (!displayNone_ && !pending_.hidden) {
    if (pending_.stroked && !pending_.strokeless) {
      Note(SvgLeftOut::kStroke);
    }
    if (pending_.fill.server) {
      Note(SvgLeftOut::kPaintServer);
    }
  }
  if (lateTransform_) {
    Note(SvgLeftOut::kLateTransform);
  }
  std::uint8_t gray = 0;
  std::uint8_t alpha = 0;
  const bool shown = Shows(gray, alpha);
  const FillRule rule = pending_.evenOdd ? FillRule::kEvenOdd : FillRule::kNonZero;

  switch (element_) {
    case Element::kPath:
    case Element::kPolygon:
    case Element::kPolyline:
      if (streaming_) {
        // Its lines are in the filler already, whether or not it shows.
        EndOutline();
        FillOutline(shown, rule, gray, alpha);
        return true;
      }
      if (heldSize_ == 0 || !shown) {
        return false;
      }
      // Its data was short enough to hold, and is gone through now that
      // the whole of its tag has been read.
      BeginOutline(ElementMatrix());
      draining_ = true;
      drainRule_ = rule;
      drainGray_ = gray;
      drainAlpha_ = alpha;
      return true;
    case Element::kRect:
      if (!shown || !DrawRect()) {
        return false;
      }
      break;
    case Element::kCircle: {
      const Fixed radius = LengthAt(kRadius, 0);
      if (!shown || !DrawEllipse(LengthAt(kMiddleX, 0), LengthAt(kMiddleY, 0), radius, radius)) {
        return false;
      }
      break;
    }
    case Element::kEllipse:
      if (!shown ||
          !DrawEllipse(LengthAt(kMiddleX, 0), LengthAt(kMiddleY, 0), LengthAt(kRadiusX, 0), LengthAt(kRadiusY, 0))) {
        return false;
      }
      break;
    case Element::kLine:  // Has no inside to fill.
    case Element::kIgnored:
    case Element::kSvg:
    case Element::kGroup:
      return false;
  }
  FillOutline(true, rule, gray, alpha);
  return true;
}

void SvgRenderer::BeginOutline(const Matrix& matrix) noexcept {
  flattener_ = PathFlattener(*filler_, matrix, tolerance_);
  // What lies outside the strip needs no cutting up.
  bool clamped = false;
  const auto pixels = [&clamped](std::int64_t count) noexcept { return Clamp32(count * kFixedOne, clamped); };
  flattener_.KeepTo({.x = 0, .y = pixels(filler_->Top())},
                    {.x = pixels(filler_->Width()), .y = pixels(std::int64_t{filler_->Top()} + filler_->Rows())});
  reader_.Reset();
  talliedCommands_ = 0;
  talliedLines_ = 0;
  // The points of a polygon are path data but for the letter it begins
  // with, and the one a polygon ends with.
  if (element_ == Element::kPolygon || element_ == Element::kPolyline) {
    reader_.Feed("M");
  }
  closesOutline_ = element_ == Element::kPolygon;
}

void SvgRenderer::EndOutline() noexcept {
  if (closesOutline_) {
    reader_.Feed("Z");
  }
  if (reader_.Finish() != SvgPathStatus::kOk) {
    Note(SvgLeftOut::kBadPath);
  }
  if (reader_.Clamped()) {
    Note(SvgLeftOut::kHeldNumber);
  }
  Tally();
}

void SvgRenderer::Tally() noexcept {
  report_.commands += reader_.Commands() - talliedCommands_;
  talliedCommands_ = reader_.Commands();
  report_.lines += flattener_.Lines() - talliedLines_;
  talliedLines_ = flattener_.Lines();
}

void SvgRenderer::FillOutline(bool shown, FillRule rule, std::uint8_t gray, std::uint8_t alpha) noexcept {
  if (flattener_.Clamped()) {
    Note(SvgLeftOut::kHeldNumber);
  }
  Tally();
  // With nothing of it to show, this only clears the outline away.
  filler_->Fill(rule, gray, shown ? alpha : 0);
  if (shown) {
    ++report_.shapes;
  }
}

void SvgRenderer::Drain() noexcept {
  heldAt_ = static_cast<std::uint16_t>(
      heldAt_ +
      reader_.Step({held_.data() + heldAt_, static_cast<std::size_t>(heldSize_ - heldAt_)}, kCommandsPerStep));
  Tally();
  if (heldAt_ < heldSize_) {
    return;
  }
  draining_ = false;
  heldSize_ = 0;
  heldAt_ = 0;
  if (!streaming_) {
    // That was the whole of an outline whose tag has ended.
    EndOutline();
    FillOutline(true, drainRule_, drainGray_, drainAlpha_);
  }
}

std::size_t SvgRenderer::TakePathData(std::string_view piece, bool ofInput) noexcept {
  // With nothing to draw into, or once the outline has been given up, the
  // data is not read.
  if (filler_ == nullptr || outlineBad_) {
    return piece.size();
  }
  if (streaming_) {
    // A piece an entity stood for cannot be handed back: it is a few
    // bytes, and is taken whole.
    const std::size_t used = ofInput ? reader_.Step(piece, kCommandsPerStep) : piece.size();
    if (!ofInput) {
      reader_.Feed(piece);
    }
    Tally();
    return used;
  }

  // Space before the data begins is not worth holding.
  std::size_t from = 0;
  while (heldSize_ == 0 && from < piece.size() && IsSvgSpace(piece[from])) {
    ++from;
  }
  // The array has more room than kHeldPathData, kept for the few bytes of
  // a piece that cannot be handed back.
  const std::size_t room =
      ofInput ? kHeldPathData - std::min<std::size_t>(heldSize_, kHeldPathData) : held_.size() - heldSize_;
  const std::size_t fits = std::min(piece.size() - from, room);
  std::copy(piece.begin() + static_cast<std::ptrdiff_t>(from), piece.begin() + static_cast<std::ptrdiff_t>(from + fits),
            held_.begin() + heldSize_);
  heldSize_ = static_cast<std::uint16_t>(heldSize_ + fits);
  if (from + fits == piece.size()) {
    return piece.size();
  }
  if (!ofInput) {
    // Entities one after another in path data, where there was no room
    // left even for one: the outline is given up.
    outlineBad_ = true;
    heldSize_ = 0;
    Note(SvgLeftOut::kBadPath);
    return piece.size();
  }

  // Too long to hold. From here the data is drawn as it arrives, with what
  // is known of the element so far: first what was held, then the rest.
  if (unreadable_) {
    outlineBad_ = true;
    heldSize_ = 0;
    return piece.size();
  }
  streaming_ = true;
  BeginOutline(ElementMatrix());
  draining_ = true;
  return from + fits;
}

bool SvgRenderer::DrawRect() noexcept {
  const Fixed x = LengthAt(kX, 0);
  const Fixed y = LengthAt(kY, 0);
  const Fixed width = LengthAt(kWidth, 0);
  const Fixed height = LengthAt(kHeight, 0);
  if (width <= 0 || height <= 0) {
    return false;
  }
  // A corner's radius that is written one way only is the same the other
  // way, and neither is more than half the side it is on.
  const bool hasX = (lengthsSet_ & BitOf(kRadiusX)) != 0;
  const bool hasY = (lengthsSet_ & BitOf(kRadiusY)) != 0;
  Fixed radiusX = hasX ? lengths_[kRadiusX] : (hasY ? lengths_[kRadiusY] : 0);
  Fixed radiusY = hasY ? lengths_[kRadiusY] : radiusX;
  radiusX = std::clamp<Fixed>(radiusX, 0, width / 2);
  radiusY = std::clamp<Fixed>(radiusY, 0, height / 2);

  bool clamped = false;
  const Fixed right = Sum(x, width, clamped);
  const Fixed bottom = Sum(y, height, clamped);
  BeginOutline(ElementMatrix());
  if (radiusX == 0 || radiusY == 0) {
    flattener_.MoveTo({.x = x, .y = y});
    flattener_.LineTo({.x = right, .y = y});
    flattener_.LineTo({.x = right, .y = bottom});
    flattener_.LineTo({.x = x, .y = bottom});
    report_.commands += 5;
  } else {
    // Where the corners begin and end along each side.
    const Fixed leftIn = Sum(x, radiusX, clamped);
    const Fixed rightIn = Sum(right, -radiusX, clamped);
    const Fixed topIn = Sum(y, radiusY, clamped);
    const Fixed bottomIn = Sum(bottom, -radiusY, clamped);
    // Each side, and then the quarter of an ellipse that turns its corner.
    flattener_.MoveTo({.x = leftIn, .y = y});
    flattener_.LineTo({.x = rightIn, .y = y});
    flattener_.ArcTo(radiusX, radiusY, 0, false, true, {.x = right, .y = topIn});
    flattener_.LineTo({.x = right, .y = bottomIn});
    flattener_.ArcTo(radiusX, radiusY, 0, false, true, {.x = rightIn, .y = bottom});
    flattener_.LineTo({.x = leftIn, .y = bottom});
    flattener_.ArcTo(radiusX, radiusY, 0, false, true, {.x = x, .y = bottomIn});
    flattener_.LineTo({.x = x, .y = topIn});
    flattener_.ArcTo(radiusX, radiusY, 0, false, true, {.x = leftIn, .y = y});
    report_.commands += 10;
  }
  flattener_.Close();
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
  }
  return true;
}

bool SvgRenderer::DrawEllipse(Fixed middleX, Fixed middleY, Fixed radiusX, Fixed radiusY) noexcept {
  if (radiusX <= 0 || radiusY <= 0) {
    return false;
  }
  bool clamped = false;
  const Fixed left = Sum(middleX, -radiusX, clamped);
  const Fixed right = Sum(middleX, radiusX, clamped);
  const Fixed top = Sum(middleY, -radiusY, clamped);
  const Fixed bottom = Sum(middleY, radiusY, clamped);
  BeginOutline(ElementMatrix());
  // Four quarters, from its right-hand end round by the bottom.
  flattener_.MoveTo({.x = right, .y = middleY});
  flattener_.ArcTo(radiusX, radiusY, 0, false, true, {.x = middleX, .y = bottom});
  flattener_.ArcTo(radiusX, radiusY, 0, false, true, {.x = left, .y = middleY});
  flattener_.ArcTo(radiusX, radiusY, 0, false, true, {.x = middleX, .y = top});
  flattener_.ArcTo(radiusX, radiusY, 0, false, true, {.x = right, .y = middleY});
  flattener_.Close();
  report_.commands += 6;
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
  }
  return true;
}

}  // namespace bn::draw
