#pragma once

#include "reader/app.h"

// The crash test: a firmware built to crash on purpose, for checking crash
// reports on a device. It is chosen when configuring the build,
//
//   tools/idf -DBN_CRASH_TEST=fault build flash     (or abort, oom, stack)
//
// and crashes in the way named ten seconds after starting. It does not crash
// again on the start that follows a crash, so the report can be read and the
// firmware cannot loop.
//
// In a normal build these functions do nothing and crash_test.cpp is not
// compiled, so the entry point calls them unconditionally.

namespace bn::firmware {

#ifdef BN_CRASH_TEST

// Says on the console what this run is going to do. Call once, at start.
void AnnounceCrashTest() noexcept;

// Crashes once `app` has been up for ten seconds, if this run is one that
// should. Call from the main loop.
void RunCrashTest(const reader::App& app) noexcept;

#else

inline void AnnounceCrashTest() noexcept {}
inline void RunCrashTest(const reader::App& /*app*/) noexcept {}

#endif

}  // namespace bn::firmware
