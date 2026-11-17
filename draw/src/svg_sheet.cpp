#include "draw/svg_sheet.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bn::draw {

namespace {

bool IsCssSpace(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::string_view Trimmed(std::string_view text) noexcept {
  std::size_t from = 0;
  std::size_t to = text.size();
  while (from < to && IsCssSpace(text[from])) {
    ++from;
  }
  while (to > from && IsCssSpace(text[to - 1])) {
    --to;
  }
  return {text.data() + from, to - from};
}

// The text without the `<!--` and `-->` that a sheet may be written
// inside, wherever they stand at its front.
std::string_view WithoutMarks(std::string_view text) noexcept {
  constexpr std::string_view kOpen = "<!--";
  constexpr std::string_view kClose = "-->";
  text = Trimmed(text);
  for (;;) {
    if (text.starts_with(kOpen)) {
      text = Trimmed({text.data() + kOpen.size(), text.size() - kOpen.size()});
    } else if (text.starts_with(kClose)) {
      text = Trimmed({text.data() + kClose.size(), text.size() - kClose.size()});
    } else {
      return text;
    }
  }
}

bool BeginsName(char c) noexcept {
  const auto byte = static_cast<unsigned char>(c);
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || byte >= 0x80U;
}

bool InName(char c) noexcept { return BeginsName(c) || (c >= '0' && c <= '9'); }

// One selector, if it is one this reads: a class or an element's name.
bool ReadSelector(std::string_view text, SvgSelector& selector) noexcept {
  text = Trimmed(text);
  SvgSelector read;
  if (!text.empty() && text.front() == '.') {
    read.kind = SvgSelector::Kind::kClass;
    text = {text.data() + 1, text.size() - 1};
  } else {
    read.kind = SvgSelector::Kind::kElement;
  }
  if (text.empty() || !BeginsName(text.front()) || !std::all_of(text.begin(), text.end(), InName)) {
    return false;
  }
  read.name = text;
  selector = read;
  return true;
}

}  // namespace

std::size_t SvgSheetReader::Next(std::string_view text, SvgSheetToken& token) noexcept {
  token = SvgSheetToken();
  if (ruleEnded_) {
    ruleEnded_ = false;
    token.kind = SvgSheetToken::Kind::kRuleEnd;
    return 0;
  }
  if (state_ == State::kTelling && NextSelector(token)) {
    return 0;
  }
  std::size_t at = 0;
  while (at < text.size()) {
    const char c = text[at];
    ++at;
    if (Take(c, token)) {
      return at;
    }
  }
  return at;
}

bool SvgSheetReader::Take(char c, SvgSheetToken& token) noexcept {
  // A comment is nothing at all, anywhere but inside a string.
  if (inComment_) {
    if (afterStar_ && c == '/') {
      inComment_ = false;
      afterStar_ = false;
    } else {
      afterStar_ = c == '*';
    }
    return false;
  }
  if (quote_ == 0) {
    if (afterSlash_) {
      afterSlash_ = false;
      if (c == '*') {
        inComment_ = true;
        return false;
      }
      // The `/` held back was itself. On its own it ends nothing, so
      // there is nothing to tell from it.
      static_cast<void>(Byte('/', token));
    }
    if (c == '/') {
      afterSlash_ = true;
      return false;
    }
  }
  return Byte(c, token);
}

bool SvgSheetReader::InString(char c) noexcept {
  if (quote_ != 0) {
    if (escaped_) {
      escaped_ = false;
    } else if (c == '\\') {
      escaped_ = true;
    } else if (c == quote_) {
      quote_ = 0;
    }
    return true;
  }
  if (c == '"' || c == '\'') {
    quote_ = c;
    return true;
  }
  return false;
}

bool SvgSheetReader::Byte(char c, SvgSheetToken& token) noexcept {
  switch (state_) {
    case State::kSelectors:
      if (InString(c)) {
        // Part of selectors that are not ones this reads; kept, to say so.
      } else if (c == '{') {
        const std::string_view written = WithoutMarks({selectors_.data(), selectorsSize_});
        if (!written.empty() && written.front() == '@') {
          dropped_ |= static_cast<std::uint8_t>(SvgSheetDropped::kAtRule);
          Skip(1);
        } else if (ReadSelectors()) {
          state_ = State::kTelling;
          return NextSelector(token);
        } else {
          Skip(1);
        }
        return false;
      } else if (c == ';') {
        // An `@` rule with no block, such as `@import`, ends here. In front
        // of anything else a semicolon is part of selectors that are not
        // ones this reads.
        const std::string_view written = WithoutMarks({selectors_.data(), selectorsSize_});
        if (!written.empty() && written.front() == '@') {
          dropped_ |= static_cast<std::uint8_t>(SvgSheetDropped::kAtRule);
          selectorsSize_ = 0;
          selectorsTooLong_ = false;
          return false;
        }
      } else if (c == '}') {
        // A `}` with no rule open: what came before it is no rule.
        selectorsSize_ = 0;
        selectorsTooLong_ = false;
        return false;
      }
      if (selectorsSize_ < selectors_.size()) {
        selectors_[selectorsSize_] = c;
        ++selectorsSize_;
      } else {
        selectorsTooLong_ = true;
      }
      return false;

    case State::kTelling:
      // Bytes are only taken once the selectors have all been told.
      return false;

    case State::kProperty:
      if (c == ':') {
        state_ = State::kValue;
        valueSize_ = 0;
        brackets_ = 0;
      } else if (c == ';') {
        // A name with no value: no declaration.
        propertySize_ = 0;
        declarationTooLong_ = false;
      } else if (c == '}') {
        state_ = State::kSelectors;
        selectorsSize_ = 0;
        selectorsTooLong_ = false;
        token.kind = SvgSheetToken::Kind::kRuleEnd;
        return true;
      } else if (c == '{') {
        // A block where a declaration should be: passed over, and the rule
        // goes on after it.
        Skip(1);
        resume_ = State::kProperty;
      } else if (!IsCssSpace(c)) {
        if (propertySize_ < property_.size()) {
          property_[propertySize_] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
          ++propertySize_;
        } else {
          declarationTooLong_ = true;
        }
      }
      return false;

    case State::kValue:
      if (InString(c)) {
        // Kept as written.
      } else if (c == '(') {
        brackets_ = static_cast<std::uint8_t>(std::min(brackets_ + 1, 255));
      } else if (c == ')') {
        brackets_ = static_cast<std::uint8_t>(brackets_ > 0 ? brackets_ - 1 : 0);
      } else if (c == ';' && brackets_ == 0) {
        state_ = State::kProperty;
        return EndDeclaration(token);
      } else if (c == '}') {
        // The rule ends, and its last declaration with it.
        state_ = State::kSelectors;
        selectorsSize_ = 0;
        selectorsTooLong_ = false;
        if (EndDeclaration(token)) {
          ruleEnded_ = true;
        } else {
          token.kind = SvgSheetToken::Kind::kRuleEnd;
        }
        return true;
      } else if (c == '{') {
        // A block inside a value makes it no declaration this reads.
        propertySize_ = 0;
        declarationTooLong_ = false;
        Skip(1);
        resume_ = State::kProperty;
        return false;
      }
      if (valueSize_ < value_.size()) {
        value_[valueSize_] = c;
        ++valueSize_;
      } else {
        declarationTooLong_ = true;
      }
      return false;

    case State::kSkipping:
      if (InString(c)) {
        // A brace in a string is no block.
      } else if (c == '{') {
        depth_ = std::min<std::uint32_t>(depth_ + 1, UINT32_MAX - 1);
      } else if (c == '}') {
        --depth_;
        if (depth_ == 0) {
          state_ = resume_;
          selectorsSize_ = 0;
          selectorsTooLong_ = false;
          propertySize_ = 0;
          declarationTooLong_ = false;
        }
      }
      return false;
  }
  return false;
}

bool SvgSheetReader::ReadSelectors() noexcept {
  const bool tooLong = selectorsTooLong_;
  const std::string_view written = WithoutMarks({selectors_.data(), selectorsSize_});
  selectorsTooLong_ = false;
  if (tooLong) {
    dropped_ |= static_cast<std::uint8_t>(SvgSheetDropped::kTooLong);
    return false;
  }
  if (written.empty()) {
    dropped_ |= static_cast<std::uint8_t>(SvgSheetDropped::kSelector);
    return false;
  }
  for (std::string_view rest = written;;) {
    const std::size_t comma = rest.find(',');
    SvgSelector selector;
    const auto upTo = static_cast<std::ptrdiff_t>(std::min(comma, rest.size()));
    if (!ReadSelector(std::string_view(rest.begin(), rest.begin() + upTo), selector)) {
      dropped_ |= static_cast<std::uint8_t>(SvgSheetDropped::kSelector);
      return false;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(comma + 1);
  }
  // They are told from where the marks before them end.
  telling_ = static_cast<std::size_t>(written.data() - selectors_.data());
  tellingEnd_ = telling_ + written.size();
  return true;
}

bool SvgSheetReader::NextSelector(SvgSheetToken& token) noexcept {
  if (telling_ >= tellingEnd_) {
    state_ = State::kProperty;
    propertySize_ = 0;
    declarationTooLong_ = false;
    return false;
  }
  const std::string_view rest(selectors_.data() + telling_, tellingEnd_ - telling_);
  const std::size_t comma = rest.find(',');
  const std::size_t length = comma == std::string_view::npos ? rest.size() : comma;
  telling_ += comma == std::string_view::npos ? rest.size() : comma + 1;
  // Each was read once already, when the `{` came.
  static_cast<void>(
      ReadSelector(std::string_view(rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(length)), token.selector));
  token.kind = SvgSheetToken::Kind::kSelector;
  return true;
}

bool SvgSheetReader::EndDeclaration(SvgSheetToken& token) noexcept {
  const bool tooLong = declarationTooLong_;
  const std::string_view property(property_.data(), propertySize_);
  const std::string_view value = Trimmed({value_.data(), valueSize_});
  propertySize_ = 0;
  declarationTooLong_ = false;
  quote_ = 0;
  escaped_ = false;
  brackets_ = 0;
  if (tooLong) {
    dropped_ |= static_cast<std::uint8_t>(SvgSheetDropped::kTooLong);
    return false;
  }
  if (property.empty()) {
    return false;
  }
  token.kind = SvgSheetToken::Kind::kDeclaration;
  token.property = property;
  token.value = value;
  return true;
}

void SvgSheetReader::Skip(std::uint32_t depth) noexcept {
  state_ = State::kSkipping;
  resume_ = State::kSelectors;
  depth_ = depth;
  quote_ = 0;
  escaped_ = false;
}

void SvgSheetReader::Finish() noexcept {
  const bool inRule = state_ != State::kSelectors || ruleEnded_;
  const bool leftOver = !WithoutMarks({selectors_.data(), selectorsSize_}).empty() || selectorsTooLong_;
  if (inRule || inComment_ || quote_ != 0 || leftOver) {
    dropped_ |= static_cast<std::uint8_t>(SvgSheetDropped::kUnended);
  }
  const std::uint8_t dropped = dropped_;
  *this = SvgSheetReader();
  dropped_ = dropped;
}

}  // namespace bn::draw
