#include "core/byte_source.h"

#include <gtest/gtest.h>

#include <array>

namespace {

using bn::core::MemoryByteSource;

TEST(MemoryByteSource, ReadsInChunksThenReportsEnd) {
  const std::array<std::uint8_t, 5> data{1, 2, 3, 4, 5};
  MemoryByteSource source(data);
  std::array<std::uint8_t, 3> buffer{};

  EXPECT_EQ(source.Read(buffer), 3);
  EXPECT_EQ(buffer, (std::array<std::uint8_t, 3>{1, 2, 3}));
  EXPECT_EQ(source.Remaining(), 2u);

  EXPECT_EQ(source.Read(buffer), 2);
  EXPECT_EQ(buffer[0], 4);
  EXPECT_EQ(buffer[1], 5);

  EXPECT_EQ(source.Read(buffer), 0);
  EXPECT_EQ(source.Read(buffer), 0);
}

TEST(MemoryByteSource, EmptyDestinationReadsNothing) {
  const std::array<std::uint8_t, 2> data{7, 8};
  MemoryByteSource source(data);
  EXPECT_EQ(source.Read({}), 0);
  EXPECT_EQ(source.Remaining(), 2u);
}

TEST(MemoryByteSource, EmptySourceIsAtEnd) {
  MemoryByteSource source({});
  std::array<std::uint8_t, 4> buffer{};
  EXPECT_EQ(source.Read(buffer), 0);
}

}  // namespace
