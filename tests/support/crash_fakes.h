#pragma once

#include <cstddef>
#include <cstdint>

#include "core/crash.h"

namespace bn::test {

// A report with every field in use, each value different from its
// neighbours, so a mix-up in encoding shows.
inline core::CrashReport SampleCrashReport() {
  core::CrashReport report{};
  report.timeUs = 0x0000'0002'8000'1234ULL;  // Needs more than 32 bits.
  report.bootsSince = 3;
  report.stackAddress = 0x3FC9F000;
  report.arch = core::CrashArch::kEsp32C3;
  core::SetCrashText(report.reason, "Load access fault");
  core::SetCrashText(report.task, "main");
  core::SetCrashText(report.version, "0.1.0-dev+abc1234");
  core::SetCrashText(report.buildId, "0123456789");

  report.registerCount = core::kCrashMaxRegisters;
  for (std::size_t i = 0; i < report.registers.size(); ++i) {
    report.registers[i] = 0x42000000U + static_cast<std::uint32_t>(i * 0x101);
  }
  report.stackSize = core::kCrashMaxStackBytes;
  for (std::size_t i = 0; i < report.stack.size(); ++i) {
    report.stack[i] = static_cast<std::uint8_t>((i * 7) + 1);
  }
  report.traceCount = core::kCrashMaxTraceRecords;
  for (std::size_t i = 0; i < report.trace.size(); ++i) {
    report.trace[i] = core::TraceRecord{.startUs = static_cast<std::uint32_t>(1000 * i),
                                        .durationUs = static_cast<std::uint32_t>(i),
                                        .arg = static_cast<std::uint32_t>(i * 3),
                                        .id = core::MakeSpanId(core::TracePart::kReader, static_cast<std::uint8_t>(i)),
                                        .task = static_cast<std::uint8_t>(i % 3),
                                        .kind = (i % 2) == 0 ? core::TraceKind::kSpan : core::TraceKind::kInstant};
  }
  return report;
}

// Holds whatever report the test gives it.
class FakeCrashStore final : public core::CrashStore {
 public:
  const core::CrashReport* Saved() noexcept override { return hasReport ? &report : nullptr; }
  void SetVersion(const char* text) noexcept override { version = text; }

  core::CrashReport report{};
  bool hasReport = false;
  const char* version = nullptr;
};

}  // namespace bn::test
