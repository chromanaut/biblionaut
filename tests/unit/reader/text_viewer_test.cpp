#include "reader/text_viewer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/byte_map.h"
#include "display_fakes.h"
#include "font_image.h"
#include "storage_fakes.h"
#include "stream_fakes.h"
#include "trace_fakes.h"

namespace {

using bn::core::Button;
using bn::core::InputEvent;
using bn::core::InputEventKind;
using bn::core::PanelTurn;
using bn::reader::TextViewer;
using bn::test::FakeDisplay;
using bn::test::ImageFace;
using bn::test::ImageGlyph;
using Op = FakeDisplay::Op;
using State = TextViewer::State;

// A clock that moves on a little every time it is read, so that whatever
// is timed takes some time.
class SteppingClock final : public bn::core::Clock {
 public:
  std::uint64_t NowUs() noexcept override {
    now += step;
    return now;
  }
  std::uint64_t now = 0;
  std::uint64_t step = 10;
};

constexpr int kMargin = TextViewer::kMargin;

// A glyph `width` by `height` standing on the baseline, its rows of ink
// going round the levels from `first`, so that a letter has every level in
// it and no two letters next to each other are alike.
ImageGlyph Striped(std::uint16_t advance, std::uint8_t width, std::uint8_t height, std::uint8_t first) {
  ImageGlyph glyph;
  glyph.advance = advance;
  glyph.width = width;
  glyph.height = height;
  glyph.top = static_cast<std::int8_t>(height);
  for (std::size_t y = 0; y < height; ++y) {
    for (std::size_t x = 0; x < width; ++x) {
      glyph.ink.push_back(static_cast<std::uint8_t>(1 + ((first + y + (x / 3)) % 3)));
    }
  }
  return glyph;
}

// A face of `role` and `style` at `size`: the letters a to z, each two
// fifths of the size wide and half of it high, moving the pen half the
// size; a space of a quarter of it; a hyphen; and a line of the size and a
// fifth. The italic's letters are a pixel wider.
ImageFace TestFace(std::uint8_t role, std::uint8_t style, std::uint16_t size) {
  ImageFace face = bn::test::EmptyFace(role, style, size);
  face.ascent = static_cast<std::int16_t>(size * 4 / 5);
  face.descent = static_cast<std::int16_t>(size / 4);
  face.lineHeight = static_cast<std::uint16_t>(size * 6 / 5);
  const auto width = static_cast<std::uint8_t>((size * 2 / 5) + (style != 0 ? 1 : 0));
  for (std::uint32_t letter = 'a'; letter <= 'z'; ++letter) {
    face.glyphs[letter] = Striped(static_cast<std::uint16_t>(size * 2), width, static_cast<std::uint8_t>(size / 2),
                                  static_cast<std::uint8_t>(letter));
  }
  // Pens that do not fall on whole pixels: an a moves the pen half a pixel
  // further than the other letters do, and a b a quarter.
  face.glyphs['a'].advance = static_cast<std::uint16_t>(face.glyphs['a'].advance + 2);
  face.glyphs['b'].advance = static_cast<std::uint16_t>(face.glyphs['b'].advance + 1);
  face.glyphs[' '] = bn::test::Blank(size);
  face.glyphs['-'] = bn::test::Block(size, 5, 2);
  return face;
}

// Serif and sans at 20 and 30, the serif with an italic; and three letters
// of one level of ink each: x full, y two thirds, z a third; and w, which
// is hung four pixels back over the letter before it.
std::vector<ImageFace> TestFaces() {
  std::vector<ImageFace> faces;
  for (const std::uint16_t size : {std::uint16_t{20}, std::uint16_t{30}}) {
    for (const std::uint8_t role : {std::uint8_t{0}, std::uint8_t{1}}) {
      ImageFace face = TestFace(role, 0, size);
      const auto width = static_cast<std::uint8_t>(size * 2 / 5);
      const auto height = static_cast<std::uint8_t>(size / 2);
      face.glyphs['x'] = bn::test::Block(static_cast<std::uint16_t>(size * 2), width, height, 3);
      face.glyphs['y'] = bn::test::Block(static_cast<std::uint16_t>(size * 2), width, height, 2);
      face.glyphs['z'] = bn::test::Block(static_cast<std::uint16_t>(size * 2), width, height, 1);
      face.glyphs['w'] = bn::test::Block(static_cast<std::uint16_t>(size * 2), width, height, 2);
      face.glyphs['w'].left = -4;
      if (role == 1) {
        // The sans is a pixel taller, to be told from the serif.
        for (auto& [code, glyph] : face.glyphs) {
          if (glyph.height > 2) {
            glyph = bn::test::Block(glyph.advance, glyph.width, static_cast<std::uint8_t>(glyph.height + 1), 3);
          }
        }
      }
      faces.push_back(face);
    }
    faces.push_back(TestFace(0, bn::draw::kItalic, size));
  }
  return faces;
}

struct Fixture {
  explicit Fixture(std::vector<ImageFace> faces = TestFaces()) : described(std::move(faces)) {
    image = bn::test::FontImageBytes(described);
    display.present = true;
    display.levels = 4;
    display.Resize({.width = 160, .height = 120});
  }

  void Text(const std::string& text) { storage.Put("/layout/paragraphs.txt", {text.begin(), text.end()}); }
  void Start() {
    ASSERT_EQ(storage.Start(), bn::core::StorageStatus::kOk);
    // The image as a device has it: all of it mappable at once, unless the
    // test has said how many pages of kPageBytes there are.
    map = pages == 0 ? std::make_unique<bn::core::MemoryByteMap>(image)
                     : std::make_unique<bn::core::MemoryByteMap>(image, kPageBytes, pages);
    viewer = std::make_unique<TextViewer>(display, clock, storage, stream, *map);
    if (tour) {
      viewer->Tour();
    }
    viewer->Start();
  }
  // Turns of the loop until nothing more is asked of the panel and there is
  // no work waiting; the panel finishes what it is given at once. Returns
  // the turns it took.
  int Settle(int most = 200000) {
    int quiet = 0;
    for (int turn = 0; turn < most; ++turn) {
      display.busy = false;
      const std::size_t calls = display.calls.size();
      viewer->Tick();
      if (viewer->CurrentState() != State::kReady) {
        return turn;
      }
      quiet = display.calls.size() == calls && !display.busy && !viewer->WorkWaiting() ? quiet + 1 : 0;
      if (quiet > 5) {
        return turn;
      }
    }
    ADD_FAILURE() << "never settled";
    return most;
  }
  void Press(Button button) {
    viewer->OnInput(InputEvent{.timeUs = clock.now, .button = button, .kind = InputEventKind::kPress});
  }
  std::vector<std::string> Lines() const {
    std::vector<std::string> lines;
    for (const bn::test::StreamItem& item : bn::test::ParseStream(sink.received)) {
      if (item.isFrame && item.type == bn::core::FrameType::kText) {
        lines.push_back(item.Text());
      }
    }
    return lines;
  }
  std::string LastLine() const {
    const std::vector<std::string> lines = Lines();
    return lines.empty() ? std::string() : lines.back();
  }
  // The lines that begin with `with`: "text: page " for those of the pages
  // shown, "text: s" for those of the faces held.
  std::vector<std::string> LinesThatBegin(const std::string& with) const {
    std::vector<std::string> lines;
    for (const std::string& line : Lines()) {
      if (line.rfind(with, 0) == 0) {
        lines.push_back(line);
      }
    }
    return lines;
  }
  // The lines of the pages shown, each as far as its face and size:
  // "text: page 1, serif 20".
  std::vector<std::string> PagesSaid() const {
    const std::vector<std::string> lines = LinesThatBegin("text: page ");
    std::vector<std::string> said;
    said.reserve(lines.size());
    for (const std::string& line : lines) {
      said.push_back(line.substr(0, line.find(" px")));
    }
    return said;
  }
  std::vector<Op> PanelOps() const {
    std::vector<Op> ops;
    for (const FakeDisplay::Call& call : display.calls) {
      if (call.op != Op::kWrite) {
        ops.push_back(call.op);
      }
    }
    return ops;
  }
  // The last call of `op`.
  FakeDisplay::Call Last(Op op) const {
    for (std::size_t i = display.calls.size(); i > 0; --i) {
      if (display.calls[i - 1].op == op) {
        return display.calls[i - 1];
      }
    }
    ADD_FAILURE() << "no such call";
    return {};
  }

  // The level the panel shows at a pixel of the page, by this file's own
  // account of how a page lies on a panel that is turned.
  int ShownAt(int x, int y) const {
    const int width = display.size.width;
    const int height = display.size.height;
    int panelX = x;
    int panelY = y;
    switch (display.turn) {
      case PanelTurn::kNone:
        break;
      case PanelTurn::kHalf:
        panelX = width - 1 - x;
        panelY = height - 1 - y;
        break;
      case PanelTurn::kClockwise:  // The page's top is the panel's left edge.
        panelX = y;
        panelY = height - 1 - x;
        break;
      case PanelTurn::kAnticlockwise:
        panelX = width - 1 - y;
        panelY = x;
        break;
    }
    return display.LevelAt(static_cast<std::size_t>(panelX), static_cast<std::size_t>(panelY));
  }
  int PageWidth() const {
    const bool quarter = display.turn == PanelTurn::kClockwise || display.turn == PanelTurn::kAnticlockwise;
    return quarter ? display.size.height : display.size.width;
  }
  int PageHeight() const {
    const bool quarter = display.turn == PanelTurn::kClockwise || display.turn == PanelTurn::kAnticlockwise;
    return quarter ? display.size.width : display.size.height;
  }

  // The ink of a page that is one line of `text` in the face `face`, the
  // last of its paragraph, worked out here: the pen from the margin, each
  // letter's picture where its glyph says, the darker where two meet.
  std::vector<std::uint8_t> InkOfOneLine(const std::string& text, const ImageFace& face) const {
    std::vector<std::uint8_t> ink(static_cast<std::size_t>(PageWidth()) * static_cast<std::size_t>(PageHeight()), 0);
    int pen = kMargin * 4;
    const int baseline = kMargin + face.ascent;
    for (const char letter : text) {
      const ImageGlyph& glyph = face.glyphs.at(static_cast<std::uint32_t>(letter));
      const int left = ((pen + 2) / 4) + glyph.left;
      const int top = baseline - glyph.top;
      for (int y = 0; y < glyph.height; ++y) {
        for (int x = 0; x < glyph.width; ++x) {
          std::uint8_t& at = ink[(static_cast<std::size_t>(top + y) * static_cast<std::size_t>(PageWidth())) +
                                 static_cast<std::size_t>(left + x)];
          at = std::max(at, glyph.ink[(static_cast<std::size_t>(y) * glyph.width) + static_cast<std::size_t>(x)]);
        }
      }
      pen += glyph.advance;
    }
    return ink;
  }
  // How many pixels of the page the panel shows otherwise than `ink` says:
  // paper the lightest level, full ink the darkest, as `levelOf` has it.
  template <typename LevelOf>
  int WrongPixels(const std::vector<std::uint8_t>& ink, LevelOf levelOf) const {
    int wrong = 0;
    for (int y = 0; y < PageHeight(); ++y) {
      for (int x = 0; x < PageWidth(); ++x) {
        const std::uint8_t wanted =
            ink[(static_cast<std::size_t>(y) * static_cast<std::size_t>(PageWidth())) + static_cast<std::size_t>(x)];
        wrong += ShownAt(x, y) == levelOf(wanted) ? 0 : 1;
      }
    }
    return wrong;
  }

  // The page's glyphs, by the line they are on: the top of the tallest
  // picture on each.
  std::map<int, std::vector<TextViewer::Placed>> ByLine(int lineHeight) const {
    std::map<int, std::vector<TextViewer::Placed>> lines;
    for (const TextViewer::Placed& placed : viewer->Glyphs()) {
      lines[(placed.y + placed.height - kMargin) / lineHeight].push_back(placed);
    }
    return lines;
  }

  static constexpr std::size_t kPageBytes = 256;

  std::vector<ImageFace> described;
  std::vector<std::uint8_t> image;
  std::size_t pages = 0;
  bool tour = false;
  std::unique_ptr<bn::core::MemoryByteMap> map;
  bn::test::TraceFixture<64> trace;
  bn::test::FakeSink sink;
  bn::core::TraceStream stream{trace.tracer, sink};
  bn::test::FakeStorage storage;
  SteppingClock clock;
  FakeDisplay display;
  std::unique_ptr<TextViewer> viewer;
};

// Four levels: full ink the darkest, 0, and paper the lightest, 3.
int FourLevels(std::uint8_t ink) { return 3 - ink; }

TEST(TextViewer, ShowsAPageInBlackAndWhiteAndThenBringsOutItsGrays) {
  Fixture fixture;
  fixture.Text("abxyz\n");
  fixture.Start();
  ASSERT_EQ(fixture.viewer->CurrentState(), State::kReady);
  EXPECT_TRUE(fixture.viewer->WorkWaiting());
  fixture.Settle();

  // A full refresh that leaves the voltages up, and the gray pass after it.
  EXPECT_EQ(fixture.PanelOps(), (std::vector<Op>{Op::kStart, Op::kRefresh, Op::kGrayRefresh}));
  EXPECT_EQ(fixture.Last(Op::kRefresh).mode, bn::core::RefreshMode::kFull);
  EXPECT_EQ(fixture.Last(Op::kRefresh).after, bn::core::AfterRefresh::kStayPowered);
  EXPECT_EQ(fixture.Last(Op::kGrayRefresh).after, bn::core::AfterRefresh::kPowerOff);
  EXPECT_EQ(fixture.viewer->Shown(), 1U);
  EXPECT_TRUE(fixture.viewer->GrayShown());
  EXPECT_TRUE(fixture.viewer->AtEnd());
  EXPECT_FALSE(fixture.viewer->WorkWaiting());
  EXPECT_EQ(fixture.viewer->Glyphs().size(), 5U);
  EXPECT_EQ(fixture.viewer->Lines(), 1U);
  EXPECT_EQ(fixture.viewer->Size(), 30);  // The middle of the image's sizes.

  // Every pixel of the panel at the level of its ink.
  const std::vector<std::uint8_t> ink = fixture.InkOfOneLine("abxyz", fixture.described[3]);
  EXPECT_EQ(fixture.WrongPixels(ink, FourLevels), 0);
  // And there is ink of every level in it.
  EXPECT_EQ(std::set<std::uint8_t>(ink.begin(), ink.end()), (std::set<std::uint8_t>{0, 1, 2, 3}));

  // Before the gray pass, every pixel with any ink was black.
  ASSERT_EQ(fixture.display.oldAtGray.size(), 1U);
  const std::string line = fixture.LastLine();
  EXPECT_EQ(line.rfind("text: page 1, serif 30 px, 1 line, 5 glyphs: set ", 0), 0U) << line;
  EXPECT_NE(line.find(" ms, gray planes "), std::string::npos) << line;
  EXPECT_NE(line.find("; the end of the text"), std::string::npos) << line;
}

TEST(TextViewer, ShowsThePageTheSameWhicheverWayThePanelIsTurned) {
  for (const PanelTurn turn : {PanelTurn::kNone, PanelTurn::kClockwise, PanelTurn::kHalf, PanelTurn::kAnticlockwise}) {
    Fixture fixture;
    fixture.display.turn = turn;
    // Not a multiple of the band's rows high, nor of eight pixels wide.
    fixture.display.Resize({.width = 171, .height = 139});
    // A w is hung back over the letter before it: the darker ink shows.
    fixture.Text("abwxw\n");
    fixture.Start();
    fixture.Settle();
    ASSERT_EQ(fixture.viewer->Shown(), 1U) << static_cast<int>(turn);
    EXPECT_EQ(fixture.viewer->PageWidth(), fixture.PageWidth());
    EXPECT_EQ(fixture.viewer->PageHeight(), fixture.PageHeight());
    const std::vector<std::uint8_t> ink = fixture.InkOfOneLine("abwxw", fixture.described[3]);
    EXPECT_EQ(fixture.WrongPixels(ink, FourLevels), 0) << static_cast<int>(turn);
    // The text is somewhere, and not the same way up in each.
    EXPECT_GT(std::count_if(ink.begin(), ink.end(), [](std::uint8_t level) { return level != 0; }), 400);
  }
}

TEST(TextViewer, MakesTheDarkerInkBlackOnAPanelOfTwoLevels) {
  Fixture fixture;
  fixture.display.levels = 2;
  fixture.display.Resize({.width = 160, .height = 120});
  fixture.Text("abxyz\n");
  fixture.Start();
  fixture.Settle();
  // One refresh, the voltages off after it, and no gray pass.
  EXPECT_EQ(fixture.PanelOps(), (std::vector<Op>{Op::kStart, Op::kRefresh}));
  EXPECT_EQ(fixture.Last(Op::kRefresh).after, bn::core::AfterRefresh::kPowerOff);
  EXPECT_FALSE(fixture.viewer->GrayShown());
  const std::vector<std::uint8_t> ink = fixture.InkOfOneLine("abxyz", fixture.described[3]);
  // Two or three levels of ink are black, and one is paper.
  EXPECT_EQ(fixture.WrongPixels(ink, [](std::uint8_t level) { return level >= 2 ? 0 : 1; }), 0);
  EXPECT_EQ(fixture.LastLine().find("gray"), std::string::npos) << fixture.LastLine();
}

TEST(TextViewer, GivesAPageWithNoGrayInItNoGrayPass) {
  Fixture fixture;
  fixture.Text("xxx xx\n");
  fixture.Start();
  fixture.Settle();
  EXPECT_EQ(fixture.PanelOps(), (std::vector<Op>{Op::kStart, Op::kRefresh}));
  EXPECT_EQ(fixture.Last(Op::kRefresh).after, bn::core::AfterRefresh::kPowerOff);
  EXPECT_FALSE(fixture.viewer->GrayShown());
  EXPECT_EQ(fixture.viewer->Shown(), 1U);
  EXPECT_EQ(fixture.LastLine().find("gray"), std::string::npos) << fixture.LastLine();
}

TEST(TextViewer, SpreadsEveryLineOfAParagraphButTheLastToTheFullWidth) {
  Fixture fixture;
  // Tall enough for the whole paragraph.
  fixture.display.Resize({.width = 400, .height = 600});
  std::string text;
  for (int word = 0; word < 40; ++word) {
    text += std::string(static_cast<std::size_t>(2 + (word % 4)), 'x') + " ";
  }
  fixture.Text(text + "\n");
  fixture.Start();
  fixture.Settle();
  ASSERT_TRUE(fixture.viewer->AtEnd());
  const auto lines = fixture.ByLine(36);
  ASSERT_GT(lines.size(), 3U);
  EXPECT_EQ(lines.size(), fixture.viewer->Lines());
  std::size_t number = 0;
  for (const auto& [line, glyphs] : lines) {
    int left = 10000;
    int right = 0;
    for (const TextViewer::Placed& placed : glyphs) {
      left = std::min<int>(left, placed.x);
      right = std::max<int>(right, placed.x + placed.width);
      // Every letter stands on its line's baseline.
      EXPECT_EQ(placed.y + placed.height, kMargin + 24 + (line * 36));
    }
    EXPECT_EQ(left, kMargin);
    if (++number < lines.size()) {
      // The last letter's picture is three pixels short of its advance, and
      // the pen is put to the nearest pixel.
      EXPECT_NEAR(right, 400 - kMargin - 3, 1) << line;
    } else {
      // The last is set as it comes: no letter is further from the one
      // before it than a letter's advance and a space's, 15 and 7.5.
      std::vector<int> across;
      for (const TextViewer::Placed& placed : glyphs) {
        across.push_back(placed.x);
      }
      std::sort(across.begin(), across.end());
      for (std::size_t i = 1; i < across.size(); ++i) {
        EXPECT_LE(across[i] - across[i - 1], 23);
      }
      EXPECT_LE(right, 400 - kMargin - 3);
    }
  }
}

TEST(TextViewer, BreaksAWordWhereItMayBeBrokenAndShowsAHyphen) {
  Fixture fixture;
  // Room for a word and half of the next on a line.
  fixture.display.Resize({.width = 300, .height = 400});
  fixture.Text("hyphenation hyphenation hyphenation hyphenation hyphenation hyphenation\n");
  fixture.Start();
  fixture.Settle();
  // The hyphen's picture is the only one five by two.
  std::size_t hyphens = 0;
  for (const TextViewer::Placed& placed : fixture.viewer->Glyphs()) {
    hyphens += placed.width == 5 && placed.height == 2 ? 1 : 0;
  }
  EXPECT_GT(hyphens, 0U);
  // Every letter is on the page, and the hyphens besides.
  EXPECT_EQ(fixture.viewer->Glyphs().size(), 66U + hyphens);  // Six words of eleven letters.
}

TEST(TextViewer, SetsWhatIsBetweenAsterisksInItalic) {
  Fixture fixture;
  fixture.display.Resize({.width = 400, .height = 300});
  fixture.Text("xx *xx* xx\n");
  fixture.Start();
  fixture.Settle();
  std::vector<int> widths;
  for (const TextViewer::Placed& placed : fixture.viewer->Glyphs()) {
    widths.push_back(placed.width);
  }
  // The italic's letters are a pixel wider; the asterisks are not shown.
  EXPECT_EQ(widths, (std::vector<int>{12, 12, 13, 13, 12, 12}));
}

TEST(TextViewer, EndsALineWhereTheTextDoesAndSetsParagraphsAThirdOfALineApart) {
  Fixture fixture;
  fixture.display.Resize({.width = 400, .height = 300});
  fixture.Text("\n\nxx\r\nxxx\n\n\nxxxx\n");
  fixture.Start();
  fixture.Settle();
  // Each glyph's baseline and how many are on it.
  std::map<int, int> baselines;
  for (const TextViewer::Placed& placed : fixture.viewer->Glyphs()) {
    ++baselines[placed.y + placed.height];
  }
  // A line is 36 high and the first baseline 24 below the margin; the
  // second paragraph is a third of a line further down.
  EXPECT_EQ(baselines, (std::map<int, int>{{kMargin + 24, 2}, {kMargin + 60, 3}, {kMargin + 108, 4}}));
  EXPECT_EQ(fixture.viewer->Lines(), 3U);
  EXPECT_TRUE(fixture.viewer->AtEnd());
}

// Words of `letters` letters, `words` of them, in paragraphs of ten.
std::string ManyWords(int words, int letters = 4) {
  std::string text;
  for (int word = 0; word < words; ++word) {
    text += std::string(static_cast<std::size_t>(letters), static_cast<char>('a' + (word % 20)));
    text += (word % 10) == 9 ? "\n\n" : " ";
  }
  return text + "\n";
}

TEST(TextViewer, GoesOnToTheNextPageAndBackOverThoseShown) {
  Fixture fixture;
  fixture.Text(ManyWords(60));
  fixture.Start();
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U);
  EXPECT_FALSE(fixture.viewer->AtEnd());
  const std::vector<TextViewer::Placed> first(fixture.viewer->Glyphs().begin(), fixture.viewer->Glyphs().end());
  const auto same = [](const std::vector<TextViewer::Placed>& a, std::span<const TextViewer::Placed> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& one, const auto& other) {
             return one.x == other.x && one.y == other.y && one.picture == other.picture;
           });
  };

  // Back from the first page is nowhere.
  fixture.Press(Button::kLeft);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Shown(), 1U);

  // On through the text to its end, by half refreshes: every letter shown
  // once.
  std::size_t letters = first.size();
  std::size_t pages = 1;
  std::vector<TextViewer::Placed> second;
  while (!fixture.viewer->AtEnd() && pages < 100) {
    fixture.Press(pages % 2 == 0 ? Button::kRight : Button::kDown);
    fixture.Settle();
    ++pages;
    ASSERT_EQ(fixture.viewer->Shown(), pages);
    ASSERT_EQ(fixture.viewer->Page(), pages - 1);
    EXPECT_EQ(fixture.Last(Op::kRefresh).mode, bn::core::RefreshMode::kHalf);
    letters += fixture.viewer->Glyphs().size();
    if (pages == 2) {
      second.assign(fixture.viewer->Glyphs().begin(), fixture.viewer->Glyphs().end());
      EXPECT_FALSE(same(first, fixture.viewer->Glyphs()));
    }
  }
  EXPECT_GT(pages, 3U);
  EXPECT_EQ(letters, 60U * 4U);
  EXPECT_NE(fixture.LastLine().find("; the end of the text"), std::string::npos) << fixture.LastLine();

  // Past the end is nowhere.
  fixture.Press(Button::kRight);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Shown(), pages);

  // And back: the pages are the ones that were shown.
  for (std::size_t back = pages; back > 2; --back) {
    fixture.Press(Button::kUp);
    fixture.Settle();
  }
  EXPECT_EQ(fixture.viewer->Page(), 1U);
  EXPECT_TRUE(same(second, fixture.viewer->Glyphs()));
  EXPECT_FALSE(fixture.viewer->AtEnd());
  fixture.Press(Button::kLeft);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Page(), 0U);
  EXPECT_TRUE(same(first, fixture.viewer->Glyphs()));
  EXPECT_EQ(fixture.LastLine().rfind("text: page 1, ", 0), 0U) << fixture.LastLine();
}

TEST(TextViewer, ChangesTheSizeAndTheFaceFromThePageShown) {
  Fixture fixture;
  fixture.display.Resize({.width = 300, .height = 200});
  fixture.Text(ManyWords(80));
  fixture.Start();
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Sizes(), (std::vector<std::uint16_t>{20, 30}));
  EXPECT_EQ(fixture.viewer->Size(), 30);
  EXPECT_EQ(fixture.viewer->Role(), bn::draw::FaceRole::kSerif);
  fixture.Press(Button::kRight);
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Page(), 1U);
  // The letter the second page begins with.
  const std::uint32_t begins = fixture.viewer->Glyphs()[0].picture;
  const auto pictureOf = [&](std::size_t face, std::uint32_t letter) {
    bn::draw::FontImage fonts;
    EXPECT_EQ(fonts.Open(fixture.image), bn::draw::FontStatus::kOk);
    bn::draw::Glyph glyph;
    EXPECT_TRUE(fonts.FaceAt(face).Find(letter, glyph));
    return static_cast<std::uint32_t>(glyph.picture.data() - fixture.image.data());
  };
  std::uint32_t letter = 0;
  for (std::uint32_t tried = 'a'; tried <= 'z'; ++tried) {
    letter = pictureOf(3, tried) == begins ? tried : letter;
  }
  ASSERT_NE(letter, 0U);

  // Confirm: the other size, the text set again from where the page began,
  // which is then the first page there is.
  fixture.Press(Button::kConfirm);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Size(), 20);
  EXPECT_EQ(fixture.viewer->Page(), 0U);
  EXPECT_EQ(fixture.viewer->Glyphs()[0].picture, pictureOf(0, letter));
  EXPECT_EQ(fixture.viewer->Glyphs()[0].height, 10);
  EXPECT_EQ(fixture.LastLine().rfind("text: page 1, serif 20 px, ", 0), 0U) << fixture.LastLine();
  fixture.Press(Button::kLeft);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Page(), 0U);

  // Back: the sans, from the same place.
  fixture.Press(Button::kBack);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Role(), bn::draw::FaceRole::kSans);
  EXPECT_EQ(fixture.viewer->Glyphs()[0].picture, pictureOf(1, letter));
  EXPECT_EQ(fixture.viewer->Glyphs()[0].height, 11);
  EXPECT_EQ(fixture.LastLine().rfind("text: page 1, sans 20 px, ", 0), 0U) << fixture.LastLine();
  // Round to the first size again, and the serif.
  fixture.Press(Button::kConfirm);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Size(), 30);
  fixture.Press(Button::kBack);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Role(), bn::draw::FaceRole::kSerif);
  EXPECT_EQ(fixture.viewer->Glyphs()[0].picture, begins);
}

TEST(TextViewer, WorksASliceAtATimeHoweverLongAParagraphIs) {
  Fixture fixture;
  // One paragraph of 12,000 words, and a clock that gives a turn of the
  // loop a few steps of work.
  std::string text;
  for (int word = 0; word < 12000; ++word) {
    text += "abcd ";
  }
  fixture.Text(text + "\n");
  fixture.clock.step = 2500;
  fixture.Start();
  int turns = 0;
  std::uint64_t longest = 0;
  while (fixture.viewer->Shown() == 0 && turns < 100000) {
    fixture.display.busy = false;
    const std::uint64_t before = fixture.clock.now;
    fixture.viewer->Tick();
    longest = std::max(longest, fixture.clock.now - before);
    ++turns;
    if (fixture.display.Count(Op::kRefresh) == 0) {
      EXPECT_TRUE(fixture.viewer->WorkWaiting());
    }
  }
  ASSERT_EQ(fixture.viewer->Shown(), 1U);
  // Many turns, none of them much longer than a slice.
  EXPECT_GT(turns, 500);
  EXPECT_LT(longest, TextViewer::kSliceUs + (20 * fixture.clock.step));
  EXPECT_FALSE(fixture.viewer->AtEnd());
  // A word to a line, and two lines to the page.
  EXPECT_EQ(fixture.viewer->Glyphs().size(), 8U);
}

TEST(TextViewer, GivesUpAPagesGraysForWhatIsAskedMeanwhile) {
  Fixture fixture;
  fixture.Text(ManyWords(60));
  fixture.Start();
  // Until the first refresh is asked for, and then over.
  for (int turn = 0; turn < 10000 && fixture.display.Count(Op::kRefresh) == 0; ++turn) {
    fixture.viewer->Tick();
  }
  ASSERT_EQ(fixture.display.Count(Op::kRefresh), 1U);
  fixture.display.busy = false;
  fixture.viewer->Tick();  // The gray marks are begun.
  EXPECT_TRUE(fixture.viewer->WorkWaiting());
  fixture.Press(Button::kRight);
  fixture.Settle();

  // The voltages switched off in place of the first page's gray pass; the
  // second page shown, with its own.
  EXPECT_EQ(fixture.PanelOps(),
            (std::vector<Op>{Op::kStart, Op::kRefresh, Op::kPowerOff, Op::kRefresh, Op::kGrayRefresh}));
  EXPECT_EQ(fixture.viewer->Page(), 1U);
  EXPECT_EQ(fixture.viewer->Shown(), 2U);
  EXPECT_TRUE(fixture.viewer->GrayShown());
  const std::vector<std::string> lines = fixture.LinesThatBegin("text: page ");
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_NE(lines[0].find("; no grays: something else was asked for"), std::string::npos) << lines[0];
  EXPECT_EQ(lines[1].find("no grays"), std::string::npos) << lines[1];

  // What cannot be done is not waited for: a press at the first page for
  // the one before it leaves the grays to come.
  Fixture other;
  other.Text(ManyWords(60));
  other.Start();
  for (int turn = 0; turn < 10000 && other.display.Count(Op::kRefresh) == 0; ++turn) {
    other.viewer->Tick();
  }
  other.Press(Button::kLeft);
  other.Settle();
  EXPECT_EQ(other.viewer->Shown(), 1U);
  EXPECT_EQ(other.viewer->Page(), 0U);
}

TEST(TextViewer, GivesUpAPageBeingWrittenForTheOneAskedFor) {
  Fixture fixture;
  fixture.display.Resize({.width = 300, .height = 400});
  fixture.Text(ManyWords(200));
  // A clock that lets a turn write one band.
  fixture.clock.step = 30000;
  fixture.Start();
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U);
  fixture.Press(Button::kRight);
  // Until the second page's first bands are written.
  const std::size_t writes = fixture.display.Count(Op::kWrite);
  for (int turn = 0; turn < 10000 && fixture.display.Count(Op::kWrite) < writes + 4; ++turn) {
    fixture.viewer->Tick();
  }
  ASSERT_EQ(fixture.viewer->Page(), 1U);
  ASSERT_EQ(fixture.display.Count(Op::kRefresh), 1U);
  fixture.Press(Button::kRight);
  fixture.Settle();
  // The second page was never shown: one refresh more, for the third.
  EXPECT_EQ(fixture.viewer->Page(), 2U);
  EXPECT_EQ(fixture.display.Count(Op::kRefresh), 2U);
  EXPECT_EQ(fixture.viewer->Shown(), 2U);
  EXPECT_EQ(fixture.LastLine().rfind("text: page 3, ", 0), 0U) << fixture.LastLine();
}

TEST(TextViewer, SleepsOnceThePageUnderWayIsShown) {
  Fixture fixture;
  fixture.Text(ManyWords(60));
  fixture.Start();
  fixture.viewer->Tick();
  fixture.viewer->Sleep();
  fixture.Press(Button::kRight);  // Not taken: it is going to sleep.
  EXPECT_FALSE(fixture.viewer->Resting());
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->CurrentState(), State::kAsleep);
  EXPECT_TRUE(fixture.viewer->Resting());
  EXPECT_FALSE(fixture.viewer->WorkWaiting());
  // The page was shown, without waiting for its grays, and then the panel
  // put to sleep.
  EXPECT_EQ(fixture.PanelOps(), (std::vector<Op>{Op::kStart, Op::kRefresh, Op::kPowerOff, Op::kSleep}));
  EXPECT_EQ(fixture.viewer->Page(), 0U);
}

TEST(TextViewer, ShowsAnEmptyPageWhenTheCardHasNoText) {
  Fixture fixture;
  fixture.Start();
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->CurrentState(), State::kReady);
  EXPECT_EQ(fixture.viewer->Shown(), 1U);
  EXPECT_TRUE(fixture.viewer->AtEnd());
  EXPECT_TRUE(fixture.viewer->Glyphs().empty());
  EXPECT_TRUE(std::all_of(fixture.display.shownLevels.begin(), fixture.display.shownLevels.end(),
                          [](std::uint8_t level) { return level == 3; }));
  const std::vector<std::string> lines = fixture.Lines();
  ASSERT_EQ(lines.size(), 3U);
  EXPECT_EQ(lines[0], "text: no text at /layout/paragraphs.txt (not found).");
  EXPECT_EQ(lines[1].rfind("text: serif 30 px: 2 faces held, ", 0), 0U) << lines[1];
  EXPECT_EQ(lines[2].rfind("text: page 1, serif 30 px, 0 lines, 0 glyphs: ", 0), 0U) << lines[2];

  // And one of nothing but empty lines.
  Fixture blank;
  blank.Text("\n\r\n\n");
  blank.Start();
  blank.Settle();
  EXPECT_EQ(blank.viewer->Shown(), 1U);
  EXPECT_TRUE(blank.viewer->AtEnd());
  EXPECT_TRUE(blank.viewer->Glyphs().empty());
}

TEST(TextViewer, FailsWithAFontsImageItCannotUse) {
  Fixture fixture;
  fixture.image.assign(100, 0x55);
  fixture.Text("xx\n");
  fixture.Start();
  EXPECT_EQ(fixture.viewer->CurrentState(), State::kFailed);
  EXPECT_TRUE(fixture.viewer->Resting());
  EXPECT_EQ(fixture.LastLine(), "text: the fonts image is no good (bad image).");
  EXPECT_TRUE(fixture.display.calls.empty());
  fixture.viewer->Tick();
  fixture.Press(Button::kRight);
  EXPECT_FALSE(fixture.viewer->WorkWaiting());

  // An image with a monospace face alone has nothing to read in.
  Fixture mono({bn::test::EmptyFace(2, 0, 20)});
  mono.Start();
  EXPECT_EQ(mono.viewer->CurrentState(), State::kFailed);
  EXPECT_EQ(mono.LastLine(), "text: the fonts image has no serif or sans in regular.");

  // One with a sans alone is read in the sans.
  Fixture sans({TestFace(1, 0, 20)});
  sans.Text("ab\n");
  sans.Start();
  sans.Settle();
  EXPECT_EQ(sans.viewer->Role(), bn::draw::FaceRole::kSans);
  EXPECT_EQ(sans.viewer->Shown(), 1U);
  EXPECT_EQ(sans.viewer->Glyphs().size(), 2U);
  // And has no other size or face to change to.
  sans.Press(Button::kConfirm);
  sans.Settle();
  sans.Press(Button::kBack);
  sans.Settle();
  EXPECT_EQ(sans.viewer->Shown(), 1U);
}

TEST(TextViewer, FailsWhenThePanelDoes) {
  Fixture absent;
  absent.display.present = false;
  absent.Text("xx\n");
  absent.Start();
  EXPECT_EQ(absent.viewer->CurrentState(), State::kFailed);

  // A panel that takes no rows.
  Fixture deaf;
  deaf.Text("xx\n");
  deaf.Start();
  deaf.display.failWrites = true;
  deaf.Settle();
  EXPECT_EQ(deaf.viewer->CurrentState(), State::kFailed);
  EXPECT_EQ(deaf.viewer->Shown(), 0U);

  // A panel that never finishes its refresh.
  Fixture stuck;
  stuck.Text("xx\n");
  stuck.clock.step = 100000;
  stuck.Start();
  for (int turn = 0; turn < 1000 && stuck.viewer->CurrentState() == State::kReady; ++turn) {
    stuck.viewer->Tick();
  }
  EXPECT_EQ(stuck.viewer->CurrentState(), State::kFailed);
  EXPECT_EQ(stuck.display.Count(Op::kRefresh), 1U);
  EXPECT_EQ(stuck.viewer->Shown(), 0U);
}

TEST(TextViewer, GivesWhatIsLeftOverOfALineToTheFirstPlacesBetweenItsWords) {
  Fixture fixture;
  fixture.display.Resize({.width = 400, .height = 600});
  // Words of one letter: fifteen to a line come to 1,320 quarters of the
  // 1,408 there are, and fourteen to 1,230; either way what is over does
  // not go into the places between them a whole number of times.
  std::string text;
  for (int word = 0; word < 70; ++word) {
    text += "x ";
  }
  fixture.Text(text + "\n");
  fixture.Start();
  fixture.Settle();
  ASSERT_TRUE(fixture.viewer->AtEnd());
  const auto lines = fixture.ByLine(36);
  ASSERT_GT(lines.size(), 3U);
  std::size_t number = 0;
  for (const auto& [line, glyphs] : lines) {
    if (++number == lines.size()) {
      break;
    }
    int last = 0;
    for (const TextViewer::Placed& placed : glyphs) {
      last = std::max<int>(last, placed.x);
    }
    // The last letter's pen is its advance short of the right margin, to
    // the quarter: (96 + 1408 - 60) quarters, which is pixel 361.
    EXPECT_EQ(last, 361) << line;
    EXPECT_TRUE(glyphs.size() == 14 || glyphs.size() == 15) << glyphs.size();
  }
}

TEST(TextViewer, PutsALineOnEveryPageHoweverTallALineIs) {
  // A face whose lines are taller than the page has room for.
  ImageFace tall = TestFace(0, 0, 20);
  tall.lineHeight = 100;
  Fixture fixture({tall});
  fixture.Text("ab\ncd\n\nef\n");
  fixture.Start();
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U);
  EXPECT_EQ(fixture.viewer->Lines(), 1U);
  EXPECT_EQ(fixture.viewer->Glyphs().size(), 2U);
  EXPECT_FALSE(fixture.viewer->AtEnd());
  // And so the text is got through, a line to a page.
  for (std::size_t page = 2; page <= 3; ++page) {
    fixture.Press(Button::kRight);
    fixture.Settle();
    ASSERT_EQ(fixture.viewer->Shown(), page);
    EXPECT_EQ(fixture.viewer->Lines(), 1U);
    EXPECT_EQ(fixture.viewer->Glyphs().size(), 2U);
  }
  EXPECT_TRUE(fixture.viewer->AtEnd());
}

TEST(TextViewer, DrawsWhatTheFaceLacksFromTheFallbackFaceOrAsItsGlyphForNone) {
  std::vector<ImageFace> faces = {TestFace(0, 0, 20), bn::test::EmptyFace(3, 0, 20)};
  // The fallback has an alpha, seven by seven, and the serif's glyph for
  // what it lacks is three by nine.
  faces[1].glyphs[0x3B1] = bn::test::Block(40, 7, 7, 3);
  faces[0].missing = bn::test::Block(40, 3, 9, 3);
  Fixture fixture(faces);
  // An x, an alpha, a beta, which neither has, a byte that is no character
  // at all, and an x.
  fixture.Text("x\xCE\xB1\xCE\xB2\xFFx\n");
  fixture.Start();
  fixture.Settle();
  std::vector<std::pair<int, int>> sizes;
  for (const TextViewer::Placed& placed : fixture.viewer->Glyphs()) {
    sizes.emplace_back(placed.width, placed.height);
  }
  EXPECT_EQ(sizes, (std::vector<std::pair<int, int>>{{8, 10}, {7, 7}, {3, 9}, {3, 9}, {8, 10}}));
}

TEST(TextViewer, LeavesOutTheGlyphsOfAPageThatHasMoreThanItKeeps) {
  // A face four pixels high, whose letters are two wide and two apart.
  ImageFace tiny = bn::test::EmptyFace(0, 0, 6);
  tiny.ascent = 3;
  tiny.descent = 1;
  tiny.lineHeight = 4;
  tiny.glyphs['x'] = bn::test::Block(8, 1, 2, 3);
  tiny.glyphs[' '] = bn::test::Blank(8);
  Fixture fixture({tiny});
  fixture.display.Resize({.width = 600, .height = 200});
  fixture.Text(std::string(20000, 'x') + "\n");
  fixture.Start();
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Shown(), 1U);
  // 276 letters a line and 38 lines to a page: more than the list holds.
  EXPECT_EQ(fixture.viewer->Glyphs().size(), TextViewer::kMostGlyphs);
  EXPECT_NE(fixture.LastLine().find(" glyphs left out"), std::string::npos) << fixture.LastLine();
  // The next page begins where this one's lines ended, whatever was left
  // out of it.
  EXPECT_FALSE(fixture.viewer->AtEnd());
  fixture.Press(Button::kRight);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Shown(), 2U);
  EXPECT_EQ(fixture.viewer->Page(), 1U);
  EXPECT_EQ(fixture.viewer->Lines(), 38U);
}

// The whole pages of Fixture::kPageBytes that `length` bytes of the image
// from `at` touch.
std::size_t PagesOf(std::size_t at, std::size_t length) {
  return ((at % Fixture::kPageBytes) + length + Fixture::kPageBytes - 1) / Fixture::kPageBytes;
}

TEST(TextViewer, HoldsOnlyTheFacesOfTheSizeAndTheFaceTextIsSetIn) {
  // The serif and the sans at 20 and 30, the serif with an italic, and a
  // fallback face at 20.
  std::vector<ImageFace> faces = TestFaces();
  faces.push_back(TestFace(3, 0, 20));
  Fixture fixture(faces);
  std::vector<bn::test::ImageFacePlaces> places;
  static_cast<void>(bn::test::FontImageBytes(faces, &places));
  const auto stretch = [&](std::size_t face) {
    return places[face].pictures + places[face].pictureBytes - places[face].ranges;
  };
  fixture.Text("ab *cd* ef\n");
  fixture.Start();
  fixture.Settle();

  // At 30 in the serif: its regular and its italic, and no fallback, which
  // the image has none of at that size. Beside them only the table of
  // faces is mapped.
  ASSERT_EQ(fixture.viewer->Size(), 30);
  EXPECT_EQ(fixture.map->Stretches(), 3U);
  EXPECT_EQ(fixture.map->MappedBytes(), 16 + (7 * 48) + stretch(3) + stretch(5));
  std::vector<std::string> lines = fixture.LinesThatBegin("text: s");
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_EQ(lines[0].rfind("text: serif 30 px: 2 faces held, ", 0), 0U) << lines[0];
  EXPECT_NE(lines[0].find(" KB of the image; "), std::string::npos) << lines[0];
  EXPECT_EQ(lines[0].find("no room"), std::string::npos) << lines[0];
  EXPECT_EQ(fixture.viewer->Glyphs().size(), 6U);

  // Confirm, for 20: those are given up, and the three of that size held.
  fixture.Press(Button::kConfirm);
  fixture.Settle();
  ASSERT_EQ(fixture.viewer->Size(), 20);
  EXPECT_EQ(fixture.map->Stretches(), 4U);
  EXPECT_EQ(fixture.map->MappedBytes(), 16 + (7 * 48) + stretch(0) + stretch(2) + stretch(6));
  lines = fixture.LinesThatBegin("text: s");
  ASSERT_EQ(lines.size(), 2U);
  EXPECT_EQ(lines[1].rfind("text: serif 20 px: 3 faces held, ", 0), 0U) << lines[1];
  EXPECT_EQ(fixture.viewer->Glyphs().size(), 6U);

  // Back, for the sans, which has no italic: its regular is both, and is
  // held once, with the fallback.
  fixture.Press(Button::kBack);
  fixture.Settle();
  EXPECT_EQ(fixture.map->Stretches(), 3U);
  EXPECT_EQ(fixture.map->MappedBytes(), 16 + (7 * 48) + stretch(1) + stretch(6));
  lines = fixture.LinesThatBegin("text: s");
  ASSERT_EQ(lines.size(), 3U);
  EXPECT_EQ(lines[2].rfind("text: sans 20 px: 2 faces held, ", 0), 0U) << lines[2];
  EXPECT_EQ(fixture.viewer->Glyphs().size(), 6U);
  EXPECT_EQ(fixture.viewer->Shown(), 3U);
  // The whole of the image was never mapped.
  EXPECT_LT(fixture.map->MostMappedBytes(), fixture.image.size());
}

TEST(TextViewer, SaysWhenThereIsNoRoomForAFaceAndDrawsNothingInIt) {
  Fixture fixture;
  std::vector<bn::test::ImageFacePlaces> places;
  static_cast<void>(bn::test::FontImageBytes(fixture.described, &places));
  // Pages for the table of faces and for the serif's regular at 30, which
  // is held first, and none for its italic.
  const std::size_t regular = PagesOf(places[3].ranges, places[3].pictures + places[3].pictureBytes - places[3].ranges);
  fixture.pages = PagesOf(0, 16 + (6 * 48)) + regular;
  fixture.Text("ab *cd* ef\n");
  fixture.Start();
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->CurrentState(), State::kReady);
  EXPECT_EQ(fixture.viewer->Shown(), 1U);
  const std::vector<std::string> lines = fixture.LinesThatBegin("text: s");
  ASSERT_EQ(lines.size(), 1U);
  EXPECT_EQ(lines[0].rfind("text: serif 30 px: 1 face held, ", 0), 0U) << lines[0];
  EXPECT_NE(lines[0].find("; no room for 1 face"), std::string::npos) << lines[0];
  // The four letters that are not in italic, and not the two that are.
  ASSERT_EQ(fixture.viewer->Glyphs().size(), 4U);
  std::set<std::uint8_t> widths;
  for (const TextViewer::Placed& placed : fixture.viewer->Glyphs()) {
    widths.insert(placed.width);
  }
  EXPECT_EQ(widths, (std::set<std::uint8_t>{12}));

  // With the italic's pages as well, both are held, and all six drawn.
  Fixture roomy;
  roomy.pages =
      fixture.pages + PagesOf(places[5].ranges, places[5].pictures + places[5].pictureBytes - places[5].ranges);
  roomy.Text("ab *cd* ef\n");
  roomy.Start();
  roomy.Settle();
  EXPECT_EQ(roomy.viewer->Glyphs().size(), 6U);
  EXPECT_EQ(roomy.LinesThatBegin("text: s").back().find("no room"), std::string::npos);
  EXPECT_EQ(roomy.map->MappedBytes(), roomy.pages * Fixture::kPageBytes);
}

TEST(TextViewer, GoesThroughEachFaceAtEachSizeByItselfOnATour) {
  Fixture fixture;
  fixture.tour = true;
  fixture.Text(ManyWords(200));
  fixture.Start();
  fixture.Settle();
  // From the smallest size: two pages in the serif and two in the sans;
  // then the next size, in the face it was left in, and then the other.
  const std::vector<std::string> pages = fixture.PagesSaid();
  EXPECT_EQ(pages,
            (std::vector<std::string>{"text: page 1, serif 20", "text: page 2, serif 20", "text: page 1, sans 20",
                                      "text: page 2, sans 20", "text: page 1, sans 30", "text: page 2, sans 30",
                                      "text: page 1, serif 30", "text: page 2, serif 30"}));
  EXPECT_TRUE(fixture.viewer->TourDone());
  EXPECT_EQ(fixture.LastLine(), "text: the tour is done.");
  EXPECT_EQ(fixture.viewer->Shown(), 8U);
  // No page lost its grays to the one after it: what the tour asks for, it
  // asks when a page has been shown, grays and all. (The sans has none.)
  for (const std::string& line : fixture.LinesThatBegin("text: page ")) {
    EXPECT_EQ(line.find("no grays"), std::string::npos) << line;
    if (line.find(" serif ") != std::string::npos) {
      EXPECT_NE(line.find("gray pass"), std::string::npos) << line;
    }
  }
  // The buttons are then as they always are.
  fixture.Press(Button::kRight);
  fixture.Settle();
  EXPECT_EQ(fixture.viewer->Shown(), 9U);
  EXPECT_EQ(fixture.viewer->Page(), 2U);
  EXPECT_EQ(fixture.LastLine().rfind("text: page 3, serif 30 px, ", 0), 0U) << fixture.LastLine();
}

TEST(TextViewer, OnATourShowsATextOfOnePageOnceInEachFaceAtEachSize) {
  // A sans alone, at two sizes, and text that ends on its first page.
  Fixture fixture({TestFace(1, 0, 20), TestFace(1, 0, 30)});
  fixture.tour = true;
  fixture.Text("ab cd\n");
  fixture.Start();
  fixture.Settle();
  const std::vector<std::string> pages = fixture.PagesSaid();
  EXPECT_EQ(pages, (std::vector<std::string>{"text: page 1, sans 20", "text: page 1, sans 30"}));
  EXPECT_TRUE(fixture.viewer->TourDone());

  // Without a tour, the first page is of the middle size, and no other
  // follows by itself.
  Fixture still({TestFace(1, 0, 20), TestFace(1, 0, 30)});
  still.Text("ab cd\n");
  still.Start();
  still.Settle();
  EXPECT_EQ(still.viewer->Size(), 30);
  EXPECT_EQ(still.viewer->Shown(), 1U);
  EXPECT_FALSE(still.viewer->TourDone());
}

}  // namespace
