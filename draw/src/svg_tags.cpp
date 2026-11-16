#include "draw/svg_tags.h"

namespace bn::draw {

namespace {

using Kind = SvgTagToken::Kind;

constexpr bool IsSpace(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
constexpr bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool IsLetter(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// A name begins with a letter, `_` or `:`, or with any character beyond
// ASCII, and goes on with those, digits, `-` and `.`.
constexpr bool BeginsName(char c) noexcept {
  return IsLetter(c) || c == '_' || c == ':' || static_cast<unsigned char>(c) >= 0x80;
}
constexpr bool CarriesName(char c) noexcept { return BeginsName(c) || IsDigit(c) || c == '-' || c == '.'; }

// What follows `<!` where text is marked as text.
constexpr std::string_view kCDataStart = "[CDATA[";

// The largest character there is, and the range kept for writing the
// larger ones in UTF-16, which are not characters themselves.
constexpr std::uint32_t kLastCharacter = 0x10FFFF;
constexpr std::uint32_t kFirstSurrogate = 0xD800;
constexpr std::uint32_t kLastSurrogate = 0xDFFF;

}  // namespace

const char* SvgTagStatusName(SvgTagStatus status) noexcept {
  switch (status) {
    case SvgTagStatus::kOk:
      return "ok";
    case SvgTagStatus::kMalformed:
      return "malformed";
    case SvgTagStatus::kUnbalanced:
      return "unbalanced";
    case SvgTagStatus::kTruncated:
      return "truncated";
  }
  return "unknown";
}

std::size_t SvgTagReader::Give(Kind kind, std::string_view text, bool ofInput, std::size_t taken,
                               SvgTagToken& token) noexcept {
  token.kind = kind;
  token.text = text;
  token.ofInput = ofInput;
  offset_ += static_cast<std::uint32_t>(taken);
  return taken;
}

std::size_t SvgTagReader::Fail(SvgTagStatus status, std::size_t taken, std::size_t size) noexcept {
  status_ = status;
  errorOffset_ = offset_ + static_cast<std::uint32_t>(taken);
  offset_ += static_cast<std::uint32_t>(size);
  return size;
}

void SvgTagReader::AddToName(char c) noexcept {
  if (nameSize_ < name_.size()) {
    name_[nameSize_] = c;
    ++nameSize_;
  }
}

std::size_t SvgTagReader::Decoded() noexcept {
  const std::string_view name(entity_.data() + 1, entitySize_ - 1U);
  char one = 0;
  if (name == "amp") {
    one = '&';
  } else if (name == "lt") {
    one = '<';
  } else if (name == "gt") {
    one = '>';
  } else if (name == "quot") {
    one = '"';
  } else if (name == "apos") {
    one = '\'';
  }
  if (one != 0) {
    entity_[0] = one;
    return 1;
  }
  if (name.size() < 2 || name[0] != '#') {
    return 0;
  }

  // A character by its number, in tens or, after an `x`, in sixteens.
  const bool sixteens = name[1] == 'x' || name[1] == 'X';
  std::uint32_t character = 0;
  std::size_t digits = 0;
  for (std::size_t at = sixteens ? 2 : 1; at < name.size(); ++at) {
    const char c = name[at];
    std::uint32_t digit = 0;
    if (IsDigit(c)) {
      digit = static_cast<std::uint32_t>(c - '0');
    } else if (sixteens && c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint32_t>(c - 'a') + 10;
    } else if (sixteens && c >= 'A' && c <= 'F') {
      digit = static_cast<std::uint32_t>(c - 'A') + 10;
    } else {
      return 0;
    }
    character = (character * (sixteens ? 16U : 10U)) + digit;
    if (character > kLastCharacter) {
      return 0;
    }
    ++digits;
  }
  if (digits == 0 || character == 0 || (character >= kFirstSurrogate && character <= kLastSurrogate)) {
    return 0;
  }

  // As UTF-8: one byte for ASCII, and up to four beyond it.
  if (character < 0x80) {
    entity_[0] = static_cast<char>(character);
    return 1;
  }
  if (character < 0x800) {
    entity_[0] = static_cast<char>(0xC0U | (character >> 6U));
    entity_[1] = static_cast<char>(0x80U | (character & 0x3FU));
    return 2;
  }
  if (character < 0x10000) {
    entity_[0] = static_cast<char>(0xE0U | (character >> 12U));
    entity_[1] = static_cast<char>(0x80U | ((character >> 6U) & 0x3FU));
    entity_[2] = static_cast<char>(0x80U | (character & 0x3FU));
    return 3;
  }
  entity_[0] = static_cast<char>(0xF0U | (character >> 18U));
  entity_[1] = static_cast<char>(0x80U | ((character >> 12U) & 0x3FU));
  entity_[2] = static_cast<char>(0x80U | ((character >> 6U) & 0x3FU));
  entity_[3] = static_cast<char>(0x80U | (character & 0x3FU));
  return 4;
}

std::size_t SvgTagReader::Next(std::string_view bytes, SvgTagToken& token) noexcept {
  token = SvgTagToken();
  const std::size_t size = bytes.size();
  if (status_ != SvgTagStatus::kOk) {
    return size;
  }
  std::size_t at = 0;
  while (at < size) {
    const char c = bytes[at];
    switch (state_) {
      case State::kContent: {
        const std::size_t from = at;
        while (at < size && bytes[at] != '<' && bytes[at] != '&') {
          ++at;
        }
        if (at > from) {
          return Give(Kind::kText, {bytes.data() + from, at - from}, true, at, token);
        }
        if (c == '<') {
          tagOffset_ = offset_ + static_cast<std::uint32_t>(at);
          state_ = State::kLess;
          ++at;
        } else {
          ++at;
          back_ = State::kContent;
          entity_[0] = '&';
          entitySize_ = 1;
          state_ = State::kEntity;
        }
        break;
      }

      case State::kLess:
        if (c == '/') {
          ++at;
          nameSize_ = 0;
          state_ = State::kCloseName;
        } else if (c == '?') {
          ++at;
          matched_ = 0;
          state_ = State::kInstruction;
        } else if (c == '!') {
          ++at;
          nameSize_ = 0;
          state_ = State::kBang;
        } else if (BeginsName(c)) {
          nameSize_ = 0;
          state_ = State::kOpenName;
        } else {
          return Fail(SvgTagStatus::kMalformed, at, size);
        }
        break;

      case State::kOpenName:
        if (!CarriesName(c)) {
          state_ = State::kInTag;
          return Give(Kind::kOpen, Name(), false, at, token);
        }
        AddToName(c);
        ++at;
        break;

      case State::kInTag:
        if (IsSpace(c)) {
          ++at;
        } else if (c == '/') {
          ++at;
          state_ = State::kEmptySlash;
        } else if (c == '>') {
          ++at;
          if (depth_ < UINT32_MAX) {
            ++depth_;
          }
          state_ = State::kContent;
          return Give(Kind::kOpened, {}, false, at, token);
        } else if (BeginsName(c)) {
          nameSize_ = 0;
          state_ = State::kAttributeName;
        } else {
          return Fail(SvgTagStatus::kMalformed, at, size);
        }
        break;

      case State::kEmptySlash:
        if (c != '>') {
          return Fail(SvgTagStatus::kMalformed, at, size);
        }
        ++at;
        state_ = State::kContent;
        token.empty = true;
        return Give(Kind::kOpened, {}, false, at, token);

      case State::kAttributeName:
        if (!CarriesName(c)) {
          state_ = State::kAfterAttributeName;
          return Give(Kind::kAttribute, Name(), false, at, token);
        }
        AddToName(c);
        ++at;
        break;

      case State::kAfterAttributeName:
        if (IsSpace(c)) {
          ++at;
        } else if (c == '=') {
          ++at;
          state_ = State::kBeforeValue;
        } else {
          // An attribute with no value, as HTML writes them: it has none.
          state_ = State::kInTag;
        }
        break;

      case State::kBeforeValue:
        if (IsSpace(c)) {
          ++at;
        } else if (c == '"' || c == '\'') {
          ++at;
          quote_ = c;
          state_ = State::kValue;
        } else {
          return Fail(SvgTagStatus::kMalformed, at, size);
        }
        break;

      case State::kValue: {
        const std::size_t from = at;
        while (at < size && bytes[at] != quote_ && bytes[at] != '&') {
          ++at;
        }
        if (at > from) {
          return Give(Kind::kValue, {bytes.data() + from, at - from}, true, at, token);
        }
        ++at;
        if (c == quote_) {
          state_ = State::kInTag;
        } else {
          back_ = State::kValue;
          entity_[0] = '&';
          entitySize_ = 1;
          state_ = State::kEntity;
        }
        break;
      }

      case State::kEntity: {
        const Kind kind = back_ == State::kValue ? Kind::kValue : Kind::kText;
        if (c == ';') {
          ++at;
          state_ = back_;
          const std::size_t length = Decoded();
          if (length > 0) {
            return Give(kind, {entity_.data(), length}, false, at, token);
          }
          ++unknownEntities_;
        } else if (entitySize_ < entity_.size() && (CarriesName(c) || c == '#')) {
          entity_[entitySize_] = c;
          ++entitySize_;
          ++at;
        } else {
          // Not an entity after all: the `&` and what followed it are
          // given as they were written, and `c` is read again as what it
          // is.
          state_ = back_;
          return Give(kind, {entity_.data(), entitySize_}, false, at, token);
        }
        break;
      }

      case State::kCloseName:
        if (CarriesName(c)) {
          AddToName(c);
          ++at;
        } else if (nameSize_ == 0) {
          return Fail(SvgTagStatus::kMalformed, at, size);
        } else {
          state_ = State::kAfterCloseName;
        }
        break;

      case State::kAfterCloseName:
        if (IsSpace(c)) {
          ++at;
        } else if (c == '>') {
          if (depth_ == 0) {
            return Fail(SvgTagStatus::kUnbalanced, at, size);
          }
          ++at;
          --depth_;
          state_ = State::kContent;
          return Give(Kind::kClose, Name(), false, at, token);
        } else {
          return Fail(SvgTagStatus::kMalformed, at, size);
        }
        break;

      case State::kInstruction:
        ++at;
        if (c == '>' && matched_ == 1) {
          state_ = State::kContent;
        }
        matched_ = c == '?' ? 1 : 0;
        break;

      case State::kBang:
        // A comment begins `<!--`, marked text `<![CDATA[`, and anything
        // else is a declaration, of which a file has only its document
        // type.
        if (nameSize_ == 0 && c != '-' && c != '[') {
          quote_ = 0;
          nesting_ = 0;
          state_ = State::kDeclaration;
        } else if (nameSize_ == 0) {
          AddToName(c);
          ++at;
        } else if (name_[0] == '-') {
          if (c != '-') {
            return Fail(SvgTagStatus::kMalformed, at, size);
          }
          ++at;
          matched_ = 0;
          state_ = State::kComment;
        } else {
          if (c != kCDataStart[nameSize_]) {
            return Fail(SvgTagStatus::kMalformed, at, size);
          }
          ++at;
          ++nameSize_;
          if (nameSize_ == kCDataStart.size()) {
            matched_ = 0;
            state_ = State::kCData;
          }
        }
        break;

      case State::kComment:
        // Over at `-->`: `matched_` is how many dashes have just gone by.
        ++at;
        if (c == '>' && matched_ == 2) {
          state_ = State::kContent;
        } else if (c != '-') {
          matched_ = 0;
        } else if (matched_ < 2) {
          ++matched_;
        }
        break;

      case State::kCData:
        // Over at `]]>`. A bracket is kept back until it is known whether
        // it is part of that: `matched_` is how many are.
        if (matched_ == 0) {
          const std::size_t from = at;
          while (at < size && bytes[at] != ']') {
            ++at;
          }
          if (at > from) {
            return Give(Kind::kText, {bytes.data() + from, at - from}, true, at, token);
          }
          ++at;
          matched_ = 1;
        } else if (c == ']' && matched_ == 1) {
          ++at;
          matched_ = 2;
        } else if (c == ']') {
          // A third: the first of them was text.
          ++at;
          entity_[0] = ']';
          return Give(Kind::kText, {entity_.data(), 1}, false, at, token);
        } else if (c == '>' && matched_ == 2) {
          ++at;
          matched_ = 0;
          state_ = State::kContent;
        } else {
          // The brackets were text, and `c` is read again as what it is.
          const std::size_t brackets = matched_;
          matched_ = 0;
          entity_[0] = ']';
          entity_[1] = ']';
          return Give(Kind::kText, {entity_.data(), brackets}, false, at, token);
        }
        break;

      case State::kDeclaration:
        // Over at a `>` that is not inside quotes or square brackets.
        ++at;
        if (quote_ != 0) {
          if (c == quote_) {
            quote_ = 0;
          }
        } else if (c == '"' || c == '\'') {
          quote_ = c;
        } else if (c == '[') {
          if (nesting_ < UINT8_MAX) {
            ++nesting_;
          }
        } else if (c == ']') {
          if (nesting_ > 0) {
            --nesting_;
          }
        } else if (c == '>' && nesting_ == 0) {
          state_ = State::kContent;
        }
        break;
    }
  }
  offset_ += static_cast<std::uint32_t>(size);
  return size;
}

SvgTagStatus SvgTagReader::Finish() noexcept {
  if (status_ == SvgTagStatus::kOk && (state_ != State::kContent || depth_ != 0)) {
    status_ = SvgTagStatus::kTruncated;
    errorOffset_ = offset_;
  }
  return status_;
}

}  // namespace bn::draw
