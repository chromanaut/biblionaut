#include "core/crash.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/checksum.h"
#include "core/endian.h"
#include "crash_fakes.h"

namespace {

using bn::core::CrashReport;
using bn::core::CrashText;
using bn::core::DecodeCrashReport;
using bn::core::EncodeCrashReport;
using bn::core::EncodedCrashReportSize;
using bn::core::SetCrashText;
using bn::test::SampleCrashReport;

std::vector<std::uint8_t> EncodeAll(const CrashReport& report) {
  std::vector<std::uint8_t> bytes(EncodedCrashReportSize(report));
  EXPECT_EQ(EncodeCrashReport(report, 0, bytes), bytes.size());
  return bytes;
}

// Replaces the checksum at the end so that changed bytes still check out.
void FixChecksum(std::vector<std::uint8_t>& bytes) {
  const std::span<std::uint8_t> all(bytes);
  bn::core::StoreLe32(all.last<4>(), bn::core::Crc32(all.first(all.size() - 4)));
}

void ExpectSameReport(const CrashReport& a, const CrashReport& b) {
  EXPECT_EQ(a.timeUs, b.timeUs);
  EXPECT_EQ(a.bootsSince, b.bootsSince);
  EXPECT_EQ(a.stackAddress, b.stackAddress);
  EXPECT_EQ(a.stackSize, b.stackSize);
  EXPECT_EQ(a.arch, b.arch);
  EXPECT_EQ(a.registerCount, b.registerCount);
  EXPECT_EQ(a.traceCount, b.traceCount);
  EXPECT_EQ(CrashText(a.reason), CrashText(b.reason));
  EXPECT_EQ(CrashText(a.task), CrashText(b.task));
  EXPECT_EQ(CrashText(a.version), CrashText(b.version));
  EXPECT_EQ(CrashText(a.buildId), CrashText(b.buildId));
  EXPECT_EQ(a.registers, b.registers);
  EXPECT_EQ(a.stack, b.stack);
  for (std::size_t i = 0; i < a.trace.size(); ++i) {
    EXPECT_EQ(a.trace[i].startUs, b.trace[i].startUs) << i;
    EXPECT_EQ(a.trace[i].durationUs, b.trace[i].durationUs) << i;
    EXPECT_EQ(a.trace[i].arg, b.trace[i].arg) << i;
    EXPECT_EQ(a.trace[i].id, b.trace[i].id) << i;
    EXPECT_EQ(a.trace[i].task, b.trace[i].task) << i;
    EXPECT_EQ(a.trace[i].kind, b.trace[i].kind) << i;
  }
}

TEST(CrashReport, SurvivesEncodingAndDecoding) {
  const CrashReport report = SampleCrashReport();
  const std::vector<std::uint8_t> bytes = EncodeAll(report);
  EXPECT_EQ(bytes.size(), bn::core::kMaxEncodedCrashReportSize - (95 - 17) - (15 - 4) - (39 - 17) - (19 - 10));
  EXPECT_LE(bytes.size(), bn::core::kMaxEncodedCrashReportSize);

  CrashReport decoded{};
  ASSERT_TRUE(DecodeCrashReport(bytes, decoded));
  ExpectSameReport(report, decoded);
}

TEST(CrashReport, EncodesOnlyThePartsInUse) {
  CrashReport report{};
  report.arch = bn::core::CrashArch::kEsp32C3;
  report.bootsSince = 1;
  SetCrashText(report.reason, "abort");
  report.registerCount = 2;
  report.registers[0] = 0x42001234;
  report.registers[1] = 0x42005678;
  report.registers[2] = 0xDEADBEEF;  // Beyond the count: not part of the report.
  report.stackSize = 8;
  report.stack[8] = 0xEE;  // Likewise.

  const std::vector<std::uint8_t> bytes = EncodeAll(report);
  EXPECT_EQ(bytes.size(), 22u + (1 + 5) + 1 + 1 + 1 + 8 + 8 + 4);

  CrashReport decoded{};
  ASSERT_TRUE(DecodeCrashReport(bytes, decoded));
  EXPECT_EQ(decoded.registers[1], 0x42005678u);
  EXPECT_EQ(decoded.registers[2], 0u);
  EXPECT_EQ(decoded.stack[8], 0u);
  EXPECT_EQ(decoded.traceCount, 0u);
  EXPECT_EQ(CrashText(decoded.reason), "abort");
  EXPECT_EQ(CrashText(decoded.task), "");
}

TEST(CrashReport, CanBeEncodedInPiecesOfAnySize) {
  const CrashReport report = SampleCrashReport();
  const std::vector<std::uint8_t> whole = EncodeAll(report);

  for (const std::size_t pieceSize : {std::size_t{1}, std::size_t{7}, std::size_t{160}, std::size_t{4096}}) {
    std::vector<std::uint8_t> joined;
    std::vector<std::uint8_t> piece(pieceSize);
    while (true) {
      const std::size_t count = EncodeCrashReport(report, joined.size(), piece);
      if (count == 0) {
        break;
      }
      ASSERT_LE(count, pieceSize);
      joined.insert(joined.end(), piece.begin(), piece.begin() + static_cast<std::ptrdiff_t>(count));
    }
    EXPECT_EQ(joined, whole) << "piece size " << pieceSize;
  }
  // Past the end there is nothing.
  std::array<std::uint8_t, 8> extra{};
  EXPECT_EQ(EncodeCrashReport(report, whole.size(), extra), 0u);
  EXPECT_EQ(EncodeCrashReport(report, whole.size() + 100, extra), 0u);
}

TEST(CrashReport, CountsBeyondTheArraysAreLimitedWhenEncoding) {
  // What survived a restart is not trusted: a count larger than its array
  // must not make the encoder read past it.
  CrashReport report = SampleCrashReport();
  report.registerCount = 200;
  report.stackSize = 60'000;
  report.traceCount = 255;

  const std::vector<std::uint8_t> bytes = EncodeAll(report);
  EXPECT_LE(bytes.size(), bn::core::kMaxEncodedCrashReportSize);
  CrashReport decoded{};
  ASSERT_TRUE(DecodeCrashReport(bytes, decoded));
  EXPECT_EQ(decoded.registerCount, bn::core::kCrashMaxRegisters);
  EXPECT_EQ(decoded.stackSize, bn::core::kCrashMaxStackBytes);
  EXPECT_EQ(decoded.traceCount, bn::core::kCrashMaxTraceRecords);
}

TEST(CrashReport, RejectsDamagedOrIncompleteBytes) {
  const std::vector<std::uint8_t> good = EncodeAll(SampleCrashReport());
  CrashReport decoded{};

  EXPECT_FALSE(DecodeCrashReport({}, decoded));
  EXPECT_FALSE(DecodeCrashReport(std::span(good).first(3), decoded));
  EXPECT_FALSE(DecodeCrashReport(std::span(good).first(good.size() - 1), decoded));

  for (const std::size_t at : {std::size_t{0}, std::size_t{30}, good.size() / 2, good.size() - 1}) {
    std::vector<std::uint8_t> damaged = good;
    damaged[at] ^= 0x40U;
    EXPECT_FALSE(DecodeCrashReport(damaged, decoded)) << "byte " << at;
  }

  std::vector<std::uint8_t> longer = good;
  longer.push_back(0);
  EXPECT_FALSE(DecodeCrashReport(longer, decoded));
}

TEST(CrashReport, RejectsContentThatChecksOutButIsWrong) {
  const std::vector<std::uint8_t> good = EncodeAll(SampleCrashReport());
  CrashReport decoded{};

  std::vector<std::uint8_t> otherFormat = good;
  otherFormat[0] = bn::core::kCrashFormat + 1;
  FixChecksum(otherFormat);
  EXPECT_FALSE(DecodeCrashReport(otherFormat, decoded));

  std::vector<std::uint8_t> tooManyRegisters = good;
  tooManyRegisters[2] = bn::core::kCrashMaxRegisters + 1;
  FixChecksum(tooManyRegisters);
  EXPECT_FALSE(DecodeCrashReport(tooManyRegisters, decoded));

  // Claims more trace than the bytes hold.
  std::vector<std::uint8_t> shortOfTrace = good;
  shortOfTrace.erase(shortOfTrace.end() - 20, shortOfTrace.end() - 4);
  FixChecksum(shortOfTrace);
  EXPECT_FALSE(DecodeCrashReport(shortOfTrace, decoded));

  // A text longer than its field.
  std::vector<std::uint8_t> longTask = good;
  const std::size_t taskLengthAt = 22 + 1 + std::strlen("Load access fault");
  ASSERT_EQ(longTask[taskLengthAt], 4u);
  longTask[taskLengthAt] = 16;
  FixChecksum(longTask);
  EXPECT_FALSE(DecodeCrashReport(longTask, decoded));
}

TEST(CrashText, IsCutToFitAndAlwaysTerminated) {
  std::array<char, 8> field{};
  field.fill('x');
  SetCrashText(field, "abc");
  EXPECT_EQ(CrashText(field), "abc");
  EXPECT_EQ(field[7], '\0');  // The rest is cleared too.

  SetCrashText(field, "0123456789");
  EXPECT_EQ(CrashText(field), "0123456");
  EXPECT_EQ(field[7], '\0');

  SetCrashText(field, "");
  EXPECT_EQ(CrashText(field), "");

  // A field that lost its terminator still gives text that fits one.
  field.fill('y');
  EXPECT_EQ(CrashText(field), "yyyyyyy");

  SetCrashText(std::span<char>(), "ignored");
  EXPECT_EQ(CrashText(std::span<const char>()), "");
}

}  // namespace
