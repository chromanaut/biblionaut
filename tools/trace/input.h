#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bn::trace_tool {

// Where USB serial devices appear: /dev/cu.usbmodem* on macOS, /dev/ttyACM*
// on Linux.
std::span<const std::string_view> SerialDevicePatterns() noexcept;

// The paths matching any of `patterns` (shell wildcards), sorted.
std::vector<std::string> FindDevices(std::span<const std::string_view> patterns);

// Chooses the device to read when the user named none: the only candidate.
// Returns an empty string and sets `error` if there is none or more than one.
std::string ChooseDevice(const std::vector<std::string>& candidates, std::string& error);

// Where the stream's bytes come from: a capture file, standard input, or a
// serial device such as /dev/cu.usbmodem101. A serial device is switched to
// raw mode so that every byte arrives unchanged.
class Input {
 public:
  Input() = default;
  ~Input();

  Input(const Input&) = delete;
  Input& operator=(const Input&) = delete;

  // Opens `path`; "-" means standard input. Returns false and sets `error`
  // if it cannot.
  bool Open(const std::string& path, std::string& error);

  // True for a serial device or other terminal: a source that has no end and
  // is read until the user stops.
  bool IsLive() const noexcept { return live_; }

  // Restarts the device on the other end of a serial line by pulsing its
  // reset line. Returns false if the line's control signals cannot be
  // driven, in which case nothing was done. What the device had still to
  // send from before the restart arrives first; see BootFilter.
  bool ResetDevice();

  enum class Result : std::uint8_t {
    kData,     // `count` bytes were read.
    kTimeout,  // Nothing arrived within the time allowed.
    kEnd,      // The input has ended or failed.
  };

  // Waits up to `timeoutMs` for bytes and reads what is available.
  Result Read(std::span<std::uint8_t> buffer, int timeoutMs, std::size_t& count);

 private:
  int fd_ = -1;
  bool ownsFd_ = false;
  bool live_ = false;
};

}  // namespace bn::trace_tool
