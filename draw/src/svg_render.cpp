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
  kUse,
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
    {.name = "use", .fate = Fate::kUse, .note = 0},
    {.name = "image", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kImage)},
    {.name = "text", .fate = Fate::kPassedOver, .note = Noted(SvgLeftOut::kText)},
    {.name = "style", .fate = Fate::kPassedOver, .note = 0},
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

// A number of four bytes, as it is written in the memory for definitions.
void Write32(std::span<std::uint8_t> to, std::uint32_t value) noexcept {
  for (std::size_t byte = 0; byte < 4; ++byte) {
    to[byte] = static_cast<std::uint8_t>(value >> (8U * byte));
  }
}
std::uint32_t Read32(std::span<const std::uint8_t> from) noexcept {
  std::uint32_t value = 0;
  for (std::size_t byte = 0; byte < 4; ++byte) {
    value |= std::uint32_t{from[byte]} << (8U * byte);
  }
  return value;
}
void Write64(std::span<std::uint8_t> to, std::int64_t value) noexcept {
  const auto bits = static_cast<std::uint64_t>(value);
  Write32(to.first(4), static_cast<std::uint32_t>(bits));
  Write32(to.subspan(4, 4), static_cast<std::uint32_t>(bits >> 32U));
}
std::int64_t Read64(std::span<const std::uint8_t> from) noexcept {
  return static_cast<std::int64_t>(std::uint64_t{Read32(from.first(4))} |
                                   (std::uint64_t{Read32(from.subspan(4, 4))} << 32U));
}

// Bytes kept, as the characters they are.
std::string_view AsText(std::span<const std::uint8_t> bytes) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the bytes kept, as the characters they are.
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Whether `classes`, as `class` writes them with space between, has `name`
// among them.
bool HasClass(std::string_view classes, std::string_view name) noexcept {
  std::size_t at = 0;
  while (at < classes.size()) {
    while (at < classes.size() && IsSvgSpace(classes[at])) {
      ++at;
    }
    std::size_t end = at;
    while (end < classes.size() && !IsSvgSpace(classes[end])) {
      ++end;
    }
    if (end > at && std::string_view(classes.begin() + static_cast<std::ptrdiff_t>(at),
                                     classes.begin() + static_cast<std::ptrdiff_t>(end)) == name) {
      return true;
    }
    at = end;
  }
  return false;
}

// A direction, made as long as it can be while its parts stay within
// `most`: only which way it goes matters, and the longer it is the more
// exactly that is kept.
void Lengthen(std::int64_t& x, std::int64_t& y, std::int64_t most) noexcept {
  if (x == 0 && y == 0) {
    return;
  }
  const auto size = [](std::int64_t value) noexcept { return value < 0 ? -value : value; };
  while (std::max(size(x), size(y)) > most) {
    x = ShiftDown(x, 1);
    y = ShiftDown(y, 1);
  }
  while (std::max(size(x), size(y)) <= most / 2) {
    x *= 2;
    y *= 2;
  }
}

// A direction turned by a matrix, which does not move it.
void Turn(const Matrix& matrix, std::int64_t& x, std::int64_t& y) noexcept {
  constexpr std::int64_t kMost = 1073741824;
  Lengthen(x, y, kMost);
  const std::int64_t across = ShiftDown((std::int64_t{matrix.a} * x) + (std::int64_t{matrix.c} * y), kCoefficientBits);
  const std::int64_t down = ShiftDown((std::int64_t{matrix.b} * x) + (std::int64_t{matrix.d} * y), kCoefficientBits);
  x = across;
  y = down;
  Lengthen(x, y, kMost);
}

// `over` times GrayRamp::kOne over `under`, which is not nothing: how far
// along a ramp something is. Held to GrayRamp::kFurthest either way.
std::int64_t Along(std::int64_t over, std::int64_t under) noexcept {
  constexpr std::int64_t kSmallEnough = 274877906944;  // 2^38: what is left over times kOne is then within 64 bits.
  constexpr std::int64_t kMostWhole = 68719476736;     // 2^36: a whole number of ramps that is far enough.
  if (under < 0) {
    under = -under;
    over = -over;
  }
  while (under >= kSmallEnough) {
    under = ShiftDown(under, 1);
    over = ShiftDown(over, 1);
  }
  const std::int64_t whole = over / under;
  if (whole > kMostWhole || whole < -kMostWhole) {
    return whole > 0 ? GrayRamp::kFurthest : -GrayRamp::kFurthest;
  }
  const std::int64_t rest = over % under;
  return std::clamp((whole * GrayRamp::kOne) + ((rest * GrayRamp::kOne) / under), -GrayRamp::kFurthest,
                    GrayRamp::kFurthest);
}

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
      return "use of what is not defined";
    case SvgLeftOut::kStyleSheet:
      return "what a style sheet says that is not read";
    case SvgLeftOut::kStroke:
      return "a stroke";
    case SvgLeftOut::kPaintServer:
      return "a paint that is not a linear gradient before it";
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
      return "the edges of an inner svg that is turned";
    case SvgLeftOut::kSwitch:
      return "a switch";
    case SvgLeftOut::kUnknownEntity:
      return "an unknown entity";
    case SvgLeftOut::kNoRoom:
      return "definitions there was no room for";
    case SvgLeftOut::kUseLimit:
      return "use past its limit";
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

SvgRenderer::SvgRenderer() noexcept
    : flattener_(nowhere_, Matrix()), reader_(flattener_), strokeOut_(nowhere_, Matrix()) {}

void SvgRenderer::Begin(Canvas* canvas, Fixed width, Fixed height, Fixed tolerance) noexcept {
  tags_ = SvgTagReader();
  canvas_ = (canvas != nullptr && canvas->Rows() > 0) ? canvas : nullptr;
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
  keptBytes_ = 0;
  recordBytes_ = 0;
  records_ = 0;
  rules_ = 0;
  keepingFrom_ = 0;
  keeping_ = false;
  blockFull_ = false;
  openIdCount_ = 0;
  reusing_ = 0;
  firstOfUse_ = false;
  sheet_ = SvgSheetReader();
  openingSheet_ = false;
  inSheet_ = false;
  shapeSeen_ = false;
  ruleSize_ = 0;
  settingsAt_ = 0;
  ruleTooLong_ = false;
  paintElement_ = PaintElement::kNone;
  building_ = false;
  classesSize_ = 0;
  fillNameSize_ = 0;
  element_ = Element::kIgnored;
  attribute_ = Attribute::kIgnored;
  streaming_ = false;
  draining_ = false;
  stroking_ = false;
  strokeNext_ = false;
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
  if (strokeNext_) {
    StrokeNext();
    return 0;
  }
  // Something a `use` is drawing again is read to its end before any more
  // of the file is.
  if (reusing_ > 0) {
    ReuseSome();
    return 0;
  }
  std::size_t taken = 0;
  for (std::uint32_t tokens = 0; tokens < kTokensPerStep && taken < bytes.size(); ++tokens) {
    SvgTagToken token;
    const std::uint32_t before = tags_.Offset();
    std::size_t used = tags_.Next({bytes.data() + taken, bytes.size() - taken}, token);
    const bool failed = tags_.Status() != SvgTagStatus::kOk;
    // What is inside `defs` is kept as it goes by, as it is written, and
    // before it is acted on, so that an element is all there by the time
    // its end makes it one that can be used. None of it is handed back:
    // nothing in `defs` is drawn as it is read, so no value there is taken
    // less than whole. Where the tags go wrong, what came before the
    // fault is kept and no more, whatever else the call was given.
    //
    // A piece of a style sheet's text is the one thing not kept here but
    // as it is read (ReadSheet()): a piece is as long as the bytes to hand
    // made it, and a rule that ends in the middle of one takes memory too.
    if (keeping_) {
      std::string_view passing(bytes.data() + taken,
                               failed ? std::min<std::size_t>(tags_.ErrorOffset() - before, used) : used);
      if (IsSheetTextToKeep(token)) {
        passing.remove_suffix(token.text.size());
      }
      Keep(passing);
    }
    const bool enough = Handle(tags_, token, used);
    taken += used;
    if (failed) {
      status_ = SvgStatus::kMalformed;
    }
    if (status_ != SvgStatus::kOk) {
      return bytes.size();
    }
    if (enough || draining_ || strokeNext_ || reusing_ > 0) {
      break;
    }
  }
  return taken;
}

bool SvgRenderer::Handle(SvgTagReader& reader, const SvgTagToken& token, std::size_t& taken) noexcept {
  switch (token.kind) {
    case Kind::kNone:
      break;
    case Kind::kText:
      if (inSheet_) {
        ReadSheet(token.text, IsSheetTextToKeep(token));
      }
      break;
    case Kind::kOpen:
      // Where it begins among the bytes kept, if they are being kept.
      tagAt_ = reader.TagOffset() - keepingFrom_;
      Open(token.text);
      break;
    case Kind::kAttribute:
      BeginAttribute(token.text);
      break;
    case Kind::kValue: {
      if (attribute_ == Attribute::kId) {
        // Not held: only where it is written among the bytes kept, which
        // a piece of it straight from the file is the last of so far. An
        // entity is not kept as what it stands for, so a name written
        // with one cannot be set beside what a `use` asks for.
        if (!token.ofInput) {
          idPlain_ = false;
        } else if (idLength_ == 0) {
          idAt_ = reader.Offset() - static_cast<std::uint32_t>(token.text.size()) - keepingFrom_;
        }
        idLength_ += static_cast<std::uint32_t>(token.text.size());
        break;
      }
      const std::size_t wanted = TakeValue(token.text, token.ofInput);
      if (wanted < token.text.size()) {
        reader.GiveBack(token.text.size() - wanted);
        taken -= token.text.size() - wanted;
        return true;
      }
      return streaming_;
    }
    case Kind::kOpened:
      return Opened(token.empty);
    case Kind::kClose:
      return Close();
  }
  return false;
}

// ---- What is kept, and drawn again ----

void SvgRenderer::BeginKeeping() noexcept {
  keeping_ = true;
  // The bytes kept are a run of the file from here on, after whatever was
  // kept before: an element that begins at some place in the file begins
  // that much less than this further on among them.
  keepingFrom_ = tags_.Offset() - keptBytes_;
  openIdCount_ = 0;
}

void SvgRenderer::Keep(std::string_view bytes) noexcept {
  if (blockFull_) {
    return;
  }
  // As much as there is room for, between what is kept already and the
  // records at the other end: so how much is kept in the end does not
  // depend on the pieces the file came in.
  const std::size_t room = block_.size() - std::size_t{recordBytes_} - keptBytes_;
  const std::size_t fits = std::min(bytes.size(), room);
  std::copy_n(bytes.begin(), fits, block_.subspan(keptBytes_, fits).begin());
  keptBytes_ += static_cast<std::uint32_t>(fits);
  report_.kept = keptBytes_;
  if (fits < bytes.size()) {
    // No more is kept from here on. What was kept whole can still be used.
    blockFull_ = true;
    Note(SvgLeftOut::kNoRoom);
  }
}

void SvgRenderer::Define(std::uint32_t at, std::uint32_t idAt) noexcept {
  static_cast<void>(AddRecord(Record::kDefinition, at, idAt, {}));
}

bool SvgRenderer::AddRecord(Record kind, std::uint32_t number, std::uint32_t other,
                            std::span<const std::uint8_t> said) noexcept {
  // It goes below the last, from the end of the memory backwards.
  const std::size_t size = kBytesPerDefinition + said.size();
  if (blockFull_ || std::size_t{recordBytes_} + size + keptBytes_ > block_.size()) {
    blockFull_ = true;
    Note(SvgLeftOut::kNoRoom);
    return false;
  }
  const std::size_t top = block_.size() - recordBytes_;
  std::copy(said.begin(), said.end(), block_.subspan(top - size, said.size()).begin());
  const std::span<std::uint8_t> head = block_.subspan(top - kBytesPerDefinition, kBytesPerDefinition);
  Write32(head.first(4), (static_cast<std::uint32_t>(kind) << 24U) | (number & 0xFFFFFFU));
  Write32(head.last(4), other);
  recordBytes_ += static_cast<std::uint32_t>(size);
  ++records_;
  return true;
}

bool SvgRenderer::RecordAt(std::uint32_t top, Kept& kept) const noexcept {
  const std::size_t lowest = block_.size() - recordBytes_;
  if (top <= lowest || top > block_.size() || top - lowest < kBytesPerDefinition) {
    return false;
  }
  const std::span<const std::uint8_t> head = block_.subspan(top - kBytesPerDefinition, kBytesPerDefinition);
  const std::uint32_t first = Read32(head.first(4));
  kept.kind = static_cast<Record>(first >> 24U);
  kept.number = first & 0xFFFFFFU;
  kept.other = Read32(head.last(4));
  const std::uint32_t said = kept.kind == Record::kDefinition ? 0 : kept.number;
  if (std::size_t{said} + kBytesPerDefinition > top - lowest) {
    return false;
  }
  kept.top = top;
  kept.bottom = top - static_cast<std::uint32_t>(kBytesPerDefinition) - said;
  kept.said = block_.subspan(kept.bottom, said);
  return true;
}

bool SvgRenderer::Find(std::uint32_t& at) const noexcept {
  Kept kept;
  for (auto top = static_cast<std::uint32_t>(block_.size()); RecordAt(top, kept); top = kept.bottom) {
    if (kept.kind != Record::kDefinition) {
      continue;
    }
    const std::uint32_t idAt = kept.other;
    // It has that name if the letters are the same and the name ends
    // there: with the quotation mark it began with, which is the byte
    // before it.
    if (idAt == 0 || std::size_t{idAt} + wantedSize_ >= keptBytes_) {
      continue;
    }
    const std::span<const std::uint8_t> written = block_.subspan(idAt, wantedSize_);
    if (std::equal(written.begin(), written.end(), wanted_.begin(),
                   [](std::uint8_t one, char other) noexcept { return one == static_cast<std::uint8_t>(other); }) &&
        block_[idAt + wantedSize_] == block_[idAt - 1]) {
      at = kept.number;
      return true;
    }
  }
  return false;
}

std::uint32_t SvgRenderer::FindGradient(std::string_view name) const noexcept {
  Kept kept;
  for (auto top = static_cast<std::uint32_t>(block_.size()); RecordAt(top, kept); top = kept.bottom) {
    // What a gradient says begins with its name.
    if (kept.kind == Record::kGradient && !kept.said.empty() && kept.said[0] < kept.said.size() &&
        AsText(kept.said.subspan(1, kept.said[0])) == name) {
      return kept.top;
    }
  }
  return 0;
}

// ---- A style sheet ----

bool SvgRenderer::IsSheetTextToKeep(const SvgTagToken& token) const noexcept {
  return keeping_ && inSheet_ && token.kind == Kind::kText && token.ofInput;
}

void SvgRenderer::ReadSheet(std::string_view piece, bool keep) noexcept {
  for (;;) {
    SvgSheetToken token;
    const std::size_t took = sheet_.Next(piece, token);
    if (keep) {
      // Kept as far as it has been read, before what it says is acted on.
      // So a rule is recorded when the text up to its end has been kept
      // and no more of it, in whatever pieces the text came: the memory is
      // taken in the order the file has things in, and what fits does not
      // depend on the pieces.
      std::string_view read = piece;
      read.remove_suffix(piece.size() - took);
      Keep(read);
    }
    piece.remove_prefix(took);
    switch (token.kind) {
      case SvgSheetToken::Kind::kNone:
        return;
      case SvgSheetToken::Kind::kSelector: {
        const std::string_view name = token.selector.name;
        if (ruleSize_ == 0) {
          rule_[0] = 0;
          ruleSize_ = 1;
        }
        if (settingsAt_ != 0 || name.size() > UINT8_MAX || ruleSize_ + 2 + name.size() > rule_.size() ||
            rule_[0] == UINT8_MAX) {
          ruleTooLong_ = true;
          break;
        }
        rule_[ruleSize_] = token.selector.kind == SvgSelector::Kind::kClass ? 0 : 1;
        rule_[ruleSize_ + 1] = static_cast<std::uint8_t>(name.size());
        std::copy(name.begin(), name.end(), rule_.begin() + ruleSize_ + 2);
        ruleSize_ += static_cast<std::uint32_t>(2 + name.size());
        ++rule_[0];
        break;
      }
      case SvgSheetToken::Kind::kDeclaration: {
        // What the renderer has no use for is not kept; nor is what only a
        // gradient's stop can say, since only a shape or a group is styled.
        const Property property = PropertyOf(token.property);
        if (property == Property::kNone || property == Property::kStopColor || property == Property::kStopOpacity ||
            ruleSize_ == 0) {
          break;
        }
        const std::string_view value = WithoutImportant(token.value);
        if (settingsAt_ == 0) {
          if (ruleSize_ + 1 > rule_.size()) {
            ruleTooLong_ = true;
            break;
          }
          settingsAt_ = ruleSize_;
          rule_[ruleSize_] = 0;
          ++ruleSize_;
        }
        if (value.size() > UINT8_MAX || ruleSize_ + 2 + value.size() > rule_.size() ||
            rule_[settingsAt_] == UINT8_MAX) {
          ruleTooLong_ = true;
          break;
        }
        rule_[ruleSize_] = static_cast<std::uint8_t>(property);
        rule_[ruleSize_ + 1] = static_cast<std::uint8_t>(value.size());
        std::copy(value.begin(), value.end(), rule_.begin() + ruleSize_ + 2);
        ruleSize_ += static_cast<std::uint32_t>(2 + value.size());
        ++rule_[settingsAt_];
        break;
      }
      case SvgSheetToken::Kind::kRuleEnd:
        EndRule();
        break;
    }
  }
}

void SvgRenderer::EndRule() noexcept {
  if (ruleTooLong_) {
    Note(SvgLeftOut::kStyleSheet);
  } else if (ruleSize_ > 0 && rule_[0] > 0 && settingsAt_ != 0 && rule_[settingsAt_] > 0 &&
             AddRecord(Record::kRule, ruleSize_, 0, std::span(rule_).first(ruleSize_))) {
    ++rules_;
    report_.rules = rules_;
  }
  ruleSize_ = 0;
  settingsAt_ = 0;
  ruleTooLong_ = false;
}

void SvgRenderer::Style() noexcept {
  // For each property, what the rule that wins says, and how much the
  // selector it won by counts for: a class 1, an element's name 0.
  struct Winner {
    std::uint32_t at = 0;
    std::uint8_t size = 0;
    int counts = -1;
  };
  std::array<Winner, kProperties> winners{};
  const std::string_view classes(classes_.data(), classesSize_);
  const std::string_view element(elementName_.data(), elementNameSize_);
  Kept kept;
  std::uint32_t seen = 0;
  for (auto top = static_cast<std::uint32_t>(block_.size()); seen < rules_ && RecordAt(top, kept); top = kept.bottom) {
    if (kept.kind != Record::kRule) {
      continue;
    }
    ++seen;
    const std::span<const std::uint8_t> said = kept.said;
    // Each part is a byte, another, and as many bytes as the second says.
    std::size_t at = 1;
    const auto fits = [&said](std::size_t from) noexcept {
      return from + 2 <= said.size() && from + 2 + said[from + 1] <= said.size();
    };
    int counts = -1;
    for (std::uint8_t selector = 0; !said.empty() && selector < said[0]; ++selector) {
      if (!fits(at)) {
        break;
      }
      const std::string_view name = AsText(said.subspan(at + 2, said[at + 1]));
      if (said[at] == 0 && HasClass(classes, name)) {
        counts = 1;
      } else if (said[at] == 1 && !element.empty() && name == element) {
        counts = std::max(counts, 0);
      }
      at += 2 + std::size_t{said[at + 1]};
    }
    if (counts < 0 || at >= said.size()) {
      continue;
    }
    const std::uint8_t settings = said[at];
    ++at;
    for (std::uint8_t setting = 0; setting < settings && fits(at); ++setting) {
      const std::size_t property = said[at];
      if (property < kProperties && counts >= winners[property].counts) {
        winners[property] = {
            .at = static_cast<std::uint32_t>(kept.bottom + at + 2), .size = said[at + 1], .counts = counts};
      }
      at += 2 + std::size_t{said[at + 1]};
    }
  }
  for (std::size_t property = 1; property < kProperties; ++property) {
    if (winners[property].counts >= 0) {
      SetProperty(static_cast<Property>(property), AsText(block_.subspan(winners[property].at, winners[property].size)),
                  false);
    }
  }
}

// ---- A gradient ----

std::size_t SvgRenderer::Pack(const Gradient& gradient, std::span<std::uint8_t> said) noexcept {
  std::size_t at = 0;
  said[at] = gradient.nameSize;
  std::copy_n(gradient.name.begin(), gradient.nameSize, said.subspan(at + 1, gradient.nameSize).begin());
  at += 1 + std::size_t{gradient.nameSize};
  said[at] = static_cast<std::uint8_t>((gradient.userSpace ? 1U : 0U) | (static_cast<unsigned>(gradient.spread) << 1U));
  ++at;
  for (const SvgLength& end : gradient.ends) {
    Write64(said.subspan(at, 8), end.value);
    said[at + 8] = static_cast<std::uint8_t>(end.unit);
    at += 9;
  }
  for (const std::int32_t part : {gradient.transform.a, gradient.transform.b, gradient.transform.c,
                                  gradient.transform.d, gradient.transform.e, gradient.transform.f}) {
    Write32(said.subspan(at, 4), static_cast<std::uint32_t>(part));
    at += 4;
  }
  said[at] = gradient.stops;
  ++at;
  for (std::size_t stop = 0; stop < gradient.stops; ++stop) {
    said[at] = static_cast<std::uint8_t>(gradient.offsets[stop]);
    said[at + 1] = static_cast<std::uint8_t>(gradient.offsets[stop] >> 8U);
    said[at + 2] = gradient.grays[stop];
    said[at + 3] = gradient.alphas[stop];
    at += 4;
  }
  return at;
}

bool SvgRenderer::Unpack(std::span<const std::uint8_t> said, Gradient& gradient) noexcept {
  Gradient read;
  if (said.empty() || said[0] > kLongestId || said.size() < std::size_t{said[0]} + 1 + 1 + 36 + 24 + 1) {
    return false;
  }
  std::size_t at = 0;
  read.nameSize = said[0];
  std::copy_n(said.begin() + 1, read.nameSize, read.name.begin());
  at += 1 + std::size_t{read.nameSize};
  read.userSpace = (said[at] & 1U) != 0;
  read.spread = static_cast<GrayRamp::Spread>(std::min((static_cast<unsigned>(said[at]) >> 1U) & 3U, 2U));
  ++at;
  for (SvgLength& end : read.ends) {
    end.value = Read64(said.subspan(at, 8));
    end.unit = static_cast<SvgUnit>(std::min<unsigned>(said[at + 8], static_cast<unsigned>(SvgUnit::kEx)));
    at += 9;
  }
  std::array<std::int32_t, 6> parts{};
  for (std::int32_t& part : parts) {
    part = static_cast<std::int32_t>(Read32(said.subspan(at, 4)));
    at += 4;
  }
  read.transform = {.a = parts[0], .b = parts[1], .c = parts[2], .d = parts[3], .e = parts[4], .f = parts[5]};
  read.stops = std::min<std::uint8_t>(said[at], kMostStops);
  ++at;
  if (said.size() < at + (4 * std::size_t{read.stops})) {
    return false;
  }
  for (std::size_t stop = 0; stop < read.stops; ++stop) {
    read.offsets[stop] =
        static_cast<std::uint16_t>(static_cast<unsigned>(said[at]) | (static_cast<unsigned>(said[at + 1]) << 8U));
    read.grays[stop] = said[at + 2];
    read.alphas[stop] = said[at + 3];
    at += 4;
  }
  gradient = read;
  return true;
}

void SvgRenderer::SetPaint(std::string_view value) noexcept {
  value = WithoutSpace(value);
  bool clamped = false;
  const auto name = [&value](std::array<char, kLongestId>& to, std::uint8_t& size) noexcept {
    if (value.size() <= to.size()) {
      std::copy(value.begin(), value.end(), to.begin());
      size = static_cast<std::uint8_t>(value.size());
    }
  };
  // Bits of Gradient::set: its ends, then its units, its transform and its spread.
  constexpr unsigned kUnitsBit = 4;
  constexpr unsigned kTransformBit = 5;
  constexpr unsigned kSpreadBit = 6;
  switch (paintSlot_) {
    case PaintSlot::kName:
      name(gradient_.name, gradient_.nameSize);
      break;
    case PaintSlot::kLink:
      if (value.size() > 1 && value.front() == '#') {
        value.remove_prefix(1);
        name(gradient_.link, gradient_.linkSize);
      }
      break;
    case PaintSlot::kX1:
    case PaintSlot::kY1:
    case PaintSlot::kX2:
    case PaintSlot::kY2: {
      const auto end = static_cast<std::size_t>(paintSlot_) - static_cast<std::size_t>(PaintSlot::kX1);
      SvgLength length;
      if (ParseSvgLength(value, length, clamped)) {
        gradient_.ends[end] = length;
        gradient_.set = static_cast<std::uint8_t>(gradient_.set | (1U << end));
      } else {
        Note(SvgLeftOut::kBadValue);
      }
      break;
    }
    case PaintSlot::kUnits:
      if (value == "userSpaceOnUse" || value == "objectBoundingBox") {
        gradient_.userSpace = value == "userSpaceOnUse";
        gradient_.set = static_cast<std::uint8_t>(gradient_.set | (1U << kUnitsBit));
      }
      break;
    case PaintSlot::kTransform: {
      Matrix matrix;
      if (ParseSvgTransform(value, matrix, clamped)) {
        gradient_.transform = matrix;
        gradient_.set = static_cast<std::uint8_t>(gradient_.set | (1U << kTransformBit));
      } else {
        Note(SvgLeftOut::kBadValue);
      }
      break;
    }
    case PaintSlot::kSpread:
      // As SVG writes them, letter for letter, as it does the units.
      if (value == "pad" || value == "reflect" || value == "repeat") {
        gradient_.spread = value == "reflect"  ? GrayRamp::Spread::kReflect
                           : value == "repeat" ? GrayRamp::Spread::kRepeat
                                               : GrayRamp::Spread::kPad;
        gradient_.set = static_cast<std::uint8_t>(gradient_.set | (1U << kSpreadBit));
      }
      break;
    case PaintSlot::kOffset: {
      SvgLength length;
      if (ParseSvgLength(value, length, clamped)) {
        const Fixed part = std::clamp<Fixed>(LengthIn(length, kFixedOne, clamped), 0, kFixedOne);
        stopOffset_ = static_cast<std::uint16_t>((std::int64_t{part} * UINT16_MAX + (kFixedOne / 2)) / kFixedOne);
      } else {
        Note(SvgLeftOut::kBadValue);
      }
      break;
    }
  }
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
  }
}

void SvgRenderer::AddStop() noexcept {
  if (gradient_.stops == kMostStops) {
    Note(SvgLeftOut::kPaintServer);
    return;
  }
  const std::size_t stop = gradient_.stops;
  // A stop before the one before it is taken to be where that one is, by
  // the ramp (GrayRamp::Begin()).
  gradient_.offsets[stop] = stopOffset_;
  gradient_.grays[stop] = stopGray_;
  gradient_.alphas[stop] = Both(stopAlpha_, stopOpacity_);
  ++gradient_.stops;
}

bool SvgRenderer::EndGradient() noexcept {
  building_ = false;
  if (gradient_.nameSize == 0) {
    return false;  // Nothing can name it.
  }
  bool looked = false;
  if (gradient_.linkSize > 0) {
    // What it does not say for itself, it takes from the one it names,
    // which was made whole from those it names when it was kept.
    looked = true;
    Kept kept;
    Gradient named;
    ++report_.looked;
    const std::uint32_t top = FindGradient({gradient_.link.data(), gradient_.linkSize});
    if (top != 0 && RecordAt(top, kept) && Unpack(kept.said, named)) {
      if (gradient_.stops == 0) {
        gradient_.stops = named.stops;
        gradient_.offsets = named.offsets;
        gradient_.grays = named.grays;
        gradient_.alphas = named.alphas;
      }
      for (std::size_t end = 0; end < gradient_.ends.size(); ++end) {
        if ((gradient_.set & (1U << end)) == 0) {
          gradient_.ends[end] = named.ends[end];
        }
      }
      if ((gradient_.set & (1U << 4U)) == 0) {
        gradient_.userSpace = named.userSpace;
      }
      if ((gradient_.set & (1U << 5U)) == 0) {
        gradient_.transform = named.transform;
      }
      if ((gradient_.set & (1U << 6U)) == 0) {
        gradient_.spread = named.spread;
      }
    }
  }
  // Its name, units and spread, four ends of nine bytes, a transform, and its stops.
  std::array<std::uint8_t, 1 + kLongestId + 1 + 36 + 24 + 1 + (std::size_t{4} * kMostStops)> said{};
  const std::size_t size = Pack(gradient_, said);
  if (AddRecord(Record::kGradient, static_cast<std::uint32_t>(size), 0, std::span(said).first(size))) {
    ++report_.gradients;
  }
  return looked;
}

bool SvgRenderer::Ramp(const Filling& filling) noexcept {
  Kept kept;
  Gradient gradient;
  if (!RecordAt(filling.gradient, kept) || !Unpack(kept.said, gradient) || gradient.stops == 0) {
    return false;  // With no stops it paints nothing.
  }
  // Its stops, with where each is in the filler's terms.
  const std::size_t last = gradient.stops - 1;
  ramp_.stops = gradient.stops;
  for (std::size_t stop = 0; stop < gradient.stops; ++stop) {
    ramp_.offsets[stop] = static_cast<std::int32_t>(
        ((std::int64_t{gradient.offsets[stop]} * GrayRamp::kOne) + (UINT16_MAX / 2)) / UINT16_MAX);
    ramp_.grays[stop] = gradient.grays[stop];
    ramp_.alphas[stop] = gradient.alphas[stop];
  }
  ramp_.spread = gradient.spread;

  // Where it runs from and to, in its own space: of the shape's box, from
  // nothing to one, unless it is in the space the shape is drawn in.
  bool clamped = false;
  const bool user = gradient.userSpace;
  std::array<Fixed, 4> ends{};
  for (std::size_t end = 0; end < ends.size(); ++end) {
    const Fixed whole = !user ? kFixedOne : (end % 2 == 0 ? filling.viewWidth : filling.viewHeight);
    ends[end] = LengthIn(gradient.ends[end], whole, clamped);
  }
  Point from;
  Point size;
  if (!user) {
    // A shape with no width or no height has no box to lay it over.
    if (!flattener_.HasBounds()) {
      return false;
    }
    from = flattener_.BoundsFrom();
    size = {.x = Sum(flattener_.BoundsTo().x, -from.x, clamped), .y = Sum(flattener_.BoundsTo().y, -from.y, clamped)};
    if (size.x <= 0 || size.y <= 0) {
      return false;
    }
  }
  if (ends[0] == ends[2] && ends[1] == ends[3]) {
    // No length to it: all of it is its last stop.
    ramp_.stops = 1;
    ramp_.offsets[0] = 0;
    ramp_.grays[0] = gradient.grays[last];
    ramp_.alphas[0] = gradient.alphas[last];
    ramp_.Begin();
    ramp_.at = 0;
    ramp_.across = 0;
    ramp_.down = 0;
    return true;
  }
  // From its own space to pixels.
  const auto inPixels = [&](Fixed x, Fixed y) noexcept {
    Point point = Apply(gradient.transform, {.x = x, .y = y}, clamped);
    if (!user) {
      point = {.x = Clamp32(from.x + ShiftDown(std::int64_t{point.x} * size.x, kFixedBits), clamped),
               .y = Clamp32(from.y + ShiftDown(std::int64_t{point.y} * size.y, kFixedBits), clamped)};
    }
    return Apply(filling.matrix, point, clamped);
  };
  const Point start = inPixels(ends[0], ends[1]);
  const Point stop = inPixels(ends[2], ends[3]);
  // Its grays are the same along lines square to it in its own space,
  // which in pixels need not be square to it: which way they go.
  std::int64_t alongX = -(std::int64_t{ends[3]} - ends[1]);
  std::int64_t alongY = std::int64_t{ends[2]} - ends[0];
  Turn(gradient.transform, alongX, alongY);
  if (!user) {
    alongX *= size.x;
    alongY *= size.y;
  }
  Turn(filling.matrix, alongX, alongY);
  constexpr std::int64_t kShort = 16777216;
  Lengthen(alongX, alongY, kShort);
  // How far along a place is: how far across those lines it is from the
  // start, over how far the stop is.
  const std::int64_t toStopX = std::int64_t{stop.x} - start.x;
  const std::int64_t toStopY = std::int64_t{stop.y} - start.y;
  const std::int64_t whole = (alongX * toStopY) - (alongY * toStopX);
  if (whole == 0) {
    return false;  // The shape is drawn flat, and has no inside.
  }
  constexpr std::int64_t kMiddle = kFixedOne / 2;
  ramp_.across = Along(-alongY * kFixedOne, whole);
  ramp_.down = Along(alongX * kFixedOne, whole);
  ramp_.at = Along((alongX * (kMiddle - start.y)) - (alongY * (kMiddle - start.x)), whole);
  ramp_.Begin();
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
  }
  return true;
}

bool SvgRenderer::OpenUse(bool empty) noexcept {
  // What it draws stands where the `use` does, moved by its transform and
  // then by its `x` and `y`, and takes from the `use` whatever paint it
  // does not say for itself: the `use` is a group round it.
  bool clamped = false;
  pending_.matrix = ElementMatrix();
  const Fixed x = LengthAt(kX, 0);
  const Fixed y = LengthAt(kY, 0);
  if (x != 0 || y != 0) {
    pending_.matrix = Multiply(pending_.matrix, Translation(x, y), clamped);
  }
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
  }
  pending_.opacity = Both(Parent().opacity, ownOpacity_);
  // Whatever is written inside a `use` is not drawn.
  const std::uint32_t skippingAfter = empty ? 0 : 1;
  if (displayNone_ || unreadable_) {
    skipping_ = skippingAfter;
    return false;
  }
  if (!hasWanted_) {
    Note(SvgLeftOut::kUse);
    skipping_ = skippingAfter;
    return false;
  }
  // Looking for what it names goes through everything that is defined,
  // so from here on this is enough work for one call, found or not.
  std::uint32_t at = 0;
  ++report_.looked;
  if (!Find(at)) {
    Note(SvgLeftOut::kUse);
    skipping_ = skippingAfter;
    return true;
  }
  if (reusing_ == kDeepestUse || report_.reused >= kMostReused) {
    Note(SvgLeftOut::kUseLimit);
    skipping_ = skippingAfter;
    return true;
  }
  if (depth_ == kDeepest) {
    Note(SvgLeftOut::kTooDeep);
    skipping_ = skippingAfter;
    return true;
  }
  if (ownOpacity_ != 255) {
    Note(SvgLeftOut::kGroupOpacity);
  }
  Reuse& reuse = reuses_[reusing_];
  reuse = Reuse();
  reuse.at = at;
  reuse.levels = depth_;
  // The file's own reading goes on from here once this is over.
  reuse.skipping = skippingAfter;
  ++reusing_;
  skipping_ = 0;
  ++depth_;
  levels_[depth_] = pending_;
  report_.deepest = std::max(report_.deepest, depth_);
  // What it names is read next. If that is an `svg`, it is as wide and as
  // high as the `use` says.
  firstOfUse_ = true;
  useHasWidth_ = (lengthsSet_ & BitOf(kWidth)) != 0;
  useHasHeight_ = (lengthsSet_ & BitOf(kHeight)) != 0;
  useWidth_ = lengths_[kWidth];
  useHeight_ = lengths_[kHeight];
  return true;
}

void SvgRenderer::ReuseSome() noexcept {
  std::size_t left = kReusedPerStep;
  for (std::uint32_t tokens = 0; tokens < kTokensPerStep && reusing_ > 0 && left > 0; ++tokens) {
    const std::uint32_t index = reusing_ - 1;
    Reuse& reuse = reuses_[index];
    if (reuse.over || reuse.at >= keptBytes_ || reuse.reader.Status() != SvgTagStatus::kOk) {
      EndReuse();
      continue;
    }
    SvgTagToken token;
    const std::span<const std::uint8_t> rest =
        block_.subspan(reuse.at, std::min<std::size_t>(keptBytes_ - reuse.at, left));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the bytes kept, as the characters they are.
    std::size_t used = reuse.reader.Next({reinterpret_cast<const char*>(rest.data()), rest.size()}, token);
    const bool enough = Handle(reuse.reader, token, used);
    left -= used;
    reuse.at += static_cast<std::uint32_t>(used);
    report_.reused += static_cast<std::uint32_t>(used);
    // It is over when the element it began with is: at once, if that was
    // written `<a/>`, and otherwise when as many have closed as opened.
    if (token.kind == Kind::kOpened && !token.empty) {
      ++reuse.open;
    } else if ((token.kind == Kind::kOpened && reuse.open == 0) || (token.kind == Kind::kClose && reuse.open <= 1)) {
      reuse.over = true;
    } else if (token.kind == Kind::kClose) {
      --reuse.open;
    }
    // If it has itself begun the drawing of something, it waits for that.
    if (reuse.over && reusing_ == index + 1) {
      EndReuse();
    }
    if (enough || draining_ || strokeNext_) {
      break;
    }
  }
}

void SvgRenderer::EndReuse() noexcept {
  --reusing_;
  const Reuse& reuse = reuses_[reusing_];
  depth_ = reuse.levels;
  skipping_ = reuse.skipping;
  firstOfUse_ = false;
}

SvgStatus SvgRenderer::Finish() noexcept {
  // A shape the file ended in the middle of is not drawn.
  if (canvas_ != nullptr) {
    canvas_->Fill(FillRule::kNonZero, 0, 0);
  }
  draining_ = false;
  streaming_ = false;
  stroking_ = false;
  strokeNext_ = false;
  reusing_ = 0;
  keeping_ = false;
  if (inSheet_) {
    // A sheet the file ended in: its last rule, if it was in one, is lost.
    sheet_.Finish();
    Note(SvgLeftOut::kStyleSheet);
    inSheet_ = false;
  }
  building_ = false;
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
  isDefs_ = false;
  overflows_ = false;
  hasId_ = false;
  idPlain_ = true;
  idAt_ = 0;
  idLength_ = 0;
  hasWanted_ = false;
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
  // What a style sheet would know it by: its name, and its classes.
  elementNameSize_ = name.size() <= elementName_.size() ? static_cast<std::uint8_t>(name.size()) : 0;
  std::copy_n(name.begin(), elementNameSize_, elementName_.begin());
  classesSize_ = 0;
  fillNameSize_ = 0;
  // A style sheet, and what a gradient is made of, are read wherever they
  // are in the drawing, inside what is passed over or not; but not again
  // from what was kept.
  openingSheet_ = false;
  paintElement_ = PaintElement::kNone;
  if (reusing_ == 0 && rootSeen_ && (depth_ > 0 || skipping_ > 0)) {
    if (name == "style" && !inSheet_) {
      openingSheet_ = true;
      if (shapeSeen_) {
        Note(SvgLeftOut::kStyleSheet);
      }
    } else if (name == "linearGradient" && !building_) {
      paintElement_ = PaintElement::kGradient;
      building_ = true;
      gradient_ = Gradient();
      gradient_.ends[2] = {.value = 100 * kNumberOne, .unit = SvgUnit::kPercent};
    } else if (name == "stop" && building_ && skipping_ == gradientSkipping_) {
      paintElement_ = PaintElement::kStop;
      stopOffset_ = 0;
      stopGray_ = 0;
      stopAlpha_ = 255;
      stopOpacity_ = 255;
    }
  }
  // A `symbol` is drawn only where a `use` names it, and there it is an
  // `svg`: it may have a view box, and what is in it is cut off at its
  // edges.
  if (firstOfUse_ && name == "symbol") {
    name = "svg";
  }
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
  isDefs_ = name == "defs";
  switch (fate) {
    case Fate::kSvg:
      element_ = Element::kSvg;
      isInnerSvg_ = true;
      break;
    case Fate::kUse:
      element_ = Element::kUse;
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
  if (paintElement_ != PaintElement::kNone) {
    // What a gradient and its stops say of it, and nothing else.
    struct PaintName {
      std::string_view name;
      PaintSlot slot;
      bool ofStop;
    };
    constexpr PaintName kPaintNames[] = {
        {.name = "id", .slot = PaintSlot::kName, .ofStop = false},
        {.name = "href", .slot = PaintSlot::kLink, .ofStop = false},
        {.name = "xlink:href", .slot = PaintSlot::kLink, .ofStop = false},
        {.name = "x1", .slot = PaintSlot::kX1, .ofStop = false},
        {.name = "y1", .slot = PaintSlot::kY1, .ofStop = false},
        {.name = "x2", .slot = PaintSlot::kX2, .ofStop = false},
        {.name = "y2", .slot = PaintSlot::kY2, .ofStop = false},
        {.name = "gradientUnits", .slot = PaintSlot::kUnits, .ofStop = false},
        {.name = "gradientTransform", .slot = PaintSlot::kTransform, .ofStop = false},
        {.name = "spreadMethod", .slot = PaintSlot::kSpread, .ofStop = false},
        {.name = "offset", .slot = PaintSlot::kOffset, .ofStop = true},
    };
    const bool ofStop = paintElement_ == PaintElement::kStop;
    for (const PaintName& known : kPaintNames) {
      if (name == known.name && known.ofStop == ofStop) {
        attribute_ = Attribute::kPaint;
        paintSlot_ = known.slot;
        return;
      }
    }
    if (ofStop && name == "style") {
      attribute_ = Attribute::kStyle;
      nameSize_ = 0;
      nameTooLong_ = false;
      inDeclarationValue_ = false;
      brackets_ = 0;
    } else if (ofStop) {
      property_ = PropertyOf(name);
      if (property_ == Property::kStopColor || property_ == Property::kStopOpacity) {
        attribute_ = Attribute::kProperty;
      }
    }
    return;
  }
  // An element's name matters where it is being kept for a `use` to find.
  if (name == "id" && keeping_ && !hasId_) {
    hasId_ = true;
    attribute_ = Attribute::kId;
    return;
  }
  if (element_ == Element::kIgnored) {
    return;
  }
  if (name == "class") {
    attribute_ = Attribute::kClass;
    return;
  }
  if (element_ == Element::kUse && (name == "href" || name == "xlink:href")) {
    attribute_ = Attribute::kHref;
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
    bool ofBoxes = false;   // `svg` and `rect` have it,
    bool ofRounds = false;  // or `circle` and `ellipse`,
    bool ofLines = false;   // or `line`.
  };
  constexpr LengthName kLengths[] = {
      {.name = "x", .slot = kX, .ofBoxes = true},
      {.name = "y", .slot = kY, .ofBoxes = true},
      {.name = "width", .slot = kWidth, .ofBoxes = true},
      {.name = "height", .slot = kHeight, .ofBoxes = true},
      {.name = "rx", .slot = kRadiusX, .ofBoxes = true, .ofRounds = true},
      {.name = "ry", .slot = kRadiusY, .ofBoxes = true, .ofRounds = true},
      {.name = "cx", .slot = kMiddleX, .ofRounds = true},
      {.name = "cy", .slot = kMiddleY, .ofRounds = true},
      {.name = "r", .slot = kRadius, .ofRounds = true},
      {.name = "x1", .slot = kX, .ofLines = true},
      {.name = "y1", .slot = kY, .ofLines = true},
      {.name = "x2", .slot = kWidth, .ofLines = true},
      {.name = "y2", .slot = kHeight, .ofLines = true},
  };
  const bool box = element_ == Element::kSvg || element_ == Element::kRect;
  const bool round = element_ == Element::kCircle || element_ == Element::kEllipse;
  const bool line = element_ == Element::kLine;
  // A `use` has a place, and a size that only an `svg` it draws takes.
  const bool placed = element_ == Element::kUse && (name == "x" || name == "y" || name == "width" || name == "height");
  for (const LengthName& length : kLengths) {
    if (name == length.name &&
        ((box && length.ofBoxes) || (round && length.ofRounds) || (line && length.ofLines) || placed)) {
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
  constexpr PropertyName kPropertyNames[] = {
      {.name = "fill", .property = Property::kFill},
      {.name = "fill-rule", .property = Property::kFillRule},
      {.name = "fill-opacity", .property = Property::kFillOpacity},
      {.name = "opacity", .property = Property::kOpacity},
      {.name = "color", .property = Property::kColor},
      {.name = "display", .property = Property::kDisplay},
      {.name = "visibility", .property = Property::kVisibility},
      {.name = "stroke", .property = Property::kStroke},
      {.name = "stroke-width", .property = Property::kStrokeWidth},
      {.name = "stroke-opacity", .property = Property::kStrokeOpacity},
      {.name = "stroke-linejoin", .property = Property::kStrokeJoin},
      {.name = "stroke-linecap", .property = Property::kStrokeCap},
      {.name = "stroke-miterlimit", .property = Property::kMiterLimit},
      {.name = "stroke-dasharray", .property = Property::kDashArray},
      {.name = "stroke-dashoffset", .property = Property::kDashOffset},
      {.name = "clip-path", .property = Property::kClipPath},
      {.name = "mask", .property = Property::kMask},
      {.name = "filter", .property = Property::kFilter},
      {.name = "marker", .property = Property::kMarker},
      {.name = "marker-start", .property = Property::kMarker},
      {.name = "marker-mid", .property = Property::kMarker},
      {.name = "marker-end", .property = Property::kMarker},
      {.name = "overflow", .property = Property::kOverflow},
      {.name = "stop-color", .property = Property::kStopColor},
      {.name = "stop-opacity", .property = Property::kStopOpacity},
  };
  for (const PropertyName& known : kPropertyNames) {
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
    case Attribute::kId:  // Seen to before this is asked.
      break;
    case Attribute::kClass:
      // As many classes as there is room for: the rest are not looked at.
      for (const char c : piece) {
        if (classesSize_ < classes_.size()) {
          classes_[classesSize_] = c;
          ++classesSize_;
        }
      }
      break;
    case Attribute::kPaint:
    case Attribute::kHref:
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
  if (attribute == Attribute::kIgnored || attribute == Attribute::kPathData || attribute == Attribute::kId ||
      attribute == Attribute::kClass) {
    return;
  }
  if (attribute == Attribute::kStyle) {
    EndDeclaration();
    return;
  }
  if (attribute == Attribute::kHref) {
    // The name of something in this file: "#" and its `id`. Anything
    // else, or a name too long to hold, names nothing that can be found.
    const std::string_view name = WithoutSpace({value_.data(), valueSize_});
    hasWanted_ = !valueTooLong_ && name.size() > 1 && name.size() - 1 <= wanted_.size() && name.front() == '#';
    if (hasWanted_) {
      wantedSize_ = static_cast<std::uint32_t>(name.size() - 1);
      std::copy(name.begin() + 1, name.end(), wanted_.begin());
    }
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
    case Attribute::kPaint:
      SetPaint(value);
      break;
    case Attribute::kIgnored:
    case Attribute::kPathData:
    case Attribute::kStyle:
    case Attribute::kId:
    case Attribute::kHref:
    case Attribute::kClass:
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
  const std::uint32_t bit = std::uint32_t{1} << static_cast<unsigned>(property);
  if (fromStyle) {
    fromStyle_ |= bit;
  } else if ((fromStyle_ & bit) != 0) {
    return;
  }
  value = WithoutImportant(value);
  const Level& parent = Parent();
  const bool inherit = IsWord(value, "inherit");
  const bool none = IsWord(value, "none");
  if (paintElement_ == PaintElement::kStop) {
    // A stop of a gradient says only its colour and how solid it is.
    if (property == Property::kStopColor && !inherit) {
      if (IsWord(value, "currentcolor")) {
        stopGray_ = 0;
        stopAlpha_ = 255;
      } else if (!ParseSvgColour(value, stopGray_, stopAlpha_)) {
        Note(SvgLeftOut::kBadValue);
      }
    } else if (property == Property::kStopOpacity && !inherit && !ParseSvgOpacity(value, stopOpacity_)) {
      Note(SvgLeftOut::kBadValue);
    }
    return;
  }
  switch (property) {
    case Property::kNone:
    case Property::kStopColor:
    case Property::kStopOpacity:
      break;
    case Property::kFill: {
      SvgPaint paint;
      if (!ParseSvgPaint(value, paint)) {
        Note(SvgLeftOut::kBadValue);
      } else if (paint.kind == SvgPaint::Kind::kInherit) {
        pending_.fill = parent.fill;
        pending_.gradient = parent.gradient;
        fillNameSize_ = 0;
      } else {
        pending_.fill = paint;
        pending_.gradient = 0;
        fillNameSize_ = 0;
        if (paint.server) {
          TakeFillName(value);
        }
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
        pending_.stroke = parent.stroke;
      } else {
        pending_.stroke = paint;
      }
      break;
    }
    case Property::kStrokeWidth:
    case Property::kStrokeOpacity:
    case Property::kStrokeJoin:
    case Property::kStrokeCap:
    case Property::kMiterLimit:
    case Property::kDashArray:
    case Property::kDashOffset:
      SetStrokeProperty(property, value, inherit);
      break;
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
    case Property::kOverflow:
      // Whether an `svg` inside another lets what is in it show beyond
      // its edges. It does not unless it says so.
      overflows_ = IsWord(value, "visible") || IsWord(value, "auto");
      break;
  }
}

void SvgRenderer::SetStrokeProperty(Property property, std::string_view value, bool inherit) noexcept {
  const Level& parent = Parent();
  StrokeStyle& style = pending_.strokeStyle;
  // A percentage of a width or a length along the stroke is of the
  // diagonal over the square root of two, as a radius's is.
  const auto across = static_cast<std::uint64_t>(std::max<Fixed>(parent.viewWidth, 0));
  const auto down = static_cast<std::uint64_t>(std::max<Fixed>(parent.viewHeight, 0));
  const auto whole = static_cast<Fixed>(SquareRoot(((across * across) + (down * down)) / 2));
  bool clamped = false;
  bool bad = false;
  switch (property) {
    case Property::kStrokeWidth: {
      SvgLength width;
      if (inherit) {
        style.width = parent.strokeStyle.width;
      } else if (ParseSvgLength(value, width, clamped) && width.value >= 0) {
        style.width = LengthIn(width, whole, clamped);
      } else {
        bad = true;
      }
      break;
    }
    case Property::kStrokeOpacity:
      if (inherit) {
        pending_.strokeOpacity = parent.strokeOpacity;
      } else {
        bad = !ParseSvgOpacity(value, pending_.strokeOpacity);
      }
      break;
    case Property::kStrokeJoin:
      // SVG 2's "miter-clip" and "arcs" are drawn as a miter.
      if (inherit) {
        style.join = parent.strokeStyle.join;
      } else if (IsWord(value, "miter") || IsWord(value, "miter-clip") || IsWord(value, "arcs")) {
        style.join = StrokeJoin::kMiter;
      } else if (IsWord(value, "round")) {
        style.join = StrokeJoin::kRound;
      } else if (IsWord(value, "bevel")) {
        style.join = StrokeJoin::kBevel;
      } else {
        bad = true;
      }
      break;
    case Property::kStrokeCap:
      if (inherit) {
        style.cap = parent.strokeStyle.cap;
      } else if (IsWord(value, "butt")) {
        style.cap = StrokeCap::kButt;
      } else if (IsWord(value, "round")) {
        style.cap = StrokeCap::kRound;
      } else if (IsWord(value, "square")) {
        style.cap = StrokeCap::kSquare;
      } else {
        bad = true;
      }
      break;
    case Property::kMiterLimit: {
      const std::string_view text = WithoutSpace(value);
      std::size_t at = 0;
      Number limit = 0;
      if (inherit) {
        style.miterLimit = parent.strokeStyle.miterLimit;
      } else if (ScanNumber(text, at, limit, clamped) && at == text.size() && limit >= kNumberOne) {
        style.miterLimit = FixedOf(limit, clamped);
      } else {
        bad = true;
      }
      break;
    }
    case Property::kDashArray: {
      std::array<SvgLength, StrokeStyle::kMostDashes> lengths{};
      std::size_t count = 0;
      if (inherit) {
        style.dashes = parent.strokeStyle.dashes;
        style.dashCount = parent.strokeStyle.dashCount;
      } else if (IsWord(value, "none")) {
        style.dashCount = 0;
      } else if (ParseSvgLengths(value, lengths, count, clamped)) {
        // A length less than nothing makes the stroke solid, as does a
        // list of more lengths than are kept, which is reported as one that
        // could not be read.
        style.dashCount = static_cast<std::uint8_t>(count);
        for (std::size_t i = 0; i < count; ++i) {
          style.dashes[i] = LengthIn(lengths[i], whole, clamped);
          bad = bad || lengths[i].value < 0;
        }
      } else {
        style.dashCount = 0;
        bad = true;
      }
      break;
    }
    case Property::kDashOffset: {
      SvgLength offset;
      if (inherit) {
        style.dashOffset = parent.strokeStyle.dashOffset;
      } else if (ParseSvgLength(value, offset, clamped)) {
        style.dashOffset = LengthIn(offset, whole, clamped);
      } else {
        bad = true;
      }
      break;
    }
    default:
      break;
  }
  if (bad) {
    Note(SvgLeftOut::kBadValue);
  }
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
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

void SvgRenderer::TakeFillName(std::string_view value) noexcept {
  // "url(#name)", with or without quotes round what is in the brackets.
  constexpr std::string_view kUrl = "url(";
  const std::size_t close = value.find(')');
  // The brackets cannot close before they open, in the first letters.
  if (value.size() < kUrl.size() || close == std::string_view::npos ||
      !IsWord(std::string_view(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(kUrl.size())), kUrl)) {
    return;
  }
  std::string_view inside = WithoutSpace(std::string_view(value.begin() + static_cast<std::ptrdiff_t>(kUrl.size()),
                                                          value.begin() + static_cast<std::ptrdiff_t>(close)));
  if (inside.size() >= 2 && (inside.front() == '\'' || inside.front() == '"') && inside.back() == inside.front()) {
    inside = WithoutSpace(std::string_view(inside.begin() + 1, inside.end() - 1));
  }
  if (inside.size() > 1 && inside.front() == '#' && inside.size() - 1 <= fillName_.size()) {
    std::copy(inside.begin() + 1, inside.end(), fillName_.begin());
    fillNameSize_ = static_cast<std::uint8_t>(inside.size() - 1);
  }
}

bool SvgRenderer::Opened(bool empty) noexcept {
  EndAttribute();
  // What the style sheet says of it, and the gradient it is filled with,
  // each a look through what is kept: that is enough for one call.
  bool looked = false;
  if (skipping_ == 0 && element_ != Element::kIgnored) {
    if (rules_ > 0) {
      Style();
      ++report_.looked;
      looked = true;
    }
    if (fillNameSize_ > 0) {
      pending_.gradient = FindGradient({fillName_.data(), fillNameSize_});
      ++report_.looked;
      looked = true;
    }
    if (element_ != Element::kSvg && element_ != Element::kGroup && element_ != Element::kUse) {
      shapeSeen_ = true;
    }
  }
  const PaintElement paint = paintElement_;
  if (paint == PaintElement::kStop) {
    AddStop();
  }
  const bool drew = OpenedElement(empty);
  if (paint == PaintElement::kGradient) {
    if (empty) {
      looked = EndGradient() || looked;
    } else {
      gradientSkipping_ = skipping_;
    }
  }
  if (openingSheet_ && !empty) {
    inSheet_ = true;
    sheetSkipping_ = skipping_;
    sheet_ = SvgSheetReader();
  }
  return drew || looked;
}

bool SvgRenderer::OpenedElement(bool empty) noexcept {
  if (firstOfUse_) {
    firstOfUse_ = false;
    if (element_ == Element::kSvg) {
      if (useHasWidth_) {
        lengths_[kWidth] = useWidth_;
        lengthsSet_ |= BitOf(kWidth);
      }
      if (useHasHeight_) {
        lengths_[kHeight] = useHeight_;
        lengthsSet_ |= BitOf(kHeight);
      }
    }
  }
  if (skipping_ > 0) {
    if (keeping_ && hasId_ && idPlain_ && idLength_ > 0) {
      // An element in `defs` with a name: it can be found by it once all
      // of it has been kept, which for one written `<a/>` is now.
      if (empty) {
        Define(tagAt_, idAt_);
      } else if (openIdCount_ < openIds_.size()) {
        openIds_[openIdCount_] = {.at = tagAt_, .idAt = idAt_, .skipping = skipping_ + 1};
        ++openIdCount_;
      } else {
        Note(SvgLeftOut::kNoRoom);
      }
    }
    if (!empty) {
      ++skipping_;
    }
    return false;
  }
  if (element_ == Element::kIgnored) {
    if (!empty) {
      skipping_ = 1;
      // What `defs` holds is kept, unless it is being read from what was
      // kept already.
      if (isDefs_ && reusing_ == 0) {
        BeginKeeping();
      }
    }
    return false;
  }
  if (element_ == Element::kUse) {
    return OpenUse(empty);
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
    // One that is hidden is not laid out, nor is what it would cut off
    // reported.
    shows = shows && OpenInnerSvg();
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

bool SvgRenderer::Close() noexcept {
  bool looked = false;
  if (inSheet_ && skipping_ == sheetSkipping_) {
    sheet_.Finish();
    if (sheet_.Dropped() != 0) {
      Note(SvgLeftOut::kStyleSheet);
    }
    inSheet_ = false;
    ruleSize_ = 0;
    settingsAt_ = 0;
    ruleTooLong_ = false;
  }
  if (building_ && skipping_ == gradientSkipping_) {
    looked = EndGradient();
  }
  if (skipping_ > 0) {
    // An element with a name that is now over, in `defs`, can be found.
    if (keeping_ && openIdCount_ > 0 && openIds_[openIdCount_ - 1].skipping == skipping_) {
      --openIdCount_;
      Define(openIds_[openIdCount_].at, openIds_[openIdCount_].idAt);
    }
    --skipping_;
    if (skipping_ == 0) {
      keeping_ = false;
    }
  } else if (depth_ > 0) {
    --depth_;
  }
  return looked;
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
  if (placed) {
    pending_.matrix = Multiply(place_, pending_.matrix, clamped);
    // Nothing is drawn outside the picture, whatever the drawing's
    // `overflow` says: there is nothing there to draw on. Placed turned
    // other than by quarters, the picture is no box of pixels, and what
    // lies outside it is drawn.
    const bool square = place_.b == 0 && place_.c == 0;
    const bool quarter = place_.a == 0 && place_.d == 0;
    if (square || quarter) {
      const Point one = Apply(place_, {.x = 0, .y = 0}, clamped);
      const Point other = Apply(place_, {.x = width_, .y = height_}, clamped);
      pending_.boxed = true;
      pending_.boxFrom = {.x = std::min(one.x, other.x), .y = std::min(one.y, other.y)};
      pending_.boxTo = {.x = std::max(one.x, other.x), .y = std::max(one.y, other.y)};
    }
  }
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
  }
  return true;
}

void SvgRenderer::BoxIn(const SvgBox& space) noexcept {
  const Level& parent = Parent();
  // A box that has been turned or slanted is no box of pixels, but for a
  // quarter turn. What is in one that is not is then drawn whole, and that
  // is reported.
  const bool square = parent.matrix.b == 0 && parent.matrix.c == 0;
  const bool quarter = parent.matrix.a == 0 && parent.matrix.d == 0;
  if (!square && !quarter) {
    Note(SvgLeftOut::kViewportClip);
    return;
  }
  bool clamped = false;
  const Point one = Apply(parent.matrix, {.x = space.x, .y = space.y}, clamped);
  const Point other = Apply(
      parent.matrix, {.x = Sum(space.x, space.width, clamped), .y = Sum(space.y, space.height, clamped)}, clamped);
  Point from = {.x = std::min(one.x, other.x), .y = std::min(one.y, other.y)};
  Point to = {.x = std::max(one.x, other.x), .y = std::max(one.y, other.y)};
  // Within whatever box it is in already.
  if (parent.boxed) {
    from = {.x = std::max(from.x, parent.boxFrom.x), .y = std::max(from.y, parent.boxFrom.y)};
    to = {.x = std::min(to.x, parent.boxTo.x), .y = std::min(to.y, parent.boxTo.y)};
  }
  pending_.boxed = true;
  pending_.boxFrom = from;
  pending_.boxTo = to;
  if (clamped) {
    Note(SvgLeftOut::kHeldNumber);
  }
}

bool SvgRenderer::OpenInnerSvg() noexcept {
  const Level& parent = Parent();
  const SvgBox space = {.x = LengthAt(kX, 0),
                        .y = LengthAt(kY, 0),
                        .width = LengthAt(kWidth, parent.viewWidth),
                        .height = LengthAt(kHeight, parent.viewHeight)};
  if (space.width <= 0 || space.height <= 0) {
    return false;
  }
  // What is in it is cut off at its edges, unless it says otherwise.
  if (!overflows_) {
    BoxIn(space);
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
  if (canvas_ == nullptr || displayNone_ || unreadable_ || lateTransform_ || outlineBad_ || pending_.hidden) {
    return false;
  }
  std::uint8_t solid = pending_.fill.alpha;
  gray = pending_.fill.gray;
  if (pending_.gradient != 0) {
    // A gradient that was found: how solid it is is its own, at each place.
    gray = 0;
    solid = 255;
  } else if (pending_.fill.kind == SvgPaint::Kind::kCurrent) {
    gray = pending_.color;
    solid = pending_.colorAlpha;
  } else if (pending_.fill.kind != SvgPaint::Kind::kGray) {
    return false;
  }
  alpha = Both(Both(solid, pending_.fillOpacity), Both(Parent().opacity, ownOpacity_));
  return alpha != 0;
}

bool SvgRenderer::StrokeShows(Filling& filling) const noexcept {
  if (canvas_ == nullptr || displayNone_ || unreadable_ || lateTransform_ || outlineBad_ || pending_.hidden ||
      pending_.strokeStyle.width <= 0) {
    return false;
  }
  const SvgPaint& paint = pending_.stroke;
  std::uint8_t gray = paint.gray;
  std::uint8_t solid = paint.alpha;
  if (paint.kind == SvgPaint::Kind::kCurrent) {
    gray = pending_.color;
    solid = pending_.colorAlpha;
  } else if (paint.kind != SvgPaint::Kind::kGray) {
    return false;
  }
  filling = {.rule = FillRule::kNonZero,
             .gray = gray,
             .alpha = Both(Both(solid, pending_.strokeOpacity), Both(Parent().opacity, ownOpacity_)),
             .boxed = pending_.boxed,
             .boxFrom = pending_.boxFrom,
             .boxTo = pending_.boxTo,
             .gradient = 0,
             .matrix = Matrix(),
             .viewWidth = 0,
             .viewHeight = 0};
  return filling.alpha != 0;
}

bool SvgRenderer::DrawShape() noexcept {
  if (!displayNone_ && !pending_.hidden) {
    // A stroke painted with a gradient or a pattern: what stands in for it
    // is drawn.
    if (pending_.stroke.server && pending_.strokeStyle.width > 0) {
      Note(SvgLeftOut::kStroke);
    }
    if (pending_.fill.server && pending_.gradient == 0) {
      Note(SvgLeftOut::kPaintServer);
    }
  }
  if (lateTransform_) {
    Note(SvgLeftOut::kLateTransform);
  }
  std::uint8_t gray = 0;
  std::uint8_t alpha = 0;
  // A line has no inside to fill.
  const bool shown = element_ != Element::kLine && Shows(gray, alpha);
  const Filling filling = {.rule = pending_.evenOdd ? FillRule::kEvenOdd : FillRule::kNonZero,
                           .gray = gray,
                           .alpha = alpha,
                           .boxed = pending_.boxed,
                           .boxFrom = pending_.boxFrom,
                           .boxTo = pending_.boxTo,
                           .gradient = shown ? pending_.gradient : 0,
                           .matrix = pending_.gradient != 0 && shown ? ElementMatrix() : Matrix(),
                           .viewWidth = pending_.viewWidth,
                           .viewHeight = pending_.viewHeight};
  Filling stroke;
  const bool stroked = StrokeShows(stroke);

  switch (element_) {
    case Element::kPath:
    case Element::kPolygon:
    case Element::kPolyline:
      if (streaming_) {
        // Its lines are in the canvas already, whether or not it shows:
        // its stroke's, if it was being stroked, or else its own.
        EndOutline();
        if (stroking_) {
          const bool same = pending_.strokeStyle == streamStyle_;
          if (shown || (stroked && !same)) {
            Note(SvgLeftOut::kStroke);
          }
          FillOutline(stroked && same, stroke);
        } else {
          if (stroked) {
            Note(SvgLeftOut::kStroke);
          }
          FillOutline(shown, filling);
        }
        return true;
      }
      if (heldSize_ == 0 || (!shown && !stroked)) {
        return false;
      }
      // Its data was short enough to hold, and is gone through now that
      // the whole of its tag has been read: for its fill, and then for its
      // stroke.
      if (shown) {
        BeginOutline(ElementMatrix());
        drain_ = filling;
        StrokeLater(stroked, stroke);
      } else {
        BeginStroke(ElementMatrix(), pending_.strokeStyle);
        drain_ = stroke;
      }
      draining_ = true;
      return true;
    case Element::kRect:
    case Element::kCircle:
    case Element::kEllipse:
    case Element::kLine:
      if ((!shown && !stroked) || !HasOutline()) {
        return false;
      }
      if (shown) {
        BeginOutline(ElementMatrix());
        Outline();
        FillOutline(true, filling);
        StrokeLater(stroked, stroke);
      } else {
        BeginStroke(ElementMatrix(), pending_.strokeStyle);
        Outline();
        FillOutline(true, stroke);
      }
      return true;
    case Element::kIgnored:
    case Element::kSvg:
    case Element::kGroup:
    case Element::kUse:
      return false;
  }
  return false;
}

void SvgRenderer::StrokeLater(bool stroked, const Filling& stroke) noexcept {
  strokeNext_ = stroked;
  if (stroked) {
    strokeFilling_ = stroke;
    strokeMatrix_ = ElementMatrix();
  }
}

void SvgRenderer::StrokeNext() noexcept {
  strokeNext_ = false;
  BeginStroke(strokeMatrix_, pending_.strokeStyle);
  if (element_ == Element::kPath || element_ == Element::kPolygon || element_ == Element::kPolyline) {
    // The data held is gone through again.
    heldAt_ = 0;
    draining_ = true;
    drain_ = strokeFilling_;
    return;
  }
  Outline();
  FillOutline(true, strokeFilling_);
}

void SvgRenderer::BeginOutline(const Matrix& matrix) noexcept {
  stroking_ = false;
  flattener_ = PathFlattener(*canvas_, matrix, tolerance_);
  // What lies outside the strip needs no cutting up.
  bool clamped = false;
  const auto pixels = [&clamped](std::int64_t count) noexcept { return Clamp32(count * kFixedOne, clamped); };
  flattener_.KeepTo({.x = 0, .y = pixels(canvas_->Top())},
                    {.x = pixels(canvas_->Width()), .y = pixels(std::int64_t{canvas_->Top()} + canvas_->Rows())});
  StartReading();
}

void SvgRenderer::BeginStroke(const Matrix& matrix, const StrokeStyle& style) noexcept {
  stroking_ = true;
  strokeOut_ = PathFlattener(*canvas_, matrix, tolerance_);
  const std::int64_t scale = std::max<Coefficient>(LargestScale(matrix), 1);
  // A stroke is thinnest in pixels across the way the matrix makes things
  // smallest, which is at least its determinant over how much it makes
  // them larger: in kCoefficientOne-ths, from the four coefficients each
  // multiplied out and brought back to that.
  StrokeStyle drawn = style;
  widening_ = false;
  if (thinnest_ > 0) {
    const std::int64_t determinant = ShiftDown(std::int64_t{matrix.a} * matrix.d, kCoefficientBits) -
                                     ShiftDown(std::int64_t{matrix.b} * matrix.c, kCoefficientBits);
    const std::int64_t smallest = (((determinant < 0 ? -determinant : determinant) * 1024) / scale) * 1024;
    const std::int64_t pixels = ShiftDown(std::int64_t{style.width} * smallest, kCoefficientBits);
    if (smallest > 0 && pixels < thinnest_) {
      bool clamped = false;
      drawn.width = Clamp32((std::int64_t{thinnest_} * kCoefficientOne) / smallest, clamped);
      widening_ = true;
    }
  }
  stroker_.Begin(strokeOut_, drawn);
  // The outline is cut into lines in its own coordinates, as finely as it
  // would be in pixels: the tolerance there is the tolerance in pixels
  // over how much the matrix makes things larger. Not more than a 4,096th
  // of what a position can be, which keeps its square in range.
  constexpr std::int64_t kLoosest = 16777216;
  const std::int64_t tolerance =
      std::clamp<std::int64_t>((std::int64_t{tolerance_} * kCoefficientOne) / scale, 1, kLoosest);
  flattener_ = PathFlattener(stroker_, Matrix(), static_cast<Fixed>(tolerance));
  StartReading();
}

void SvgRenderer::StartReading() noexcept {
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
  // The lines that went into the canvas: a stroke's, if it is one.
  const std::uint32_t lines = stroking_ ? strokeOut_.Lines() : flattener_.Lines();
  report_.lines += lines - talliedLines_;
  talliedLines_ = lines;
}

void SvgRenderer::FillOutline(bool shown, const Filling& filling) noexcept {
  if (stroking_) {
    stroker_.Finish();
    if (stroker_.Clamped() || strokeOut_.Clamped()) {
      Note(SvgLeftOut::kHeldNumber);
    }
    if (stroker_.DashesCut()) {
      Note(SvgLeftOut::kStroke);
    }
  }
  if (flattener_.Clamped()) {
    Note(SvgLeftOut::kHeldNumber);
  }
  Tally();
  // A gradient that comes to nothing for this shape shows nothing.
  const bool graded = shown && filling.gradient != 0;
  if (graded && !Ramp(filling)) {
    shown = false;
  }
  // With nothing of it to show, this only clears the outline away.
  const std::uint8_t alpha = shown ? filling.alpha : 0;
  if (shown && graded) {
    if (filling.boxed) {
      canvas_->Fill(filling.rule, ramp_, alpha, filling.boxFrom, filling.boxTo);
    } else {
      canvas_->Fill(filling.rule, ramp_, alpha);
    }
  } else if (filling.boxed) {
    canvas_->Fill(filling.rule, filling.gray, alpha, filling.boxFrom, filling.boxTo);
  } else {
    canvas_->Fill(filling.rule, filling.gray, alpha);
  }
  if (shown && stroking_) {
    ++report_.strokes;
    if (widening_) {
      ++report_.widened;
    }
  } else if (shown) {
    ++report_.shapes;
  }
  stroking_ = false;
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
  if (!streaming_) {
    // That was the whole of an outline whose tag has ended.
    EndOutline();
    FillOutline(true, drain_);
    if (strokeNext_) {
      // Its data is gone through again, for its stroke.
      heldAt_ = 0;
      return;
    }
  }
  heldSize_ = 0;
  heldAt_ = 0;
}

std::size_t SvgRenderer::TakePathData(std::string_view piece, bool ofInput) noexcept {
  // With nothing to draw into, or once the outline has been given up, the
  // data is not read.
  if (canvas_ == nullptr || outlineBad_) {
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
  // It is stroked if by then it has a stroke and no fill, and filled
  // otherwise. A fill that names a paint is taken to have one, since what it
  // names is not looked for until the tag ends.
  streaming_ = true;
  Filling unused;
  std::uint8_t gray = 0;
  std::uint8_t alpha = 0;
  if (StrokeShows(unused) && !Shows(gray, alpha) && !pending_.fill.server) {
    streamStyle_ = pending_.strokeStyle;
    BeginStroke(ElementMatrix(), streamStyle_);
  } else {
    BeginOutline(ElementMatrix());
  }
  draining_ = true;
  return from + fits;
}

bool SvgRenderer::HasOutline() const noexcept {
  switch (element_) {
    case Element::kRect:
      return LengthAt(kWidth, 0) > 0 && LengthAt(kHeight, 0) > 0;
    case Element::kCircle:
      return LengthAt(kRadius, 0) > 0;
    case Element::kEllipse:
      return LengthAt(kRadiusX, 0) > 0 && LengthAt(kRadiusY, 0) > 0;
    case Element::kLine:
      return true;
    case Element::kIgnored:
    case Element::kSvg:
    case Element::kGroup:
    case Element::kPath:
    case Element::kPolygon:
    case Element::kPolyline:
    case Element::kUse:
      break;
  }
  return false;
}

void SvgRenderer::Outline() noexcept {
  switch (element_) {
    case Element::kRect:
      OutlineRect();
      break;
    case Element::kCircle: {
      const Fixed radius = LengthAt(kRadius, 0);
      OutlineEllipse(LengthAt(kMiddleX, 0), LengthAt(kMiddleY, 0), radius, radius);
      break;
    }
    case Element::kEllipse:
      OutlineEllipse(LengthAt(kMiddleX, 0), LengthAt(kMiddleY, 0), LengthAt(kRadiusX, 0), LengthAt(kRadiusY, 0));
      break;
    case Element::kLine:
      // From (x1, y1) to (x2, y2), which are in the places of x, y, width
      // and height.
      flattener_.MoveTo({.x = LengthAt(kX, 0), .y = LengthAt(kY, 0)});
      flattener_.LineTo({.x = LengthAt(kWidth, 0), .y = LengthAt(kHeight, 0)});
      report_.commands += 2;
      break;
    case Element::kIgnored:
    case Element::kSvg:
    case Element::kGroup:
    case Element::kPath:
    case Element::kPolygon:
    case Element::kPolyline:
    case Element::kUse:
      break;
  }
}

void SvgRenderer::OutlineRect() noexcept {
  const Fixed x = LengthAt(kX, 0);
  const Fixed y = LengthAt(kY, 0);
  const Fixed width = LengthAt(kWidth, 0);
  const Fixed height = LengthAt(kHeight, 0);
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
}

void SvgRenderer::OutlineEllipse(Fixed middleX, Fixed middleY, Fixed radiusX, Fixed radiusY) noexcept {
  bool clamped = false;
  const Fixed left = Sum(middleX, -radiusX, clamped);
  const Fixed right = Sum(middleX, radiusX, clamped);
  const Fixed top = Sum(middleY, -radiusY, clamped);
  const Fixed bottom = Sum(middleY, radiusY, clamped);
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
}

}  // namespace bn::draw
