#include "layout/line_digest.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "core/checksum.h"

namespace {

using bn::layout::LineDigest;
using bn::layout::TextLine;

TEST(LineDigest, TakesEachLineAsItsHeaderSays) {
  const TextLine line{.begin = 0x01020304,
                      .end = 0x05060708,
                      .width = 1760,
                      .number = 3,
                      .spaces = 0x0A0B,
                      .hyphen = true,
                      .last = false,
                      .overfull = true};
  LineDigest digest;
  digest.Add(line);

  const std::array<std::uint8_t, 19> bytes = {0x04, 0x03, 0x02, 0x01, 0x08, 0x07, 0x06, 0x05, 0xE0, 0x06,
                                              0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0B, 0x0A, 0x05};
  EXPECT_EQ(digest.Checksum(), bn::core::Crc32(bytes));
  EXPECT_EQ(digest.Lines(), 1U);
}

TEST(LineDigest, TellsLinesApartByAnythingAboutThem) {
  const TextLine base{.begin = 10, .end = 40, .width = 900, .number = 1, .spaces = 4};
  const auto checksumOf = [](const TextLine& line) {
    LineDigest digest;
    digest.Add(line);
    return digest.Checksum();
  };
  const std::uint32_t plain = checksumOf(base);
  TextLine changed = base;
  changed.end = 41;
  EXPECT_NE(checksumOf(changed), plain);
  changed = base;
  changed.number = 0;
  EXPECT_NE(checksumOf(changed), plain);
  changed = base;
  changed.last = true;
  EXPECT_NE(checksumOf(changed), plain);
  changed = base;
  changed.spaces = 5;
  EXPECT_NE(checksumOf(changed), plain);

  // And by their order.
  LineDigest forwards;
  forwards.Add(base);
  forwards.Add(changed);
  LineDigest backwards;
  backwards.Add(changed);
  backwards.Add(base);
  EXPECT_NE(forwards.Checksum(), backwards.Checksum());
  EXPECT_EQ(forwards.Lines(), 2U);
}

}  // namespace
