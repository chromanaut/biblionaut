// The fuzz target for the XML parser (epub/xml.h), and with it for the
// heap inside a block that the parser's memory comes from
// (core/region_heap.h), used as expat uses it.
//
// What it is given:
//
//   byte 0   bits 0 and 1: how much memory the parser has
//            bits 2 to 4:  how many bytes the source gives at a call
//            bits 5 to 7:  what the handler does besides listen: nothing;
//                          stops the parser at the first, second, third,
//                          tenth or hundredth thing it is told; tries to
//                          use the parser from inside itself; or the
//                          parser is stopped between two steps
//   the rest a document, or what passes for one
//
// What has to hold, whatever the bytes are:
//
//   - parsing ends, with a status that parsing can give;
//   - a step asks its source once, and for no more than kChunkBytes;
//   - the parser's memory is in order after every step, and nothing was
//     given back to it that it had not given out;
//   - it says it ran out of memory only when its memory refused something;
//   - every name, value and piece of text it tells of is all there, and
//     text comes in whole characters;
//   - an attribute is found by its name, with the value it has;
//   - it tells of no more text than twice the bytes it has read (the most
//     a change of encoding can make of them): nothing expands;
//   - where it says it has got to is not past what it has read;
//   - a handler that stops it is told nothing more, and one that tries to
//     begin or step it from inside is refused and changes nothing;
//   - afterwards the same parser reads a small good document rightly.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/byte_source.h"
#include "epub/xml.h"
#include "fuzz_support.h"

namespace {

using bn::epub::XmlAttributes;
using bn::epub::XmlParser;
using bn::epub::XmlStatus;

// The first two are enough for any small document. The others are not
// enough to begin with, or run out early: what is being looked at there is
// how the parser takes being refused.
constexpr std::array<std::size_t, 4> kMemoryBytes = {32768, 16384, 8192, 2048};
constexpr unsigned kAmpleMemories = 2;
constexpr std::array<std::size_t, 8> kSourcePieces = {1024, 1, 2, 7, 100, 513, 1000, 1023};

// What the handler does besides listen (bits 5 to 7 of byte 0).
constexpr unsigned kMeddles = 6;
constexpr unsigned kStoppedBetweenSteps = 7;
// For the values below those two: the count of things told at which the
// handler stops the parser. 0 for never.
constexpr std::array<std::uint32_t, 6> kStopAt = {0, 1, 2, 3, 10, 100};

std::span<const std::uint8_t> BytesOf(std::string_view text) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters, as the bytes they are.
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// Listens to a document, checks what it is told, and misbehaves as asked.
class Listener final : public bn::epub::XmlHandler {
 public:
  Listener(XmlParser& parser, std::uint32_t stopAt, bool meddles) noexcept
      : parser_(parser), stopAt_(stopAt), meddles_(meddles) {}

  void OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept override {
    BN_FUZZ_CHECK(!name.empty(), "an element with no name began");
    (void)bn::fuzz::Touch(name);
    for (std::size_t i = 0; i < attributes.Count(); ++i) {
      const std::string_view attribute = attributes.Name(i);
      (void)bn::fuzz::Touch(attribute);
      (void)bn::fuzz::Touch(attributes.Value(i));
      // Found by its name, with the value the first of that name has.
      std::size_t first = 0;
      while (attributes.Name(first) != attribute) {
        ++first;
      }
      std::string_view found;
      BN_FUZZ_CHECK(attributes.Find(attribute, found) && found.data() == attributes.Value(first).data(),
                    "an attribute was not found by its name");
    }
    BN_FUZZ_CHECK(attributes.Name(attributes.Count()).empty() && attributes.Value(attributes.Count()).empty(),
                  "there was an attribute past the last");
    Told();
  }

  void OnEndElement(std::string_view name) noexcept override {
    BN_FUZZ_CHECK(!name.empty(), "an element with no name ended");
    (void)bn::fuzz::Touch(name);
    Told();
  }

  void OnText(std::string_view text) noexcept override {
    BN_FUZZ_CHECK(!text.empty(), "a piece of text with nothing in it was told of");
    BN_FUZZ_CHECK(bn::fuzz::IsWholeCharacters(text), "a piece of text was cut inside a character");
    textBytes += text.size();
    Told();
  }

  void OnEntity(std::string_view name) noexcept override {
    (void)bn::fuzz::Touch(name);
    Told();
  }

  std::uint32_t told = 0;      // How many things it has been told.
  std::size_t textBytes = 0;   // How much text.
  bool stoppedParser = false;  // It has stopped the parser.

 private:
  void Told() noexcept {
    BN_FUZZ_CHECK(!stoppedParser, "a handler was told of something after one had stopped the parser");
    ++told;
    if (meddles_ && told == 2) {
      // Tries to start the parser on another document, and to step it,
      // from inside. Neither may do anything.
      bn::core::MemoryByteSource other(BytesOf("<other/>"));
      BN_FUZZ_CHECK(parser_.Begin(other, *this) == XmlStatus::kInUse && !parser_.Step() &&
                        parser_.Status() == XmlStatus::kOk && other.Remaining() == 8,
                    "a parser could be used from inside one of its own handlers");
    }
    if (told == stopAt_) {
      parser_.Stop();
      stoppedParser = true;
    }
  }

  XmlParser& parser_;
  std::uint32_t stopAt_;
  bool meddles_;
};

// Writes down a small document, to see that a parser still reads rightly.
class Recorder final : public bn::epub::XmlHandler {
 public:
  void OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept override {
    events += "(" + std::string(name);
    for (std::size_t i = 0; i < attributes.Count(); ++i) {
      events += " " + std::string(attributes.Name(i)) + "=" + std::string(attributes.Value(i));
    }
  }
  void OnEndElement(std::string_view name) noexcept override { events += ")" + std::string(name); }
  void OnText(std::string_view text) noexcept override { events += "-" + std::string(text); }
  void OnEntity(std::string_view name) noexcept override { events += "&" + std::string(name); }

  std::string events;
};

void CheckMemory(const XmlParser& parser) {
  BN_FUZZ_CHECK(parser.Memory().IsConsistent(), "the parser's memory is not in order");
  BN_FUZZ_CHECK(parser.Memory().MisuseCount() == 0, "the parser gave back memory it had not been given");
  BN_FUZZ_CHECK(parser.Memory().Used() <= parser.Memory().Capacity() &&
                    parser.Memory().HighWaterSinceReset() <= parser.Memory().Capacity(),
                "the parser has more memory in use than it was given");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const unsigned behaviour = (choices >> 5U) & 7U;
  const std::span<const std::uint8_t> document = input.Rest();

  // The parser's memory starts each time as it is on no device: full of
  // something else.
  static std::vector<std::uint8_t> memory(kMemoryBytes[0]);
  std::fill(memory.begin(), memory.end(), std::uint8_t{0xEE});
  const unsigned memoryChoice = choices & 3U;
  const std::span<std::uint8_t> given = std::span<std::uint8_t>(memory).first(kMemoryBytes[memoryChoice]);
  XmlParser parser(given);

  bn::fuzz::PiecewiseSource source(document, kSourcePieces[(choices >> 2U) & 7U]);
  Listener listener(parser, behaviour < kStopAt.size() ? kStopAt[behaviour] : 0, behaviour == kMeddles);

  bool stoppedBetweenSteps = false;
  bool more = parser.Begin(source, listener) == XmlStatus::kOk;
  std::size_t steps = 0;
  while (more) {
    const std::size_t takenBefore = source.Taken();
    const std::size_t callsBefore = source.Calls();
    more = parser.Step();
    ++steps;
    BN_FUZZ_CHECK(source.Calls() - callsBefore <= 1 && source.Taken() - takenBefore <= XmlParser::kChunkBytes,
                  "a step took more from its source than one piece");
    CheckMemory(parser);
    BN_FUZZ_CHECK(parser.ByteOffset() <= source.Taken(), "the parser says it has got past what it has read");
    BN_FUZZ_CHECK(listener.textBytes <= 2 * source.Taken(), "more text came out than twice what went in");
    if (more && behaviour == kStoppedBetweenSteps && steps == 2) {
      parser.Stop();
      stoppedBetweenSteps = true;
      BN_FUZZ_CHECK(parser.Status() == XmlStatus::kStopped, "a parser stopped between steps does not say so");
    }
  }

  const XmlStatus status = parser.Status();
  BN_FUZZ_CHECK(status == XmlStatus::kOk || status == XmlStatus::kNotWellFormed ||
                    status == XmlStatus::kUnsupportedEncoding || status == XmlStatus::kNoMemory ||
                    status == XmlStatus::kStopped,
                "parsing ended with a status it cannot end with here");
  BN_FUZZ_CHECK(parser.Finished() == (status == XmlStatus::kOk),
                "a document is called read to its end that was not, or the other way about");
  BN_FUZZ_CHECK(!parser.Step() && parser.Status() == status, "a parser that had ended went on");
  if (listener.stoppedParser || stoppedBetweenSteps) {
    BN_FUZZ_CHECK(status == XmlStatus::kStopped, "a parser that was stopped does not say so");
  } else {
    BN_FUZZ_CHECK(status != XmlStatus::kStopped, "a parser says it was stopped that nobody stopped");
  }
  if (status == XmlStatus::kNoMemory) {
    BN_FUZZ_CHECK(parser.Memory().FailureCount() > 0,
                  "the parser says it ran out of memory, and its memory refused nothing");
  }
  BN_FUZZ_CHECK(
      (parser.ErrorCode() != 0) == (status == XmlStatus::kNotWellFormed || status == XmlStatus::kUnsupportedEncoding),
      "the parser gives a number for a mistake when there was none, or none when there was one");
  BN_FUZZ_CHECK(parser.Line() >= 1, "the parser is at a line before the first");

  // Whatever that document did to it, the parser reads the next one
  // rightly, if it has the memory to read anything.
  bn::core::MemoryByteSource good(BytesOf(R"(<?xml version="1.0"?><a b="c">d<e f="g"/>&amp;</a>)"));
  Recorder recorder;
  const XmlStatus again = parser.Parse(good, recorder);
  CheckMemory(parser);
  if (again == XmlStatus::kNoMemory) {
    BN_FUZZ_CHECK(memoryChoice >= kAmpleMemories, "a small good document did not fit in 16 KB");
  } else {
    BN_FUZZ_CHECK(again == XmlStatus::kOk && parser.Finished() && recorder.events == "(a b=c-d(e f=g)e-&)a",
                  "a parser did not read a good document rightly after another");
  }
  return 0;
}
