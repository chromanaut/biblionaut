// The fuzz target for the reader of an SVG file's tags (draw/svg_tags.h).
//
// What it is given:
//
//   byte 0   bits 0 to 2: how many bytes the reader is given at a call
//            bits 3 to 4: how much of a piece of a value or of text is
//                         taken before the rest is handed back: all of
//                         it, or its first one, three or sixteen bytes
//   the rest a file, or what passes for one
//
// The file is read twice: once whole, with every piece taken as it is
// given, and once cut up and handed back as byte 0 says. What has to hold,
// whatever the bytes are:
//
//   - a call takes no more than it was given, and tells nothing only when
//     it has taken all of it;
//   - a call takes a byte or tells something, so there are no more calls
//     than twice the bytes: it cannot be made to go round;
//   - a piece said to be of the input is the last of the bytes just taken;
//   - until reading fails, Offset() is how many bytes it has taken and
//     not been handed back, and when an element begins or ends,
//     TagOffset() is where its tag's `<` is, with the name it tells after
//     it;
//   - a name is not empty and no longer than kLongestName, and a piece of
//     a value or of text is not empty;
//   - what an entity stands for is one character;
//   - things are told in an order tags can have: an attribute inside a
//     tag, a value after its attribute, nothing else until the tag is over;
//   - Depth() is how many elements were opened and not closed;
//   - nothing expands: no more bytes are told of than were taken;
//   - once reading has failed it takes whatever it is given and tells
//     nothing, and where it says it failed is not past what it was given;
//   - a file is read to its end without complaint only if every element
//     that was opened was closed;
//   - the two readings tell the same things and end the same way.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "draw/svg_tags.h"
#include "fuzz_support.h"

namespace {

using bn::draw::SvgTagReader;
using bn::draw::SvgTagStatus;
using bn::draw::SvgTagToken;
using Kind = bn::draw::SvgTagToken::Kind;

constexpr std::array<std::size_t, 8> kPieces = {SIZE_MAX, 1, 2, 3, 7, 64, 255, 1024};
// How many bytes of a piece are taken before the rest is handed back.
constexpr std::array<std::size_t, 4> kTaken = {SIZE_MAX, 1, 3, 16};

// Whether `text` is one character of UTF-8 and nothing more: as many bytes
// as its first says, all of them there.
bool IsOneCharacter(std::string_view text) {
  if (text.empty() || !bn::fuzz::IsWholeCharacters(text)) {
    return false;
  }
  const auto first = static_cast<unsigned char>(text.front());
  const std::size_t length = first < 0x80U ? 1 : (first < 0xE0U ? 2 : (first < 0xF0U ? 3 : 4));
  return text.size() == length;
}

// What a reading came to.
struct Reading {
  // Everything it told, one thing after another, each begun with a byte
  // that says what kind of thing it is. Pieces of one value, or of one run
  // of text, are joined: where they are cut is the one thing that may
  // differ between two readings.
  std::string told;
  // Where each tag began that an element began or ended with.
  std::vector<std::uint32_t> places;
  SvgTagStatus status = SvgTagStatus::kOk;
  std::uint32_t errorOffset = 0;
  std::uint32_t unknownEntities = 0;
};

Reading Read(std::string_view file, std::size_t piece, std::size_t taken) {
  Reading reading;
  SvgTagReader reader;
  // Where the reading is among the things a tag has.
  bool inTag = false;         // An element has begun and its attributes are not over.
  bool hasAttribute = false;  // An attribute has begun in this tag, so a value may come.
  Kind last = Kind::kNone;
  std::uint32_t depth = 0;
  std::size_t given = 0;  // Bytes the reader has taken and kept.
  std::size_t toldBytes = 0;
  std::size_t calls = 0;

  for (std::size_t at = 0; at < file.size();) {
    std::string_view bytes = file.substr(at, piece);
    at += bytes.size();
    while (!bytes.empty()) {
      SvgTagToken token;
      std::size_t took = reader.Next(bytes, token);
      ++calls;
      BN_FUZZ_CHECK(took <= bytes.size(), "a call took more than it was given");
      if (token.kind == Kind::kNone) {
        BN_FUZZ_CHECK(took == bytes.size(), "a call told nothing and left bytes untaken");
        BN_FUZZ_CHECK(token.text.empty(), "a call that told nothing gave some text");
      } else {
        BN_FUZZ_CHECK(reader.Status() == SvgTagStatus::kOk, "something was told after reading had failed");
      }
      (void)bn::fuzz::Touch(token.text);
      if (token.ofInput) {
        BN_FUZZ_CHECK(token.kind == Kind::kValue || token.kind == Kind::kText, "a name was said to be of the input");
        BN_FUZZ_CHECK(token.text.data() + token.text.size() == bytes.data() + took,
                      "a piece of the input is not the last of the bytes taken");
        // Only the front of it is wanted: the rest goes back, and comes
        // again.
        if (token.text.size() > taken) {
          const std::size_t back = token.text.size() - taken;
          reader.GiveBack(back);
          took -= back;
          token.text = token.text.substr(0, taken);
        }
      } else if (token.kind == Kind::kValue || token.kind == Kind::kText) {
        // A piece that is not of the input stands for something in it:
        // the one character an entity named; or an `&` that began no
        // entity, with what followed it; or square brackets that did not
        // end text marked as text.
        BN_FUZZ_CHECK(IsOneCharacter(token.text) || token.text.starts_with('&') || token.text == "]]",
                      "what stood for something in the input is not one character");
      }

      switch (token.kind) {
        case Kind::kNone:
          break;
        case Kind::kOpen:
          BN_FUZZ_CHECK(!inTag, "an element began inside another's tag");
          BN_FUZZ_CHECK(!token.text.empty() && token.text.size() <= SvgTagReader::kLongestName,
                        "an element's name is empty or too long");
          inTag = true;
          hasAttribute = false;
          reading.told += '\x01';
          break;
        case Kind::kAttribute:
          BN_FUZZ_CHECK(inTag, "an attribute came outside a tag");
          BN_FUZZ_CHECK(!token.text.empty() && token.text.size() <= SvgTagReader::kLongestName,
                        "an attribute's name is empty or too long");
          hasAttribute = true;
          reading.told += '\x02';
          break;
        case Kind::kValue:
          BN_FUZZ_CHECK(inTag && hasAttribute, "a value came with no attribute to be the value of");
          BN_FUZZ_CHECK(!token.text.empty(), "a piece of a value with nothing in it was told of");
          if (last != Kind::kValue) {
            reading.told += '\x03';
          }
          break;
        case Kind::kOpened:
          BN_FUZZ_CHECK(inTag, "a tag ended that had not begun");
          BN_FUZZ_CHECK(token.text.empty(), "the end of a tag came with some text");
          inTag = false;
          depth += token.empty ? 0 : 1;
          reading.told += token.empty ? '\x04' : '\x05';
          break;
        case Kind::kClose:
          BN_FUZZ_CHECK(!inTag, "an element ended inside another's tag");
          BN_FUZZ_CHECK(!token.text.empty() && token.text.size() <= SvgTagReader::kLongestName,
                        "the name of an element that ended is empty or too long");
          BN_FUZZ_CHECK(depth > 0, "an element was closed that was never opened, and nothing was said");
          --depth;
          reading.told += '\x06';
          break;
        case Kind::kText:
          BN_FUZZ_CHECK(!inTag, "text came inside a tag");
          BN_FUZZ_CHECK(!token.text.empty(), "a piece of text with nothing in it was told of");
          if (last != Kind::kText) {
            reading.told += '\x07';
          }
          break;
      }
      if (token.kind != Kind::kNone) {
        last = token.kind;
        reading.told += token.text;
        toldBytes += token.text.size();
      }
      if (token.kind == Kind::kOpen || token.kind == Kind::kClose) {
        // Its tag is in the file where the reader says it began: a `<`,
        // a `/` if the element is ending, and the name.
        const std::size_t place = reader.TagOffset();
        const std::string_view tag = place < file.size() ? file.substr(place) : std::string_view();
        const std::string_view before = token.kind == Kind::kOpen ? "<" : "</";
        BN_FUZZ_CHECK(tag.starts_with(before) && tag.substr(before.size()).starts_with(token.text),
                      "a tag is not where the reader says it began");
        reading.places.push_back(reader.TagOffset());
      }
      bytes.remove_prefix(took);
      given += took;
      BN_FUZZ_CHECK(reader.Status() != SvgTagStatus::kOk || reader.Offset() == given,
                    "the reader's count of bytes is not what it has taken");
      BN_FUZZ_CHECK(reader.Depth() == depth, "the reader's count of open elements is not what was opened and closed");
      BN_FUZZ_CHECK(toldBytes <= given, "more bytes were told of than were taken");
    }
  }
  BN_FUZZ_CHECK(calls <= (2 * file.size()) + 2, "there were more calls than twice the bytes");

  const SvgTagStatus before = reader.Status();
  reading.status = reader.Finish();
  BN_FUZZ_CHECK(before == SvgTagStatus::kOk || reading.status == before, "a reading that had failed ended otherwise");
  BN_FUZZ_CHECK(reading.status != SvgTagStatus::kOk || (depth == 0 && !inTag),
                "a file was read to its end without complaint with a tag or an element still open");
  BN_FUZZ_CHECK(reading.status == SvgTagStatus::kOk || reading.status == SvgTagStatus::kMalformed ||
                    reading.status == SvgTagStatus::kUnbalanced || reading.status == SvgTagStatus::kTruncated,
                "reading ended with a status there is not");
  BN_FUZZ_CHECK(reader.Finish() == reading.status, "the end of a reading changed when it was asked for again");
  reading.errorOffset = reader.ErrorOffset();
  reading.unknownEntities = reader.UnknownEntities();
  BN_FUZZ_CHECK(reading.errorOffset <= file.size(), "reading is said to have failed past the end of the file");
  BN_FUZZ_CHECK(reading.status != SvgTagStatus::kOk || reading.errorOffset == 0,
                "a reading that did not fail says where it failed");

  // Whatever it is given now, it takes and tells nothing of, if it failed.
  if (reading.status != SvgTagStatus::kOk) {
    SvgTagToken token;
    BN_FUZZ_CHECK(reader.Next("<a b='c'>d</a>", token) == 14 && token.kind == Kind::kNone &&
                      reader.Status() == reading.status && reader.ErrorOffset() == reading.errorOffset,
                  "a reader that had failed went on");
  }
  return reading;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bn::fuzz::Input input(data, size);
  const unsigned choices = input.Byte();
  const std::span<const std::uint8_t> rest = input.Rest();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes, as the characters they are.
  const std::string_view file(reinterpret_cast<const char*>(rest.data()), rest.size());

  const Reading whole = Read(file, SIZE_MAX, SIZE_MAX);
  const Reading cut = Read(file, kPieces[choices & 7U], kTaken[(choices >> 3U) & 3U]);
  BN_FUZZ_CHECK(cut.told == whole.told, "a file cut up was read differently from the same file whole");
  BN_FUZZ_CHECK(cut.places == whole.places, "a file cut up had its tags in other places than the same file whole");
  BN_FUZZ_CHECK(cut.status == whole.status && cut.errorOffset == whole.errorOffset &&
                    cut.unknownEntities == whole.unknownEntities,
                "a file cut up ended differently from the same file whole");
  return 0;
}
