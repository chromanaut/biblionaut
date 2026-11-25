#include "core/inflate.h"

#include <algorithm>
#include <cstring>

namespace bn::core {

namespace {

constexpr unsigned kMaxCodeBits = 15;
constexpr unsigned kLiteralFastBits = 9;
constexpr unsigned kDistanceFastBits = 6;

constexpr std::size_t kLiteralSymbols = 288;  // In the fixed code; a block's own code may have up to 286.
constexpr std::size_t kMaxLiteralSymbols = 286;
constexpr std::size_t kDistanceSymbols = 30;
constexpr std::size_t kCodeLengthSymbols = 19;
constexpr int kEndOfBlock = 256;

// A length symbol (257 and up) stands for a base length plus some extra
// bits read after it; likewise a distance symbol.
constexpr std::array<std::uint16_t, 29> kLengthBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtraBits = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistanceBase = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                                         33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                                         1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistanceExtraBits = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                             6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// The order in which a block gives the lengths of its code-length code.
constexpr std::array<std::uint8_t, kCodeLengthSymbols> kCodeLengthOrder = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                                           11, 4,  12, 3, 13, 2, 14, 1, 15};

// The low `count` bits of `value`, in the opposite order.
constexpr std::uint32_t Reversed(std::uint32_t value, unsigned count) noexcept {
  std::uint32_t result = 0;
  for (unsigned i = 0; i < count; ++i) {
    result = (result << 1U) | ((value >> i) & 1U);
  }
  return result;
}

// A copy this long or longer is handed to the C library, which copies a
// word at a time. Most repeats in text are shorter, and for those the call
// costs more than the copying.
constexpr std::size_t kLongCopy = 16;

// Copies `count` bytes, which is at least one, from `from` to `to`, the
// lowest first. The two may lie over each other: with `to` the higher, the
// bytes at `from` are repeated over and over, which is what a repeat that
// reaches into its own output asks for.
void CopyForward(std::uint8_t* to, const std::uint8_t* from, std::size_t count) noexcept {
  const std::uint8_t* const end = to + count;
  do {
    *to++ = *from++;
  } while (to != end);
}

// Writes at `at` in the window the `count` bytes that lie `distance` bytes
// before it, going back round the window's end if they begin there. What
// is written stays within the window: `count` is at least one, `at + count`
// is no more than the window's size, and `distance` is from 1 to its size.
void Repeat(std::uint8_t* window, std::size_t at, std::size_t distance, std::size_t count) noexcept {
  if (distance > at) {
    // It begins before the window's start, which is to say near its end:
    // `distance - at` bytes lie there. They are above where they are going,
    // so copying the lowest first takes each before anything is written
    // over it, as memmove does.
    const std::size_t first = std::min(count, distance - at);
    const std::uint8_t* const from = window + (at + Inflater::kWindowBytes - distance);
    if (first >= kLongCopy) {
      std::memmove(window + at, from, first);
    } else {
      CopyForward(window + at, from, first);
    }
    at += first;
    count -= first;
  }
  if (count == 0) {
    return;
  }
  // The rest lie below where they are going, `distance` bytes back.
  std::uint8_t* const to = window + at;
  if (count >= kLongCopy && distance >= count) {
    std::memcpy(to, to - distance, count);
  } else if (count >= kLongCopy && distance == 1) {
    std::memset(to, *(to - 1), count);  // One byte, over and over.
  } else {
    // Short, or lying over what it is producing: the lowest byte first.
    CopyForward(to, to - distance, count);
  }
}

}  // namespace

// What the header says of its size.
static_assert(sizeof(Inflater) < 3072);

Inflater::Inflater(ByteSource& source, std::span<std::uint8_t> window) noexcept : source_(&source), window_(window) {
  literal_.symbols = literalSymbols_;
  literal_.fast = literalFast_;
  literal_.fastBits = kLiteralFastBits;
  distance_.symbols = distanceSymbols_;
  distance_.fast = distanceFast_;
  distance_.fastBits = kDistanceFastBits;
  codeLength_.symbols = codeLengthSymbols_;
  Reset(source);
}

void Inflater::Reset(ByteSource& source) noexcept {
  source_ = &source;
  status_ = window_.size() >= kWindowBytes ? Status::kRunning : Status::kNoWindow;
  stage_ = Stage::kBlockStart;
  lastBlock_ = false;
  fixedBuilt_ = false;
  sourceEnded_ = false;
  blockProduced_ = false;
  emptyRun_ = 0;
  bitBuffer_ = 0;
  bitCount_ = 0;
  inputPosition_ = 0;
  inputEnd_ = 0;
  windowPosition_ = 0;
  history_ = 0;
  bytesOut_ = 0;
  storedLeft_ = 0;
  matchLeft_ = 0;
  matchDistance_ = 0;
}

std::ptrdiff_t Inflater::Read(std::span<std::uint8_t> dst) noexcept {
  std::size_t done = 0;
  unsigned started = 0;  // Blocks begun in this call.
  while (done < dst.size() && status_ == Status::kRunning) {
    switch (stage_) {
      case Stage::kBlockStart:
        // Enough for one call: it goes back with what it has, and the next
        // carries on from here. With nothing to go back with it cannot,
        // since that would say the stream had ended; but then every block
        // so far has produced nothing, and EndBlock() puts an end to that.
        if (started == kMaxBlocksPerRead && done > 0) {
          return static_cast<std::ptrdiff_t>(done);
        }
        StartBlock();
        ++started;
        break;
      case Stage::kStored:
        done += CopyStored(dst.subspan(done));
        break;
      case Stage::kCompressed:
        done += Decompress(dst.subspan(done));
        break;
    }
  }
  if (done > 0 || status_ == Status::kRunning || status_ == Status::kDone) {
    return static_cast<std::ptrdiff_t>(done);
  }
  return kError;
}

void Inflater::Fail(Status status) noexcept {
  // The first thing to go wrong is the one worth knowing: a source that
  // fails also looks like a stream cut short.
  if (status_ == Status::kRunning) {
    status_ = status;
  }
}

// ---- The input, a bit at a time ----

bool Inflater::Refill() noexcept {
  if (sourceEnded_) {
    return false;
  }
  const std::ptrdiff_t read = source_->Read(input_);
  if (read <= 0) {
    sourceEnded_ = true;
    if (read < 0) {
      Fail(Status::kSourceFailed);
    }
    return false;
  }
  inputPosition_ = 0;
  inputEnd_ = std::min(static_cast<std::size_t>(read), input_.size());
  return true;
}

// Tops the bit buffer up to more than 24 bits, or as near as the input
// allows.
void Inflater::Fill() noexcept {
  while (bitCount_ <= 24) {
    if (inputPosition_ == inputEnd_ && !Refill()) {
      return;
    }
    bitBuffer_ |= static_cast<std::uint32_t>(input_[inputPosition_]) << bitCount_;
    ++inputPosition_;
    bitCount_ += 8;
  }
}

void Inflater::Drop(unsigned count) noexcept {
  bitBuffer_ >>= count;
  bitCount_ -= count;
}

// Takes the next `count` bits, at most 16. False, with the stream failed,
// if the input ends first.
bool Inflater::Bits(unsigned count, std::uint32_t& value) noexcept {
  if (bitCount_ < count) {
    Fill();
    if (bitCount_ < count) {
      Fail(Status::kTruncated);
      return false;
    }
  }
  value = bitBuffer_ & ((1U << count) - 1U);
  Drop(count);
  return true;
}

// ---- Prefix codes ----
//
// A code is kept as the number of patterns there are of each length and
// the symbols in the order of their patterns, and decoded from those
// without a tree. That is the method of puff.c, the small reference
// inflater that comes with zlib; the table for short patterns is added to
// it here.

// Works a code out from the length of each symbol's bit pattern, 0 for a
// symbol that has none: that is all a DEFLATE stream says about a code.
Inflater::Built Inflater::Build(Code& code, std::span<const std::uint8_t> lengths) noexcept {
  code.counts.fill(0);
  for (const std::uint8_t length : lengths) {
    ++code.counts[length];
  }
  code.counts[0] = 0;

  // Each length has only so many patterns to give out: `left` is how many
  // would still be free after this length.
  int left = 1;
  for (unsigned length = 1; length <= kMaxCodeBits; ++length) {
    left = (left * 2) - code.counts[length];
    if (left < 0) {
      return Built::kOverSubscribed;
    }
  }

  // The symbols in order of their patterns: by length, then by symbol.
  std::array<std::uint16_t, kMaxCodeBits + 1> offsets{};
  for (unsigned length = 1; length < kMaxCodeBits; ++length) {
    offsets[length + 1] = static_cast<std::uint16_t>(offsets[length] + code.counts[length]);
  }
  code.symbolCount = 0;
  for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
    if (lengths[symbol] != 0) {
      code.symbols[offsets[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
      ++code.symbolCount;
    }
  }

  // The table for short patterns. A pattern arrives lowest bit first, so
  // each entry that begins with it, whatever bits follow, gets the symbol.
  if (!code.fast.empty()) {
    std::fill(code.fast.begin(), code.fast.end(), std::uint16_t{0});
    std::uint32_t pattern = 0;
    std::size_t index = 0;
    for (unsigned length = 1; length <= kMaxCodeBits; ++length) {
      for (unsigned i = 0; i < code.counts[length]; ++i, ++index, ++pattern) {
        if (length > code.fastBits) {
          continue;
        }
        const auto entry = static_cast<std::uint16_t>((length << 9U) | code.symbols[index]);
        for (std::uint32_t at = Reversed(pattern, length); at < code.fast.size(); at += 1U << length) {
          code.fast[at] = entry;
        }
      }
      pattern <<= 1U;
    }
  }
  return left > 0 ? Built::kIncomplete : Built::kComplete;
}

// A bit at a time, keeping count of where the patterns of each length
// begin. This is how the code-length code is read, which has no table; and
// it is what Decompress() falls back on for a pattern too long for its
// code's table, or one the code does not have. The short patterns of the
// other two codes never come here: Decompress() looks those up itself.
int Inflater::Decode(const Code& code) noexcept {
  Fill();
  int pattern = 0;
  int first = 0;
  int index = 0;
  std::uint32_t bits = bitBuffer_;
  for (unsigned length = 1; length <= kMaxCodeBits; ++length) {
    if (length > bitCount_) {
      Fail(Status::kTruncated);
      return -1;
    }
    pattern += static_cast<int>(bits & 1U);  // Its lowest bit is free: it was doubled last time round.
    bits >>= 1U;
    const int count = code.counts[length];
    if (pattern - count < first) {
      Drop(length);
      return code.symbols[static_cast<std::size_t>(index) + static_cast<std::size_t>(pattern - first)];
    }
    index += count;
    if (index >= code.symbolCount) {
      break;  // No symbol has a longer pattern: these bits stand for none.
    }
    first = (first + count) * 2;
    pattern *= 2;
  }
  Fail(Status::kBadData);
  return -1;
}

// ---- Blocks ----

void Inflater::StartBlock() noexcept {
  if (lastBlock_) {
    status_ = Status::kDone;
    return;
  }
  std::uint32_t last = 0;
  std::uint32_t kind = 0;
  if (!Bits(1, last) || !Bits(2, kind)) {
    return;
  }
  lastBlock_ = last != 0;
  blockProduced_ = false;
  switch (kind) {
    case 0:
      if (StartStored()) {
        stage_ = Stage::kStored;
      }
      break;
    case 1:
      UseFixedCodes();
      stage_ = Stage::kCompressed;
      break;
    case 2:
      if (ReadCodes()) {
        stage_ = Stage::kCompressed;
      }
      break;
    default:
      Fail(Status::kBadData);
      break;
  }
}

void Inflater::EndBlock() noexcept {
  stage_ = Stage::kBlockStart;
  if (blockProduced_) {
    emptyRun_ = 0;
  } else if (++emptyRun_ > kMaxEmptyBlocksInARow) {
    // Nothing a compressor writes. Left alone, a stream of these would
    // keep one call going until the source ran out.
    Fail(Status::kBadData);
  }
}

bool Inflater::StartStored() noexcept {
  // The rest of the current byte is padding. Then the length, twice: the
  // second time with every bit turned over.
  Drop(bitCount_ % 8);
  std::uint32_t length = 0;
  std::uint32_t check = 0;
  if (!Bits(16, length) || !Bits(16, check)) {
    return false;
  }
  if ((length ^ 0xFFFFU) != check) {
    Fail(Status::kBadData);
    return false;
  }
  storedLeft_ = length;
  return true;
}

void Inflater::UseFixedCodes() noexcept {
  if (fixedBuilt_) {
    return;
  }
  std::fill_n(lengths_.begin(), 144, std::uint8_t{8});
  std::fill_n(lengths_.begin() + 144, 112, std::uint8_t{9});
  std::fill_n(lengths_.begin() + 256, 24, std::uint8_t{7});
  std::fill_n(lengths_.begin() + 280, 8, std::uint8_t{8});
  (void)Build(literal_, std::span(lengths_).first(kLiteralSymbols));
  std::fill_n(lengths_.begin(), kDistanceSymbols, std::uint8_t{5});
  (void)Build(distance_, std::span(lengths_).first(kDistanceSymbols));
  fixedBuilt_ = true;
}

// Reads the codes a block brings with it.
bool Inflater::ReadCodes() noexcept {
  fixedBuilt_ = false;
  std::uint32_t literalCount = 0;
  std::uint32_t distanceCount = 0;
  std::uint32_t lengthCount = 0;
  if (!Bits(5, literalCount) || !Bits(5, distanceCount) || !Bits(4, lengthCount)) {
    return false;
  }
  literalCount += 257;
  distanceCount += 1;
  lengthCount += 4;
  if (literalCount > kMaxLiteralSymbols || distanceCount > kDistanceSymbols) {
    Fail(Status::kBadData);
    return false;
  }

  // First a small code, whose symbols are code lengths and ways of
  // repeating them...
  std::fill_n(lengths_.begin(), kCodeLengthSymbols, std::uint8_t{0});
  for (std::size_t i = 0; i < lengthCount; ++i) {
    std::uint32_t length = 0;
    if (!Bits(3, length)) {
      return false;
    }
    lengths_[kCodeLengthOrder[i]] = static_cast<std::uint8_t>(length);
  }
  if (Build(codeLength_, std::span(lengths_).first(kCodeLengthSymbols)) != Built::kComplete) {
    Fail(Status::kBadData);
    return false;
  }

  // ...then, written in it, the lengths for the two codes the data uses.
  const std::size_t total = literalCount + distanceCount;
  std::size_t index = 0;
  while (index < total) {
    const int symbol = Decode(codeLength_);
    if (symbol < 0) {
      return false;
    }
    if (symbol < 16) {
      lengths_[index++] = static_cast<std::uint8_t>(symbol);
      continue;
    }
    std::uint8_t repeated = 0;
    std::uint32_t times = 0;
    if (symbol == 16) {
      // The length before, three to six times more.
      if (index == 0) {
        Fail(Status::kBadData);
        return false;
      }
      repeated = lengths_[index - 1];
      if (!Bits(2, times)) {
        return false;
      }
      times += 3;
    } else if (symbol == 17) {
      if (!Bits(3, times)) {
        return false;
      }
      times += 3;
    } else {
      if (!Bits(7, times)) {
        return false;
      }
      times += 11;
    }
    if (index + times > total) {
      Fail(Status::kBadData);
      return false;
    }
    std::fill_n(lengths_.begin() + static_cast<std::ptrdiff_t>(index), times, repeated);
    index += times;
  }

  // A block with no way to say where it ends could not end.
  if (lengths_[kEndOfBlock] == 0) {
    Fail(Status::kBadData);
    return false;
  }
  // A code may leave patterns unused only if it has at most one symbol,
  // with a pattern one bit long. (Data that then uses a pattern the code
  // does not have is caught when it is decoded.)
  const auto usable = [](const Code& code, Built built) noexcept {
    if (built == Built::kOverSubscribed) {
      return false;
    }
    std::uint32_t longer = 0;
    for (unsigned length = 2; length <= kMaxCodeBits; ++length) {
      longer += code.counts[length];
    }
    return built == Built::kComplete || (longer == 0 && code.counts[1] <= 1);
  };
  const std::span<const std::uint8_t> all(lengths_);
  if (!usable(literal_, Build(literal_, all.first(literalCount))) ||
      !usable(distance_, Build(distance_, all.subspan(literalCount, distanceCount)))) {
    Fail(Status::kBadData);
    return false;
  }
  return true;
}

// ---- Output ----

std::size_t Inflater::CopyStored(std::span<std::uint8_t> dst) noexcept {
  std::size_t done = 0;
  const std::size_t mask = kWindowBytes - 1;
  // Whole bytes that had already been taken into the bit buffer, in
  // reading the block's length: two, or none.
  while (storedLeft_ > 0 && done < dst.size() && bitCount_ >= 8) {
    const auto byte = static_cast<std::uint8_t>(bitBuffer_);
    Drop(8);
    window_[windowPosition_] = byte;
    windowPosition_ = (windowPosition_ + 1) & mask;
    dst[done++] = byte;
    --storedLeft_;
  }
  // The rest are in the input as they are to come out: as many at a time
  // as lie together there, and fit in the window before its end.
  while (storedLeft_ > 0 && done < dst.size()) {
    if (inputPosition_ == inputEnd_ && !Refill()) {
      Fail(Status::kTruncated);
      break;
    }
    const std::size_t wanted = std::min<std::size_t>(storedLeft_, dst.size() - done);
    const std::size_t count = std::min(std::min(wanted, inputEnd_ - inputPosition_), kWindowBytes - windowPosition_);
    const std::uint8_t* const from = input_.data() + inputPosition_;
    std::memcpy(window_.data() + windowPosition_, from, count);
    std::memcpy(dst.data() + done, from, count);
    inputPosition_ += count;
    windowPosition_ = (windowPosition_ + count) & mask;
    done += count;
    storedLeft_ -= static_cast<std::uint32_t>(count);
  }
  history_ = static_cast<std::uint32_t>(std::min<std::size_t>(history_ + done, kWindowBytes));
  bytesOut_ += static_cast<std::uint32_t>(done);
  blockProduced_ = blockProduced_ || done > 0;
  if (storedLeft_ == 0 && status_ == Status::kRunning) {
    EndBlock();
  }
  return done;
}

// This is where the time goes: once round the loop for every literal and
// every repeat of a compressed block.
//
// What it produces is written into the window, where it has to be in any
// case for later repeats to find, and copied from there to `dst` in one
// piece at the end. So that the piece is one, a call stops at the window's
// end, and Read() calls again for the rest.
//
// The state of the input and of a repeat is kept in local variables while
// the loop runs, and put back in the object at the end and around anything
// that reads it there. Left in the object it would be loaded and stored
// again for every symbol: every byte of output is written through a
// pointer to bytes, and the compiler has to take such a write for one that
// might change any member. For the same reason the loop is written out in
// one piece, with what Fill(), Decode() and Bits() do repeated in it:
// handed to a function, the variables would have to be in memory for it to
// reach them.
std::size_t Inflater::Decompress(std::span<std::uint8_t> dst) noexcept {
  constexpr std::uint32_t kLiteralFastMask = (1U << kLiteralFastBits) - 1U;
  constexpr std::uint32_t kDistanceFastMask = (1U << kDistanceFastBits) - 1U;
  std::uint8_t* const window = window_.data();
  const std::size_t start = windowPosition_;
  const std::size_t limit = start + std::min(dst.size(), kWindowBytes - start);
  const std::size_t history = history_;
  const std::uint16_t* const literalFast = literalFast_.data();
  const std::uint16_t* const distanceFast = distanceFast_.data();
  const std::uint8_t* const input = input_.data();
  std::size_t at = start;
  std::uint32_t bits = bitBuffer_;
  unsigned count = bitCount_;
  const std::uint8_t* in = input + inputPosition_;
  const std::uint8_t* inEnd = input + inputEnd_;
  std::uint32_t matchLeft = matchLeft_;
  std::uint32_t matchDistance = matchDistance_;
  bool ended = false;

  while (at < limit) {
    // A repeat of earlier output, perhaps left unfinished by the last call.
    if (matchLeft > 0) {
      const std::size_t now = std::min<std::size_t>(matchLeft, limit - at);
      Repeat(window, at, matchDistance, now);
      at += now;
      matchLeft -= static_cast<std::uint32_t>(now);
      continue;
    }

    // A literal, the end of the block, or the length of a repeat. The
    // buffer is topped up first, as Fill() does it.
    while (count <= 24) {
      if (in == inEnd) {
        if (!Refill()) {
          break;
        }
        in = input;
        inEnd = input + inputEnd_;
      }
      bits |= static_cast<std::uint32_t>(*in) << count;
      ++in;
      count += 8;
    }
    std::uint32_t symbol = literalFast[bits & kLiteralFastMask];
    if (symbol != 0) {
      const unsigned length = symbol >> 9U;
      if (length > count) {
        Fail(Status::kTruncated);
        break;
      }
      bits >>= length;
      count -= length;
      symbol &= 0x1FFU;
    } else {
      // A pattern too long for the table, or one the code does not have:
      // Decode() goes through it a bit at a time.
      bitBuffer_ = bits;
      bitCount_ = count;
      inputPosition_ = static_cast<std::size_t>(in - input);
      const int found = Decode(literal_);
      bits = bitBuffer_;
      count = bitCount_;
      in = input + inputPosition_;
      inEnd = input + inputEnd_;
      if (found < 0) {
        break;
      }
      symbol = static_cast<std::uint32_t>(found);
    }
    if (symbol < kEndOfBlock) {
      window[at] = static_cast<std::uint8_t>(symbol);
      ++at;
      continue;
    }
    if (symbol == kEndOfBlock) {
      ended = true;
      break;
    }

    // A length, then how far back to copy it from.
    const std::size_t lengthIndex = symbol - (kEndOfBlock + 1);
    if (lengthIndex >= kLengthBase.size()) {
      Fail(Status::kBadData);
      break;
    }
    // The length's extra bits, five at the most. The buffer was filled
    // for the symbol, which took at most 15 bits of it: if it is short of
    // these, the input has ended, and filling it again would bring nothing.
    unsigned wanted = kLengthExtraBits[lengthIndex];
    if (count < wanted) {
      Fail(Status::kTruncated);
      break;
    }
    const std::uint32_t length = kLengthBase[lengthIndex] + (bits & ((1U << wanted) - 1U));
    bits >>= wanted;
    count -= wanted;

    // The distance's symbol, found as the first was.
    while (count <= 24) {
      if (in == inEnd) {
        if (!Refill()) {
          break;
        }
        in = input;
        inEnd = input + inputEnd_;
      }
      bits |= static_cast<std::uint32_t>(*in) << count;
      ++in;
      count += 8;
    }
    symbol = distanceFast[bits & kDistanceFastMask];
    if (symbol != 0) {
      const unsigned patternBits = symbol >> 9U;
      if (patternBits > count) {
        Fail(Status::kTruncated);
        break;
      }
      bits >>= patternBits;
      count -= patternBits;
      symbol &= 0x1FFU;
    } else {
      bitBuffer_ = bits;
      bitCount_ = count;
      inputPosition_ = static_cast<std::size_t>(in - input);
      const int found = Decode(distance_);
      bits = bitBuffer_;
      count = bitCount_;
      in = input + inputPosition_;
      inEnd = input + inputEnd_;
      if (found < 0) {
        break;
      }
      symbol = static_cast<std::uint32_t>(found);
    }
    if (symbol >= kDistanceBase.size()) {
      Fail(Status::kBadData);
      break;
    }
    // Its extra bits, up to 13: more than the buffer may have left, so it
    // is filled again if it is short, as Bits() does it.
    wanted = kDistanceExtraBits[symbol];
    if (count < wanted) {
      while (count <= 24) {
        if (in == inEnd) {
          if (!Refill()) {
            break;
          }
          in = input;
          inEnd = input + inputEnd_;
        }
        bits |= static_cast<std::uint32_t>(*in) << count;
        ++in;
        count += 8;
      }
      if (count < wanted) {
        Fail(Status::kTruncated);
        break;
      }
    }
    const std::uint32_t distance = kDistanceBase[symbol] + (bits & ((1U << wanted) - 1U));
    bits >>= wanted;
    count -= wanted;
    // Further back than anything yet produced.
    if (distance > std::min(history + (at - start), kWindowBytes)) {
      Fail(Status::kBadData);
      break;
    }
    matchLeft = length;
    matchDistance = distance;
  }

  bitBuffer_ = bits;
  bitCount_ = count;
  inputPosition_ = static_cast<std::size_t>(in - input);
  matchLeft_ = matchLeft;
  matchDistance_ = matchDistance;
  const std::size_t done = at - start;
  if (done > 0) {
    std::memcpy(dst.data(), window + start, done);
  }
  windowPosition_ = at & (kWindowBytes - 1);
  history_ = static_cast<std::uint32_t>(std::min(history + done, kWindowBytes));
  bytesOut_ += static_cast<std::uint32_t>(done);
  blockProduced_ = blockProduced_ || done > 0;
  if (ended) {
    EndBlock();
  }
  return done;
}

}  // namespace bn::core
