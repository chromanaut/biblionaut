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

// A store in memory, which can be made to fail, and counts its reads and
// keeps what each asked for.
class MemoryStore final : public SpoolStore {
 public:
  struct Asked {
    std::uint32_t offset = 0;
    std::size_t size = 0;
    bool operator==(const Asked&) const = default;
  };

  std::vector<std::uint8_t> bytes;
  std::size_t failAfterChunks = SIZE_MAX;
  std::size_t failAfterReads = SIZE_MAX;
  std::size_t reads = 0;
  std::vector<Asked> asked;

  bool Append(std::span<const std::uint8_t> more) noexcept override {
    if (bytes.size() / SpoolWriter::kChunkBytes >= failAfterChunks) {
      return false;
    }
    bytes.insert(bytes.end(), more.begin(), more.end());
    return true;
  }
  bool ReadAt(std::uint32_t offset, std::span<std::uint8_t> out) noexcept override {
    ++reads;
    asked.push_back({.offset = offset, .size = out.size()});
    if (offset + out.size() > bytes.size() || reads > failAfterReads) {
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
  std::size_t largestRead = 0;
  // What each band asked its store for, and how many steps it was given
  // back in.
  std::vector<std::vector<MemoryStore::Asked>> asked;
  std::vector<std::size_t> steps;
};

// The file drawn once into a SpoolWriter, and given back into bands of
// `rows` rows, through memory for `together` chunks.
Spooled Spool(std::string_view file, std::size_t width, std::size_t height, std::size_t rows, std::size_t links = 4096,
              MemoryStore* given = nullptr, std::size_t together = 1) {
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
  // A few bytes more than the chunks it is for: only whole chunks are used.
  std::vector<std::uint8_t> memory((together * SpoolWriter::kChunkBytes) + 7);
  for (std::size_t band = 0; band < bands; ++band) {
    const std::size_t top = band * rows;
    const std::size_t here = std::min(rows, height - top);
    Filler filler;
    EXPECT_TRUE(filler.Begin(std::span(spooled.gray).subspan(top * width, here * width), width,
                             static_cast<std::int32_t>(top), cells));
    EXPECT_TRUE(writer.BeginBand(band, memory));
    store.asked.clear();
    std::size_t steps = 0;
    for (;;) {
      const std::uint32_t visited = filler.PixelsVisited();
      const std::size_t reads = store.reads;
      const bool more = writer.ReplayStep(filler);
      ++steps;
      // A filling looks at no more than the band's pixels.
      spooled.mostFillsInAStep = std::max<std::size_t>(
          spooled.mostFillsInAStep, (filler.PixelsVisited() - visited + (width * here) - 1) / (width * here));
      spooled.mostReadsInAStep = std::max(spooled.mostReadsInAStep, store.reads - reads);
      if (!more) {
        break;
      }
    }
    for (const MemoryStore::Asked& read : store.asked) {
      spooled.largestRead = std::max(spooled.largestRead, read.size);
    }
    spooled.asked.push_back(store.asked);
    spooled.steps.push_back(steps);
    spooled.status = writer.Status();
    if (spooled.status != SpoolStatus::kOk) {
      break;  // Nothing more can be given back.
    }
  }
  return spooled;
}

void ExpectTheSame(std::string_view file, std::size_t width, std::size_t height) {
  for (const std::size_t rows : {std::size_t{1}, std::size_t{7}, std::size_t{16}, height}) {
    const std::vector<std::uint8_t> direct = Direct(file, width, height, rows);
    // Through memory for one chunk, for three and for sixteen.
    for (const std::size_t together : {std::size_t{1}, std::size_t{3}, std::size_t{16}}) {
      const Spooled spooled = Spool(file, width, height, rows, 4096, nullptr, together);
      ASSERT_EQ(spooled.status, SpoolStatus::kOk) << rows;
      EXPECT_EQ(spooled.gray, direct) << "bands of " << rows << " rows, " << together << " chunks together: " << file;
      EXPECT_LE(spooled.mostReadsInAStep, 1u);
      EXPECT_LE(spooled.largestRead, together * SpoolWriter::kChunkBytes);
      EXPECT_LE(spooled.mostFillsInAStep, 1u);
    }
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

TEST(Spool, ChunksOfABandThatLieTogetherInTheStoreAreReadTogether) {
  // Two bands. A long outline in the upper one alone, whose chunks lie one
  // after another in the store; then small shapes in the two by turns,
  // whose chunks lie by turns; then a long outline in the lower one.
  std::string upper = "M2 2";
  std::string lower = "M2 22";
  for (int step = 0; step < 2600; ++step) {
    upper += step % 2 == 0 ? "l.02 .01" : "l.01 -.01";
    lower += step % 2 == 0 ? "l.02 .01" : "l.01 -.01";
  }
  std::string turns;
  for (int shape = 0; shape < 160; ++shape) {
    turns += "<circle cx='" + std::to_string(4 + (shape % 30)) + "' cy='" + (shape % 2 == 0 ? "8" : "24") + "' r='3'/>";
  }
  const std::string file = Svg(40, 32, "<path d='" + upper + "z'/>" + turns + "<path d='" + lower + "z'/>");
  const std::size_t chunk = SpoolWriter::kChunkBytes;

  // A chunk at a time, every read is one chunk, and says which chunks a
  // band has and in what order.
  const Spooled single = Spool(file, 40, 32, 16);
  ASSERT_EQ(single.status, SpoolStatus::kOk);
  ASSERT_EQ(single.asked.size(), 2u);
  std::size_t chunks = 0;
  for (const std::vector<MemoryStore::Asked>& band : single.asked) {
    for (const MemoryStore::Asked& read : band) {
      EXPECT_EQ(read.size, chunk);
      EXPECT_EQ(read.offset % chunk, 0u);
    }
    chunks += band.size();
  }
  EXPECT_EQ(chunks, single.chunks);

  for (const std::size_t together : {std::size_t{2}, std::size_t{4}, std::size_t{16}}) {
    const Spooled spooled = Spool(file, 40, 32, 16, 4096, nullptr, together);
    ASSERT_EQ(spooled.status, SpoolStatus::kOk) << together;
    EXPECT_EQ(spooled.gray, single.gray) << together;
    // No more steps and no fewer: a step still gives back one chunk.
    EXPECT_EQ(spooled.steps, single.steps) << together;
    std::size_t longer = 0;
    std::size_t alone = 0;
    for (std::size_t band = 0; band < 2; ++band) {
      // What it should have asked for: the band's chunks in their order,
      // each run of them that lie one after another as one read, no longer
      // than the memory.
      std::vector<MemoryStore::Asked> expected;
      for (const MemoryStore::Asked& one : single.asked[band]) {
        if (!expected.empty() && expected.back().offset + expected.back().size == one.offset &&
            expected.back().size < together * chunk) {
          expected.back().size += chunk;
        } else {
          expected.push_back(one);
        }
      }
      EXPECT_EQ(spooled.asked[band], expected) << "band " << band << ", " << together << " together";
      for (const MemoryStore::Asked& read : expected) {
        longer += read.size > chunk ? 1 : 0;
        alone += read.size == chunk ? 1 : 0;
      }
    }
    // The drawing has both: chunks read together, as many as the memory
    // holds, and chunks that have another band's on either side.
    EXPECT_EQ(spooled.largestRead, together * chunk) << together;
    EXPECT_GE(longer, 4u) << together;
    EXPECT_GT(alone, 4u) << together;
  }

  // Kept as one band, every chunk follows the one before it, to the last
  // in the store: they are read as many at a time as the memory holds, and
  // the last read ends where the store does.
  for (const std::size_t together : {std::size_t{1}, std::size_t{4}, std::size_t{16}}) {
    MemoryStore store;
    const Spooled spooled = Spool(file, 40, 32, 32, 4096, &store, together);
    ASSERT_EQ(spooled.status, SpoolStatus::kOk) << together;
    ASSERT_EQ(spooled.asked.size(), 1u);
    const std::vector<MemoryStore::Asked>& reads = spooled.asked[0];
    ASSERT_EQ(reads.size(), (spooled.chunks + together - 1) / together) << together;
    for (std::size_t read = 0; read < reads.size(); ++read) {
      EXPECT_EQ(reads[read].offset, read * together * chunk) << together;
      EXPECT_EQ(reads[read].size, std::min(together * chunk, store.bytes.size() - reads[read].offset)) << together;
    }
    EXPECT_EQ(spooled.gray, Direct(file, 40, 32, 32)) << together;
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
  // Or gives back some of what was kept and then no more, a chunk at a
  // time or several.
  for (const std::size_t together : {std::size_t{1}, std::size_t{4}}) {
    MemoryStore unreadable;
    unreadable.failAfterReads = 3;
    EXPECT_EQ(Spool(file, 40, 30, 4, 4096, &unreadable, together).status, SpoolStatus::kStoreFailed) << together;
  }
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
    // Read a chunk at a time in some rounds, and several in others.
    std::vector<std::uint8_t> chunk(SpoolWriter::kChunkBytes * static_cast<std::size_t>(1 + (round % 5)));
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
