#pragma once

namespace bn::reader {

// Something a build can be told to do to the device it runs on, to find
// out how the device behaves: the storage check and the book check are
// two. It is begun when the application starts, and again at each press of
// the Power button, and it does a little of its work each time the
// application gives it a turn.
//
// The application starts, turns and destroys its checks through this and
// nothing else, so that a firmware which was not told to run a check does
// not carry the check's code.
class DeviceCheck {
 public:
  DeviceCheck() noexcept = default;
  DeviceCheck(const DeviceCheck&) = delete;
  DeviceCheck& operator=(const DeviceCheck&) = delete;
  virtual ~DeviceCheck() = default;

  // Starts a run from the beginning, unless one is under way.
  virtual void Begin() noexcept = 0;

  // Does the next piece of a run, if one is under way.
  virtual void Tick() noexcept = 0;
};

}  // namespace bn::reader
