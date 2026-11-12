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
//
// The two planes are roles, "new image" and "old image", and the controller
// moves the roles between its two memories by itself. This is how the X4's
// panel was found to behave, by reading its planes back; a port to a panel
// that does otherwise has to make it look like this.
//
//  - A fast refresh swaps the roles. The plane that held the new image is
//    now the old-image plane, and holds what the panel shows. The new-image
//    plane holds the image before that, ready to be written over.
//  - Switching the drive voltages off puts the roles back as they were
//    when the voltages came on. After an odd number of fast refreshes that
//    undoes the last swap: the old-image plane is then one image behind the
//    panel, and anything written to the new-image plane for later is in the
//    wrong one.
//  - Nothing can be written while the panel is busy.
//
// So, for whatever drives the panel:
//
//  - With the voltages on, the old-image plane needs nothing after a fast
//    refresh. Write the next image and refresh again.
//  - After the voltages have been switched off, write the old-image plane
//    with what the panel shows before the next fast refresh, and write
//    again anything written ahead of time.
//  - A full or half refresh ignores the old-image plane and swaps nothing.
//    Writing both planes before one leaves everything right.

namespace bn::core {

// Which of the controller's two images: see above for how the two change
// places.
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
  // Each pixel is driven to what the new-image plane says, except where
  // both planes say white: those are left as they are. Quick and without a
  // flash, but each one leaves a little ghosting behind. It is only right
  // if the old-image plane really holds what is on the panel: where it
  // does not, black that should have gone stays. Afterwards the planes
  // have changed places.
  kFast = 2,
};

// What the panel does once a refresh is finished.
enum class AfterRefresh : std::uint8_t {
  kStayPowered = 0,  // Keep its drive voltages up: the next refresh starts sooner.
  // Switch them off. The image stays; nothing more is drawn from the
  // battery. The planes go back to the roles they had before the voltages
  // came on.
  kPowerOff = 1,
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

  // Copies whole rows out of a plane, in the form WriteRows() takes them:
  // what the panel itself holds. This is for checking that the planes are
  // what the code driving the panel believes them to be. It is slow, and no
  // part of showing a picture. Returns false if the panel is busy, or
  // cannot be read at all.
  [[nodiscard]] virtual bool ReadRows(DisplayPlane plane, std::uint16_t firstRow,
                                      std::span<std::uint8_t> rows) noexcept = 0;

  // Starts a refresh and returns at once; Busy() says when it is over.
  // Returns false, starting nothing, if the panel is not started or is busy.
  [[nodiscard]] virtual bool StartRefresh(RefreshMode mode, AfterRefresh after) noexcept = 0;

  // True while the panel is working on something and will take nothing new.
  virtual bool Busy() noexcept = 0;

  // Switches the panel's drive voltages off, if a refresh left them on, and
  // returns at once; Busy() says when it is done. The image stays. The
  // planes keep what they hold but go back to the roles they had before
  // the voltages came on, so the old-image plane may need writing again
  // (see the top of this file). Returns false if the panel is not started
  // or is busy.
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
