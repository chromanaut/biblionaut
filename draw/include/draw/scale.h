#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "draw/divide.h"
#include "draw/row_sink.h"

// Making a picture larger or smaller, a row at a time.
//
// The picture's rows come in through Row(), top first, as a reader of a
// picture gives them (it is a RowSink), and the rows of the picture at its
// new size go out to another RowSink, one at each call of Step(). Nothing
// of the picture is kept but what the rows still to come need: two rows of
// the new width at the most.
//
// Each direction is taken by itself, so a picture can be made narrower and
// taller at once:
//
//   - Smaller: each new pixel is the average of the old pixels it covers,
//     each counted by how much of it is covered. No old pixel is left out,
//     so a line one pixel wide in a large picture is still there in a small
//     one, lighter for being spread.
//   - Larger: each new pixel is a blend of the two old pixels nearest its
//     middle, by how near each is (bilinear). Past the middle of the first
//     or the last old pixel, that pixel is used as it is.
//   - The same size: the old pixels as they are.
//
// The grays are averaged as they are stored, not as the light they stand
// for. A picture's grays are already spaced so that their steps look about
// even, and averaging them as they are keeps fine dark lines darker than
// averaging light would.
//
// It works in whole numbers, and rounds three times, each to the nearest
// with a half going up: a blend's weight, to 4096ths of the way from one old
// pixel to the next; a pixel of a row made the new width, to 256ths of a
// gray; and a new pixel, once it has been made the new height too, to a
// gray. So a new pixel is within a gray of what the same working-out in
// real numbers comes to. No pixel is divided as such: each number that
// every pixel of a row is divided by is made a multiplier once
// (draw/divide.h), which gives the same quotient.
//
// Memory is the caller's: two rows of the new width at four bytes a pixel,
// and one at a byte (SumsFor(), the width itself).
//
// One call's work. Row() goes over the old row once and the new width once,
// and gives nothing out. Step() makes one new row and gives it out. When a
// picture is made taller one old row can be needed by several new ones,
// and so all the new rows an old one has made ready have to be given out
// before the next old row is taken: a row given before then is refused,
// and the scaling stops (kOutOfTurn).
//
// Used by one task.

namespace bn::draw {

// How scaling a picture is going.
enum class ScaleStatus : std::uint8_t {
  kOk = 0,     // Well so far, or to the end.
  kNotBegun,   // Begin() has not been called, or did not go well.
  kBadSize,    // A side of nothing, or of more than kMaxSide.
  kNoRoom,     // The memory given is too little for the new width.
  kOutOfTurn,  // A row given that is not the next, not as wide as the picture, past its last, or given while new
               // rows were still waiting to be given out. Nothing more is taken.
};

// The name of a status, for diagnostics: "ok", "not begun", ...
const char* ScaleStatusName(ScaleStatus status) noexcept;

class Scaler final : public RowSink {
 public:
  // The longest side, old or new, a picture may have. The sums of a new
  // pixel stay inside 32 bits up to this.
  static constexpr std::uint32_t kMaxSide = 65535;

  // How many 32-bit words of sums a new width needs.
  static constexpr std::size_t SumsFor(std::uint32_t newWidth) noexcept { return std::size_t{newWidth} * 2U; }

  // `sums` and `row` are the scaler's memory: SumsFor() words and a byte for
  // each pixel of the new width. Both are the caller's, and have to last as
  // long as the scaler is used.
  Scaler(std::span<std::uint32_t> sums, std::span<std::uint8_t> row) noexcept;

  Scaler(const Scaler&) = delete;
  Scaler& operator=(const Scaler&) = delete;
  ~Scaler() = default;

  // Gets ready to make a picture of `oldWidth` by `oldHeight` pixels into
  // one of `newWidth` by `newHeight`, whose rows go to `out`, which has to
  // last until the picture is done. kOk, or why not.
  ScaleStatus Begin(std::uint32_t oldWidth, std::uint32_t oldHeight, std::uint32_t newWidth, std::uint32_t newHeight,
                    RowSink& out) noexcept;

  // Takes the old picture's row `y`: the next one, as wide as the picture.
  void Row(std::uint32_t y, std::span<const std::uint8_t> gray) noexcept override;

  // Gives out the next new row, if one is ready. Returns whether it gave
  // one: call it until it says no before giving the next old row.
  [[nodiscard]] bool Step() noexcept;

  ScaleStatus Status() const noexcept { return status_; }
  // Whether a new row is ready to be given out.
  bool Ready() const noexcept;
  // Whether every new row has been given out.
  bool Finished() const noexcept { return status_ == ScaleStatus::kOk && given_ == newHeight_; }
  // How many old rows have been taken, and new rows given out.
  std::uint32_t RowsTaken() const noexcept { return taken_; }
  std::uint32_t RowsGiven() const noexcept { return given_; }

 private:
  // Where a new pixel or row lies over the old ones, when a picture is made
  // larger: the old one before its middle, and how far from that one to
  // the next it is, in 4096ths. Moved on one new pixel at a time, exactly.
  class Blend {
   public:
    void Begin(std::uint32_t oldSize, std::uint32_t newSize) noexcept;
    void Next() noexcept;
    // The old one before the new one's middle; -1 if the middle is before
    // the first old one's.
    std::int32_t Before() const noexcept { return before_; }
    // How far towards the next one, from 0 to 4096, rounded.
    std::uint32_t Weight() const noexcept;

   private:
    std::int32_t before_ = 0;
    std::uint32_t remainder_ = 0;  // Over twice the new size.
    std::uint32_t oldSize_ = 0;
    std::uint32_t newSize_ = 0;
    Divider twiceNew_;
  };

  // The old row made the new width: each new pixel, in 256ths of a gray,
  // given to `put` with its place, from the left.
  template <typename Put>
  void Across(std::span<const std::uint8_t> gray, Put put) const noexcept;

  enum class Vertical : std::uint8_t { kSame, kSmaller, kLarger };

  void Fail(ScaleStatus status) noexcept;

  std::span<std::uint32_t> sums_;
  std::span<std::uint8_t> row_;
  RowSink* out_ = nullptr;
  ScaleStatus status_ = ScaleStatus::kNotBegun;

  std::uint32_t oldWidth_ = 0;
  std::uint32_t oldHeight_ = 0;
  std::uint32_t newWidth_ = 0;
  std::uint32_t newHeight_ = 0;
  Vertical vertical_ = Vertical::kSame;

  std::uint32_t taken_ = 0;  // Old rows taken,
  std::uint32_t given_ = 0;  // and new rows given.

  // Across: made larger, the walk over the old pixels as it begins each
  // row; made smaller, a division by the old width.
  Blend across_;
  Divider byOldWidth_;

  // Down, made smaller: which of the two rows of sums is the new row being
  // made, how many of its units it has had, and whether the other is a
  // whole new row waiting to be given out.
  std::uint32_t current_ = 0;
  std::uint32_t filled_ = 0;
  bool waiting_ = false;
  Divider byUnits_;  // By the units of a new row, in 256ths of a gray.

  // Down, made larger: the new rows' walk over the old ones. The two rows of
  // sums hold the last two old rows taken, an even row in the first and an
  // odd one in the second.
  Blend down_;
};

}  // namespace bn::draw
