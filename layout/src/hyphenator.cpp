#include "layout/hyphenator.h"

#include <algorithm>

#include "core/utf8.h"

namespace bn::layout {

namespace {

// The table's pieces, in bytes.
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kListedBytes = 4;
constexpr std::size_t kEdgeBytes = 5;

constexpr std::uint8_t kVersion = 1;
constexpr std::uint32_t kNoNext = 0xFFFF;    // In an edge's `next`: no pattern goes on.
constexpr std::uint32_t kLastEdge = 0x8000;  // In an edge's `value`: the last edge of its state.
constexpr std::uint32_t kValueMask = 0x7FFF;

// Which letter a character is, besides the letters a table names:
constexpr std::uint8_t kWordEnd = 0;      // the mark at each end of a word;
constexpr std::uint8_t kNoLetter = 0xFF;  // not a letter in this language.

// Numbers in the table, low byte first. Whoever calls has seen that the
// bytes are there.
std::uint32_t Read16(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  return std::uint32_t{bytes[at]} | (std::uint32_t{bytes[at + 1]} << 8U);
}
std::uint32_t Read24(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  return Read16(bytes, at) | (std::uint32_t{bytes[at + 2]} << 16U);
}

}  // namespace

const char* HyphenStatusName(HyphenStatus status) noexcept {
  switch (status) {
    case HyphenStatus::kOk:
      return "ok";
    case HyphenStatus::kNotOpen:
      return "not open";
    case HyphenStatus::kBadTable:
      return "bad table";
  }
  return "unknown";
}

HyphenStatus Hyphenator::Open(std::span<const std::uint8_t> table) noexcept {
  status_ = HyphenStatus::kBadTable;
  listed_ = {};
  edges_ = {};
  values_ = {};
  work_ = 0;
  if (table.size() < kHeaderBytes) {
    return status_;
  }
  if (table[0] != 'B' || table[1] != 'N' || table[2] != 'H' || table[3] != 'Y' || table[4] != kVersion ||
      table[15] != 0) {
    return status_;
  }
  const std::uint8_t left = table[5];
  const std::uint8_t right = table[6];
  const std::uint8_t deepest = table[7];
  const std::size_t listed = Read16(table, 8);
  const std::size_t edges = Read16(table, 10);
  const std::size_t values = Read16(table, 12);
  const std::uint8_t widest = table[14];
  if (left == 0 || right == 0 || deepest == 0 || deepest > kMaxDeepest || widest == 0 || listed > kMaxListed) {
    return status_;
  }
  // The table is its pieces and nothing more.
  const std::size_t listedBytes = listed * kListedBytes;
  const std::size_t edgeBytes = edges * kEdgeBytes;
  if (table.size() != kHeaderBytes + listedBytes + edgeBytes + values) {
    return status_;
  }
  const std::span<const std::uint8_t> list = table.subspan(kHeaderBytes, listedBytes);
  // The letter list has to be in order, or a character could not be found
  // in it by halving; and it names only letters.
  std::uint32_t before = 0;
  for (std::size_t i = 0; i < listed; ++i) {
    const std::uint32_t codePoint = Read24(list, i * kListedBytes);
    const std::uint8_t letter = list[(i * kListedBytes) + 3];
    if (codePoint > core::kLargestCodePoint || (i != 0 && codePoint <= before) || letter == kWordEnd ||
        letter == kNoLetter) {
      return status_;
    }
    before = codePoint;
  }

  listed_ = list;
  edges_ = table.subspan(kHeaderBytes + listedBytes, edgeBytes);
  values_ = table.subspan(kHeaderBytes + listedBytes + edgeBytes, values);
  leftMin_ = left;
  rightMin_ = right;
  deepest_ = deepest;
  widest_ = widest;
  status_ = HyphenStatus::kOk;
  return status_;
}

std::uint32_t Hyphenator::MostWorkFor(std::size_t letters) const noexcept {
  if (status_ != HyphenStatus::kOk) {
    return 0;
  }
  // A stretch of letters is gone through from each of its letters and from
  // the mark at each end of it, and a word of so many characters has no
  // more stretches than would make that twice the characters and one.
  const auto starts = static_cast<std::uint32_t>((2 * std::min(letters, kMaxLetters)) + 1);
  // From each, as many steps as the longest pattern has letters; and at
  // each step, the edges of one state, and the numbers of one pattern.
  return starts * deepest_ * (std::uint32_t{widest_} + deepest_ + 1U);
}

std::uint8_t Hyphenator::LetterOf(std::uint32_t codePoint) const noexcept {
  std::size_t low = 0;
  std::size_t high = listed_.size() / kListedBytes;
  while (low < high) {
    const std::size_t middle = low + ((high - low) / 2);
    const std::uint32_t listed = Read24(listed_, middle * kListedBytes);
    if (listed == codePoint) {
      return listed_[(middle * kListedBytes) + 3];
    }
    if (listed < codePoint) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return kNoLetter;
}

std::size_t Hyphenator::Find(std::string_view word, std::span<std::uint8_t> places) noexcept {
  work_ = 0;
  if (status_ != HyphenStatus::kOk) {
    return 0;
  }
  // The word's characters, each as the letter it is.
  std::size_t letters = 0;
  for (std::size_t at = 0; at < word.size();) {
    if (letters == kMaxLetters) {
      return 0;  // Too long a word to look at.
    }
    const core::Character one = core::CharacterAt(word, at);
    symbols_[letters] = one.good ? LetterOf(one.codePoint) : kNoLetter;
    offsets_[letters] = static_cast<std::uint8_t>(at);
    ++letters;
    at += one.bytes;
  }

  // Each stretch of letters is a word of its own.
  std::size_t count = 0;
  for (std::size_t first = 0; first < letters;) {
    if (symbols_[first] == kNoLetter) {
      ++first;
      continue;
    }
    std::size_t end = first;
    while (end < letters && symbols_[end] != kNoLetter) {
      ++end;
    }
    count = FindIn(first, end, places, count);
    first = end;
  }
  return count;
}

std::size_t Hyphenator::FindIn(std::size_t first, std::size_t end, std::span<std::uint8_t> places,
                               std::size_t count) noexcept {
  const std::size_t length = end - first;
  if (length < std::size_t{leftMin_} + rightMin_) {
    return count;
  }
  // The stretch with a mark at each end is `length + 2` symbols. A level
  // belongs to the place before each of them, and one to the place after
  // the last.
  const std::size_t symbols = length + 2;
  const auto symbolAt = [&](std::size_t n) noexcept {
    return n == 0 || n == length + 1 ? kWordEnd : symbols_[first + n - 1];
  };
  std::array<std::uint8_t, kMaxLetters + 3> levels{};

  const std::size_t edgeCount = edges_.size() / kEdgeBytes;
  for (std::size_t from = 0; from < symbols; ++from) {
    // Every pattern that begins here: down the tree for as long as the
    // word's letters are there to follow.
    std::size_t state = 0;
    const std::size_t reach = std::min<std::size_t>(symbols - from, deepest_);
    for (std::size_t step = 0; step < reach; ++step) {
      const std::uint8_t wanted = symbolAt(from + step);
      bool found = false;
      std::uint32_t next = kNoNext;
      std::uint32_t value = 0;
      for (std::size_t n = 0; n < widest_ && state + n < edgeCount; ++n) {
        ++work_;
        const std::size_t at = (state + n) * kEdgeBytes;
        const std::uint32_t valueAndLast = Read16(edges_, at + 3);
        if (edges_[at] == wanted) {
          found = true;
          next = Read16(edges_, at + 1);
          value = valueAndLast & kValueMask;
          break;
        }
        if ((valueAndLast & kLastEdge) != 0) {
          break;
        }
      }
      if (!found) {
        break;
      }
      if (value != 0) {
        Apply(static_cast<std::uint16_t>(value), from, step + 1, levels);
      }
      if (next == kNoNext) {
        break;
      }
      state = next;
    }
  }

  // A break may come where the level is odd, with letters enough on each
  // side of it.
  for (std::size_t before = leftMin_; before + rightMin_ <= length; ++before) {
    if ((levels[before + 1] & 1U) != 0 && count < places.size()) {
      places[count] = offsets_[first + before];
      ++count;
    }
  }
  return count;
}

void Hyphenator::Apply(std::uint16_t value, std::size_t from, std::size_t length,
                       std::span<std::uint8_t> levels) noexcept {
  // A table that points outside its values, or gives a pattern more numbers
  // than it has places, is not followed there.
  const std::size_t at = value - 1U;
  if (at >= values_.size()) {
    return;
  }
  const std::size_t count = values_[at];
  if (count > std::size_t{deepest_} + 1 || values_.size() - at - 1 < count * 2) {
    return;
  }
  work_ += static_cast<std::uint32_t>(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t back = values_[at + 1 + (2 * i)];
    const std::uint8_t number = values_[at + 2 + (2 * i)];
    if (back <= length) {
      std::uint8_t& level = levels[from + length - back];
      level = std::max(level, number);
    }
  }
}

}  // namespace bn::layout
