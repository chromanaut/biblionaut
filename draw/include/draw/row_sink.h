#pragma once

#include <cstdint>
#include <span>

namespace bn::draw {

// Is given a picture's rows of gray, top first, one at a time. The pieces
// that take a picture from a book to the panel hand rows on to one another
// through this: a reader of a picture gives its rows to the scaler, which
// gives the rows of the picture at the size it is shown at to whatever
// comes next.
class RowSink {
 public:
  // Row `y` of the picture, counted from 0 at the top. `gray` is its
  // pixels from the left, a byte each, from black (0) to white (255): as
  // many as the picture is wide. They are the giver's, and stay as they are
  // only until it is next asked to go on.
  virtual void Row(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept = 0;

 protected:
  ~RowSink() = default;  // Never destroyed through the interface.
};

}  // namespace bn::draw
