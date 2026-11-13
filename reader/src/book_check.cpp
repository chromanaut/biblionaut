#include "reader/book_check.h"

#include <algorithm>
#include <cstring>

#include "core/checksum.h"
#include "core/fixed_text.h"
#include "core/metrics.h"
#include "core/trace.h"
#include "epub/package_digest.h"
#include "reader/trace_ids.h"

namespace bn::reader {

namespace {

using core::StorageStatus;
using epub::ZipStatus;

// 1 if the last check of the books passed, 0 if it failed.
core::Gauge bookCheck{"book_check"};
// How many books the last check read.
core::Gauge bookCheckBooks{"book_check_books"};
// What the last check found the processor's speed to be, in KB (of 1024
// bytes) a second, storage's time left out: at inflating and checksumming
// together, counted in what came out; and at the checksum alone.
core::Gauge inflateKbPerSecond{"inflate_kb_per_s"};
core::Gauge checksumKbPerSecond{"checksum_kb_per_s"};
// And at parsing XML, counted in what the parser was given. The check's
// own counting and checksumming of what was found is in this.
core::Gauge xmlKbPerSecond{"xml_kb_per_s"};

constexpr std::string_view kBooksDirectory = "/books";
constexpr std::string_view kTopDirectory = "/";

// The checksum is timed over the chunk, this many times at each step.
constexpr std::size_t kChecksumRoundsPerStep = 16;
constexpr std::size_t kChecksumRounds = BookCheck::kChecksumBytes / BookCheck::kChunkBytes;

// A line of text put together piece by piece. What does not fit is left
// off the end.
class Line {
 public:
  Line& Text(std::string_view text) noexcept {
    for (const char ch : text) {
      Put(ch);
    }
    return *this;
  }
  Line& Number(std::uint64_t value) noexcept {
    std::array<char, 20> digits{};
    std::size_t count = 0;
    do {
      digits[count++] = static_cast<char>('0' + (value % 10));
      value /= 10;
    } while (value != 0);
    while (count > 0) {
      Put(digits[--count]);
    }
    return *this;
  }
  // Eight digits of hexadecimal, as checksums are written.
  Line& Hex8(std::uint32_t value) noexcept {
    constexpr std::string_view kDigits = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4) {
      Put(kDigits[(value >> static_cast<unsigned>(shift)) & 0xFU]);
    }
    return *this;
  }
  // A number and what it counts: "1 read", "2 reads".
  Line& Count(std::uint64_t value, std::string_view one, std::string_view many) noexcept {
    return Number(value).Text(value == 1 ? one : many);
  }
  std::string_view View() const noexcept { return {text_.data(), length_}; }
  // Starts another line in the same place.
  void Clear() noexcept { length_ = 0; }

 private:
  void Put(char ch) noexcept {
    if (length_ < text_.size()) {
      text_[length_++] = ch;
    }
  }

  std::array<char, core::kMaxTextLength> text_{};
  std::size_t length_ = 0;
};

// KB (of 1024 bytes) a second; 0 if no time passed.
std::uint64_t KbPerSecond(std::uint64_t bytes, std::uint64_t microseconds) noexcept {
  return microseconds == 0 ? 0 : (bytes * 1'000'000) / microseconds / 1024;
}

std::uint32_t Clamped(std::uint64_t value) noexcept {
  return value > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(value);
}

// Whether a file of this name is taken for a book: it ends in ".epub", in
// capitals or not, and does not begin with a dot, as the files do that
// some computers leave beside the real ones.
bool IsBookName(std::string_view name) noexcept {
  constexpr std::string_view kEnding = ".epub";
  if (name.size() <= kEnding.size() || name.front() == '.') {
    return false;
  }
  const std::size_t start = name.size() - kEnding.size();
  for (std::size_t i = 0; i < kEnding.size(); ++i) {
    char ch = name[start + i];
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (ch != kEnding[i]) {
      return false;
    }
  }
  return true;
}

// "card ... ; inflating ... ; all ... ms.", the times of one book or of
// all of them, added to `line`.
void AddTimes(Line& line, const BookCheck::Figures& figures) noexcept {
  line.Text("card ")
      .Number(figures.storageUs / 1000)
      .Text(" ms for ")
      .Count(figures.reads, " read of ", " reads of ")
      .Number(figures.readBytes)
      .Text(" bytes (")
      .Number(KbPerSecond(figures.readBytes, figures.storageUs))
      .Text(" KB/s); inflating ")
      .Number(figures.inflateUs / 1000)
      .Text(" ms: ")
      .Number(figures.inflatedIn)
      .Text(" bytes in (")
      .Number(KbPerSecond(figures.inflatedIn, figures.inflateUs))
      .Text(" KB/s), ")
      .Number(figures.inflatedOut)
      .Text(" out (")
      .Number(KbPerSecond(figures.inflatedOut, figures.inflateUs))
      .Text(" KB/s); all ")
      .Number(figures.workUs / 1000)
      .Text(" ms.");
}

// "XML: ... documents, ...", what parsing the XML of one book or of all
// of them came to, added to `line`. A book's line carries the checksum of
// what was found and the most memory a document of it used; the line for
// all of them, which has no one checksum, says how much of what was found
// the check itself checksummed, and the memory has a line of its own.
void AddXml(Line& line, const BookCheck::Figures& figures, const std::uint32_t* checksum) noexcept {
  line.Text("XML: ")
      .Count(figures.xmlDocuments, " document, ", " documents, ")
      .Number(figures.xmlBytes)
      .Text(" bytes, ")
      .Count(figures.xmlEvents, " event", " events");
  if (checksum != nullptr) {
    line.Text(", checksum ").Hex8(*checksum);
  }
  line.Text("; parsing ")
      .Number(figures.xmlUs / 1000)
      .Text(" ms (")
      .Number(KbPerSecond(figures.xmlBytes, figures.xmlUs))
      .Text(" KB/s); ");
  if (checksum != nullptr) {
    line.Text("most memory ").Number(figures.xmlMostMemory);
  } else {
    line.Number(figures.xmlEventBytes).Text(" bytes of events checksummed");
  }
  line.Text(".");
  if (figures.xmlNotXml != 0) {
    line.Text(" Not XML: ").Number(figures.xmlNotXml).Text(".");
  }
  if (figures.xmlRefused != 0) {
    line.Text(" Refused for memory: ").Number(figures.xmlRefused).Text(".");
  }
}

// How the documents' use of the parser's memory was spread.
void AddXmlMemory(Line& line, const BookCheck::Figures& figures) noexcept {
  line.Text("XML memory: the most a document used ")
      .Number(figures.xmlMostMemory)
      .Text(" bytes; documents over 8 KB: ")
      .Number(figures.xmlOver8K)
      .Text(", over 12 KB: ")
      .Number(figures.xmlOver12K)
      .Text(", over 16 KB: ")
      .Number(figures.xmlOver16K)
      .Text(".");
}

// "... ms: card ... ms for ... reads of ... bytes", what a piece of
// reading cost, added to `line`.
void AddCost(Line& line, std::uint64_t allUs, std::uint64_t storageUs, std::uint32_t reads,
             std::uint64_t readBytes) noexcept {
  line.Number(allUs / 1000)
      .Text(" ms: card ")
      .Number(storageUs / 1000)
      .Text(" ms for ")
      .Count(reads, " read of ", " reads of ")
      .Number(readBytes)
      .Text(" bytes");
}

// What reading one package cost, or all of them.
void AddPackageCost(Line& line, const BookCheck::Figures& figures) noexcept {
  AddCost(line, figures.packageUs, figures.packageStorageUs, figures.packageReads, figures.packageReadBytes);
}

// And one table of contents, or all of them.
void AddTocCost(Line& line, const BookCheck::Figures& figures) noexcept {
  AddCost(line, figures.tocUs, figures.tocStorageUs, figures.tocReads, figures.tocReadBytes);
}

// One of the names a book gives itself, as it is shown: in quotation
// marks, and cut to `Room` bytes at a character with "..." after it if it
// is longer; or that the book has none.
template <std::size_t Room>
void AddName(Line& line, std::string_view what, std::string_view name) noexcept {
  if (name.empty()) {
    line.Text("no ").Text(what);
    return;
  }
  core::FixedText<Room> shown;
  shown.Append(name);
  line.Text(what).Text(" \"").Text(shown.View()).Text(shown.Truncated() ? "...\"" : "\"");
}

void Add(BookCheck::Figures& total, const BookCheck::Figures& book) noexcept {
  total.entries += book.entries;
  total.failedEntries += book.failedEntries;
  total.bytesOut += book.bytesOut;
  total.bytesIn += book.bytesIn;
  total.inflatedOut += book.inflatedOut;
  total.inflatedIn += book.inflatedIn;
  total.reads += book.reads;
  total.readBytes += book.readBytes;
  total.storageUs += book.storageUs;
  total.inflateUs += book.inflateUs;
  total.workUs += book.workUs;
  total.xmlDocuments += book.xmlDocuments;
  total.xmlNotXml += book.xmlNotXml;
  total.xmlRefused += book.xmlRefused;
  total.xmlBytes += book.xmlBytes;
  total.xmlEvents += book.xmlEvents;
  total.xmlEventBytes += book.xmlEventBytes;
  total.xmlUs += book.xmlUs;
  total.xmlMostMemory = std::max(total.xmlMostMemory, book.xmlMostMemory);
  total.xmlOver8K += book.xmlOver8K;
  total.xmlOver12K += book.xmlOver12K;
  total.xmlOver16K += book.xmlOver16K;
  total.packagesRead += book.packagesRead;
  total.packagesNotRead += book.packagesNotRead;
  total.packageReads += book.packageReads;
  total.packageReadBytes += book.packageReadBytes;
  total.packageStorageUs += book.packageStorageUs;
  total.packageUs += book.packageUs;
  total.packageLongestUs = std::max(total.packageLongestUs, book.packageLongestUs);
  total.packageMostMemory = std::max(total.packageMostMemory, book.packageMostMemory);
  total.tocsRead += book.tocsRead;
  total.tocsNav += book.tocsNav;
  total.tocsNone += book.tocsNone;
  total.tocsNotRead += book.tocsNotRead;
  total.tocEntries += book.tocEntries;
  total.tocReads += book.tocReads;
  total.tocReadBytes += book.tocReadBytes;
  total.tocStorageUs += book.tocStorageUs;
  total.tocUs += book.tocUs;
  total.tocLongestUs = std::max(total.tocLongestUs, book.tocLongestUs);
}

}  // namespace

// ---- The meter ----

namespace {

// Adds the time from its making to its end to `total`.
class Stopwatch {
 public:
  Stopwatch(core::Clock& clock, std::uint64_t& total) noexcept : clock_(clock), total_(total), began_(clock.NowUs()) {}
  ~Stopwatch() { total_ += clock_.NowUs() - began_; }

  Stopwatch(const Stopwatch&) = delete;
  Stopwatch& operator=(const Stopwatch&) = delete;

 private:
  core::Clock& clock_;
  std::uint64_t& total_;
  std::uint64_t began_;
};

}  // namespace

StorageStatus BookCheck::Meter::Start() noexcept {
  const Stopwatch timed(clock_, counts_.timeUs);
  return storage_.Start();
}

StorageStatus BookCheck::Meter::Open(std::string_view path, core::OpenMode mode, core::FileHandle& file) noexcept {
  const Stopwatch timed(clock_, counts_.timeUs);
  return storage_.Open(path, mode, file);
}

void BookCheck::Meter::Close(core::FileHandle file) noexcept { storage_.Close(file); }

StorageStatus BookCheck::Meter::Size(core::FileHandle file, std::uint32_t& size) noexcept {
  const Stopwatch timed(clock_, counts_.timeUs);
  return storage_.Size(file, size);
}

StorageStatus BookCheck::Meter::ReadAt(core::FileHandle file, std::uint32_t offset, std::span<std::uint8_t> dst,
                                       std::size_t& read) noexcept {
  const Stopwatch timed(clock_, counts_.timeUs);
  const StorageStatus status = storage_.ReadAt(file, offset, dst, read);
  ++counts_.reads;
  counts_.readBytes += read;
  return status;
}

// The check changes nothing, so the calls that would are passed on untimed.
StorageStatus BookCheck::Meter::WriteAt(core::FileHandle file, std::uint32_t offset,
                                        std::span<const std::uint8_t> src) noexcept {
  return storage_.WriteAt(file, offset, src);
}

StorageStatus BookCheck::Meter::Sync(core::FileHandle file) noexcept { return storage_.Sync(file); }

StorageStatus BookCheck::Meter::Stat(std::string_view path, core::FileInfo& info) noexcept {
  const Stopwatch timed(clock_, counts_.timeUs);
  return storage_.Stat(path, info);
}

StorageStatus BookCheck::Meter::Remove(std::string_view path) noexcept { return storage_.Remove(path); }

StorageStatus BookCheck::Meter::Rename(std::string_view from, std::string_view to) noexcept {
  return storage_.Rename(from, to);
}

StorageStatus BookCheck::Meter::MakeDirectory(std::string_view path) noexcept { return storage_.MakeDirectory(path); }

StorageStatus BookCheck::Meter::OpenDirectory(std::string_view path, core::DirectoryHandle& directory) noexcept {
  const Stopwatch timed(clock_, counts_.timeUs);
  return storage_.OpenDirectory(path, directory);
}

StorageStatus BookCheck::Meter::NextInDirectory(core::DirectoryHandle directory, std::span<char> name,
                                                std::size_t& length, core::FileInfo& info) noexcept {
  const Stopwatch timed(clock_, counts_.timeUs);
  return storage_.NextInDirectory(directory, name, length, info);
}

void BookCheck::Meter::CloseDirectory(core::DirectoryHandle directory) noexcept { storage_.CloseDirectory(directory); }

// ---- The check ----

// Its size is what a build that runs it takes from the heap.
static_assert(sizeof(BookCheck) < 112640, "the book check has grown past 110 KB");

// ---- What feeds the parser ----

void BookCheck::Feeder::Start() noexcept {
  pending_ = {};
  ended_ = false;
  failed_ = false;
  given_ = 0;
}

std::ptrdiff_t BookCheck::Feeder::Read(std::span<std::uint8_t> dst) noexcept {
  if (pending_.empty() && !ended_) {
    const std::uint64_t began = owner_.clock_.NowUs();
    const std::ptrdiff_t read = owner_.PullChunk();
    sourceUs_ += owner_.clock_.NowUs() - began;
    if (read <= 0) {
      ended_ = true;
      failed_ = read < 0;
    } else {
      pending_ = std::span<const std::uint8_t>(owner_.chunk_).first(static_cast<std::size_t>(read));
    }
  }
  if (failed_) {
    return kError;
  }
  const std::size_t count = std::min(dst.size(), pending_.size());
  if (count != 0) {
    std::memcpy(dst.data(), pending_.data(), count);
    pending_ = pending_.subspan(count);
    given_ += count;
  }
  return static_cast<std::ptrdiff_t>(count);
}

BookCheck::BookCheck(core::Storage& storage, core::Clock& clock, core::TraceStream& stream) noexcept
    : clock_(clock),
      stream_(stream),
      meter_(storage, clock),
      directoryFile_(directoryBuffer_),
      entriesFile_(entriesBuffer_),
      entryReader_(window_),
      xml_(xmlMemory_),
      feeder_(*this),
      package_(packageMemory_) {}

BookCheck::~BookCheck() {
  CloseBook();
  CloseListings();
}

void BookCheck::Begin() noexcept {
  if (state_ == State::kRunning) {
    return;
  }
  state_ = State::kRunning;
  books_ = 0;
  failedBooks_ = 0;
  totals_ = {};
  checksum_ = 0;
  checksumUs_ = 0;
  inTopDirectory_ = false;
  foldersPassedOver_ = 0;
  beganTrace_ = BN_TRACE_NOW();
  MoveTo(Step::kChecksum);
}

void BookCheck::Tick() noexcept {
  if (state_ != State::kRunning) {
    return;
  }
  std::uint64_t now = clock_.NowUs();
  const std::uint64_t sliceEnds = now + kSliceUs;
  do {
    const Step step = step_;
    const Meter::Counts before = meter_.Now();
    const std::uint64_t inSourceBefore = feeder_.SourceUs();
    DoStep();
    const std::uint64_t after = clock_.NowUs();
    // The steps that are the reading of a book are counted to it.
    if (step == Step::kOpenBook || step == Step::kNextEntry || step == Step::kParseEntry || step == Step::kReadEntry ||
        step == Step::kFindEntry) {
      const Meter::Counts& counts = meter_.Now();
      const std::uint64_t took = after - now;
      const std::uint64_t inStorage = counts.timeUs - before.timeUs;
      book_.workUs += took;
      book_.storageUs += inStorage;
      book_.reads += counts.reads - before.reads;
      book_.readBytes += counts.readBytes - before.readBytes;
      if (step == Step::kReadEntry && entry_.method == epub::kZipDeflated) {
        book_.inflateUs += took > inStorage ? took - inStorage : 0;
      }
      if (step == Step::kParseEntry) {
        // The step's time is the reading of the entry, which the feeder
        // timed and which is storage and inflating as it is for any
        // other entry, and the parsing, which is the rest.
        const std::uint64_t inSource = feeder_.SourceUs() - inSourceBefore;
        book_.xmlUs += took > inSource ? took - inSource : 0;
        if (entry_.method == epub::kZipDeflated) {
          book_.inflateUs += inSource > inStorage ? inSource - inStorage : 0;
        }
      }
      if (step == Step::kOpenBook) {
        openReads_ = book_.reads;
        openUs_ = book_.workUs;
      }
    }
    now = after;
  } while (state_ == State::kRunning && now < sliceEnds);
}

void BookCheck::DoStep() noexcept {
  switch (step_) {
    case Step::kNone:
      break;
    case Step::kChecksum:
      TimeChecksum();
      break;
    case Step::kOpenDirectory:
      OpenDirectory();
      break;
    case Step::kOpenFolder:
      OpenFolder();
      break;
    case Step::kNextBook:
      NextBook();
      break;
    case Step::kReadPackage:
      ReadPackage();
      break;
    case Step::kReadToc:
      ReadToc();
      break;
    case Step::kOpenBook:
      OpenBook();
      break;
    case Step::kNextEntry:
      NextEntry();
      break;
    case Step::kParseEntry:
      ParseEntry();
      break;
    case Step::kReadEntry:
      ReadEntry();
      break;
    case Step::kFindEntry:
      FindEntry();
      break;
    case Step::kEndBook:
      EndBook();
      break;
  }
}

void BookCheck::TimeChecksum() noexcept {
  if (count_ == 0) {
    // Bytes of no particular pattern: the checksum takes as long over any.
    for (std::size_t i = 0; i < chunk_.size(); ++i) {
      chunk_[i] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(i) * 2654435761U) >> 24U);
    }
  }
  const std::uint64_t began = clock_.NowUs();
  for (std::size_t round = 0; round < kChecksumRoundsPerStep; ++round) {
    checksum_ = core::Crc32(chunk_, checksum_);
  }
  checksumUs_ += clock_.NowUs() - began;
  count_ += kChecksumRoundsPerStep;
  if (count_ < kChecksumRounds) {
    return;
  }
  const std::uint64_t rate = KbPerSecond(kChecksumBytes, checksumUs_);
  checksumKbPerSecond.Set(Clamped(rate));
  Line line;
  line.Text("book check: the checksum alone, ")
      .Number(kChecksumBytes)
      .Text(" bytes in ")
      .Number(checksumUs_)
      .Text(" us (")
      .Number(rate)
      .Text(" KB/s).");
  (void)stream_.SendText(line.View());
  MoveTo(Step::kOpenDirectory);
}

void BookCheck::OpenDirectory() noexcept {
  const std::string_view directory = inTopDirectory_ ? kTopDirectory : kBooksDirectory;
  const StorageStatus status = meter_.OpenDirectory(directory, listings_[0]);
  if (status == StorageStatus::kNotFound && !inTopDirectory_) {
    (void)stream_.SendText("book check: there is no /books. Looking in the top directory.");
    inTopDirectory_ = true;
    return;
  }
  if (status != StorageStatus::kOk) {
    Line line;
    line.Text("book check: FAILED. ").Text(directory).Text(" cannot be listed: ").Text(core::StorageStatusName(status));
    (void)stream_.SendText(line.View());
    Finish(false);
    return;
  }
  // The path of whatever is found there starts with the directory's.
  std::size_t length = 0;
  if (!inTopDirectory_) {
    for (const char ch : directory) {
      path_[length++] = ch;
    }
  }
  path_[length++] = '/';
  prefixLengths_[0] = length;
  depth_ = 0;
  booksHere_ = 0;
  MoveTo(Step::kNextBook);
}

void BookCheck::OpenFolder() noexcept {
  // Its path is in place already, with the '/' after it.
  const std::string_view folder(path_.data(), prefixLengths_[1] - 1);
  const StorageStatus status = meter_.OpenDirectory(folder, listings_[1]);
  if (status != StorageStatus::kOk) {
    Line line;
    line.Text("book check: FAILED. ").Text(folder).Text(" cannot be listed: ").Text(core::StorageStatusName(status));
    (void)stream_.SendText(line.View());
    Finish(false);
    return;
  }
  depth_ = 1;
  MoveTo(Step::kNextBook);
}

void BookCheck::NextBook() noexcept {
  // The name goes where the path wants it. Storage leaves out whatever
  // would not fit there, which is whatever would make too long a path.
  const std::size_t prefixLength = prefixLengths_[depth_];
  const std::span<char> name = std::span(path_).subspan(prefixLength);
  std::size_t length = 0;
  core::FileInfo info;
  const StorageStatus status = meter_.NextInDirectory(listings_[depth_], name, length, info);
  if (status != StorageStatus::kOk) {
    Line line;
    line.Text("book check: FAILED. The listing stopped: ").Text(core::StorageStatusName(status));
    (void)stream_.SendText(line.View());
    Finish(false);
    return;
  }
  if (length == 0) {
    meter_.CloseDirectory(listings_[depth_]);
    listings_[depth_] = core::kNoDirectory;
    if (depth_ == 1) {
      depth_ = 0;  // Back to the directory the folder is in.
      return;
    }
    LookInTopDirectoryOrFinish();
    return;
  }
  const std::string_view found(name.data(), length);
  if (info.isDirectory) {
    // A folder is looked in, unless its name begins with a dot, as those
    // do that computers and other firmware keep for themselves; it is
    // itself inside a folder; or its name leaves no room in a path for
    // the shortest name a book could have.
    constexpr std::size_t kShortestBookName = 6;
    if (found.front() == '.' || prefixLength + length + 1 + kShortestBookName > path_.size()) {
      return;
    }
    if (depth_ == 1) {
      ++foldersPassedOver_;
      return;
    }
    path_[prefixLength + length] = '/';
    prefixLengths_[1] = prefixLength + length + 1;
    MoveTo(Step::kOpenFolder);
    return;
  }
  if (!IsBookName(found)) {
    return;
  }
  pathLength_ = prefixLength + length;
  bookSize_ = info.size;
  ++booksHere_;
  ++books_;
  book_ = {};
  openReads_ = 0;
  openUs_ = 0;
  firstFailedEntry_ = 0;
  firstFailure_ = ZipStatus::kOk;
  digest_.Reset();
  packageChecksum_ = 0;
  tocChecksum_ = 0;
  bookBeganTrace_ = BN_TRACE_NOW();
  Line line;
  line.Text("book ").Number(books_).Text(": ").Number(bookSize_).Text(" bytes, ").Text(Path());
  (void)stream_.SendText(line.View());
  MoveTo(Step::kReadPackage);
}

void BookCheck::LookInTopDirectoryOrFinish() noexcept {
  if (booksHere_ == 0 && !inTopDirectory_) {
    (void)stream_.SendText("book check: no book in /books. Looking in the top directory.");
    inTopDirectory_ = true;
    MoveTo(Step::kOpenDirectory);
    return;
  }
  if (foldersPassedOver_ != 0) {
    Line passedOver;
    passedOver.Text("book check: ")
        .Count(foldersPassedOver_, " folder", " folders")
        .Text(" inside a folder not looked in: only one level down is.");
    (void)stream_.SendText(passedOver.View());
  }
  if (books_ == 0) {
    (void)stream_.SendText("book check: FAILED. No book found: put a file ending in .epub in /books.");
    Finish(false);
    return;
  }
  Line line;
  line.Text("book check: ").Text(failedBooks_ == 0 ? "passed. " : "FAILED. ");
  if (failedBooks_ != 0) {
    line.Number(failedBooks_).Text(" of ");
  }
  line.Count(books_, " book, ", " books, ");
  if (totals_.failedEntries != 0) {
    line.Number(totals_.failedEntries).Text(" of ");
  }
  line.Count(totals_.entries, " entry, ", " entries, ")
      .Number(totals_.bytesOut)
      .Text(" bytes out of ")
      .Number(totals_.bytesIn)
      .Text(".");
  (void)stream_.SendText(line.View());
  Line times;
  times.Text("book check: ");
  AddTimes(times, totals_);
  (void)stream_.SendText(times.View());
  if (totals_.xmlDocuments != 0) {
    Line xml;
    xml.Text("book check: ");
    AddXml(xml, totals_, nullptr);
    (void)stream_.SendText(xml.View());
    Line memory;
    memory.Text("book check: ");
    AddXmlMemory(memory, totals_);
    (void)stream_.SendText(memory.View());
  }
  if (totals_.packagesRead + totals_.packagesNotRead != 0) {
    Line packages;
    packages.Text("book check: packages: ").Number(totals_.packagesRead).Text(" read; ");
    AddPackageCost(packages, totals_);
    packages.Text("; the longest ")
        .Number(totals_.packageLongestUs / 1000)
        .Text(" ms; the most memory ")
        .Number(totals_.packageMostMemory)
        .Text(".");
    if (totals_.packagesNotRead != 0) {
      packages.Text(" Not read: ").Number(totals_.packagesNotRead).Text(".");
    }
    (void)stream_.SendText(packages.View());
  }
  if (totals_.tocsRead + totals_.tocsNone + totals_.tocsNotRead != 0) {
    Line contents;
    contents.Text("book check: contents: ")
        .Number(totals_.tocsRead)
        .Text(" read (")
        .Number(totals_.tocsNav)
        .Text(" nav), ")
        .Count(totals_.tocEntries, " entry; ", " entries; ");
    AddTocCost(contents, totals_);
    contents.Text("; the longest ").Number(totals_.tocLongestUs / 1000).Text(" ms.");
    if (totals_.tocsNone != 0) {
      contents.Text(" None: ").Number(totals_.tocsNone).Text(".");
    }
    if (totals_.tocsNotRead != 0) {
      contents.Text(" Not read: ").Number(totals_.tocsNotRead).Text(".");
    }
    (void)stream_.SendText(contents.View());
  }
  Finish(failedBooks_ == 0);
}

void BookCheck::ReadPackage() noexcept {
  // What a reader opening the book would do, and all in one go as it
  // would be: the file opened, its directory found, its package read. If
  // that goes well the book stays open for its table of contents to be
  // read next. Otherwise it is closed again, so that the reading of its
  // entries starts as it would have without this.
  const Meter::Counts before = meter_.Now();
  const std::uint64_t began = clock_.NowUs();
  StorageStatus status = directoryFile_.Open(meter_, Path(), core::OpenMode::kRead);
  if (status == StorageStatus::kOk) {
    status = entriesFile_.Open(meter_, Path(), core::OpenMode::kRead);
  }
  if (status != StorageStatus::kOk || archive_.Open(directoryFile_) != ZipStatus::kOk) {
    // Not a file that can be opened as a book at all, which the next step
    // finds and says.
    CloseBook();
    MoveTo(Step::kOpenBook);
    return;
  }
  const epub::PackageStatus read = package_.Read(archive_, entriesFile_, entryReader_, xml_);
  const std::uint64_t took = clock_.NowUs() - began;

  const Meter::Counts& counts = meter_.Now();
  book_.packageReads = counts.reads - before.reads;
  book_.packageReadBytes = counts.readBytes - before.readBytes;
  book_.packageStorageUs = counts.timeUs - before.timeUs;
  book_.packageUs = took;
  book_.packageLongestUs = took;
  // One line is used for all that is said, one after another: this
  // function's own place on the stack is under everything the reading of
  // the package put there.
  Line line;
  if (read != epub::PackageStatus::kOk) {
    CloseBook();
    MoveTo(Step::kOpenBook);
    book_.packagesNotRead = 1;
    line.Text("book ").Number(books_).Text(": package: NOT READ: ").Text(epub::PackageStatusName(read)).Text(". ");
    AddPackageCost(line, book_);
    line.Text(".");
    (void)stream_.SendText(line.View());
    return;
  }
  MoveTo(Step::kReadToc);
  book_.packagesRead = 1;
  book_.packageMostMemory = static_cast<std::uint32_t>(package_.MemoryUsed());
  packageChecksum_ = epub::PackageChecksum(package_);

  // What the book calls itself.
  line.Text("book ").Number(books_).Text(": package: ");
  AddName<80>(line, "title", package_.Title());
  line.Text(", ");
  AddName<40>(line, "author", package_.Author());
  line.Text(", ");
  AddName<16>(line, "language", package_.Language());
  line.Text(".");
  (void)stream_.SendText(line.View());

  // What its package lists.
  std::uint32_t notInTheBook = 0;
  for (const epub::SpineItem& item : package_.Spine()) {
    notInTheBook += item.present ? 0 : 1;
  }
  line.Clear();
  line.Text("book ")
      .Number(books_)
      .Text(": package: ")
      .Count(package_.ManifestItems(), " file, ", " files, ")
      .Count(package_.Spine().size(), " document, ", " documents, ");
  if (package_.Cover() != nullptr) {
    line.Text("cover ").Text(epub::MediaTypeName(package_.CoverType()));
  } else {
    line.Text("no cover");
  }
  line.Text(package_.Nav() != nullptr ? ", nav" : ", no nav")
      .Text(package_.Ncx() != nullptr ? ", ncx" : ", no ncx")
      .Text("; memory ")
      .Number(book_.packageMostMemory)
      .Text("; checksum ")
      .Hex8(packageChecksum_)
      .Text(".");
  if (notInTheBook != 0) {
    line.Text(" Not in the book: ").Number(notInTheBook).Text(".");
  }
  if (package_.UnknownSpineItems() != 0) {
    line.Text(" Not in the manifest: ").Number(package_.UnknownSpineItems()).Text(".");
  }
  if (package_.CutShort()) {
    line.Text(" Cut short.");
  }
  (void)stream_.SendText(line.View());

  // What finding that out cost.
  line.Clear();
  line.Text("book ").Number(books_).Text(": package: ");
  AddPackageCost(line, book_);
  line.Text(".");
  (void)stream_.SendText(line.View());
}

void BookCheck::ReadToc() noexcept {
  // The table of contents, with the book still open as the reading of its
  // package left it, and a piece at a time: the first piece finds the
  // file and opens it, and each one after reads 1 KB of it.
  const Meter::Counts before = meter_.Now();
  const std::uint64_t began = clock_.NowUs();
  bool more = false;
  if (count_ == 0) {
    tocDigest_.Reset();
    more = toc_.Begin(package_, entriesFile_, entryReader_, xml_, tocDigest_) == epub::TocStatus::kOk;
  } else {
    more = toc_.Step();
  }
  ++count_;
  const Meter::Counts& counts = meter_.Now();
  book_.tocReads += counts.reads - before.reads;
  book_.tocReadBytes += counts.readBytes - before.readBytes;
  book_.tocStorageUs += counts.timeUs - before.timeUs;
  book_.tocUs += clock_.NowUs() - began;
  if (more) {
    return;
  }

  // It is over, well or badly. The book is closed, so that the reading
  // of its entries starts as it would have without any of this.
  CloseBook();
  MoveTo(Step::kOpenBook);
  book_.tocLongestUs = book_.tocUs;
  const epub::TocStatus status = toc_.Status();
  Line line;
  line.Text("book ").Number(books_).Text(": contents: ");
  if (status == epub::TocStatus::kNoToc) {
    book_.tocsNone = 1;
    line.Text("none.");
    (void)stream_.SendText(line.View());
    return;
  }
  if (status != epub::TocStatus::kOk) {
    book_.tocsNotRead = 1;
    line.Text("NOT READ: ").Text(epub::TocStatusName(status)).Text(". ");
    AddTocCost(line, book_);
    line.Text(".");
    (void)stream_.SendText(line.View());
    return;
  }
  book_.tocsRead = 1;
  book_.tocsNav = toc_.Kind() == epub::TocKind::kNav ? 1 : 0;
  book_.tocEntries = tocDigest_.Entries();
  tocChecksum_ = tocDigest_.Checksum();
  line.Text(epub::TocKindName(toc_.Kind()))
      .Text(", ")
      .Count(tocDigest_.Entries(), " entry, ", " entries, ")
      .Count(tocDigest_.Levels(), " level", " levels")
      .Text("; checksum ")
      .Hex8(tocChecksum_)
      .Text("; ");
  AddTocCost(line, book_);
  line.Text(".");
  if (tocDigest_.WithoutDocument() != 0) {
    line.Text(" With no document: ").Number(tocDigest_.WithoutDocument()).Text(".");
  }
  if (toc_.CutShort()) {
    line.Text(" Cut short.");
  }
  (void)stream_.SendText(line.View());
}

void BookCheck::OpenBook() noexcept {
  StorageStatus status = directoryFile_.Open(meter_, Path(), core::OpenMode::kRead);
  if (status == StorageStatus::kOk) {
    status = entriesFile_.Open(meter_, Path(), core::OpenMode::kRead);
  }
  if (status != StorageStatus::kOk) {
    BookFailed(core::StorageStatusName(status));
    return;
  }
  const ZipStatus opened = archive_.Open(directoryFile_);
  if (opened != ZipStatus::kOk) {
    BookFailed(epub::ZipStatusName(opened));
    return;
  }
  cursor_ = {};
  MoveTo(Step::kNextEntry);
}

void BookCheck::NextEntry() noexcept {
  ZipStatus status = archive_.Next(cursor_, entry_, entryName_);
  if (status == ZipStatus::kEnd) {
    MoveTo(Step::kEndBook);
    return;
  }
  if (status != ZipStatus::kOk && status != ZipStatus::kUnsupported) {
    // The directory cannot be followed any further: what fails is the
    // entry after the last one it gave.
    EntryFailed(book_.entries + 1, status);
    MoveTo(Step::kEndBook);
    return;
  }
  ++book_.entries;
  if (status == ZipStatus::kOk) {
    status = entryReader_.Open(entriesFile_, entry_);
  }
  if (status != ZipStatus::kOk) {
    EntryFailed(book_.entries, status);
    return;
  }
  // An entry with an XML file's name is parsed as it is read. (A name too
  // long to have been kept whole is not taken for one.)
  if (entry_.nameLength <= entryName_.size() &&
      epub::HasXmlFileEnding(std::string_view(entryName_.data(), entry_.nameLength))) {
    feeder_.Start();
    if (xml_.Begin(feeder_, digest_) == epub::XmlStatus::kOk) {
      MoveTo(Step::kParseEntry);
      return;
    }
    // There was not the memory even to begin. The entry is read all the
    // same.
    ++book_.xmlDocuments;
    ++book_.xmlRefused;
  }
  MoveTo(Step::kReadEntry);
}

void BookCheck::ParseEntry() noexcept {
  if (xml_.Step()) {
    return;
  }
  // The document is over, well or badly.
  digest_.EndDocument();
  ++book_.xmlDocuments;
  book_.xmlBytes += feeder_.Given();
  switch (xml_.Status()) {
    case epub::XmlStatus::kOk:
    case epub::XmlStatus::kSourceFailed:  // The entry's own failure, which is counted as such below.
      break;
    case epub::XmlStatus::kNoMemory:
      ++book_.xmlRefused;
      break;
    default:
      ++book_.xmlNotXml;
      break;
  }
  const auto memory = static_cast<std::uint32_t>(xml_.Memory().HighWaterSinceReset());
  book_.xmlMostMemory = std::max(book_.xmlMostMemory, memory);
  book_.xmlOver8K += memory > 8U * 1024U ? 1 : 0;
  book_.xmlOver12K += memory > 12U * 1024U ? 1 : 0;
  book_.xmlOver16K += memory > 16U * 1024U ? 1 : 0;
  if (feeder_.Ended()) {
    EndEntryRead();
  } else {
    // The parser stopped part way. The rest is read out all the same, so
    // that the entry is still checked to its end.
    MoveTo(Step::kReadEntry);
  }
}

std::ptrdiff_t BookCheck::PullChunk() noexcept {
  const std::ptrdiff_t read = entryReader_.Read(chunk_);
  if (read > 0) {
    const auto bytes = static_cast<std::uint64_t>(read);
    book_.bytesOut += bytes;
    if (entry_.method == epub::kZipDeflated) {
      book_.inflatedOut += bytes;
    }
  }
  return read;
}

void BookCheck::ReadEntry() noexcept {
  if (PullChunk() <= 0) {
    EndEntryRead();
  }
}

void BookCheck::EndEntryRead() noexcept {
  book_.bytesIn += entry_.compressedSize;
  if (entry_.method == epub::kZipDeflated) {
    book_.inflatedIn += entry_.compressedSize;
  }
  if (entryReader_.Status() != ZipStatus::kOk) {
    EntryFailed(book_.entries, entryReader_.Status());
    MoveTo(Step::kNextEntry);
    return;
  }
  MoveTo(Step::kFindEntry);
}

void BookCheck::FindEntry() noexcept {
  // The other way to an entry: by its name. A name too long to have been
  // kept whole is not looked up.
  if (entry_.nameLength <= entryName_.size()) {
    epub::ZipEntry found;
    const ZipStatus status = archive_.Find(std::string_view(entryName_.data(), entry_.nameLength), found);
    if (status != ZipStatus::kOk) {
      EntryFailed(book_.entries, status);
    }
  }
  MoveTo(Step::kNextEntry);
}

void BookCheck::EndBook() noexcept {
  CloseBook();
  const bool sound = book_.failedEntries == 0;
  Line line;
  line.Text("book ").Number(books_).Text(sound ? ": ok. " : ": FAILED. ");
  if (!sound) {
    line.Number(book_.failedEntries).Text(" of ");
  }
  line.Count(book_.entries, " entry, ", " entries, ")
      .Number(book_.bytesOut)
      .Text(" bytes out of ")
      .Number(book_.bytesIn)
      .Text(". Directory found in ")
      .Count(openReads_, " read, ", " reads, ")
      .Number(openUs_ / 1000)
      .Text(" ms.");
  if (!sound) {
    line.Text(" First to fail: entry ").Number(firstFailedEntry_).Text(", ").Text(epub::ZipStatusName(firstFailure_));
  }
  (void)stream_.SendText(line.View());
  Line times;
  times.Text("book ").Number(books_).Text(": ");
  AddTimes(times, book_);
  (void)stream_.SendText(times.View());
  book_.xmlEvents = digest_.Events();
  book_.xmlEventBytes = digest_.Bytes();
  xmlChecksum_ = digest_.Checksum();
  if (book_.xmlDocuments != 0) {
    Line xml;
    xml.Text("book ").Number(books_).Text(": ");
    AddXml(xml, book_, &xmlChecksum_);
    (void)stream_.SendText(xml.View());
  }

  Add(totals_, book_);
  if (!sound) {
    ++failedBooks_;
  }
  BN_TRACE_TOP_SPAN_SINCE(kSpanBookCheckBook, bookBeganTrace_, sound ? 1 : 0);
  MoveTo(Step::kNextBook);
}

void BookCheck::EntryFailed(std::uint32_t number, ZipStatus status) noexcept {
  ++book_.failedEntries;
  if (firstFailedEntry_ == 0) {
    firstFailedEntry_ = number;
    firstFailure_ = status;
  }
}

void BookCheck::BookFailed(std::string_view why) noexcept {
  CloseBook();
  Line line;
  line.Text("book ").Number(books_).Text(": FAILED. It cannot be read: ").Text(why);
  (void)stream_.SendText(line.View());
  ++failedBooks_;
  BN_TRACE_TOP_SPAN_SINCE(kSpanBookCheckBook, bookBeganTrace_, 0);
  MoveTo(Step::kNextBook);
}

void BookCheck::CloseBook() noexcept {
  archive_.Close();
  directoryFile_.Close();
  entriesFile_.Close();
}

void BookCheck::CloseListings() noexcept {
  for (core::DirectoryHandle& listing : listings_) {
    meter_.CloseDirectory(listing);
    listing = core::kNoDirectory;
  }
}

void BookCheck::MoveTo(Step step) noexcept {
  step_ = step;
  count_ = 0;
}

void BookCheck::Finish(bool passed) noexcept {
  CloseBook();
  CloseListings();
  state_ = passed ? State::kPassed : State::kFailed;
  step_ = Step::kNone;
  bookCheck.Set(passed ? 1 : 0);
  bookCheckBooks.Set(books_);
  inflateKbPerSecond.Set(Clamped(KbPerSecond(totals_.inflatedOut, totals_.inflateUs)));
  xmlKbPerSecond.Set(Clamped(KbPerSecond(totals_.xmlBytes, totals_.xmlUs)));
  BN_TRACE_TOP_SPAN_SINCE(kSpanBookCheck, beganTrace_, passed ? 1 : 0);
}

}  // namespace bn::reader
