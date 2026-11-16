#pragma once

#include "core/power.h"

namespace bn::test {

// A device's power that a test says can sleep or not, and that counts the
// times it was put to sleep. It cannot sleep unless the test says so.
class FakePower final : public core::Power {
 public:
  bool CanSleep() noexcept override { return canSleep; }
  void Sleep() noexcept override {
    if (canSleep) {
      ++sleeps;
    }
  }

  bool canSleep = false;
  int sleeps = 0;
};

}  // namespace bn::test
