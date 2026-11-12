#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/trace.h"

// Crash reports: what a device keeps when it stops unexpectedly (a fault, an
// abort, running out of memory), so the cause can be found afterwards.
//
// The hardware port fills in a CrashReport at the moment of the crash and
// keeps it in memory that survives the restart. On the next start the
// application finds it through CrashStore and sends it off the device, where
// the addresses in it are turned back into source lines.

namespace bn::core {

// Which processor a report came from. It decides the names and order of the
// registers and which addresses are code.
enum class CrashArch : std::uint8_t {
  kUnknown = 0,
  // RISC-V. Registers: mepc, ra, sp, gp, tp, t0-t2, s0, s1, a0-a7, s2-s11,
  // t3-t6, mstatus, mtvec, mcause, mtval.
  kEsp32C3 = 1,
};

inline constexpr std::size_t kCrashMaxRegisters = 36;
inline constexpr std::size_t kCrashMaxStackBytes = 1024;
inline constexpr std::size_t kCrashMaxTraceRecords = 32;

// Everything saved about one crash. Plain data with no pointers, so it can
// sit in memory that outlives a restart. Text fields are always terminated;
// text that does not fit is cut.
struct CrashReport {
  std::uint64_t timeUs;        // The clock when it crashed: time since that boot.
  std::uint32_t bootsSince;    // Restarts since the crash: 1 on the first start after it.
  std::uint32_t stackAddress;  // Address of stack[0]: the stack pointer at the crash.
  std::uint16_t stackSize;     // Bytes of `stack` in use.
  CrashArch arch;
  std::uint8_t registerCount;  // Entries of `registers` in use.
  std::uint8_t traceCount;     // Entries of `trace` in use.

  std::array<char, 96> reason;   // "Load access fault", "abort() was called at PC 0x42001234".
  std::array<char, 16> task;     // Name of the task that was running.
  std::array<char, 40> version;  // The firmware that crashed, as in its hello.
  std::array<char, 20> buildId;  // Start of the SHA-256 of that firmware's ELF file, as hex.

  std::array<std::uint32_t, kCrashMaxRegisters> registers;  // In the order `arch` gives.
  std::array<std::uint8_t, kCrashMaxStackBytes> stack;      // The stack, upwards from the stack pointer.
  std::array<TraceRecord, kCrashMaxTraceRecords> trace;     // The last records before the crash, oldest first.
};

// Copies `text` into a text field of a report, cutting it to fit, and
// terminates it. Does nothing the code handling a crash cannot do.
void SetCrashText(std::span<char> field, std::string_view text) noexcept;

// A text field's content.
std::string_view CrashText(std::span<const char> field) noexcept;

// A report as bytes, for sending and storing:
//
//   u8   format (kCrashFormat)
//   u8   arch
//   u8   register count
//   u8   trace record count
//   u16  stack size
//   u32  boots since the crash
//   u32  time, low half; u32 time, high half
//   u32  stack address
//   reason, task, version, build id: each a length byte, then that many characters
//   registers, u32 each
//   stack bytes
//   trace records, 16 bytes each, as in a trace frame
//   u32  CRC-32 of everything before it
//
// Integers are little-endian.
inline constexpr std::uint8_t kCrashFormat = 1;

// Size of the largest encoded report.
inline constexpr std::size_t kMaxEncodedCrashReportSize = 22 + (1 + 95) + (1 + 15) + (1 + 39) + (1 + 19) +
                                                          (kCrashMaxRegisters * 4) + kCrashMaxStackBytes +
                                                          (kCrashMaxTraceRecords * 16) + 4;

// Size of `report` once encoded.
std::size_t EncodedCrashReportSize(const CrashReport& report) noexcept;

// Writes the encoded bytes of `report` from position `offset` on into `out`,
// as many as fit, and returns how many: 0 once `offset` is at the end. A
// report can therefore be sent in pieces without a buffer for all of it.
std::size_t EncodeCrashReport(const CrashReport& report, std::size_t offset, std::span<std::uint8_t> out) noexcept;

// Rebuilds a report from all of its encoded bytes. Returns false, leaving
// `report` in no particular state, if they are not a complete, undamaged
// report in a format this code knows.
[[nodiscard]] bool DecodeCrashReport(std::span<const std::uint8_t> bytes, CrashReport& report) noexcept;

// Where a crash report survives a restart. Implemented by each hardware
// port, which also does the saving, from inside its crash handler.
class CrashStore {
 public:
  // The report of the most recent crash, or null if there is none. It stays
  // available on every start until the device loses power or crashes again,
  // because the start right after a crash is often one nobody is watching.
  virtual const CrashReport* Saved() noexcept = 0;

  // Names this firmware in any report it goes on to leave. `version` must
  // stay valid from then on; a string literal.
  virtual void SetVersion(const char* version) noexcept = 0;

 protected:
  ~CrashStore() = default;  // Never destroyed through the interface.
};

}  // namespace bn::core
