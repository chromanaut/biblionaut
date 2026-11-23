#pragma once

// The gray check: a firmware that shows, at start, a picture in the
// panel's four levels through the gray pass (core::Display::
// StartGrayRefresh()), times it, and leaves each stage on the panel long
// enough to be looked at.
//
//   tools/idf -DBN_GRAY_CHECK=ON build flash    (back to normal: -DBN_GRAY_CHECK=OFF)
//
// The picture lies as the panel's rows do, so it is seen turned a quarter
// on the X4: a third of it four patches, black, dark gray, light gray and
// white; a third a ramp from black to white dithered to the four levels;
// and a third the same ramp dithered to black and white, to set beside it.
// The stages, each said on the console as it begins:
//
//   1. The picture in black and white, every gray black (a half refresh).
//   2. The gray pass over it: the four patches should be four levels, and
//      the upper ramp smoother than the lower. Held 30 seconds.
//   3. Another picture, large squares, in black and white (a half
//      refresh): nothing of the patches or the ramps should be left.
//   4. Stages 1 and 2 again, but with the drive voltages kept up from the
//      half refresh to the gray pass, as showing a page quickly would: it
//      should look as stage 2 did.
//   5. The application starts as usual, with a full refresh.
//
// What each write and refresh took is said too. In a normal build this
// does nothing and gray_check.cpp is not compiled, so the entry point calls
// it unconditionally.

namespace bn::firmware {

#ifdef BN_GRAY_CHECK

void RunGrayCheck() noexcept;

#else

inline void RunGrayCheck() noexcept {}

#endif

}  // namespace bn::firmware
