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

using bn::draw::Filler;
using bn::draw::Fixed;
using bn::draw::kFixedOne;
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
Spooled Spool(std::string_view file, std::size_t width, std::size_t height, std::size_t rows, std::size_t links = 4096,
              MemoryStore* given = nullptr) {
  Spooled spooled;
  spooled.gray.assign(width * height, kWhite);
  MemoryStore own;
  MemoryStore& store = given != nullptr ? *given : own;
  const std::size_t bands = SpoolWriter::BandsFor(static_cast<std::int32_t>(height), static_cast<std::int32_t>(rows));
  std::vector<std::uint8_t> buffers(SpoolWriter::BufferBytesFor(bands));
  std::vector<std::uint16_t> link(links);
  SpoolWriter writer;
  EXPECT_TRUE(writer.Begin(static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
                           static_cast<std::int32_t>(rows), buffers, link, store));
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

TEST(Spool, SaysWhenTheLinksHaveNoRoomOrTheStoreFails) {
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
  std::vector<std::uint16_t> links(64);
  SpoolWriter writer;
  ASSERT_TRUE(writer.Begin(40, 30, 10, buffers, links, store));
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

TEST(Spool, RefusesWhatItCannotRecord) {
  MemoryStore store;
  std::vector<std::uint8_t> buffers(SpoolWriter::BufferBytesFor(2));
  std::vector<std::uint16_t> links(8);
  SpoolWriter writer;
  EXPECT_FALSE(writer.Begin(40, 0, 16, buffers, links, store));
  EXPECT_FALSE(writer.Begin(0, 30, 16, buffers, links, store));
  EXPECT_FALSE(writer.Begin(40, 33, 16, buffers, links, store)) << "three bands, room for two";
  EXPECT_FALSE(writer.Begin(40, 30, 16, buffers, {}, store));
  EXPECT_FALSE(writer.Begin(40, 16 * 65, 16, buffers, links, store)) << "more bands than are kept";
  EXPECT_TRUE(writer.Begin(40, 30, 16, buffers, links, store));
  std::vector<std::uint8_t> chunk(SpoolWriter::kChunkBytes);
  EXPECT_FALSE(writer.BeginBand(0, chunk)) << "not finished";
  EXPECT_EQ(writer.Finish(), SpoolStatus::kOk);
  EXPECT_TRUE(writer.BeginBand(1, chunk));
  EXPECT_FALSE(writer.BeginBand(2, chunk));
}

}  // namespace
