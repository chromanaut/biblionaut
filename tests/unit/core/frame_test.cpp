#include "core/frame.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "stream_fakes.h"

namespace {

using bn::core::DecodeFrame;
using bn::core::FrameType;
using bn::core::FrameWriter;
using bn::core::MaxEncodedFrameSize;

// Encodes `payload` and returns the whole frame, delimiters included.
std::vector<std::uint8_t> Encode(FrameType type, std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> buffer(MaxEncodedFrameSize(payload.size()));
  FrameWriter writer(type, buffer);
  writer.Put(payload);
  const std::span<const std::uint8_t> frame = writer.Finish();
  return {frame.begin(), frame.end()};
}

// The part of a frame between its delimiters.
std::span<const std::uint8_t> Inner(const std::vector<std::uint8_t>& frame) {
  return std::span(frame).subspan(1, frame.size() - 2);
}

std::vector<std::uint8_t> Pattern(std::size_t size, bool withZeros) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    const auto value = static_cast<std::uint8_t>((i * 37 + 11) % 251);
    bytes[i] = (withZeros && i % 5 == 0) ? std::uint8_t{0} : static_cast<std::uint8_t>(value + 1);
  }
  return bytes;
}

TEST(Frame, IsDelimitedByZerosAndContainsNoOthers) {
  const std::array<std::uint8_t, 6> payload{0, 1, 0, 0, 2, 0};
  const std::vector<std::uint8_t> frame = Encode(FrameType::kTrace, payload);
  ASSERT_GE(frame.size(), 2u);
  EXPECT_EQ(frame.front(), 0);
  EXPECT_EQ(frame.back(), 0);
  const auto inner = Inner(frame);
  EXPECT_EQ(std::count(inner.begin(), inner.end(), 0), 0);
}

TEST(Frame, RoundTripsPayloadsOfManySizes) {
  for (const bool withZeros : {false, true}) {
    for (const std::size_t size :
         {0u, 1u, 2u, 15u, 248u, 249u, 250u, 253u, 254u, 255u, 256u, 507u, 508u, 509u, 1000u}) {
      const std::vector<std::uint8_t> payload = Pattern(size, withZeros);
      const std::vector<std::uint8_t> frame = Encode(FrameType::kText, payload);
      ASSERT_FALSE(frame.empty()) << "size " << size;
      EXPECT_LE(frame.size(), MaxEncodedFrameSize(size)) << "size " << size;

      std::vector<std::uint8_t> scratch(frame.size());
      const auto decoded = DecodeFrame(Inner(frame), scratch);
      ASSERT_TRUE(decoded.has_value()) << "size " << size << " zeros " << withZeros;
      EXPECT_EQ(decoded->type, FrameType::kText);
      EXPECT_TRUE(std::equal(decoded->payload.begin(), decoded->payload.end(), payload.begin(), payload.end()))
          << "size " << size << " zeros " << withZeros;
    }
  }
}

TEST(Frame, PiecesAndIntegersEncodeTheSameAsOneBlock) {
  std::array<std::uint8_t, 32> pieces{};
  FrameWriter writer(FrameType::kTrace, pieces);
  writer.PutU32(0x04030201);
  writer.PutU16(0x0605);
  writer.Put(std::uint8_t{7});
  const auto fromPieces = writer.Finish();

  const std::array<std::uint8_t, 7> whole{1, 2, 3, 4, 5, 6, 7};
  const std::vector<std::uint8_t> fromWhole = Encode(FrameType::kTrace, whole);
  EXPECT_TRUE(std::equal(fromPieces.begin(), fromPieces.end(), fromWhole.begin(), fromWhole.end()));
}

TEST(Frame, ABufferThatIsTooSmallYieldsNoFrame) {
  const std::vector<std::uint8_t> payload = Pattern(40, true);
  const std::size_t needed = Encode(FrameType::kText, payload).size();

  std::vector<std::uint8_t> exact(needed);
  FrameWriter fits(FrameType::kText, exact);
  fits.Put(payload);
  EXPECT_EQ(fits.Finish().size(), needed);

  for (const std::size_t size : {std::size_t{0}, std::size_t{1}, needed / 2, needed - 1}) {
    std::vector<std::uint8_t> small(size);
    FrameWriter writer(FrameType::kText, small);
    writer.Put(payload);
    EXPECT_TRUE(writer.Finish().empty()) << "buffer of " << size;
  }
}

TEST(Frame, RejectsAnyCorruptedByte) {
  const std::vector<std::uint8_t> payload = Pattern(20, true);
  const std::vector<std::uint8_t> frame = Encode(FrameType::kMetrics, payload);
  const auto inner = Inner(frame);
  std::vector<std::uint8_t> scratch(inner.size());

  for (std::size_t i = 0; i < inner.size(); ++i) {
    std::vector<std::uint8_t> damaged(inner.begin(), inner.end());
    damaged[i] = static_cast<std::uint8_t>(damaged[i] ^ 0x10U);
    EXPECT_FALSE(DecodeFrame(damaged, scratch).has_value()) << "flipped byte " << i;
  }
}

TEST(Frame, RejectsTruncatedFrames) {
  const std::vector<std::uint8_t> frame = Encode(FrameType::kText, Pattern(20, true));
  const auto inner = Inner(frame);
  std::vector<std::uint8_t> scratch(inner.size());
  for (std::size_t size = 0; size < inner.size(); ++size) {
    EXPECT_FALSE(DecodeFrame(inner.first(size), scratch).has_value()) << "cut to " << size;
  }
}

TEST(Frame, RejectsWhenScratchIsTooSmall) {
  const std::vector<std::uint8_t> frame = Encode(FrameType::kText, Pattern(8, false));
  const auto inner = Inner(frame);
  std::vector<std::uint8_t> scratch(inner.size() - 1);
  EXPECT_FALSE(DecodeFrame(inner, scratch).has_value());
}

TEST(Frame, PlainTextIsNotMistakenForAFrame) {
  const std::string_view lines[] = {"", "I (312) main_task: Calling app_main()", "Biblionaut 0.1.0 (x4)", "\x05hello"};
  for (const std::string_view line : lines) {
    const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(line.data()), line.size()};
    std::vector<std::uint8_t> scratch(bytes.size());
    EXPECT_FALSE(DecodeFrame(bytes, scratch).has_value()) << line;
  }
}

TEST(Frame, FramesAndTextCanBeSeparatedOnOneLine) {
  std::vector<std::uint8_t> line;
  const auto append = [&line](std::span<const std::uint8_t> bytes) {
    line.insert(line.end(), bytes.begin(), bytes.end());
  };
  const std::string_view boot = "boot message\r\n";
  const std::string_view crash = "Guru Meditation";

  append({reinterpret_cast<const std::uint8_t*>(boot.data()), boot.size()});
  append(Encode(FrameType::kText, Pattern(3, false)));
  append(Encode(FrameType::kTrace, Pattern(40, true)));
  append({reinterpret_cast<const std::uint8_t*>(crash.data()), crash.size()});
  append(Encode(FrameType::kMetrics, {}));

  const std::vector<bn::test::StreamItem> items = bn::test::ParseStream(line);
  ASSERT_EQ(items.size(), 5u);
  EXPECT_FALSE(items[0].isFrame);
  EXPECT_EQ(items[0].Text(), boot);
  EXPECT_TRUE(items[1].isFrame);
  EXPECT_EQ(items[1].type, FrameType::kText);
  EXPECT_TRUE(items[2].isFrame);
  EXPECT_EQ(items[2].type, FrameType::kTrace);
  EXPECT_EQ(items[2].payload, Pattern(40, true));
  EXPECT_FALSE(items[3].isFrame);
  EXPECT_EQ(items[3].Text(), crash);
  EXPECT_TRUE(items[4].isFrame);
  EXPECT_EQ(items[4].type, FrameType::kMetrics);
  EXPECT_TRUE(items[4].payload.empty());
}

}  // namespace
