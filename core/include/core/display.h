#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// The e-ink panel, as the rest of the firmware sees it.
//
// The panel's controller holds two full images in its own memory, called
// planes here. There is no copy of the screen anywhere else: pictures are
// produced a band of rows at a time and written straight into a plane, and a
// refresh then moves the panel from one image to the other.
//
// Pixels are in the panel's own orientation, which need not be the way the
// device is held: turning a page into panel rows is the drawing code's job.

namespace bn::core {

// Which of the controller's two images.
enum class DisplayPlane : std::uint8_t {
  kNew = 0,  // What the next refresh will show.
  kOld = 1,  // What the panel shows now. A fast refresh compares against it.
};

// How the panel gets from the old image to the new one.
enum class RefreshMode : std::uint8_t {
  // Every pixel is driven through black and white to its new value. The
  // slowest, and it flashes, but it leaves no trace of earlier images. The
  // old-image plane is ignored.
  kFull = 0,
  // A shortened form of the full refresh: quicker, still flashes, still
  // ignores the old-image plane.
  kHalf = 1,
  // Only pixels that differ between the two planes are driven. Quick and
  // without a flash, but each one leaves a little ghosting behind. It is
  // only right if the old-image plane really holds what is on the panel.
  kFast = 2,
};

// What the panel does once a refresh is finished.
enum class AfterRefresh : std::uint8_t {
  kStayPowered = 0,  // Keep its drive voltages up: the next refresh starts sooner.
  kPowerOff = 1,     // Switch them off. The image stays; nothing more is drawn from the battery.
};

struct DisplaySize {
  std::uint16_t width;   // Pixels in a row.
  std::uint16_t height;  // Rows.

  // Bytes one row occupies: a bit per pixel.
  constexpr std::size_t RowBytes() const noexcept { return (static_cast<std::size_t>(width) + 7) / 8; }
};

// One panel. Implemented by each hardware port. Not for use from two tasks
// at once.
class Display {
 public:
  virtual DisplaySize Size() const noexcept = 0;

  // Wakes the panel and prepares it; both planes end up white. Call before
  // anything else, and again after Sleep(). A refresh still in progress is
  // abandoned: one may have been left running by the program's last run.
  // May take some tens of milliseconds. Returns false if the panel could
  // not be set up.
  [[nodiscard]] virtual bool Start() noexcept = 0;

  // Writes whole rows into a plane, starting at row `firstRow`. `rows` holds
  // one or more rows of Size().RowBytes() bytes each: the first byte is the
  // leftmost eight pixels, its top bit the leftmost of those, and a set bit
  // is white. Returns false, writing nothing, if the panel is not started
  // or is busy, or the rows do not fit.
  [[nodiscard]] virtual bool WriteRows(DisplayPlane plane, std::uint16_t firstRow,
                                       std::span<const std::uint8_t> rows) noexcept = 0;

  // Starts a refresh and returns at once; Busy() says when it is over.
  // Returns false, starting nothing, if the panel is not started or is busy.
  [[nodiscard]] virtual bool StartRefresh(RefreshMode mode, AfterRefresh after) noexcept = 0;

  // True while the panel is working on something and will take nothing new.
  virtual bool Busy() noexcept = 0;

  // Switches the panel's drive voltages off, if a refresh left them on, and
  // returns at once; Busy() says when it is done. The image stays, and so do
  // both planes, so the next refresh needs nothing rewritten. Returns false
  // if the panel is not started or is busy.
  [[nodiscard]] virtual bool PowerOff() noexcept = 0;

  // Puts the panel into its lowest-power state. The image stays on it, but
  // the planes are not kept: Start() begins again with both white. If the
  // drive voltages are still on, this switches them off first and waits,
  // which takes a fraction of a second. Returns false if the panel is busy.
  [[nodiscard]] virtual bool Sleep() noexcept = 0;

 protected:
  ~Display() = default;  // Never destroyed through the interface.
};

}  // namespace bn::core
