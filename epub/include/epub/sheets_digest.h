#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "epub/document_sheets.h"

// For checking the finding of a book's sheets against itself somewhere
// else: on a device against the desktop, or against another finding
// altogether.

namespace bn::epub {

// What the documents of a book were given, boiled down to counts and a
// checksum. Reset() it when a book begins (DocumentSheets::BeginBook()),
// and Add() each document once its sheets have been found.
//
// Every document is taken as lines:
//
//   D path           the document, by its path in the book
//   S path           each sheet it is given, in the order of its list;
//                    `S <style>` for the sheet of its own `<style>` rules
//   X kind count     each kind of thing that was left out for it, in the
//                    order of SheetLeftOut, as SheetLeftOutName() names it,
//                    with how many; nothing for a kind with none
//
// and a document that could not be opened as its `D` line and then `N`.
// The checksum is the CRC-32 of those lines, each with its line end. The
// `D` and `S` lines are the ones `bn-css sheets` prints, but for a byte
// that it writes out with a backslash. Nothing is kept of them.
//
// Two findings that come to the same checksum gave the same documents, in
// the same order, the same sheets in the same order, and left out as many
// things of each kind for each.
class SheetsDigest {
 public:
  SheetsDigest() noexcept = default;

  // Starts again from nothing, for a book whose counts of what was left
  // out start again too.
  void Reset() noexcept;

  // Takes account of the document at `path`, whose sheets `sheets` has
  // just finished finding, or which it could not open. What was left out
  // for it is what `sheets` has counted since the document added before.
  void Add(std::string_view path, const DocumentSheets& sheets) noexcept;
  // And of one that could not be opened before its sheets could be asked
  // for at all.
  void AddUnopened(std::string_view path) noexcept;

  std::uint32_t Checksum() const noexcept { return checksum_; }
  // The documents added, and how many of them could not be opened.
  std::uint32_t Documents() const noexcept { return documents_; }
  std::uint32_t Unopened() const noexcept { return unopened_; }
  // The sheets the documents were given, a sheet counted once for each
  // document that has it; and how many documents have rules of their own.
  std::uint64_t Given() const noexcept { return given_; }
  std::uint32_t WithOwnStyle() const noexcept { return withOwnStyle_; }

 private:
  void Line(std::string_view mark, std::string_view text) noexcept;

  std::uint32_t checksum_ = 0;
  std::uint32_t documents_ = 0;
  std::uint32_t unopened_ = 0;
  std::uint64_t given_ = 0;
  std::uint32_t withOwnStyle_ = 0;
  // What had been left out, by kind, when the last document was added.
  std::array<std::uint32_t, kSheetLeftOutKinds> leftOut_{};
};

}  // namespace bn::epub
