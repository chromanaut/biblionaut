#include "draw/spool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "draw/fill.h"
#include "draw/fixed.h"
#include "draw/svg_render.h"

// A drawing kept by a SpoolWriter and given back a band at a time is set
// beside the same drawing drawn straight into each band: the two have to
// be the same, byte for byte.

namespace {

using bn::draw::Canvas;
using bn::draw::Filler;
using bn::draw::FillRule;
using bn::draw::Fixed;
using bn::draw::kFixedOne;
using bn::draw::Point;
using bn::draw::SpoolStatus;
using bn::draw::SpoolStore;
using bn::draw::SpoolWriter;
using bn::draw::SvgRenderer;

constexpr std::uint8_t kWhite = 255;

// A source of numbers that gives the same ones every run, so that a
// failure can be found again.
std::mt19937 Repeatable(std::uint32_t seed) { return std::mt19937(seed); }

// A store in memory, which can be made to fail, and counts its reads.
class MemoryStore final : public SpoolStore {
 public:
  std::vector<std::uint8_t> bytes;
  std::size_t failAfterChunks = SIZE_MAX;
  std::size_t reads = 0;

  bool Append(std::span<const std::uint8_t> more) noexcept override {
    if (bytes.size() / SpoolWriter::kChunkBytes >= failAfterChunks) {
      return false;
    }
    bytes.insert(bytes.end(), more.begin(), more.end());
    return true;
  }
  bool ReadAt(std::uint32_t offset, std::span<std::uint8_t> out) noexcept override {
    ++reads;
    if (offset + out.size() > bytes.size()) {
      return false;
    }
    std::copy_n(bytes.begin() + offset, out.size(), out.begin());
    return true;
  }
};

std::string Svg(std::size_t width, std::size_t height, std::string_view body) {
  return "<svg xmlns='http://www.w3.org/2000/svg' width='" + std::to_string(width) + "' height='" +
         std::to_string(height) + "'>" + std::string(body) + "</svg>";
}

constexpr Fixed Pixels(std::size_t count) { return static_cast<Fixed>(count * kFixedOne); }

void Feed(SvgRenderer& renderer, std::string_view file) {
  while (!file.empty()) {
    file.remove_prefix(renderer.Step(file));
  }
  static_cast<void>(renderer.Finish());
}

// The file drawn straight into bands of `rows` rows.
std::vector<std::uint8_t> Direct(std::string_view file, std::size_t width, std::size_t height, std::size_t rows) {
  std::vector<std::uint8_t> gray(width * height, kWhite);
  std::vector<std::int16_t> cells(Filler::CellsFor(width, rows));
  std::vector<std::uint8_t> kept(8192);
  SvgRenderer renderer;
  renderer.KeepDefinitionsIn(kept);
  for (std::size_t top = 0; top < height; top += rows) {
    const std::size_t here = std::min(rows, height - top);
    Filler filler;
    EXPECT_TRUE(
        filler.Begin(std::span(gray).subspan(top * width, here * width), width, static_cast<std::int32_t>(top), cells));
    renderer.Begin(&filler, Pixels(width), Pixels(height));
    Feed(renderer, file);
  }
  return gray;
}

// What a spooled drawing came to.
struct Spooled {
  std::vector<std::uint8_t> gray;
  SpoolStatus status = SpoolStatus::kOk;
  std::uint32_t chunks = 0;
  std::size_t mostFillsInAStep = 0;
  std::size_t mostReadsInAStep = 0;
};

// The file drawn once into a SpoolWriter, and given back into bands of
// `rows` rows.
Spooled Spool(std::string_view file, std::size_t width, std::size_t height, std::size_t rows, std::size_t chunks = 4096,
              MemoryStore* given = nullptr) {
  Spooled spooled;
  spooled.gray.assign(width * height, kWhite);
  MemoryStore own;
  MemoryStore& store = given != nullptr ? *given : own;
  const std::size_t bands = SpoolWriter::BandsFor(static_cast<std::int32_t>(height), static_cast<std::int32_t>(rows));
  std::vector<std::uint8_t> buffers(SpoolWriter::BufferBytesFor(bands));
  std::vector<std::uint8_t> chunkBands(chunks);
  SpoolWriter writer;
  EXPECT_TRUE(writer.Begin(static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
                           static_cast<std::int32_t>(rows), buffers, chunkBands, store));
  std::vector<std::uint8_t> kept(8192);
  SvgRenderer renderer;
  renderer.KeepDefinitionsIn(kept);
  renderer.Begin(&writer, Pixels(width), Pixels(height));
  Feed(renderer, file);
  spooled.status = writer.Finish();
  spooled.chunks = writer.Chunks();
  if (spooled.status != SpoolStatus::kOk) {
    return spooled;
  }
  std::vector<std::int16_t> cells(Filler::CellsFor(width, rows));
  std::vector<std::uint8_t> chunk(SpoolWriter::kChunkBytes);
  for (std::size_t band = 0; band < bands; ++band) {
    const std::size_t top = band * rows;
    const std::size_t here = std::min(rows, height - top);
    Filler filler;
    EXPECT_TRUE(filler.Begin(std::span(spooled.gray).subspan(top * width, here * width), width,
                             static_cast<std::int32_t>(top), cells));
    EXPECT_TRUE(writer.BeginBand(band, chunk));
    for (;;) {
      const std::uint32_t visited = filler.PixelsVisited();
      const std::size_t reads = store.reads;
      const bool more = writer.ReplayStep(filler);
      // A filling looks at no more than the band's pixels.
      spooled.mostFillsInAStep = std::max<std::size_t>(
          spooled.mostFillsInAStep, (filler.PixelsVisited() - visited + (width * here) - 1) / (width * here));
      spooled.mostReadsInAStep = std::max(spooled.mostReadsInAStep, store.reads - reads);
      if (!more) {
        break;
      }
    }
    spooled.status = writer.Status();
  }
  return spooled;
}

void ExpectTheSame(std::string_view file, std::size_t width, std::size_t height) {
  for (const std::size_t rows : {std::size_t{1}, std::size_t{7}, std::size_t{16}, height}) {
    const Spooled spooled = Spool(file, width, height, rows);
    ASSERT_EQ(spooled.status, SpoolStatus::kOk) << rows;
    EXPECT_EQ(spooled.gray, Direct(file, width, height, rows)) << "bands of " << rows << " rows: " << file;
    EXPECT_LE(spooled.mostReadsInAStep, 1u);
    EXPECT_LE(spooled.mostFillsInAStep, 1u);
  }
}

TEST(Spool, AShapeGivenBackIsTheShapeDrawn) {
  ExpectTheSame(Svg(40, 30, "<rect x='3.3' y='2.7' width='30' height='20' fill='#555'/>"), 40, 30);
  ExpectTheSame(Svg(40, 30,
                    "<circle cx='20' cy='15' r='14' fill-opacity='.5'/><path d='M0 0L40 30H0z' fill='#999' "
                    "fill-rule='evenodd'/>"),
                40, 30);
}

TEST(Spool, ATallPictureIsGivenBackAsItWasDrawn) {
  // Which bands a line crosses is worked out from where it lies, in 256ths
  // of a pixel: 300 rows are 76,800 of them, more than 16 bits hold. Shapes
  // from the top of the picture to its foot, and shapes that lie wholly in
  // its lower half, in bands of several heights.
  const std::string file =
      Svg(60, 300,
          "<path d='M5 2L55 150L5 298z'/><rect x='10' y='200.5' width='40' height='95' fill='#777'/>"
          "<circle cx='30' cy='262' r='25' fill-opacity='.5'/><path d='M58 130V299' stroke='#000'/>");
  for (const std::size_t rows : {std::size_t{10}, std::size_t{26}, std::size_t{64}, std::size_t{300}}) {
    const Spooled spooled = Spool(file, 60, 300, rows);
    ASSERT_EQ(spooled.status, SpoolStatus::kOk) << rows;
    EXPECT_EQ(spooled.gray, Direct(file, 60, 300, rows)) << "bands of " << rows << " rows";
  }
  // The shapes are there, in the lower half too.
  const std::vector<std::uint8_t> drawn = Direct(file, 60, 300, 300);
  EXPECT_NE(drawn[(250 * 60) + 30], kWhite);
  EXPECT_NE(drawn[(20 * 60) + 8], kWhite);
}

TEST(Spool, EveryKindOfFillingIsGivenBackAsItWasDrawn) {
  ExpectTheSame(Svg(48, 40,
                    "<linearGradient id='g' x2='0' y2='1' spreadMethod='reflect'><stop stop-color='#000'/>"
                    "<stop offset='.3' stop-color='#fff' stop-opacity='.5'/></linearGradient>"
                    "<rect width='48' height='40' fill='url(#g)'/>"
                    "<svg x='5.5' y='6.25' width='20' height='13'><circle cx='10' cy='6' r='11' fill='#333'/>"
                    "<rect width='30' height='30' fill='url(#g)'/></svg>"),
                48, 40);
}

TEST(Spool, StrokesAndDashesAreGivenBackAsTheyWereDrawn) {
  ExpectTheSame(Svg(48, 40,
                    "<path d='M2 2C20 40 30 -10 46 38' fill='none' stroke='#000' stroke-width='3' "
                    "stroke-dasharray='5 2' stroke-linecap='round' stroke-linejoin='round'/>"
                    "<polygon points='5,35 20,5 40,30' fill='#ccc' stroke='#444' stroke-width='1.5'/>"),
                48, 40);
}

TEST(Spool, WhatLiesBeyondThePictureAndWhatIsUsedAgainAreGivenBack) {
  std::string data = "M-20 -10";
  for (int step = 0; step < 400; ++step) {
    data += step % 2 == 0 ? "l.4 .3" : "c.1 .2 .3 -.1 .2 .2";
  }
  ExpectTheSame(Svg(40, 30,
                    "<path d='" + data +
                        "' fill='#777'/>"
                        "<defs><path id='leaf' d='M0 0c4-6 10-6 14 0c-4 6-10 6-14 0z'/></defs>"
                        "<use href='#leaf' x='5' y='20'/><use href='#leaf' transform='translate(30 5) rotate(60)'/>"
                        "<rect x='-10' y='25' width='80' height='40'/>"),
                40, 30);
}

TEST(Spool, DrawingsMadeAtRandomAreGivenBackAsTheyWereDrawn) {
  std::mt19937 random = Repeatable(1117);
  std::uniform_int_distribution<int> place(-10, 70);
  std::uniform_int_distribution<int> gray(0, 255);
  for (int round = 0; round < 30; ++round) {
    std::string body;
    for (int shape = 0; shape < 6; ++shape) {
      std::string data = "M" + std::to_string(place(random)) + " " + std::to_string(place(random));
      for (int command = 0; command < 4; ++command) {
        data += (command % 2 == 0 ? " L" : " Q") + std::to_string(place(random)) + " " + std::to_string(place(random));
        if (command % 2 == 1) {
          data += " " + std::to_string(place(random)) + " " + std::to_string(place(random));
        }
      }
      body += "<path d='" + data + (shape % 3 == 0 ? "z" : "") + "' fill='rgb(" + std::to_string(gray(random)) +
              ",0,0)'" +
              (shape % 2 == 0 ? " stroke='#000' stroke-width='" + std::to_string(1 + (shape % 4)) + "'" : "") + "/>";
    }
    ExpectTheSame(Svg(60, 45, body), 60, 45);
  }
}

TEST(Spool, SaysWhenThereIsNoRoomToSayAChunksBandOrTheStoreFails) {
  std::string body;
  for (int i = 0; i < 200; ++i) {
    body += "<circle cx='" + std::to_string(i % 40) + "' cy='" + std::to_string(i % 30) + "' r='5'/>";
  }
  const std::string file = Svg(40, 30, body);
  // Room for some of the chunks and not all.
  const Spooled all = Spool(file, 40, 30, 4);
  ASSERT_EQ(all.status, SpoolStatus::kOk);
  ASSERT_GT(all.chunks, 4u);
  EXPECT_EQ(Spool(file, 40, 30, 4, all.chunks - 1).status, SpoolStatus::kNoRoom);
  EXPECT_EQ(Spool(file, 40, 30, 4, all.chunks).status, SpoolStatus::kOk);
  MemoryStore failing;
  failing.failAfterChunks = 2;
  EXPECT_EQ(Spool(file, 40, 30, 4, 4096, &failing).status, SpoolStatus::kStoreFailed);
  EXPECT_STREQ(bn::draw::SpoolStatusName(SpoolStatus::kNoRoom), "no room");
}

TEST(Spool, WhatIsReadBackDamagedEndsInAStatus) {
  // Whatever the store gives back, giving a band back ends, in ok or a bad
  // record, and draws only into its strip.
  const std::string file = Svg(40, 30,
                               "<linearGradient id='g'><stop/><stop offset='1' stop-color='#fff'/></linearGradient>"
                               "<rect width='40' height='30' fill='url(#g)'/><path d='M0 0L40 30' stroke='#000'/>"
                               "<svg x='4' y='4' width='9' height='9'><circle cx='4' cy='4' r='6'/></svg>");
  MemoryStore store;
  std::vector<std::uint8_t> buffers(SpoolWriter::BufferBytesFor(SpoolWriter::BandsFor(30, 10)));
  std::vector<std::uint8_t> chunkBands(64);
  SpoolWriter writer;
  ASSERT_TRUE(writer.Begin(40, 30, 10, buffers, chunkBands, store));
  std::vector<std::uint8_t> kept(4096);
  SvgRenderer renderer;
  renderer.KeepDefinitionsIn(kept);
  renderer.Begin(&writer, Pixels(40), Pixels(30));
  Feed(renderer, file);
  ASSERT_EQ(writer.Finish(), SpoolStatus::kOk);
  const std::vector<std::uint8_t> written = store.bytes;
  std::mt19937 random = Repeatable(5);
  for (int round = 0; round < 300; ++round) {
    store.bytes = written;
    for (int damage = 0; damage < 1 + (round % 8); ++damage) {
      store.bytes[std::uniform_int_distribution<std::size_t>(0, written.size() - 1)(random)] =
          static_cast<std::uint8_t>(random());
    }
    std::vector<std::uint8_t> gray(std::size_t{40} * 10, kWhite);
    std::vector<std::int16_t> cells(Filler::CellsFor(40, 10));
    std::vector<std::uint8_t> chunk(SpoolWriter::kChunkBytes);
    for (std::size_t band = 0; band < writer.Bands() && writer.Status() == SpoolStatus::kOk; ++band) {
      Filler filler;
      ASSERT_TRUE(filler.Begin(gray, 40, static_cast<std::int32_t>(band * 10), cells));
      ASSERT_TRUE(writer.BeginBand(band, chunk));
      int steps = 0;
      while (writer.ReplayStep(filler)) {
        ASSERT_LT(++steps, 10000);
      }
    }
    EXPECT_TRUE(writer.Status() == SpoolStatus::kOk || writer.Status() == SpoolStatus::kBadRecord);
  }
}

// Where the outlines below begin: 200 pixels across and 8 down, in 256ths
// of a pixel.
constexpr std::int64_t kStartX = 51200;
constexpr std::int64_t kStartY = 2048;

// A place on a canvas, in 256ths of a pixel, which is what a line is kept
// in.
constexpr Point At(std::int64_t x, std::int64_t y) {
  return {.x = static_cast<Fixed>(x * (kFixedOne / 256)), .y = static_cast<Fixed>(y * (kFixedOne / 256))};
}

// What is drawn on a canvas, kept in one band and given back: how many
// bytes of the one chunk were used, and whether the picture is the picture
// the same drawing gives drawn straight into a filler.
struct Kept {
  std::size_t bytes = 0;
  bool same = false;
};
template <typename Drawn>
Kept KeptInOneBand(const Drawn& draw) {
  const std::size_t width = 400;
  const std::size_t rows = 16;
  std::vector<std::int16_t> cells(Filler::CellsFor(width, rows));
  std::vector<std::uint8_t> direct(width * rows, kWhite);
  Filler straight;
  EXPECT_TRUE(straight.Begin(direct, width, 0, cells));
  draw(straight);

  MemoryStore store;
  std::vector<std::uint8_t> buffers(SpoolWriter::BufferBytesFor(1));
  std::vector<std::uint8_t> chunkBands(8);
  SpoolWriter writer;
  EXPECT_TRUE(writer.Begin(static_cast<std::int32_t>(width), static_cast<std::int32_t>(rows),
                           static_cast<std::int32_t>(rows), buffers, chunkBands, store));
  draw(writer);
  EXPECT_EQ(writer.Finish(), SpoolStatus::kOk);
  EXPECT_EQ(writer.Chunks(), 1u);
  std::vector<std::uint8_t> given(width * rows, kWhite);
  std::vector<std::uint8_t> chunk(SpoolWriter::kChunkBytes);
  Filler filler;
  EXPECT_TRUE(filler.Begin(given, width, 0, cells));
  EXPECT_TRUE(writer.BeginBand(0, chunk));
  while (writer.ReplayStep(filler)) {
  }
  EXPECT_EQ(writer.Status(), SpoolStatus::kOk);
  Kept kept;
  kept.same = given == direct;
  // A chunk is filled out to its end with the byte that ends one, and the
  // last thing kept here is a filling as solid as can be, which ends in
  // another.
  kept.bytes = store.bytes.size();
  while (kept.bytes > 0 && store.bytes[kept.bytes - 1] == 0xC0) {
    --kept.bytes;
  }
  return kept;
}

TEST(Spool, ALineThatGoesOnTakesAsFewBytesAsItsStepsNeed) {
  // An outline of lines each of which goes on from the last, and comes back
  // to where it began: each step in 256ths of a pixel, and the bytes it
  // should take. Two if it goes less than a quarter of a pixel across and
  // half a pixel down; three if less than four pixels each way; five if
  // less than 128; nine beyond.
  struct Step {
    std::int64_t dx;
    std::int64_t dy;
    std::size_t bytes;
  };
  const std::vector<Step> steps = {
      {.dx = 63, .dy = 127, .bytes = 2},     {.dx = -63, .dy = -127, .bytes = 2},
      {.dx = -64, .dy = -128, .bytes = 2},   {.dx = 64, .dy = 128, .bytes = 3},
      {.dx = 64, .dy = 1, .bytes = 3},       {.dx = -64, .dy = -1, .bytes = 2},
      {.dx = 1, .dy = 128, .bytes = 3},      {.dx = -1, .dy = -128, .bytes = 2},
      {.dx = 1023, .dy = -1024, .bytes = 3}, {.dx = -1023, .dy = 1024, .bytes = 5},
      {.dx = 1024, .dy = 1, .bytes = 5},     {.dx = -1024, .dy = -1, .bytes = 3},
      {.dx = 1, .dy = -1025, .bytes = 5},    {.dx = -1, .dy = 1025, .bytes = 5},
      {.dx = 32767, .dy = 1, .bytes = 5},    {.dx = -32767, .dy = -1, .bytes = 5},
      {.dx = 32768, .dy = 1, .bytes = 9},    {.dx = -32768, .dy = -1, .bytes = 5},
      {.dx = 1, .dy = 40000, .bytes = 9},    {.dx = -1, .dy = -40000, .bytes = 9},
  };
  std::size_t expected = 7 + 4;  // The move to where it begins, and its filling.
  for (const Step& step : steps) {
    expected += step.bytes;
  }
  const Kept kept = KeptInOneBand([&steps](Canvas& canvas) {
    std::int64_t x = kStartX;
    std::int64_t y = kStartY;
    canvas.MoveTo(At(x, y));
    for (const Step& step : steps) {
      x += step.dx;
      y += step.dy;
      canvas.LineTo(At(x, y));
    }
    ASSERT_EQ(x, kStartX);
    ASSERT_EQ(y, kStartY);
    canvas.Fill(FillRule::kNonZero, 0x40, 255);
  });
  EXPECT_EQ(kept.bytes, expected);
  EXPECT_TRUE(kept.same);
}

TEST(Spool, ALineThatBeginsSomewhereElseIsAMoveAsShortAsWillSayIt) {
  // Outlines of one short line each, which is two bytes, and the line that
  // closes it, two more: so the band's last line ended where the outline
  // began. Each begins so far from the one before, in 256ths of a pixel,
  // and the move to it should take so many bytes: four within eight
  // pixels each way, seven where three bytes hold the place, and nine
  // beyond that.
  struct Hop {
    std::int64_t dx;
    std::int64_t dy;
    std::size_t bytes;
  };
  const std::vector<Hop> hops = {
      {.dx = kStartX, .dy = kStartY, .bytes = 7},  // The first: there is nowhere to be near.
      {.dx = 2047, .dy = -2048, .bytes = 4},
      {.dx = -2048, .dy = 2047, .bytes = 4},
      {.dx = 2048, .dy = 0, .bytes = 7},
      {.dx = 0, .dy = -2049, .bytes = 7},
      {.dx = 8388607 - kStartX - 2047, .dy = 2049, .bytes = 7},  // To the last place three bytes hold.
      {.dx = 1, .dy = 0, .bytes = 4},                            // One past it is near, all the same;
      {.dx = 4000, .dy = 0, .bytes = 9},                         // further past it, not;
      {.dx = -4000, .dy = 0, .bytes = 9},                        // nor back to one past it, from there.
      {.dx = -8388608 - 8388608, .dy = 0, .bytes = 7},           // To the first place three bytes hold.
      {.dx = -1, .dy = 0, .bytes = 4},
      {.dx = -4000, .dy = 0, .bytes = 9},
      {.dx = 4000, .dy = 0, .bytes = 9},  // To one before the first, from afar.
  };
  std::size_t expected = 4;  // The one filling.
  for (const Hop& hop : hops) {
    expected += hop.bytes + 2 + 2;
  }
  const Kept kept = KeptInOneBand([&hops](Canvas& canvas) {
    std::int64_t x = 0;
    std::int64_t y = 0;
    for (const Hop& hop : hops) {
      x += hop.dx;
      y += hop.dy;
      canvas.MoveTo(At(x, y));
      canvas.LineTo(At(x + 10, y + 20));
      canvas.Close();
    }
    canvas.Fill(FillRule::kNonZero, 0x40, 255);
  });
  EXPECT_EQ(kept.bytes, expected);
  EXPECT_TRUE(kept.same);
}

TEST(Spool, RefusesWhatItCannotRecord) {
  MemoryStore store;
  std::vector<std::uint8_t> buffers(SpoolWriter::BufferBytesFor(2));
  std::vector<std::uint8_t> chunkBands(8);
  SpoolWriter writer;
  EXPECT_FALSE(writer.Begin(40, 0, 16, buffers, chunkBands, store));
  EXPECT_FALSE(writer.Begin(0, 30, 16, buffers, chunkBands, store));
  EXPECT_FALSE(writer.Begin(40, 33, 16, buffers, chunkBands, store)) << "three bands, room for two";
  EXPECT_FALSE(writer.Begin(40, 30, 16, buffers, {}, store));
  EXPECT_FALSE(writer.Begin(40, 16 * 65, 16, buffers, chunkBands, store)) << "more bands than are kept";
  EXPECT_TRUE(writer.Begin(40, 30, 16, buffers, chunkBands, store));
  std::vector<std::uint8_t> chunk(SpoolWriter::kChunkBytes);
  EXPECT_FALSE(writer.BeginBand(0, chunk)) << "not finished";
  EXPECT_EQ(writer.Finish(), SpoolStatus::kOk);
  EXPECT_TRUE(writer.BeginBand(1, chunk));
  EXPECT_FALSE(writer.BeginBand(2, chunk));
}

}  // namespace
