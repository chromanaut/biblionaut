#pragma once

// The window check: a firmware that finds out, at start, whether the
// panel's controller takes windows narrower than whole rows, and what they
// cost. A picture shown on a panel turned a quarter, as the X4's is, has
// its rows running down the panel's columns, so eight of them are a window
// one byte wide and the panel's height. Written so, as they come, a picture
// needs no buffer of the whole panel.
//
//   tools/idf -DBN_WINDOW_CHECK=ON build flash    (back to normal: -DBN_WINDOW_CHECK=OFF)
//
// At start, before the application, it fills each plane with a pattern in
// several ways and reads the plane back, and says on the console what was
// found and how long each way took:
//
//   - whole rows, the way pictures are written now (the reference);
//   - windows one byte wide and the panel's height, one for each eight
//     columns;
//   - windows of eight rows the panel's width, one for each eight rows;
//   - a few windows of odd sizes and places over a plane already filled,
//     which must change those bytes and no others.
//
// A way that leaves a plane otherwise than it should be says how many bytes
// were wrong, and the first of them. Then the application starts as usual.
// In a normal build this does nothing and window_check.cpp is not
// compiled, so the entry point calls it unconditionally.

namespace bn::firmware {

#ifdef BN_WINDOW_CHECK

void RunWindowCheck() noexcept;

#else

inline void RunWindowCheck() noexcept {}

#endif

}  // namespace bn::firmware
