#include "core/inflate.h"

#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "core/byte_source.h"

namespace {

using bn::core::ByteSource;
using bn::core::Inflater;
using Status = bn::core::Inflater::Status;
using Bytes = std::vector<std::uint8_t>;

Bytes BytesOf(std::string_view text) { return {text.begin(), text.end()}; }

// A source that hands its bytes out a few at a time, and can fail part of
// the way through.
class PiecewiseSource final : public ByteSource {
 public:
  PiecewiseSource(Bytes bytes, std::size_t piece) : bytes_(std::move(bytes)), piece_(piece) {}

  [[nodiscard]] std::ptrdiff_t Read(std::span<std::uint8_t> dst) noexcept override {
    ++calls;
    if (position_ >= failAt) {
      return kError;
    }
    const std::size_t count = std::min({dst.size(), piece_, bytes_.size() - position_, failAt - position_});
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_), count, dst.begin());
    position_ += count;
    return static_cast<std::ptrdiff_t>(count);
  }

  // How many bytes have been taken from it so far.
  std::size_t Taken() const noexcept { return position_; }

  std::size_t failAt = SIZE_MAX;  // Fails once this many bytes have been read.
  int calls = 0;

 private:
  Bytes bytes_;
  std::size_t piece_;
  std::size_t position_ = 0;
};

struct Result {
  Bytes out;
  Status status = Status::kRunning;
  std::size_t taken = 0;             // Bytes taken from the source in all,
  std::size_t mostTakenByACall = 0;  // and the most in any one call.
};

// Inflates `compressed`, the source giving `sourcePiece` bytes at a time
// and the output being asked for `outputPiece` at a time. Stops at `limit`
// bytes of output, so that damaged input cannot make a test run for ever.
//
// Every call is held to what the inflater says one call may take from its
// source, whatever the stream is: right, damaged, or never a stream.
Result Inflate(const Bytes& compressed, std::size_t sourcePiece = SIZE_MAX, std::size_t outputPiece = 4096,
               std::size_t limit = 8U << 20U) {
  PiecewiseSource source(compressed, sourcePiece);
  // One window for every call, left as the last call filled it. Making a
  // new one each time costs more than most of these streams take to
  // inflate, and a window that is not blank is the harder test: nothing
  // already in it may ever come out.
  static Bytes window(Inflater::kWindowBytes);
  Inflater inflater(source, window);
  Result result;
  // Read straight into place, with room made ahead in steps that double:
  // far cheaper than appending piece by piece when a piece is one byte.
  std::size_t used = 0;
  while (used < limit) {
    if (result.out.size() - used < outputPiece) {
      result.out.resize(std::max({result.out.size() * 2, used + outputPiece, std::size_t{4096}}));
    }
    const std::size_t takenBefore = source.Taken();
    const std::ptrdiff_t read = inflater.Read(std::span<std::uint8_t>(result.out.data() + used, outputPiece));
    const std::size_t taken = source.Taken() - takenBefore;
    EXPECT_LE(taken, Inflater::MostInputFor(outputPiece)) << "in one call, after " << used << " bytes of output";
    result.mostTakenByACall = std::max(result.mostTakenByACall, taken);
    if (read <= 0) {
      break;
    }
    used += static_cast<std::size_t>(read);
  }
  result.out.resize(used);
  result.status = inflater.CurrentStatus();
  result.taken = source.Taken();
  EXPECT_EQ(inflater.BytesOut(), result.out.size());
  return result;
}

// Writes a DEFLATE stream a field at a time, so that the tests can say
// exactly what is in one, right or wrong.
class StreamWriter {
 public:
  // The low `count` bits of `value`, lowest first: how the format packs
  // every field but a prefix code.
  StreamWriter& Bits(std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
      Bit((value >> i) & 1U);
    }
    return *this;
  }
  // A prefix code's bit pattern, which goes highest bit first.
  StreamWriter& Pattern(std::uint32_t pattern, unsigned count) {
    for (unsigned i = count; i > 0; --i) {
      Bit((pattern >> (i - 1)) & 1U);
    }
    return *this;
  }
  StreamWriter& BlockHeader(bool last, unsigned kind) { return Bits(last ? 1 : 0, 1).Bits(kind, 2); }
  // A literal or length symbol in the fixed code.
  StreamWriter& Fixed(unsigned symbol) {
    if (symbol < 144) {
      return Pattern(0x30 + symbol, 8);
    }
    if (symbol < 256) {
      return Pattern(0x190 + (symbol - 144), 9);
    }
    if (symbol < 280) {
      return Pattern(symbol - 256, 7);
    }
    return Pattern(0xC0 + (symbol - 280), 8);
  }
  StreamWriter& FixedText(std::string_view text) {
    for (const char ch : text) {
      Fixed(static_cast<unsigned char>(ch));
    }
    return *this;
  }
  // A distance symbol in the fixed code: five bits.
  StreamWriter& FixedDistance(unsigned symbol) { return Pattern(symbol, 5); }
  StreamWriter& ToByteBoundary() {
    while (used_ % 8 != 0) {
      Bit(0);
    }
    return *this;
  }
  StreamWriter& Byte(std::uint8_t byte) { return Bits(byte, 8); }
  StreamWriter& Raw(std::string_view text) {
    for (const char ch : text) {
      Byte(static_cast<std::uint8_t>(ch));
    }
    return *this;
  }
  // A stored block's header, after the three bits that begin every block.
  StreamWriter& StoredLength(std::uint16_t length) {
    return ToByteBoundary().Bits(length, 16).Bits(static_cast<std::uint16_t>(~length), 16);
  }

  Bytes Done() {
    ToByteBoundary();
    return bytes_;
  }

 private:
  void Bit(std::uint32_t bit) {
    if (used_ % 8 == 0) {
      bytes_.push_back(0);
    }
    bytes_.back() = static_cast<std::uint8_t>(bytes_.back() | (bit << (used_ % 8)));
    ++used_;
  }

  Bytes bytes_;
  std::size_t used_ = 0;
};

// The start of a block that brings its own codes: `literals` literal and
// length codes, `distances` distance codes, and the lengths of the
// code-length code for the symbols in the order the format gives them.
StreamWriter DynamicHeader(unsigned literals, unsigned distances, std::initializer_list<unsigned> codeLengthLengths) {
  StreamWriter writer;
  writer.BlockHeader(true, 2).Bits(literals - 257, 5).Bits(distances - 1, 5);
  writer.Bits(static_cast<std::uint32_t>(codeLengthLengths.size()) - 4, 4);
  for (const unsigned length : codeLengthLengths) {
    writer.Bits(length, 3);
  }
  return writer;
}

// ---- Streams that are right ----

TEST(Inflater, ReadsAStoredBlock) {
  const Bytes stream = StreamWriter().BlockHeader(true, 0).StoredLength(5).Raw("hello").Done();
  const Result result = Inflate(stream);
  EXPECT_EQ(result.out, BytesOf("hello"));
  EXPECT_EQ(result.status, Status::kDone);
}

TEST(Inflater, ReadsAnEmptyStream) {
  // The shortest there is: a fixed-code block holding only its own end.
  const Result fixed = Inflate(StreamWriter().BlockHeader(true, 1).Fixed(256).Done());
  EXPECT_TRUE(fixed.out.empty());
  EXPECT_EQ(fixed.status, Status::kDone);

  const Result stored = Inflate(StreamWriter().BlockHeader(true, 0).StoredLength(0).Done());
  EXPECT_TRUE(stored.out.empty());
  EXPECT_EQ(stored.status, Status::kDone);
}

TEST(Inflater, ReadsLiteralsInTheFixedCode) {
  // One from each of the fixed code's four ranges of symbol.
  const Bytes expected = {'a', 0x00, 0x8F, 0x90, 0xFF, 'z'};
  StreamWriter writer;
  writer.BlockHeader(true, 1);
  for (const std::uint8_t byte : expected) {
    writer.Fixed(byte);
  }
  const Result result = Inflate(writer.Fixed(256).Done());
  EXPECT_EQ(result.out, expected);
  EXPECT_EQ(result.status, Status::kDone);
}

TEST(Inflater, RepeatsWhatCameBefore) {
  // "abc", then: length 3 from 3 back; length 4 from 1 back, which reaches
  // into itself; and the longest there is, 258, from 2 back.
  StreamWriter writer;
  writer.BlockHeader(true, 1).FixedText("abc");
  writer.Fixed(257).FixedDistance(2);  // Length 3, distance 3.
  writer.Fixed(258).FixedDistance(0);  // Length 4, distance 1.
  writer.Fixed(285).FixedDistance(1);  // Length 258, distance 2.
  const Result result = Inflate(writer.Fixed(256).Done());
  std::string expected = "abcabccccc";
  for (int i = 0; i < 258; ++i) {
    expected.push_back('c');
  }
  EXPECT_EQ(result.out, BytesOf(expected));
  EXPECT_EQ(result.status, Status::kDone);
}

TEST(Inflater, AddsTheExtraBitsOfLengthsAndDistances) {
  // Twenty bytes, then length 11 + 1 (symbol 265 with its one extra bit
  // set) from distance 13 + 3 (symbol 7 with its two extra bits set).
  const std::string start = "0123456789abcdefghij";
  StreamWriter writer;
  writer.BlockHeader(true, 1).FixedText(start);
  writer.Fixed(265).Bits(1, 1).FixedDistance(7).Bits(3, 2);
  const Result result = Inflate(writer.Fixed(256).Done());
  EXPECT_EQ(result.out, BytesOf(start + start.substr(4, 12)));
}

TEST(Inflater, ReachesBackAsFarAsTheWindowAndNoFurther) {
  // The furthest back a repeat can reach is 32,768 bytes: distance symbol
  // 29 with all thirteen of its extra bits set. zlib's compressor never
  // reaches quite that far, so the comparison with it does not cover this.
  const auto plain = [](std::size_t count) {
    Bytes bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
      bytes[i] = static_cast<std::uint8_t>((i * 31) ^ (i >> 8U));
    }
    return bytes;
  };
  // `before` bytes as they are, then 258 repeated from 32,768 back.
  const auto stream = [&](std::size_t before) {
    StreamWriter writer;
    writer.BlockHeader(false, 0).StoredLength(static_cast<std::uint16_t>(before));
    for (const std::uint8_t byte : plain(before)) {
      writer.Byte(byte);
    }
    return writer.BlockHeader(true, 1).Fixed(285).FixedDistance(29).Bits(8191, 13).Fixed(256).Done();
  };
  const auto expected = [&](std::size_t before) {
    Bytes bytes = plain(before);
    for (std::size_t i = 0; i < 258; ++i) {
      bytes.push_back(bytes[before - 32768 + i]);
    }
    return bytes;
  };

  // Back to the very first byte.
  const Result first = Inflate(stream(32768));
  EXPECT_EQ(first.status, Status::kDone);
  EXPECT_TRUE(first.out == expected(32768));

  // With more than a window's worth behind it, so the window has wrapped.
  const Result wrapped = Inflate(stream(40000), 100, 333);
  EXPECT_EQ(wrapped.status, Status::kDone);
  EXPECT_TRUE(wrapped.out == expected(40000));

  // One byte short of enough.
  const Result tooFar = Inflate(stream(32767));
  EXPECT_EQ(tooFar.status, Status::kBadData);
  EXPECT_TRUE(tooFar.out == plain(32767));
}

TEST(Inflater, ReadsBlocksOneAfterAnother) {
  StreamWriter writer;
  writer.BlockHeader(false, 0).StoredLength(3).Raw("one");
  writer.BlockHeader(false, 1).FixedText(" two").Fixed(256);
  writer.BlockHeader(false, 0).StoredLength(0);
  writer.BlockHeader(false, 1).Fixed(256);
  // A repeat may reach back into an earlier block.
  writer.BlockHeader(true, 1).Fixed(257).FixedDistance(4).Bits(0, 1).Fixed(256);  // Length 3, distance 5: "e t".
  const Result result = Inflate(writer.Done());
  EXPECT_EQ(result.out, BytesOf("one twoe t"));
  EXPECT_EQ(result.status, Status::kDone);
}

TEST(Inflater, ReadsABlockThatBringsItsOwnCodes) {
  // Literal and length symbols: 'a' (97) and the end of block, one bit
  // each. No distance codes. The code-length code has two symbols, 0 and
  // 1, a bit each, so the lengths are written as 97 zeros, a one, 158
  // zeros, a one, and one zero for the single distance code.
  StreamWriter writer = DynamicHeader(257, 1, {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
  // In that code a 0 is pattern 0 and a 1 is pattern 1.
  for (int i = 0; i < 97; ++i) {
    writer.Pattern(0, 1);
  }
  writer.Pattern(1, 1);
  for (int i = 0; i < 158; ++i) {
    writer.Pattern(0, 1);
  }
  writer.Pattern(1, 1);
  writer.Pattern(0, 1);
  // The data: 'a' is pattern 0, the end is pattern 1.
  writer.Pattern(0, 1).Pattern(0, 1).Pattern(0, 1).Pattern(1, 1);
  const Result result = Inflate(writer.Done());
  EXPECT_EQ(result.out, BytesOf("aaa"));
  EXPECT_EQ(result.status, Status::kDone);
}

TEST(Inflater, TakesNoNoticeOfWhatFollowsTheStream) {
  Bytes stream = StreamWriter().BlockHeader(true, 1).FixedText("end").Fixed(256).Done();
  stream.insert(stream.end(), {0xFF, 0xFF, 0x07, 0x00});
  const Result result = Inflate(stream);
  EXPECT_EQ(result.out, BytesOf("end"));
  EXPECT_EQ(result.status, Status::kDone);
}

// ---- Streams that are wrong ----

TEST(Inflater, RefusesABlockOfNoKnownKind) {
  const Result result = Inflate(StreamWriter().BlockHeader(true, 3).Done());
  EXPECT_TRUE(result.out.empty());
  EXPECT_EQ(result.status, Status::kBadData);
}

TEST(Inflater, RefusesAStoredBlockWhoseTwoLengthsDisagree) {
  const Bytes stream = StreamWriter().BlockHeader(true, 0).ToByteBoundary().Bits(1, 16).Bits(0, 16).Raw("x").Done();
  EXPECT_EQ(Inflate(stream).status, Status::kBadData);
}

TEST(Inflater, RefusesARepeatFromBeforeTheStart) {
  // One byte out, then a repeat from two back.
  const Bytes stream = StreamWriter().BlockHeader(true, 1).FixedText("a").Fixed(257).FixedDistance(1).Fixed(256).Done();
  const Result result = Inflate(stream);
  EXPECT_EQ(result.out, BytesOf("a")) << "what came before the fault is still given";
  EXPECT_EQ(result.status, Status::kBadData);

  // And with nothing out at all.
  const Bytes atOnce = StreamWriter().BlockHeader(true, 1).Fixed(257).FixedDistance(0).Fixed(256).Done();
  EXPECT_EQ(Inflate(atOnce).status, Status::kBadData);
}

TEST(Inflater, RefusesSymbolsTheFormatDoesNotDefine) {
  // The fixed code has patterns for length symbols 286 and 287 and for
  // distance symbols 30 and 31, none of which mean anything.
  for (const unsigned symbol : {286U, 287U}) {
    const Bytes stream = StreamWriter().BlockHeader(true, 1).FixedText("abc").Fixed(symbol).Fixed(256).Done();
    EXPECT_EQ(Inflate(stream).status, Status::kBadData) << symbol;
  }
  for (const unsigned symbol : {30U, 31U}) {
    const Bytes stream =
        StreamWriter().BlockHeader(true, 1).FixedText("abc").Fixed(257).FixedDistance(symbol).Fixed(256).Done();
    EXPECT_EQ(Inflate(stream).status, Status::kBadData) << symbol;
  }
}

TEST(Inflater, RefusesCodesThatCannotBe) {
  // A code-length code with patterns left over: one symbol of one bit.
  EXPECT_EQ(Inflate(DynamicHeader(257, 1, {0, 0, 0, 1}).Done()).status, Status::kBadData);
  // And one with too many symbols for their lengths: three of one bit.
  EXPECT_EQ(Inflate(DynamicHeader(257, 1, {1, 1, 1, 0}).Done()).status, Status::kBadData);

  // "Repeat the length before" with no length before. The code-length
  // code here has symbols 16 and 0, a bit each: 16 is pattern 1 (symbols
  // take their patterns in order).
  StreamWriter repeatFirst = DynamicHeader(257, 1, {1, 0, 0, 1});
  repeatFirst.Pattern(1, 1).Bits(0, 2);
  EXPECT_EQ(Inflate(repeatFirst.Done()).status, Status::kBadData);

  // Every length zero, so no codes at all: symbols 18 and 0, a bit each,
  // and runs of 138 and 120 zeros.
  StreamWriter nothing = DynamicHeader(257, 1, {0, 0, 1, 1});
  nothing.Pattern(1, 1).Bits(127, 7).Pattern(1, 1).Bits(109, 7);
  EXPECT_EQ(Inflate(nothing.Done()).status, Status::kBadData);

  // The rest are each a good stream but for one thing. In the good one,
  // literal 0 and the end of the block have a bit each, so has the one
  // distance code, and the data is a single zero byte. Its code-length
  // code has symbols 1 (a length of one bit) and 18 (a run of zeros),
  // which get patterns 0 and 1.
  const auto header = [](unsigned literals, unsigned distances) {
    return DynamicHeader(literals, distances, {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
  };
  const auto one = [](StreamWriter& writer) { writer.Pattern(0, 1); };
  const auto zeros = [](StreamWriter& writer, unsigned count) { writer.Pattern(1, 1).Bits(count - 11, 7); };
  const auto withData = [](StreamWriter& writer) { return writer.Pattern(0, 1).Pattern(1, 1).Done(); };

  StreamWriter good = header(257, 1);
  one(good);
  zeros(good, 138);
  zeros(good, 117);
  one(good);  // The end of the block, symbol 256.
  one(good);  // The distance code.
  const Result fine = Inflate(withData(good));
  EXPECT_EQ(fine.out, Bytes{0});
  EXPECT_EQ(fine.status, Status::kDone);

  // More literal and length codes than there are symbols: 287.
  StreamWriter manyLiterals = header(287, 1);
  one(manyLiterals);
  zeros(manyLiterals, 138);
  zeros(manyLiterals, 117);
  one(manyLiterals);
  zeros(manyLiterals, 30);
  one(manyLiterals);
  EXPECT_EQ(Inflate(withData(manyLiterals)).status, Status::kBadData);

  // More distance codes than there are symbols: 31.
  StreamWriter manyDistances = header(257, 31);
  one(manyDistances);
  zeros(manyDistances, 138);
  zeros(manyDistances, 117);
  one(manyDistances);
  one(manyDistances);
  zeros(manyDistances, 30);
  EXPECT_EQ(Inflate(withData(manyDistances)).status, Status::kBadData);

  // A run of zeros that goes ten past the last length.
  StreamWriter overrun = header(257, 1);
  one(overrun);
  zeros(overrun, 138);
  zeros(overrun, 117);
  one(overrun);
  zeros(overrun, 11);
  EXPECT_EQ(Inflate(withData(overrun)).status, Status::kBadData);

  // No code for the end of the block: literals 0 and 1 have the two
  // patterns.
  StreamWriter noEnd = header(257, 1);
  one(noEnd);
  one(noEnd);
  zeros(noEnd, 138);
  zeros(noEnd, 117);
  one(noEnd);
  EXPECT_EQ(Inflate(withData(noEnd)).status, Status::kBadData);
}

TEST(Inflater, RefusesALiteralCodeWithTooManyOrTooFewPatterns) {
  // Code-length symbols 0, 1 and 2, with lengths 1, 2 and 2: patterns 0,
  // 10 and 11.
  // (In the order the format lists them, symbol 2 is sixteenth and symbol
  // 1 is eighteenth.)
  const auto header = [] { return DynamicHeader(257, 1, {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2}); };
  const auto zeros = [](StreamWriter& writer, int count) {
    for (int i = 0; i < count; ++i) {
      writer.Pattern(0, 1);
    }
  };

  // Three symbols of one bit: more than one bit can tell apart.
  StreamWriter tooMany = header();
  tooMany.Pattern(2, 2).Pattern(2, 2);
  zeros(tooMany, 254);
  tooMany.Pattern(2, 2);
  zeros(tooMany, 1);
  EXPECT_EQ(Inflate(tooMany.Done()).status, Status::kBadData);

  // Two symbols of two bits: half the patterns unused.
  StreamWriter tooFew = header();
  tooFew.Pattern(3, 2);
  zeros(tooFew, 255);
  tooFew.Pattern(3, 2);
  zeros(tooFew, 1);
  EXPECT_EQ(Inflate(tooFew.Done()).status, Status::kBadData);

  // One symbol of one bit is allowed, though it leaves a pattern unused;
  // using that pattern is not.
  StreamWriter onlyTheEnd = header();
  zeros(onlyTheEnd, 256);
  onlyTheEnd.Pattern(2, 2);
  zeros(onlyTheEnd, 1);
  StreamWriter usesTheOther = onlyTheEnd;
  const Result fine = Inflate(onlyTheEnd.Pattern(0, 1).Done());
  EXPECT_TRUE(fine.out.empty());
  EXPECT_EQ(fine.status, Status::kDone);
  EXPECT_EQ(Inflate(usesTheOther.Pattern(1, 1).Pattern(0, 1).Done()).status, Status::kBadData);
}

TEST(Inflater, SaysWhenTheInputEndsTooSoon) {
  EXPECT_EQ(Inflate({}).status, Status::kTruncated);

  // A stored block that promises five bytes and has two.
  const Bytes stored = StreamWriter().BlockHeader(true, 0).StoredLength(5).Raw("he").Done();
  const Result result = Inflate(stored);
  EXPECT_EQ(result.out, BytesOf("he"));
  EXPECT_EQ(result.status, Status::kTruncated);

  // Every way of cutting a stream short: never the whole of it, never
  // "done", and never anything that was not in it.
  const std::string text = "the quick brown fox jumps over the lazy dog, the quick brown fox";
  StreamWriter writer;
  writer.BlockHeader(false, 1).FixedText(text).Fixed(256);
  writer.BlockHeader(true, 0).StoredLength(4).Raw("tail");
  const Bytes whole = writer.Done();
  const Bytes expected = BytesOf(text + "tail");
  ASSERT_EQ(Inflate(whole).out, expected);
  for (std::size_t length = 0; length < whole.size(); ++length) {
    const Result cut = Inflate(Bytes(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length)));
    EXPECT_EQ(cut.status, Status::kTruncated) << length;
    ASSERT_LT(cut.out.size(), expected.size()) << length;
    EXPECT_TRUE(std::equal(cut.out.begin(), cut.out.end(), expected.begin())) << length;
  }
}

TEST(Inflater, SaysWhenTheSourceFails) {
  const Bytes stream = StreamWriter().BlockHeader(true, 0).StoredLength(600).Raw(std::string(600, 'x')).Done();
  PiecewiseSource source(stream, 100);
  source.failAt = 300;
  std::vector<std::uint8_t> window(Inflater::kWindowBytes);
  Inflater inflater(source, window);
  Bytes out(1000);
  const std::ptrdiff_t first = inflater.Read(out);
  EXPECT_GT(first, 0);
  EXPECT_LT(first, 300);
  EXPECT_EQ(inflater.Read(out), ByteSource::kError);
  EXPECT_EQ(inflater.CurrentStatus(), Status::kSourceFailed);
  EXPECT_EQ(inflater.Read(out), ByteSource::kError) << "and it stays failed";
}

TEST(Inflater, NeedsAWindowOfTheFullSize) {
  const Bytes stream = StreamWriter().BlockHeader(true, 1).FixedText("x").Fixed(256).Done();
  PiecewiseSource source(stream, SIZE_MAX);
  std::vector<std::uint8_t> window(Inflater::kWindowBytes - 1);
  Inflater inflater(source, window);
  Bytes out(10);
  EXPECT_EQ(inflater.Read(out), ByteSource::kError);
  EXPECT_EQ(inflater.CurrentStatus(), Status::kNoWindow);
  EXPECT_EQ(source.calls, 0);
}

TEST(Inflater, GivesNothingToAnEmptyDestinationAndKeepsItsPlace) {
  const Bytes stream = StreamWriter().BlockHeader(true, 1).FixedText("abc").Fixed(256).Done();
  PiecewiseSource source(stream, SIZE_MAX);
  std::vector<std::uint8_t> window(Inflater::kWindowBytes);
  Inflater inflater(source, window);
  EXPECT_EQ(inflater.Read({}), 0);
  EXPECT_EQ(inflater.CurrentStatus(), Status::kRunning);
  Bytes out(10);
  EXPECT_EQ(inflater.Read(out), 3);
  EXPECT_EQ(inflater.Read(out), 0);
  EXPECT_EQ(inflater.CurrentStatus(), Status::kDone);
  EXPECT_EQ(inflater.Read(out), 0) << "the end stays the end";
}

TEST(Inflater, CanBeStartedAgainOnAnotherStream) {
  const Bytes first = StreamWriter().BlockHeader(true, 1).FixedText("first").Fixed(256).Done();
  // Cut short, so that it is left failed and part of the way through.
  const Bytes broken = StreamWriter().BlockHeader(true, 0).StoredLength(9).Raw("bro").Done();
  // A repeat from before its own start: nothing of the earlier streams may
  // be reachable.
  const Bytes reachesBack = StreamWriter().BlockHeader(true, 1).Fixed(257).FixedDistance(0).Fixed(256).Done();
  const Bytes last = StreamWriter().BlockHeader(true, 1).FixedText("last").Fixed(256).Done();

  PiecewiseSource firstSource(first, SIZE_MAX);
  std::vector<std::uint8_t> window(Inflater::kWindowBytes);
  Inflater inflater(firstSource, window);
  Bytes out(100);
  EXPECT_EQ(inflater.Read(out), 5);

  PiecewiseSource brokenSource(broken, SIZE_MAX);
  inflater.Reset(brokenSource);
  EXPECT_EQ(inflater.BytesOut(), 0u);
  EXPECT_EQ(inflater.Read(out), 3);
  EXPECT_EQ(inflater.Read(out), ByteSource::kError);

  PiecewiseSource reachingSource(reachesBack, SIZE_MAX);
  inflater.Reset(reachingSource);
  EXPECT_EQ(inflater.Read(out), ByteSource::kError);
  EXPECT_EQ(inflater.CurrentStatus(), Status::kBadData);

  PiecewiseSource lastSource(last, SIZE_MAX);
  inflater.Reset(lastSource);
  ASSERT_EQ(inflater.Read(out), 4);
  EXPECT_EQ(Bytes(out.begin(), out.begin() + 4), BytesOf("last"));
  EXPECT_EQ(inflater.Read(out), 0);
  EXPECT_EQ(inflater.CurrentStatus(), Status::kDone);
}

// ---- How much one call can be made to do ----

// A block that brings its own codes and holds `text`, which must be one
// letter repeated or nothing: the smallest such block there is. Its
// code-length code has two symbols with patterns one bit long: "a length
// of 1" and "so many lengths of 0".
void TinyDynamicBlock(StreamWriter& writer, bool last, std::string_view text) {
  writer.BlockHeader(last, 2).Bits(0, 5).Bits(0, 5).Bits(18 - 4, 4);
  // In the format's order, the lengths for 16, 17, 18, 0, 8, ..., 1: symbol
  // 18 is the third and symbol 1 the eighteenth.
  for (unsigned i = 0; i < 18; ++i) {
    writer.Bits(i == 2 || i == 17 ? 1 : 0, 3);
  }
  const auto zeros = [&writer](unsigned count) {
    while (count > 0) {
      // A run is of 11 to 138, so the last one must not be left too short.
      unsigned run = std::min(count, 138U);
      if (count - run > 0 && count - run < 11) {
        run = count - 11;
      }
      EXPECT_GE(run, 11U);
      writer.Pattern(1, 1).Bits(run - 11, 7);
      count -= run;
    }
  };
  const auto one = [&writer] { writer.Pattern(0, 1); };
  if (text.empty()) {
    zeros(256);
  } else {
    const auto letter = static_cast<unsigned char>(text.front());
    zeros(letter);
    one();
    zeros(255U - letter);
  }
  one();  // The end of the block,
  one();  // and one distance code, which nothing uses.
  // With a letter, the letter's pattern is 0 and the end's is 1; without,
  // the end's is 0.
  for (std::size_t i = 0; i < text.size(); ++i) {
    writer.Pattern(0, 1);
  }
  writer.Pattern(text.empty() ? 0 : 1, 1);
}

// A block that brings its own codes written the longest way the format has:
// every one of the 286 and 30 code lengths given by a pattern of seven
// bits. It holds one byte, a zero, or nothing. 2,283 bits before its data
// begins, which is the most the start of a block can cost.
void FattestDynamicBlock(StreamWriter& writer, bool last, bool withByte) {
  writer.BlockHeader(last, 2).Bits(286 - 257, 5).Bits(30 - 1, 5).Bits(19 - 4, 4);
  // The code-length code: lengths 0, 1, 2, 3 and 6 have patterns of 1 to 5
  // bits, which nothing here uses, and 4, 5, 8 and 9 the four of 7 bits.
  constexpr std::array<unsigned, 19> kBitsOfSymbol = {1, 2, 3, 4, 7, 7, 5, 0, 7, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  constexpr std::array<unsigned, 19> kOrder = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  for (const unsigned symbol : kOrder) {
    writer.Bits(kBitsOfSymbol.at(symbol), 3);
  }
  const auto lengths = [&writer](unsigned count, unsigned length) {
    // The patterns of 7 bits, in the order of their symbols: 4, 5, 8, 9.
    const unsigned pattern = length == 4 ? 0x7C : length == 5 ? 0x7D : length == 8 ? 0x7E : 0x7F;
    for (unsigned i = 0; i < count; ++i) {
      writer.Pattern(pattern, 7);
    }
  };
  // Literals and lengths: 226 patterns of 8 bits and 60 of 9 use every
  // pattern there is. Distances: 2 of 4 bits and 28 of 5.
  lengths(226, 8);
  lengths(60, 9);
  lengths(2, 4);
  lengths(28, 5);
  if (withByte) {
    writer.Pattern(0, 8);  // The literal 0: the first pattern of 8 bits.
  }
  writer.Pattern((226U << 1U) + (256 - 226), 9);  // The end of the block: the thirty-first of 9 bits.
}

// One block that produces nothing, of each of the three kinds in turn.
void EmptyBlock(StreamWriter& writer, unsigned kind) {
  switch (kind % 3) {
    case 0:
      writer.BlockHeader(false, 0).StoredLength(0);
      break;
    case 1:
      writer.BlockHeader(false, 1).Fixed(256);
      break;
    default:
      TinyDynamicBlock(writer, false, "");
      break;
  }
}

TEST(Inflater, ReadsTheSmallestBlockThatBringsItsOwnCodes) {
  // The blocks the tests below are made of, read right.
  StreamWriter writer;
  TinyDynamicBlock(writer, false, "aaa");
  TinyDynamicBlock(writer, false, "");
  TinyDynamicBlock(writer, true, "z");
  const Result result = Inflate(writer.Done());
  EXPECT_EQ(result.out, BytesOf("aaaz"));
  EXPECT_EQ(result.status, Status::kDone);
}

TEST(Inflater, RefusesAStreamOfBlocksThatProduceNothing) {
  // A megabyte of stored blocks with nothing in them, and a quarter of a
  // megabyte of each other kind and of all three in turn. Nothing comes
  // out, so a call has nothing to come back with: it must stop for
  // another reason, and long before the stream has been read.
  struct Case {
    unsigned kind;  // 3 for all three in turn.
    unsigned blocks;
  };
  for (const Case& made : {Case{0, 210'000}, Case{1, 210'000}, Case{2, 25'000}, Case{3, 60'000}}) {
    StreamWriter writer;
    for (unsigned block = 0; block < made.blocks; ++block) {
      EmptyBlock(writer, made.kind == 3 ? block : made.kind);
    }
    const Bytes stream = writer.Done();
    ASSERT_GT(stream.size(), 250'000u) << "kind " << made.kind;
    const Result result = Inflate(stream);
    EXPECT_EQ(result.status, Status::kBadData) << "kind " << made.kind;
    EXPECT_TRUE(result.out.empty()) << "kind " << made.kind;
    EXPECT_LT(result.taken, 1024u) << "kind " << made.kind;
  }
}

TEST(Inflater, AllowsAFewBlocksInARowThatProduceNothing) {
  // As many as it says it allows, between two blocks with something in
  // them, and at the end of a stream; and one more than that is refused,
  // after what came before has been given out.
  for (const unsigned count : {1U, Inflater::kMaxEmptyBlocksInARow, Inflater::kMaxEmptyBlocksInARow + 1}) {
    StreamWriter writer;
    writer.BlockHeader(false, 1).FixedText("before").Fixed(256);
    for (unsigned block = 0; block < count; ++block) {
      EmptyBlock(writer, block);
    }
    writer.BlockHeader(false, 1).FixedText(" after").Fixed(256);
    for (unsigned block = 1; block < count; ++block) {
      EmptyBlock(writer, block);
    }
    writer.BlockHeader(true, 1).Fixed(256);
    const Result result = Inflate(writer.Done());
    if (count <= Inflater::kMaxEmptyBlocksInARow) {
      EXPECT_EQ(result.out, BytesOf("before after")) << count;
      EXPECT_EQ(result.status, Status::kDone) << count;
    } else {
      EXPECT_EQ(result.out, BytesOf("before")) << count;
      EXPECT_EQ(result.status, Status::kBadData) << count;
    }
  }
}

TEST(Inflater, DoesOnlySoMuchInOneCallHoweverSmallTheBlocksAre) {
  // A stream can make every byte of output cost a block of its own, with
  // its own codes to read and build. A call that was asked for a great
  // deal then comes back with a little, having started only so many
  // blocks; and all of it comes out right in the end.
  constexpr std::size_t kBlocks = 20'000;
  for (const bool ownCodes : {false, true}) {
    StreamWriter writer;
    Bytes expected;
    for (std::size_t block = 0; block < kBlocks; ++block) {
      const char letter = static_cast<char>('a' + (block % 26));
      if (ownCodes) {
        TinyDynamicBlock(writer, false, std::string_view(&letter, 1));
      } else {
        writer.BlockHeader(false, 1).Fixed(static_cast<unsigned char>(letter)).Fixed(256);
      }
      expected.push_back(static_cast<std::uint8_t>(letter));
    }
    writer.BlockHeader(true, 1).Fixed(256);
    const Bytes stream = writer.Done();

    PiecewiseSource source(stream, SIZE_MAX);
    Bytes window(Inflater::kWindowBytes);
    Inflater inflater(source, window);
    Bytes out(kBlocks + 100);
    std::size_t used = 0;
    std::size_t mostInACall = 0;
    for (;;) {
      const std::ptrdiff_t read = inflater.Read(std::span(out).subspan(used));
      if (read <= 0) {
        break;
      }
      mostInACall = std::max(mostInACall, static_cast<std::size_t>(read));
      used += static_cast<std::size_t>(read);
    }
    out.resize(used);
    EXPECT_EQ(inflater.CurrentStatus(), Status::kDone) << ownCodes;
    EXPECT_TRUE(out == expected) << ownCodes;
    // Each block gives one byte, so the bytes a call gives are the blocks
    // it started.
    EXPECT_LE(mostInACall, Inflater::kMaxBlocksPerRead) << ownCodes;
    EXPECT_GE(mostInACall, 2u) << ownCodes;
  }
}

TEST(Inflater, TakesNoMoreFromItsSourceInOneCallThanItSaysItWill) {
  // The streams that cost most to read: blocks with their own codes, each
  // holding one byte, with as many empty ones between as are allowed.
  // First with the shortest such blocks, then with the longest, where
  // every call that gives a byte has had to read seventeen blocks' codes.
  // Asked for a byte at a time and for a great deal at once.
  for (const bool fattest : {false, true}) {
    StreamWriter writer;
    Bytes expected;
    for (unsigned block = 0; block < 60; ++block) {
      const bool last = block == 59;
      if (fattest) {
        FattestDynamicBlock(writer, last, true);
      } else {
        TinyDynamicBlock(writer, last, std::string_view("\0", 1));
      }
      expected.push_back(0);
      for (unsigned empty = 0; !last && empty < Inflater::kMaxEmptyBlocksInARow; ++empty) {
        if (fattest) {
          FattestDynamicBlock(writer, false, false);
        } else {
          TinyDynamicBlock(writer, false, "");
        }
      }
    }
    const Bytes stream = writer.Done();
    for (const std::size_t outputPiece : {1U, 2U, 64U, 4096U}) {
      const Result result = Inflate(stream, SIZE_MAX, outputPiece);
      EXPECT_EQ(result.status, Status::kDone) << fattest << ", " << outputPiece;
      EXPECT_TRUE(result.out == expected) << fattest << ", " << outputPiece;
      // The bound is a close one: the longest blocks come within a tenth
      // of what a call asked for one byte is allowed.
      if (fattest) {
        EXPECT_GT(result.mostTakenByACall, Inflater::MostInputFor(1) * 85 / 100) << outputPiece;
      }
    }
  }
}

// ---- Against zlib ----

// Compresses `data` with zlib as a bare DEFLATE stream. `blockBytes`, if
// not 0, has it end a block every so often, so the stream has many.
Bytes Deflate(const Bytes& data, int level, int strategy, std::size_t blockBytes = 0) {
  z_stream stream = {};
  EXPECT_EQ(deflateInit2(&stream, level, Z_DEFLATED, -15, 8, strategy), Z_OK);
  // Room for the data, and for the few bytes each ended block adds.
  Bytes out(deflateBound(&stream, static_cast<uLong>(data.size())) + 1024 +
            (blockBytes == 0 ? 0 : ((data.size() / blockBytes) + 1) * 16));
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  // zlib's interface is not const-correct about its input.
  Bytes input = data;
  std::size_t given = 0;
  int result = Z_OK;
  while (result != Z_STREAM_END) {
    const std::size_t piece = blockBytes == 0 ? input.size() - given : std::min(blockBytes, input.size() - given);
    stream.next_in = input.data() + given;
    stream.avail_in = static_cast<uInt>(piece);
    given += piece;
    result = deflate(&stream, given == input.size() ? Z_FINISH : Z_FULL_FLUSH);
    EXPECT_TRUE(result == Z_OK || result == Z_STREAM_END) << result;
    if (result != Z_OK && result != Z_STREAM_END) {
      break;
    }
  }
  out.resize(stream.total_out);
  deflateEnd(&stream);
  return out;
}

// Fills `data` with one kind of content. Each pushes a compressor a
// different way.
void FillWith(std::string_view kind, std::span<std::uint8_t> data, std::mt19937& random) {
  if (kind == "noise") {
    for (std::uint8_t& byte : data) {
      byte = static_cast<std::uint8_t>(random());
    }
  } else if (kind == "zeros") {
    std::fill(data.begin(), data.end(), std::uint8_t{0});
  } else if (kind == "text") {
    static constexpr std::array<std::string_view, 12> kWords = {"the ",   "reader ", "turned ",  "a ",
                                                                "page, ", "and ",    "then ",    "another; ",
                                                                "night ", "fell ",   "quietly ", "\n"};
    std::size_t at = 0;
    while (at < data.size()) {
      const std::string_view word = kWords[random() % kWords.size()];
      const std::size_t count = std::min(word.size(), data.size() - at);
      std::copy_n(word.begin(), count, data.begin() + static_cast<std::ptrdiff_t>(at));
      at += count;
    }
  } else {  // "runs": long repeats of a few bytes, at distances near and far.
    std::size_t at = 0;
    while (at < data.size()) {
      const auto byte = static_cast<std::uint8_t>(random() % 4);
      const std::size_t count = std::min<std::size_t>(1 + (random() % 700), data.size() - at);
      std::fill_n(data.begin() + static_cast<std::ptrdiff_t>(at), count, byte);
      at += count;
    }
  }
}

// `size` bytes of "noise", "zeros", "text" or "runs", or of "mixed":
// stretches of each.
Bytes Sample(std::string_view kind, std::size_t size, std::uint32_t seed) {
  std::mt19937 random(seed);
  Bytes data(size);
  if (kind != "mixed") {
    FillWith(kind, data, random);
    return data;
  }
  static constexpr std::array<std::string_view, 4> kKinds = {"noise", "zeros", "text", "runs"};
  std::size_t at = 0;
  while (at < size) {
    const std::size_t count = std::min<std::size_t>(1 + (random() % 9000), size - at);
    FillWith(kKinds[random() % kKinds.size()], std::span(data).subspan(at, count), random);
    at += count;
  }
  return data;
}

TEST(Inflater, UndoesWhateverZlibCompresses) {
  int streams = 0;
  for (const char* kind : {"noise", "zeros", "text", "runs", "mixed"}) {
    for (const std::size_t size : {0U, 1U, 2U, 257U, 4096U, 32767U, 32768U, 32769U, 70000U, 120000U}) {
      const Bytes data = Sample(kind, size, static_cast<std::uint32_t>(size + 17));
      for (const int level : {0, 1, 6, 9}) {
        for (const int strategy : {Z_DEFAULT_STRATEGY, Z_FIXED, Z_HUFFMAN_ONLY, Z_RLE}) {
          const Bytes compressed = Deflate(data, level, strategy);
          const Result result = Inflate(compressed);
          ASSERT_EQ(result.status, Status::kDone)
              << kind << ", " << size << " bytes, level " << level << ", strategy " << strategy;
          ASSERT_TRUE(result.out == data)
              << kind << ", " << size << " bytes, level " << level << ", strategy " << strategy;
          ++streams;
        }
      }
    }
  }
  EXPECT_EQ(streams, 5 * 10 * 4 * 4);
}

TEST(Inflater, UndoesStreamsOfManyBlocks) {
  // A new block every few hundred bytes, of whichever kind zlib thinks
  // best for each: stored, fixed and the kind that brings its own codes
  // all turn up, and repeats reach back across them.
  for (const char* kind : {"text", "mixed", "runs"}) {
    const Bytes data = Sample(kind, 200'000, 5);
    for (const std::size_t blockBytes : {300U, 5000U}) {
      const Bytes compressed = Deflate(data, 6, Z_DEFAULT_STRATEGY, blockBytes);
      const Result result = Inflate(compressed);
      ASSERT_EQ(result.status, Status::kDone) << kind << ", blocks of " << blockBytes;
      ASSERT_TRUE(result.out == data) << kind << ", blocks of " << blockBytes;
    }
  }
  // And a block for every byte.
  const Bytes data = Sample("text", 20'000, 6);
  const Result result = Inflate(Deflate(data, 6, Z_DEFAULT_STRATEGY, 1));
  ASSERT_EQ(result.status, Status::kDone);
  ASSERT_TRUE(result.out == data);
}

TEST(Inflater, GivesTheSameBytesHoweverItIsFedAndHoweverItIsAsked) {
  const Bytes data = Sample("mixed", 150'000, 99);
  const Bytes compressed = Deflate(data, 6, Z_DEFAULT_STRATEGY, 20'000);
  for (const std::size_t sourcePiece : {1U, 2U, 3U, 255U, 256U, 257U, 4096U}) {
    for (const std::size_t outputPiece : {1U, 2U, 7U, 257U, 258U, 259U, 32768U, 200'000U}) {
      const Result result = Inflate(compressed, sourcePiece, outputPiece);
      ASSERT_EQ(result.status, Status::kDone) << sourcePiece << " in, " << outputPiece << " out";
      ASSERT_TRUE(result.out == data) << sourcePiece << " in, " << outputPiece << " out";
    }
  }
}

TEST(Inflater, SurvivesDamageAnywhereInAStream) {
  // Every bit of a stream turned over, one at a time, and every way of
  // cutting it short. What comes out is of no interest: only that
  // something does, in bounded time, without touching memory that is not
  // its own (which the sanitizers watch for).
  const Bytes data = Sample("text", 1'000, 3);
  for (const int strategy : {Z_DEFAULT_STRATEGY, Z_FIXED}) {
    const Bytes compressed = Deflate(data, 6, strategy, 300);
    ASSERT_LT(compressed.size(), 700u);
    int stillRight = 0;
    for (std::size_t bit = 0; bit < compressed.size() * 8; ++bit) {
      Bytes damaged = compressed;
      damaged[bit / 8] = static_cast<std::uint8_t>(damaged[bit / 8] ^ (1U << (bit % 8)));
      const Result result = Inflate(damaged, 97, 1000, 1U << 13U);
      EXPECT_TRUE(result.status != Status::kRunning || result.out.size() >= (1U << 13U)) << "bit " << bit;
      EXPECT_NE(result.status, Status::kSourceFailed) << "bit " << bit;
      stillRight += result.out == data ? 1 : 0;
    }
    // Nearly every bit matters. (Not all: padding bits do not.)
    EXPECT_LT(stillRight, 64);

    for (std::size_t length = 0; length < compressed.size(); ++length) {
      const Result cut = Inflate(Bytes(compressed.begin(), compressed.begin() + static_cast<std::ptrdiff_t>(length)));
      ASSERT_NE(cut.status, Status::kDone) << length;
      ASSERT_LE(cut.out.size(), data.size()) << length;
      ASSERT_TRUE(std::equal(cut.out.begin(), cut.out.end(), data.begin())) << length;
    }
  }
}

TEST(Inflater, SurvivesInputThatWasNeverAStream) {
  // NOLINTNEXTLINE(bugprone-random-generator-seed): the same input every run, so that a failure can be found again.
  std::mt19937 random(2026);
  for (int trial = 0; trial < 3'000; ++trial) {
    Bytes noise(1 + (random() % 400));
    for (std::uint8_t& byte : noise) {
      byte = static_cast<std::uint8_t>(random());
    }
    const std::size_t sourcePiece = 1 + (random() % 64);
    const std::size_t outputPiece = 1 + (random() % 500);
    const Result result = Inflate(noise, sourcePiece, outputPiece, 1U << 14U);
    EXPECT_TRUE(result.status != Status::kRunning || result.out.size() >= (1U << 14U)) << "trial " << trial;
  }
}

}  // namespace
