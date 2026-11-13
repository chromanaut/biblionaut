#include "epub/book_path.h"

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string>

namespace {

using bn::epub::BookDirectory;
using bn::epub::NameHash;
using bn::epub::ResolveBookPath;

// The file `reference` means from a document in `base`, or "(none)".
std::string Resolve(const std::string& base, const std::string& reference, std::size_t room = bn::epub::kMaxBookPath) {
  std::array<char, bn::epub::kMaxBookPath> path{};
  const std::size_t length = ResolveBookPath(base, reference, std::span(path).first(room));
  return length == 0 ? "(none)" : std::string(path.data(), length);
}

TEST(BookPath, IsTheReferenceAddedToTheDirectoryOfWhatRefersToIt) {
  EXPECT_EQ(Resolve("", "content.opf"), "content.opf");
  EXPECT_EQ(Resolve("OEBPS/", "chapter1.xhtml"), "OEBPS/chapter1.xhtml");
  EXPECT_EQ(Resolve("OEBPS/", "text/chapter1.xhtml"), "OEBPS/text/chapter1.xhtml");
  EXPECT_EQ(Resolve("a/b/c/", "d/e.css"), "a/b/c/d/e.css");
  EXPECT_EQ(Resolve("OEBPS/", "a name with spaces (1).xhtml"), "OEBPS/a name with spaces (1).xhtml");
  EXPECT_EQ(Resolve("OEBPS/", "caf\xC3\xA9.xhtml"), "OEBPS/caf\xC3\xA9.xhtml");
}

TEST(BookPath, WorksOutDotsAndDoubleDots) {
  EXPECT_EQ(Resolve("OEBPS/text/", "../images/cover.jpg"), "OEBPS/images/cover.jpg");
  EXPECT_EQ(Resolve("OEBPS/text/", "../../mimetype"), "mimetype");
  EXPECT_EQ(Resolve("OEBPS/text/", "./a.xhtml"), "OEBPS/text/a.xhtml");
  EXPECT_EQ(Resolve("OEBPS/text/", "a/./b/../c.xhtml"), "OEBPS/text/a/c.xhtml");
  EXPECT_EQ(Resolve("OEBPS/", "a//b.xhtml"), "OEBPS/a/b.xhtml");
  EXPECT_EQ(Resolve("OEBPS//text/", "a.xhtml"), "OEBPS/text/a.xhtml");
  // One that would climb out of the book stays at the top of it.
  EXPECT_EQ(Resolve("OEBPS/", "../../../a.xhtml"), "a.xhtml");
  EXPECT_EQ(Resolve("", "../a.xhtml"), "a.xhtml");
  // A name that only begins with dots is a name.
  EXPECT_EQ(Resolve("OEBPS/", "...hidden"), "OEBPS/...hidden");
  EXPECT_EQ(Resolve("OEBPS/", ".hidden/..x"), "OEBPS/.hidden/..x");
}

TEST(BookPath, StartsFromTheTopForAReferenceThatBeginsWithASlash) {
  EXPECT_EQ(Resolve("OEBPS/text/", "/OEBPS/images/a.png"), "OEBPS/images/a.png");
  EXPECT_EQ(Resolve("OEBPS/", "/mimetype"), "mimetype");
}

TEST(BookPath, DropsWhatComesAfterAHashOrAQuestionMark) {
  EXPECT_EQ(Resolve("OEBPS/", "chapter1.xhtml#section-2"), "OEBPS/chapter1.xhtml");
  EXPECT_EQ(Resolve("OEBPS/", "chapter1.xhtml?x=1#y"), "OEBPS/chapter1.xhtml");
  EXPECT_EQ(Resolve("OEBPS/", "a.xhtml#b/c"), "OEBPS/a.xhtml");
  // A reference to a place in the document it is written in names no file.
  EXPECT_EQ(Resolve("OEBPS/", "#note-1"), "(none)");
  EXPECT_EQ(Resolve("OEBPS/", "?x"), "(none)");
}

TEST(BookPath, TurnsPercentEscapesIntoTheBytesTheyStandFor) {
  EXPECT_EQ(Resolve("OEBPS/", "Title%20Page.xhtml"), "OEBPS/Title Page.xhtml");
  EXPECT_EQ(Resolve("OEBPS/", "caf%C3%A9.xhtml"), "OEBPS/caf\xC3\xA9.xhtml");
  EXPECT_EQ(Resolve("OEBPS/", "a%2fb%2Fc.xhtml"), "OEBPS/a/b/c.xhtml");
  EXPECT_EQ(Resolve("", "%41%62%7a"), "Abz");
  // What only looks like one is left as it is written.
  EXPECT_EQ(Resolve("", "100%.xhtml"), "100%.xhtml");
  EXPECT_EQ(Resolve("", "a%zzb%4"), "a%zzb%4");
  EXPECT_EQ(Resolve("", "%"), "%");
  EXPECT_EQ(Resolve("", "a%2"), "a%2");
  // A reference that ends part way through one: what comes after it in
  // memory is no part of it.
  const std::string longer = "a%41";
  std::array<char, 16> path{};
  ASSERT_EQ(ResolveBookPath("", std::string_view(longer).substr(0, 3), path), 3u);
  EXPECT_EQ(std::string(path.data(), 3), "a%4");
  // An escaped ".." is still "..".
  EXPECT_EQ(Resolve("OEBPS/text/", "%2E%2E/images/a.jpg"), "OEBPS/images/a.jpg");
  // But the directory it is added to is a path already, and is not decoded.
  EXPECT_EQ(Resolve("my%20books/", "a.xhtml"), "my%20books/a.xhtml");
}

TEST(BookPath, IsNoFileOfTheBookForAReferenceToSomewhereElse) {
  for (const char* reference :
       {"http://example.com/a.xhtml", "https://example.com/", "mailto:someone@example.com", "//example.com/a.css",
        "data:image/png;base64,AAAA", "tel:123", "x-scheme+1.0:rest", ""}) {
    EXPECT_EQ(Resolve("OEBPS/", reference), "(none)") << reference;
  }
  // A colon further in is part of a name, not the end of a scheme.
  EXPECT_EQ(Resolve("OEBPS/", "notes/1:2.xhtml"), "OEBPS/notes/1:2.xhtml");
  EXPECT_EQ(Resolve("OEBPS/", "./a:b"), "OEBPS/a:b");
  EXPECT_EQ(Resolve("OEBPS/", "1a:b"), "OEBPS/1a:b") << "a scheme begins with a letter";
  EXPECT_EQ(Resolve("OEBPS/", ":b"), "OEBPS/:b");
}

TEST(BookPath, IsNoFileWhenThePathHasNoRoom) {
  EXPECT_EQ(Resolve("OEBPS/", "abc.xhtml", 15), "OEBPS/abc.xhtml");
  EXPECT_EQ(Resolve("OEBPS/", "abc.xhtml", 14), "(none)");
  EXPECT_EQ(Resolve("", "abc", 3), "abc");
  EXPECT_EQ(Resolve("", "abc", 2), "(none)");
  EXPECT_EQ(Resolve("", "abc", 0), "(none)");
  // Room that is only needed on the way is still needed.
  EXPECT_EQ(Resolve("", "long-directory-name/../a", 8), "(none)");
  // The longest path there is room for at all.
  const std::string longest(bn::epub::kMaxBookPath, 'n');
  EXPECT_EQ(Resolve("", longest), longest);
  EXPECT_EQ(Resolve("", longest + "n"), "(none)");
  // One name, with escapes in it, longer than any a book has.
  EXPECT_EQ(Resolve("", "%41" + std::string(300, 'a')), "(none)");
  EXPECT_EQ(Resolve("", "%41" + std::string(255, 'a')), "A" + std::string(255, 'a'));
}

TEST(BookPath, KnowsTheDirectoryAFileIsIn) {
  EXPECT_EQ(BookDirectory("OEBPS/text/chapter1.xhtml"), "OEBPS/text/");
  EXPECT_EQ(BookDirectory("OEBPS/content.opf"), "OEBPS/");
  EXPECT_EQ(BookDirectory("content.opf"), "");
  EXPECT_EQ(BookDirectory(""), "");
  EXPECT_EQ(BookDirectory("a/"), "a/");
}

TEST(NameHash, IsTheSameForTheSameNameAndDifferentForDifferentOnes) {
  EXPECT_EQ(NameHash("OEBPS/chapter1.xhtml"), NameHash(std::string("OEBPS/chapter") + "1.xhtml"));
  EXPECT_NE(NameHash("OEBPS/chapter1.xhtml"), NameHash("OEBPS/chapter2.xhtml"));
  EXPECT_NE(NameHash("ab"), NameHash("ba"));
  EXPECT_NE(NameHash(""), NameHash(std::string_view("\0", 1)));
  EXPECT_NE(NameHash("a"), NameHash("A"));

  // The kinds of names books have, tens of thousands of them: every one
  // gets a number of its own, and every number fits in 48 bits.
  std::set<std::uint64_t> seen;
  std::size_t names = 0;
  for (const char* directory : {"", "OEBPS/", "OEBPS/Text/", "OPS/images/", "item/xhtml/"}) {
    for (const char* stem : {"chapter", "ch", "part-", "Weir_9780804139038_epub3_c", "id", "x", "image", "split_"}) {
      for (const char* ending : {".xhtml", ".html", ".jpg", "", "_r1.xhtml"}) {
        for (int number = 0; number < 300; ++number) {
          const std::uint64_t hash = NameHash(std::string(directory) + stem + std::to_string(number) + ending);
          EXPECT_LT(hash, std::uint64_t{1} << 48U);
          seen.insert(hash);
          ++names;
        }
      }
    }
  }
  EXPECT_EQ(seen.size(), names);
  EXPECT_EQ(names, 60'000u);
}

}  // namespace
