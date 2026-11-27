#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/block_file.h"
#include "core/byte_map.h"
#include "core/clock.h"
#include "core/display.h"
#include "core/input.h"
#include "core/storage.h"
#include "core/trace_stream.h"
#include "draw/font.h"
#include "layout/face_measure.h"
#include "layout/hyphenator.h"
#include "layout/line_breaker.h"
#include "layout/paragraph_setter.h"
#include "reader/screen.h"

namespace bn::reader {

// What the application shows in place of the test pattern when it is given
// a fonts image (App::ViewText()): the text of the card's
// /layout/paragraphs.txt, set in the image's faces a page at a time, for
// trying faces and sizes on the panel.
//
//   at start           the first page, by a full refresh
//   Right or Down      the next page, by a half refresh
//   Left or Up         the page before, by a half refresh
//   Confirm            the next size of the image's, from the page shown
//   Back               serif or sans, the other of them, from the page shown
//
// The text is read as `bn-lines` reads a file: an empty line ends a
// paragraph, a line end inside one is a line the text ends, and what is
// between two asterisks is in italic. Each paragraph is set by the
// paragraph setter in the faces (layout/face_measure.h), hyphenated by the
// built-in table for American English, with lines spread to the full width
// but the last; a paragraph longer than kMostParagraphBytes is cut there.
// Paragraphs are a third of a line apart.
//
// The image is read through a core::ByteMap, as a device has it, which has
// not the address space for the whole of one. Only the faces text is being
// set in are held (draw::FontImage::Hold()): the regular and the italic of
// the serif or the sans at one size, and the fallback face at that size.
// Changing the size or the face gives those up and holds the next, and a
// line says how many are held and how much is mapped for them. A face
// there is no room for is said, and what would have been drawn in it is
// not drawn.
//
// No picture of the page is kept. Setting a page leaves a list of its
// glyphs, each with where on the page its picture goes; and the panel's
// rows are made from that list a band of kBandRows at a time, turned the
// way the device is held to be read (core::Display::ReadingTurn()), with
// the darker ink where two glyphs meet. A page of more than kMostGlyphs
// has the rest left out, and a line says so.
//
// A glyph's pixel has one of four levels of ink. On a panel that shows
// four levels (core::Display::Levels()), such as the X4's, a page is shown
// first in black and white, every pixel with any ink black, by a full or
// half refresh; then the bands are made again as marks of the pixels with
// one level of ink and with two, and the gray pass
// (core::Display::StartGrayRefresh()) makes those light and dark gray. The
// drive voltages stay up from the one refresh to the other. On a panel
// that shows two, a pixel with two or three levels of ink is black, and
// there is no gray pass.
//
// Nothing here waits. Tick() works for about kSliceUs and returns: a
// paragraph is read some bytes at a time and given to the setter a call at
// a time, its lines are placed one at a time, and the panel is written a
// band at a time; so a long paragraph takes many turns of the loop, and the
// buttons go on around it. What a button asks for is done when the page
// under way is set and the panel is free: while the planes are being
// written the page is given up for the one asked for, and so are its grays.
// Going back goes over the pages already shown: changing the size or the
// face begins again from the page shown, which is then the first that can
// be gone back to. For each page shown, a line of log text says what it
// was and how long each part of showing it took.
//
// The memory for the glyphs of a page, the bands, the breaker and the
// setter is taken from the heap at Start(): about 54 KB for the X4's
// panel, most of it the glyphs. A paragraph being set is kept as well, its text and its lines.
class TextViewer final : public Screen {
 public:
  static constexpr std::string_view kTextPath = "/layout/paragraphs.txt";

  enum class State : std::uint8_t {
    kNotStarted,
    kReady,
    kFailed,  // The image is not one, the panel failed, or there was no memory.
    kAsleep,
  };

  // The space left round the text, in pixels.
  static constexpr std::uint16_t kMargin = 24;
  // Rows of the panel made and written at a time.
  static constexpr std::size_t kBandRows = 16;
  // The most glyphs of a page that are placed.
  static constexpr std::size_t kMostGlyphs = 3072;
  // The most bytes of a paragraph that are set.
  static constexpr std::size_t kMostParagraphBytes = 65536;
  // Bytes of the file gone through at a step.
  static constexpr std::size_t kReadBytes = 256;
  // What a hyphen costs, as the layout check has it.
  static constexpr std::uint32_t kHyphenPenalty = 12100;
  // How long Tick() works before it returns.
  static constexpr std::uint64_t kSliceUs = 20'000;
  // As the other viewers: a panel still busy after this long is taken to
  // have failed.
  static constexpr std::uint64_t kGiveUpUs = 20'000'000;

  // A glyph of the page: where its picture's top left corner goes, in the
  // page's pixels, the picture's size, and where in the fonts image the
  // picture is.
  struct Placed {
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint8_t width = 0;
    std::uint8_t height = 0;
    std::uint32_t picture = 0;
  };

  // The pages a tour shows in each face at each size.
  static constexpr std::size_t kTourPages = 2;

  // `fonts` has the bytes of the fonts image (draw/font.h), and has to
  // outlast the viewer.
  TextViewer(core::Display& display, core::Clock& clock, core::Storage& storage, core::TraceStream& stream,
             core::ByteMap& fonts);
  ~TextViewer() override;
  TextViewer(const TextViewer&) = delete;
  TextViewer& operator=(const TextViewer&) = delete;

  // Has the viewer go through the text by itself, for timing it on a device
  // with nobody at the buttons: from the image's smallest size to its
  // largest, kTourPages pages in the serif or the sans and then as many in
  // the other, if the image has both at that size; a line says when it is
  // done, and the buttons are then as they always are. Call before Start().
  void Tour() noexcept { tour_ = true; }
  bool TourDone() const noexcept { return tourDone_; }

  void Start() noexcept override;
  void OnInput(const core::InputEvent& event) noexcept override;
  void Tick() noexcept override;
  void Sleep() noexcept override;
  bool Resting() const noexcept override { return state_ == State::kAsleep || state_ == State::kFailed; }
  bool WorkWaiting() const noexcept override;

  State CurrentState() const noexcept { return state_; }
  // The page on the panel or on its way, from 0, among those gone through
  // since the face or the size last changed; and how many pages have been
  // fully shown.
  std::size_t Page() const noexcept { return page_; }
  std::size_t Shown() const noexcept { return shown_; }
  // The sizes the image has, and the one text is set in.
  const std::vector<std::uint16_t>& Sizes() const noexcept { return sizes_; }
  std::uint16_t Size() const noexcept { return sizes_.empty() ? 0 : sizes_[sizeIndex_]; }
  draw::FaceRole Role() const noexcept { return role_; }
  // Whether the page last set is the last of the text.
  bool AtEnd() const noexcept { return atEnd_; }
  // Whether the page last shown had its gray pass.
  bool GrayShown() const noexcept { return grayShown_; }
  // The page last set: its glyphs, how many lines they are, and its size.
  std::span<const Placed> Glyphs() const noexcept { return {placed_.get(), placedCount_}; }
  std::size_t Lines() const noexcept { return lineCount_; }
  std::uint16_t PageWidth() const noexcept { return pageWidth_; }
  std::uint16_t PageHeight() const noexcept { return pageHeight_; }

 private:
  // Where in the text a page begins: a paragraph's place in the file, and
  // which of its lines.
  struct Place {
    std::uint32_t offset = 0;
    std::uint32_t line = 0;
  };

  // One paragraph, read and set.
  struct Paragraph {
    std::uint32_t offset = 0;  // Where it begins in the file,
    std::uint32_t next = 0;    // and where the one after it is looked for.
    bool found = false;        // False at the end of the text.
    std::string text;
    std::vector<std::pair<std::uint32_t, std::uint16_t>> styles;  // From which byte, which style.
    std::vector<std::uint32_t> breaks;                            // Where the text ends a line.
    std::vector<layout::TextLine> lines;
  };

  class LineKeeper final : public layout::TextLineSink {
   public:
    void OnLine(const layout::TextLine& line) noexcept override;
    std::vector<layout::TextLine>* into = nullptr;
  };

  enum class Phase : std::uint8_t {
    kIdle,
    kSetting,         // A page being set: its glyphs placed.
    kWriting,         // The panel's rows being written, for black and white.
    kRefreshing,      // The panel showing them.
    kGraying,         // The marks of its grays being written.
    kGrayRefreshing,  // The gray pass.
  };

  // What a paragraph is having done to it, on the way to a page.
  enum class Making : std::uint8_t {
    kFinding,    // Its place in the file looked for.
    kReading,    // Its bytes read.
    kFeeding,    // Its text given to the setter.
    kFinishing,  // The setter giving out the last lines.
    kPlacing,    // Its lines put on the page.
  };

  // What a button asked for, to be done when it can be.
  enum class Asked : std::uint8_t { kNothing, kNext, kPrevious, kSize, kFace };

  // Sets the faces in use from the role and the size: gives up those held,
  // and with them the glyphs of the page set in them, and holds these.
  void UseFaces() noexcept;
  // What a tour does when a page has been shown.
  void TourOn() noexcept;
  // Does what a button asked for, if it can be done from the page shown.
  // True if a page is now to be set.
  bool TakeAsked() noexcept;
  void BeginPage(std::uint64_t now) noexcept;
  // One step of setting the page. True when the page is set.
  bool SetStep() noexcept;
  bool ReadStep() noexcept;  // True when the paragraph is read, or there is none.
  bool FeedStep() noexcept;  // True when it has all been given to the setter.
  void PageSet(std::uint64_t now) noexcept;
  // Places `line` of the paragraph with its baseline `baseline` pixels down.
  void PlaceLine(const layout::TextLine& line, std::int32_t baseline) noexcept;
  // Places `glyph` with the pen `pen` pixels from the page's left edge.
  void PlaceGlyph(const draw::Glyph& glyph, std::int32_t pen, std::int32_t baseline) noexcept;
  // Makes the band of the panel's rows from `top`: the ink of each of its
  // pixels, from the page's glyphs. Returns how many rows it is.
  std::size_t MakeBand(std::size_t top) noexcept;
  // Writes bands for black and white, or the marks of the grays, until
  // `until`. True when the last is written; false, too, if the panel would
  // not take one, and then the viewer has failed.
  bool WriteBands(bool grays, std::uint64_t until) noexcept;
  void StartRefresh(std::uint64_t now) noexcept;
  // Leaves the page in black and white, and says why.
  void WithoutGrays(const char* why, std::uint64_t now) noexcept;
  void Finished(std::uint64_t now) noexcept;
  bool PanelBusy(std::uint64_t now) noexcept;
  void Fail() noexcept;

  core::Display& display_;
  core::Clock& clock_;
  core::Storage& storage_;
  core::TraceStream& stream_;
  core::ByteMap& fontMap_;

  // A tour: how many pages of its face and size have been shown, whether
  // the other face has had its turn at this size, and how many sizes are
  // done.
  bool tour_ = false;
  bool tourDone_ = false;
  bool tourOtherFace_ = false;
  std::size_t tourPages_ = 0;
  std::size_t tourSizes_ = 0;

  State state_ = State::kNotStarted;
  Phase phase_ = Phase::kIdle;
  Asked asked_ = Asked::kNothing;
  bool sleepAsked_ = false;
  bool firstRefresh_ = true;
  bool pageWanted_ = false;
  bool grays_ = false;     // The panel shows four levels.
  bool graySeen_ = false;  // The page being written has a pixel that is to be gray.
  bool grayNext_ = false;  // A gray pass is to follow the refresh under way.
  bool grayShown_ = false;
  const char* grayNote_ = nullptr;  // Why a page was left in black and white.
  std::uint64_t phaseStartedUs_ = 0;

  // The faces.
  draw::FontImage fonts_;
  std::vector<std::uint16_t> sizes_;
  std::size_t sizeIndex_ = 0;
  draw::FaceRole role_ = draw::FaceRole::kSerif;
  layout::FaceMeasure measure_;
  layout::Hyphenator hyphenator_;
  std::uint16_t lineHeight_ = 0;
  std::uint16_t ascent_ = 0;

  // The text, and the pages gone through.
  std::array<std::uint8_t, 4096> fileBuffer_{};
  core::BlockFile file_{fileBuffer_};
  std::vector<Place> pages_;
  std::size_t page_ = 0;
  std::size_t shown_ = 0;
  bool atEnd_ = false;

  // The paragraph being set, and how far that has gone.
  Paragraph paragraph_;
  bool paragraphSet_ = false;
  Making making_ = Making::kFinding;
  std::uint16_t readStyle_ = 0;
  bool readLineHasText_ = false;
  bool readLineBegun_ = false;
  bool readCrWaiting_ = false;
  std::size_t feedAt_ = 0;
  std::size_t feedStyleAt_ = 0;
  std::size_t feedBreakAt_ = 0;
  std::uint16_t feedStyle_ = 0;

  // The page being set.
  Place at_;
  std::int32_t top_ = 0;
  std::size_t lineCount_ = 0;
  std::size_t leftOut_ = 0;  // Glyphs there was no room in the list for.
  std::unique_ptr<Placed[]> placed_;
  std::size_t placedCount_ = 0;

  // The setter and what it works in.
  std::unique_ptr<layout::LineBreaker::Node[]> nodes_;
  std::unique_ptr<layout::ParagraphSetter::Record[]> records_;
  std::unique_ptr<layout::ParagraphSetter> setter_;
  LineKeeper lines_;

  // The panel's rows: a band's ink, two bits a pixel, and the two planes'
  // bits made from it.
  std::uint16_t pageWidth_ = 0;
  std::uint16_t pageHeight_ = 0;
  std::size_t inkRowBytes_ = 0;
  std::unique_ptr<std::uint8_t[]> ink_;
  std::unique_ptr<std::uint8_t[]> bits_;
  std::unique_ptr<std::uint8_t[]> gray_;
  std::size_t bandTop_ = 0;

  // How long the parts of showing the page took.
  std::uint64_t setUs_ = 0;
  std::uint64_t planesUs_ = 0;
  std::uint64_t refreshUs_ = 0;
  std::uint64_t grayPlanesUs_ = 0;
  std::uint64_t grayPassUs_ = 0;
};

}  // namespace bn::reader
