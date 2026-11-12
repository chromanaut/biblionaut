#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/block_file.h"
#include "core/file_replacement.h"
#include "core/storage.h"

namespace bn::reader {

// Puts storage through its paces, to find out on a device whether the card
// can be relied on and how fast it is:
//
//   1. makes a directory of its own, /biblionaut-check;
//   2. writes a file of 256 KB there, a 4 KB block at a time, and syncs it;
//   3. reads it back from end to end, checking every byte;
//   4. reads sixteen short pieces from places all over it, checking those;
//   5. replaces a small file twice by the safe way (core/file_replacement.h)
//      and checks that it holds the second contents;
//   6. removes everything it made.
//
// It does one small piece each Tick(), so that the application carries on
// around it: the panel refreshes and the buttons work while the card is
// busy, which is itself part of what is being checked, since the card and
// the panel share their wires. Because of that, how long the whole check
// takes says little; the time each read and write of the card took is in
// the trace, recorded by the hardware layer.
//
// Used by one task. It is about 9 KB, most of it buffers.
class StorageCheck {
 public:
  enum class State : std::uint8_t {
    kIdle,  // Never run.
    kRunning,
    kPassed,
    kFailed,
  };

  // What it is doing, or was doing when it failed. The numbers are what
  // the trace shows.
  enum class Step : std::uint8_t {
    kNone = 0,
    kMakeDirectory = 1,
    kCreateFile = 2,
    kWrite = 3,
    kSync = 4,
    kReopen = 5,
    kRead = 6,
    kReadScattered = 7,
    kReplace = 8,
    kReplaceAgain = 9,
    kReadReplaced = 10,
    kCleanUp = 11,
  };

  static constexpr std::size_t kBlockBytes = 4096;
  static constexpr std::size_t kBlocks = 64;
  static constexpr std::size_t kScatteredReads = 16;
  static constexpr std::size_t kScatteredBytes = 512;

  explicit StorageCheck(core::Storage& storage) noexcept;

  StorageCheck(const StorageCheck&) = delete;
  StorageCheck& operator=(const StorageCheck&) = delete;
  ~StorageCheck() = default;

  // Starts a run from the beginning, unless one is under way.
  void Begin() noexcept;

  // Does the next piece of a run, if one is under way.
  void Tick() noexcept;

  State CurrentState() const noexcept { return state_; }
  // After a failure: the step that failed, and what storage said, which is
  // kOk if storage did as it was asked but the bytes were wrong.
  Step FailedStep() const noexcept { return failedStep_; }
  core::StorageStatus FailedStatus() const noexcept { return failedStatus_; }

 private:
  void WriteBlock() noexcept;
  void ReadBlock() noexcept;
  void ReadScattered() noexcept;
  void Replace(std::uint8_t fill, std::size_t length) noexcept;
  void ReadReplaced() noexcept;
  void CleanUp() noexcept;
  // Moves on to `step`, starting its count again.
  void MoveTo(Step step) noexcept;
  void Finish(bool passed) noexcept;
  void Fail(core::StorageStatus status) noexcept;

  core::Storage& storage_;
  State state_ = State::kIdle;
  Step step_ = Step::kNone;
  std::size_t count_ = 0;  // Blocks or pieces done within the step.
  std::uint32_t beganTrace_ = 0;
  Step failedStep_ = Step::kNone;
  core::StorageStatus failedStatus_ = core::StorageStatus::kOk;

  // Aligned, and a multiple of four long, so that a device's card driver
  // takes them as they are.
  alignas(4) std::array<std::uint8_t, kBlockBytes> fileBuffer_{};
  alignas(4) std::array<std::uint8_t, kBlockBytes> data_{};
  alignas(4) std::array<std::uint8_t, 512> replacementBuffer_{};
  core::BlockFile file_;
  core::FileReplacement replacement_;
};

}  // namespace bn::reader
