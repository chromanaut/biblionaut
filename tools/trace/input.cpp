#include "trace/input.h"

#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

namespace bn::trace_tool {

std::span<const std::string_view> SerialDevicePatterns() noexcept {
  static constexpr std::array<std::string_view, 2> kPatterns = {"/dev/cu.usbmodem*", "/dev/ttyACM*"};
  return kPatterns;
}

std::vector<std::string> FindDevices(std::span<const std::string_view> patterns) {
  std::vector<std::string> found;
  for (const std::string_view pattern : patterns) {
    glob_t matches{};
    if (glob(std::string(pattern).c_str(), 0, nullptr, &matches) == 0) {
      for (std::size_t i = 0; i < matches.gl_pathc; ++i) {
        found.emplace_back(matches.gl_pathv[i]);
      }
    }
    globfree(&matches);
  }
  std::sort(found.begin(), found.end());
  return found;
}

std::string ChooseDevice(const std::vector<std::string>& candidates, std::string& error) {
  if (candidates.size() == 1) {
    return candidates.front();
  }
  if (candidates.empty()) {
    error = "no USB serial device found; plug the device in, or name a capture file or port";
    return {};
  }
  error = "more than one USB serial device found; name the one to read:";
  for (const std::string& candidate : candidates) {
    error += "\n  " + candidate;
  }
  return {};
}

Input::~Input() {
  if (ownsFd_ && fd_ >= 0) {
    close(fd_);
  }
}

bool Input::Open(const std::string& path, std::string& error) {
  if (path == "-") {
    fd_ = STDIN_FILENO;
    ownsFd_ = false;
  } else {
    // O_NONBLOCK so opening a serial device does not wait for a carrier
    // signal; O_NOCTTY so it does not become this program's terminal.
    fd_ = open(path.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
      error = "cannot open " + path + ": " + std::strerror(errno);
      return false;
    }
    ownsFd_ = true;
  }

  live_ = isatty(fd_) != 0;
  if (live_ && ownsFd_) {
    // Raw mode: no line editing, no translation of carriage returns, no
    // special meaning for any byte. Without it the terminal layer would
    // alter the binary frames.
    termios settings{};
    if (tcgetattr(fd_, &settings) != 0) {
      error = "cannot read the settings of " + path + ": " + std::strerror(errno);
      return false;
    }
    cfmakeraw(&settings);
    settings.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    if (tcsetattr(fd_, TCSANOW, &settings) != 0) {
      error = "cannot switch " + path + " to raw mode: " + std::strerror(errno);
      return false;
    }
  }
  return true;
}

// Not const: it changes the state of the device, though no field of this object.
// NOLINTNEXTLINE(readability-make-member-function-const)
bool Input::ResetDevice() {
  if (!live_ || !ownsFd_) {
    return false;
  }
  // On an ESP32 the serial adapter's RTS signal is wired to the chip's reset
  // and DTR to its boot-mode pin. Hold DTR off so the chip starts the
  // application, not its download mode, then pulse RTS.
  int dtr = TIOCM_DTR;
  int rts = TIOCM_RTS;
  if (ioctl(fd_, TIOCMBIC, &dtr) != 0 || ioctl(fd_, TIOCMBIS, &rts) != 0) {
    return false;
  }
  usleep(100'000);
  return ioctl(fd_, TIOCMBIC, &rts) == 0;
}

Input::Result Input::Read(std::span<std::uint8_t> buffer, int timeoutMs, std::size_t& count) {
  count = 0;
  pollfd descriptor{.fd = fd_, .events = POLLIN, .revents = 0};
  const int ready = poll(&descriptor, 1, timeoutMs);
  if (ready < 0) {
    return errno == EINTR ? Result::kTimeout : Result::kEnd;
  }
  if (ready == 0) {
    return Result::kTimeout;
  }

  const ssize_t got = read(fd_, buffer.data(), buffer.size());
  if (got > 0) {
    count = static_cast<std::size_t>(got);
    return Result::kData;
  }
  if (got < 0 && (errno == EAGAIN || errno == EINTR)) {
    return Result::kTimeout;
  }
  // Zero bytes from a file or pipe is the end. A device that has been
  // unplugged reads as zero bytes or as an error.
  return Result::kEnd;
}

}  // namespace bn::trace_tool
