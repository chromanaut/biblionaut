#include "draw/svg_path.h"

#include <cstddef>

namespace bn::draw {

namespace {

// How many numbers a command takes, or -1 if the letter is not a command.
int ArgumentsOf(char command) noexcept {
  switch (command) {
    case 'M':
    case 'm':
    case 'L':
    case 'l':
    case 'T':
    case 't':
      return 2;
    case 'H':
    case 'h':
    case 'V':
    case 'v':
      return 1;
    case 'C':
    case 'c':
      return 6;
    case 'S':
    case 's':
    case 'Q':
    case 'q':
      return 4;
    case 'A':
    case 'a':
      return 7;
    case 'Z':
    case 'z':
      return 0;
    default:
      return -1;
  }
}

bool IsRelative(char command) noexcept { return command >= 'a' && command <= 'z'; }

char Lower(char command) noexcept { return IsRelative(command) ? command : static_cast<char>(command - 'A' + 'a'); }

bool StartsNumber(char c) noexcept { return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'; }

}  // namespace

const char* SvgPathStatusName(SvgPathStatus status) noexcept {
  switch (status) {
    case SvgPathStatus::kOk:
      return "ok";
    case SvgPathStatus::kBadData:
      return "bad data";
  }
  return "unknown";
}

void SvgPathReader::Feed(std::string_view text) noexcept { (void)Step(text, UINT32_MAX); }

std::size_t SvgPathReader::Step(std::string_view text, std::uint32_t commands) noexcept {
  const std::uint32_t before = commands_;
  std::size_t taken = 0;
  while (taken < text.size() && commands_ - before < commands) {
    if (status_ != SvgPathStatus::kOk) {
      // Nothing after a mistake is looked at: the rest is taken unread.
      return text.size();
    }
    Take(text[taken]);
    ++bytes_;
    ++taken;
  }
  return taken;
}

SvgPathStatus SvgPathReader::Finish() noexcept {
  if (status_ == SvgPathStatus::kOk && inNumber_) {
    EndNumber();
  }
  // The data may not end part of the way through a command, on a command
  // that has had no numbers, or on a comma.
  if (status_ == SvgPathStatus::kOk && (argumentCount_ != 0 || commaPending_ || !satisfied_)) {
    Fail();
  }
  return status_;
}

void SvgPathReader::Fail() noexcept {
  status_ = SvgPathStatus::kBadData;
  errorOffset_ = bytes_;
}

// The two flags of an arc are each a single 0 or 1, and nothing need come
// between one of them and what follows it: "a1 1 0 011 1" ends in a place of
// (1, 1). So they are not read as numbers are.
bool SvgPathReader::WantsFlag() const noexcept {
  return (command_ == 'A' || command_ == 'a') && (argumentCount_ == 3 || argumentCount_ == 4);
}

void SvgPathReader::Take(char c) noexcept {
  if (inNumber_) {
    if (number_.Take(c)) {
      return;
    }
    EndNumber();
    if (status_ != SvgPathStatus::kOk) {
      return;
    }
    // `c` is not part of the number, and is looked at afresh.
  }
  if (IsSvgSpace(c)) {
    return;
  }
  if (c == ',') {
    if (!commaAllowed_) {
      Fail();
      return;
    }
    commaAllowed_ = false;
    commaPending_ = true;
    return;
  }
  if (WantsFlag()) {
    if (c == '0' || c == '1') {
      Store(c == '1' ? kNumberOne : 0);
    } else {
      Fail();
    }
    return;
  }
  if (StartsNumber(c)) {
    // A number belongs to a command, and to one that takes numbers.
    if (command_ == 0 || ArgumentsOf(command_) == 0) {
      Fail();
      return;
    }
    number_.Reset();
    inNumber_ = number_.Take(c);
    commaAllowed_ = false;
    commaPending_ = false;
    return;
  }
  const int arguments = ArgumentsOf(c);
  // A command may not come before the last one has its numbers, nor after
  // a comma, and the first has to be a move.
  if (arguments < 0 || argumentCount_ != 0 || commaPending_ || !satisfied_ || (command_ == 0 && c != 'M' && c != 'm')) {
    Fail();
    return;
  }
  command_ = c;
  commaAllowed_ = false;
  satisfied_ = false;
  if (arguments == 0) {
    Emit();
  }
}

void SvgPathReader::EndNumber() noexcept {
  inNumber_ = false;
  if (!number_.Valid()) {
    Fail();
    return;
  }
  Store(number_.Value(clamped_));
}

void SvgPathReader::Store(Number value) noexcept {
  arguments_[argumentCount_] = value;
  ++argumentCount_;
  commaAllowed_ = true;
  commaPending_ = false;
  if (argumentCount_ == ArgumentsOf(command_)) {
    argumentCount_ = 0;
    Emit();
  }
}

Number SvgPathReader::Held(Number value) noexcept {
  if (value > kFurthest || value < -kFurthest) {
    clamped_ = true;
    return value < 0 ? -kFurthest : kFurthest;
  }
  return value;
}

Number SvgPathReader::Along(Number from, std::size_t index, bool relative) noexcept {
  return Held(relative ? from + arguments_[index] : arguments_[index]);
}

SvgPathReader::Place SvgPathReader::PlaceAt(std::size_t index, bool relative) noexcept {
  return {.x = Along(current_.x, index, relative), .y = Along(current_.y, index + 1, relative)};
}

SvgPathReader::Place SvgPathReader::Mirrored() noexcept {
  return {.x = Held((current_.x * 2) - lastControl_.x), .y = Held((current_.y * 2) - lastControl_.y)};
}

Point SvgPathReader::Rounded(Place place) noexcept {
  return {.x = FixedOf(place.x, clamped_), .y = FixedOf(place.y, clamped_)};
}

// After an outline is closed, whatever is drawn next that is not a move
// begins a new outline where the closed one began.
void SvgPathReader::BeginDrawing() noexcept {
  if (needsMove_) {
    sink_->MoveTo(Rounded(start_));
    needsMove_ = false;
  }
}

void SvgPathReader::Emit() noexcept {
  const bool relative = IsRelative(command_);
  Curve curve = Curve::kNone;
  switch (Lower(command_)) {
    case 'm': {
      const Place to = PlaceAt(0, relative);
      sink_->MoveTo(Rounded(to));
      current_ = to;
      start_ = to;
      needsMove_ = false;
      // Further pairs of numbers after a move are lines.
      command_ = relative ? 'l' : 'L';
      break;
    }
    case 'l': {
      const Place to = PlaceAt(0, relative);
      BeginDrawing();
      sink_->LineTo(Rounded(to));
      current_ = to;
      break;
    }
    case 'h': {
      const Place to = {.x = Along(current_.x, 0, relative), .y = current_.y};
      BeginDrawing();
      sink_->LineTo(Rounded(to));
      current_ = to;
      break;
    }
    case 'v': {
      const Place to = {.x = current_.x, .y = Along(current_.y, 0, relative)};
      BeginDrawing();
      sink_->LineTo(Rounded(to));
      current_ = to;
      break;
    }
    case 'c':
    case 's': {
      // The short form's first control point mirrors the second of the
      // cubic before it, or is where the pen is if there was none.
      const bool full = Lower(command_) == 'c';
      const Place control1 = full ? PlaceAt(0, relative) : (lastCurve_ == Curve::kCubic ? Mirrored() : current_);
      const Place control2 = PlaceAt(full ? 2 : 0, relative);
      const Place to = PlaceAt(full ? 4 : 2, relative);
      BeginDrawing();
      sink_->CubicTo(Rounded(control1), Rounded(control2), Rounded(to));
      lastControl_ = control2;
      curve = Curve::kCubic;
      current_ = to;
      break;
    }
    case 'q':
    case 't': {
      const bool full = Lower(command_) == 'q';
      const Place control = full ? PlaceAt(0, relative) : (lastCurve_ == Curve::kQuad ? Mirrored() : current_);
      const Place to = PlaceAt(full ? 2 : 0, relative);
      BeginDrawing();
      sink_->QuadTo(Rounded(control), Rounded(to));
      lastControl_ = control;
      curve = Curve::kQuad;
      current_ = to;
      break;
    }
    case 'a': {
      // A radius written with a minus sign is taken without it.
      const Fixed radiusX = FixedOf(arguments_[0], clamped_);
      const Fixed radiusY = FixedOf(arguments_[1], clamped_);
      const Place to = PlaceAt(5, relative);
      BeginDrawing();
      sink_->ArcTo(radiusX < 0 ? -radiusX : radiusX, radiusY < 0 ? -radiusY : radiusY, AngleOfDegrees(arguments_[2]),
                   arguments_[3] != 0, arguments_[4] != 0, Rounded(to));
      current_ = to;
      break;
    }
    default:  // 'z'
      // Closing an outline that has just been closed draws nothing.
      if (!needsMove_) {
        sink_->Close();
      }
      current_ = start_;
      needsMove_ = true;
      break;
  }
  lastCurve_ = curve;
  satisfied_ = true;
  ++commands_;
}

}  // namespace bn::draw
