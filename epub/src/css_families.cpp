#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "epub/css.h"

// Which kind of face a font-family value asks for (see css.h). The names
// that are known are the ones the books name most, in the many ways they
// write them, and the faces a computer or a reading device is likely to
// have; each is classed by what the face is. A name that is not here is
// not classed, and the next name of the list is tried.

namespace bn::epub {

namespace {

// The most words of one name that are told apart. The longest name here
// has 16 letters.
constexpr std::size_t kMostWords = 16;

struct Keyword {
  std::string_view word;
  CssFamilyClass familyClass;
};

// CSS's own words for a kind of face.
constexpr auto kKeywords = std::to_array<Keyword>({
    {"serif", CssFamilyClass::kSerif},
    {"ui-serif", CssFamilyClass::kSerif},
    {"math", CssFamilyClass::kSerif},
    {"sans-serif", CssFamilyClass::kSans},
    {"system-ui", CssFamilyClass::kSans},
    {"ui-sans-serif", CssFamilyClass::kSans},
    {"monospace", CssFamilyClass::kMono},
    {"ui-monospace", CssFamilyClass::kMono},
    {"cursive", CssFamilyClass::kScript},
    {"fantasy", CssFamilyClass::kOther},
});

constexpr CssFamilyClass kSerif = CssFamilyClass::kSerif;
constexpr CssFamilyClass kSans = CssFamilyClass::kSans;
constexpr CssFamilyClass kMono = CssFamilyClass::kMono;
constexpr CssFamilyClass kScript = CssFamilyClass::kScript;
constexpr CssFamilyClass kOther = CssFamilyClass::kOther;
// A maker's name, or a word like one: it says nothing of the face, and is
// here so that a known name found inside it is not taken ("mono" in
// "Monotype").
constexpr CssFamilyClass kNothing = CssFamilyClass::kUnset;
constexpr bool kAnywhere = false;
constexpr bool kWholeWord = true;

// The names that are known, in the order of their letters. A name counts
// wherever it is found among the letters of a family's name, or, for the
// short ones that some other word could hold ("arial" in "Material"), only
// as a whole word of it.
constexpr auto kKnown = std::to_array<CssKnownFamily>({
    {"academyengraved", kSerif, kAnywhere},
    {"agencyfb", kSans, kAnywhere},
    {"akzidenz", kSans, kAnywhere},
    {"albertus", kSerif, kAnywhere},
    {"alegreya", kSerif, kAnywhere},
    {"alegreyasans", kSans, kAnywhere},
    {"andika", kSans, kAnywhere},
    {"anonymous", kMono, kAnywhere},
    {"antiqua", kSerif, kAnywhere},
    {"antonio", kSans, kAnywhere},
    {"archer", kSerif, kAnywhere},
    {"arial", kSans, kWholeWord},
    {"arialmt", kSans, kAnywhere},
    {"arialnarrow", kSans, kAnywhere},
    {"arialunicode", kSans, kAnywhere},
    {"arimo", kSans, kWholeWord},
    {"arno", kSerif, kWholeWord},
    {"athelas", kSerif, kAnywhere},
    {"avenir", kSans, kAnywhere},
    {"baskerville", kSerif, kAnywhere},
    {"bebas", kSans, kWholeWord},
    {"bebasneue", kSans, kAnywhere},
    {"bembo", kSerif, kAnywhere},
    {"benguiat", kSerif, kAnywhere},
    {"berkeley", kSerif, kAnywhere},
    {"biolinum", kSans, kAnywhere},
    {"bitter", kSerif, kWholeWord},
    {"blackletter", kOther, kAnywhere},
    {"bodoni", kSerif, kAnywhere},
    {"bookerly", kSerif, kAnywhere},
    {"bookman", kSerif, kAnywhere},
    {"cabin", kSans, kWholeWord},
    {"caecilia", kSerif, kAnywhere},
    {"caladea", kSerif, kAnywhere},
    {"caledonia", kSerif, kAnywhere},
    {"calibri", kSans, kAnywhere},
    {"calisto", kSerif, kAnywhere},
    {"calligraph", kScript, kAnywhere},
    {"cambria", kSerif, kAnywhere},
    {"cancellaresca", kScript, kAnywhere},
    {"candara", kSans, kAnywhere},
    {"cardo", kSerif, kWholeWord},
    {"carlito", kSans, kAnywhere},
    {"caslon", kSerif, kAnywhere},
    {"caveat", kScript, kAnywhere},
    {"centaur", kSerif, kAnywhere},
    {"century", kSerif, kAnywhere},
    {"centurygothic", kSans, kAnywhere},
    {"chalkboard", kScript, kAnywhere},
    {"chalkduster", kScript, kAnywhere},
    {"chancery", kScript, kAnywhere},
    {"chaparral", kSerif, kAnywhere},
    {"charis", kSerif, kAnywhere},
    {"charter", kSerif, kAnywhere},
    {"cheltenham", kSerif, kAnywhere},
    {"cinzel", kSerif, kAnywhere},
    {"clarendon", kSerif, kAnywhere},
    {"clearface", kSerif, kAnywhere},
    {"clearfacegothic", kSans, kAnywhere},
    {"cochin", kSerif, kAnywhere},
    {"comicsans", kScript, kAnywhere},
    {"consolas", kMono, kAnywhere},
    {"constantia", kSerif, kAnywhere},
    {"cookie", kScript, kWholeWord},
    {"cooperblack", kSerif, kAnywhere},
    {"corbel", kSans, kAnywhere},
    {"cormorant", kSerif, kAnywhere},
    {"corsiva", kScript, kAnywhere},
    {"courier", kMono, kAnywhere},
    {"cousine", kMono, kAnywhere},
    {"coustard", kSerif, kAnywhere},
    {"crimson", kSerif, kAnywhere},
    {"cursive", kScript, kAnywhere},
    {"didot", kSerif, kAnywhere},
    {"din", kSans, kWholeWord},
    {"dingbats", kOther, kAnywhere},
    {"dinnext", kSans, kAnywhere},
    {"domine", kSerif, kWholeWord},
    {"doulos", kSerif, kAnywhere},
    {"electra", kSerif, kAnywhere},
    {"engravers", kSerif, kAnywhere},
    {"eurostile", kSans, kAnywhere},
    {"fairfield", kSerif, kAnywhere},
    {"firacode", kMono, kAnywhere},
    {"fixed", kMono, kWholeWord},
    {"fixedsys", kMono, kAnywhere},
    {"fontawesome", kOther, kAnywhere},
    {"fonticons", kOther, kAnywhere},
    {"fraktur", kOther, kAnywhere},
    {"franklin", kSans, kAnywhere},
    {"fraunces", kSerif, kAnywhere},
    {"frutiger", kSans, kAnywhere},
    {"futura", kSans, kAnywhere},
    {"galliard", kSerif, kAnywhere},
    {"garamond", kSerif, kAnywhere},
    {"geneva", kSans, kAnywhere},
    {"gentium", kSerif, kAnywhere},
    {"georgia", kSerif, kAnywhere},
    {"gill", kSans, kWholeWord},
    {"gillsans", kSans, kAnywhere},
    {"gloriahallelujah", kScript, kAnywhere},
    {"gotham", kSans, kAnywhere},
    {"gothic", kSans, kAnywhere},
    {"goudy", kSerif, kAnywhere},
    {"greatvibes", kScript, kAnywhere},
    {"grotesk", kSans, kAnywhere},
    {"grotesque", kSans, kAnywhere},
    {"hand", kScript, kAnywhere},
    {"handel", kNothing, kAnywhere},
    {"handwriting", kScript, kAnywhere},
    {"helvetica", kSans, kAnywhere},
    {"hirakaku", kSans, kAnywhere},
    {"hiramin", kSerif, kAnywhere},
    {"hoefler", kSerif, kAnywhere},
    {"impact", kSans, kWholeWord},
    {"inconsolata", kMono, kAnywhere},
    {"inter", kSans, kWholeWord},
    {"interstate", kSans, kAnywhere},
    {"isadora", kScript, kAnywhere},
    {"janson", kSerif, kAnywhere},
    {"jenson", kSerif, kAnywhere},
    {"jetbrains", kMono, kAnywhere},
    {"joanna", kSerif, kWholeWord},
    {"junicode", kSerif, kAnywhere},
    {"kalam", kScript, kWholeWord},
    {"karla", kSans, kWholeWord},
    {"kaufmann", kScript, kAnywhere},
    {"kepler", kSerif, kWholeWord},
    {"kozgo", kSans, kAnywhere},
    {"kozmin", kSerif, kAnywhere},
    {"lato", kSans, kWholeWord},
    {"leawood", kSerif, kAnywhere},
    {"lekton", kMono, kAnywhere},
    {"lettergothic", kMono, kAnywhere},
    {"libertine", kSerif, kAnywhere},
    {"literata", kSerif, kAnywhere},
    {"lora", kSerif, kWholeWord},
    {"lucida", kSans, kAnywhere},
    {"lucidabright", kSerif, kAnywhere},
    {"lucidaconsole", kMono, kAnywhere},
    {"lucidafax", kSerif, kAnywhere},
    {"malabar", kSerif, kAnywhere},
    {"markerfelt", kScript, kAnywhere},
    {"materialicons", kOther, kAnywhere},
    {"meiryo", kSans, kAnywhere},
    {"melior", kSerif, kAnywhere},
    {"menlo", kMono, kWholeWord},
    {"merriweather", kSerif, kAnywhere},
    {"merriweathersans", kSans, kAnywhere},
    {"mincho", kSerif, kAnywhere},
    {"minion", kSerif, kAnywhere},
    {"monaco", kMono, kAnywhere},
    {"mono", kMono, kAnywhere},
    {"monotype", kNothing, kAnywhere},
    {"montserrat", kSans, kAnywhere},
    {"mrseaves", kSerif, kAnywhere},
    {"myriad", kSans, kAnywhere},
    {"neutra", kSans, kAnywhere},
    {"newsreader", kSerif, kAnywhere},
    {"nimbusroman", kSerif, kAnywhere},
    {"noteworthy", kScript, kAnywhere},
    {"nunito", kSans, kAnywhere},
    {"ocr", kMono, kWholeWord},
    {"oldstandard", kSerif, kAnywhere},
    {"oldstyle", kSerif, kAnywhere},
    {"optima", kSans, kAnywhere},
    {"oswald", kSans, kAnywhere},
    {"palatino", kSerif, kAnywhere},
    {"perpetua", kSerif, kAnywhere},
    {"pinyon", kScript, kAnywhere},
    {"plantin", kSerif, kAnywhere},
    {"playfair", kSerif, kAnywhere},
    {"poppins", kSans, kAnywhere},
    {"prestige", kMono, kAnywhere},
    {"proxima", kSans, kAnywhere},
    {"quicksand", kSans, kAnywhere},
    {"raleway", kSans, kAnywhere},
    {"roboto", kSans, kAnywhere},
    {"robotomono", kMono, kAnywhere},
    {"robotoserif", kSerif, kAnywhere},
    {"robotoslab", kSerif, kAnywhere},
    {"rockwell", kSerif, kAnywhere},
    {"roman", kSerif, kWholeWord},
    {"rubik", kSans, kWholeWord},
    {"russoone", kSans, kAnywhere},
    {"sabon", kSerif, kAnywhere},
    {"sans", kSans, kAnywhere},
    {"sanserif", kSans, kAnywhere},
    {"sansmono", kMono, kAnywhere},
    {"sansserif", kSans, kAnywhere},
    {"schoolbook", kSerif, kAnywhere},
    {"script", kScript, kAnywhere},
    {"segoe", kSans, kAnywhere},
    {"sentinel", kSerif, kAnywhere},
    {"seravek", kSans, kAnywhere},
    {"serif", kSerif, kAnywhere},
    {"shadowsintolight", kScript, kAnywhere},
    {"sitka", kSerif, kWholeWord},
    {"slab", kSerif, kAnywhere},
    {"sourcecode", kMono, kAnywhere},
    {"spectral", kSerif, kAnywhere},
    {"stencil", kOther, kAnywhere},
    {"stix", kSerif, kWholeWord},
    {"symbol", kOther, kWholeWord},
    {"tahoma", kSans, kAnywhere},
    {"taviraj", kSerif, kAnywhere},
    {"terminal", kMono, kWholeWord},
    {"tiempos", kSerif, kAnywhere},
    {"times", kSerif, kWholeWord},
    {"timeslt", kSerif, kAnywhere},
    {"timesnewroman", kSerif, kAnywhere},
    {"tinos", kSerif, kWholeWord},
    {"trajan", kSerif, kAnywhere},
    {"trebuchet", kSans, kAnywhere},
    {"turnip", kSerif, kWholeWord},
    {"typewriter", kMono, kAnywhere},
    {"ubuntu", kSans, kAnywhere},
    {"ubuntumono", kMono, kAnywhere},
    {"univers", kSans, kAnywhere},
    {"utopia", kSerif, kAnywhere},
    {"verdana", kSans, kAnywhere},
    {"vollkorn", kSerif, kAnywhere},
    {"walbaum", kSerif, kAnywhere},
    {"warnock", kSerif, kAnywhere},
    {"webdings", kOther, kAnywhere},
    {"whitney", kSans, kAnywhere},
    {"wingdings", kOther, kAnywhere},
    {"yellowtail", kScript, kAnywhere},
    {"zapfino", kScript, kAnywhere},
});

constexpr bool InOrderAndOfLetters() noexcept {
  for (std::size_t i = 0; i < kKnown.size(); ++i) {
    const std::string_view name = kKnown[i].name;
    if (name.empty() || name.size() > kCssMostFamilyLetters || (i != 0 && !(kKnown[i - 1].name < name)) ||
        (kKnown[i].familyClass == CssFamilyClass::kUnset && kKnown[i].wholeWord)) {
      return false;
    }
    for (const char ch : name) {
      if (ch < 'a' || ch > 'z') {
        return false;
      }
    }
    // A name that counts as a whole word is in no word that says nothing,
    // so a whole word found is never hidden by one.
    for (const CssKnownFamily& nothing : kKnown) {
      if (kKnown[i].wholeWord && nothing.familyClass == CssFamilyClass::kUnset &&
          nothing.name.find(name) != std::string_view::npos) {
        return false;
      }
    }
  }
  return true;
}
static_assert(InOrderAndOfLetters(),
              "the names that are known are small letters only, each once, in order; one that says nothing counts "
              "anywhere, and has no name inside it that counts as a whole word");

// The `length` letters of `text` from `at`, or as many of them as there
// are.
std::string_view Part(std::string_view text, std::size_t at, std::size_t length) noexcept {
  text.remove_prefix(std::min(at, text.size()));
  text.remove_suffix(text.size() - std::min(length, text.size()));
  return text;
}

}  // namespace

const char* CssFamilyClassName(CssFamilyClass familyClass) noexcept {
  switch (familyClass) {
    case CssFamilyClass::kUnset:
      return "unset";
    case CssFamilyClass::kSerif:
      return "serif";
    case CssFamilyClass::kSans:
      return "sans";
    case CssFamilyClass::kMono:
      return "mono";
    case CssFamilyClass::kScript:
      return "script";
    case CssFamilyClass::kOther:
      return "other";
  }
  return "?";
}

std::span<const CssKnownFamily> CssKnownFamilies() noexcept { return kKnown; }

CssFamilyClass CssClassOfFamily(std::string_view name, CssFamilyHow& how) noexcept {
  how = CssFamilyHow::kNothing;
  for (const Keyword& keyword : kKeywords) {
    if (name == keyword.word) {
      how = CssFamilyHow::kKeyword;
      return keyword.familyClass;
    }
  }
  // The name's letters with nothing between them, and where among them
  // each of its words begins and how long it is.
  struct Word {
    std::uint8_t start = 0;
    std::uint8_t length = 0;
  };
  std::array<char, kCssMostFamilyLetters> letters{};
  std::array<Word, kMostWords> words{};
  std::size_t count = 0;
  std::size_t wordCount = 0;
  bool inWord = false;
  bool counted = false;  // The word being gone through is one of `words`.
  for (const char ch : name) {
    if (ch < 'a' || ch > 'z') {
      inWord = false;
      continue;
    }
    if (count == letters.size()) {
      break;
    }
    if (!inWord) {
      inWord = true;
      counted = wordCount < words.size();
      if (counted) {
        words[wordCount++] = {.start = static_cast<std::uint8_t>(count), .length = 0};
      }
    }
    if (counted) {
      ++words[wordCount - 1].length;
    }
    letters[count++] = ch;
  }
  const std::string_view all(letters.data(), count);

  // Where the words that say nothing are: for each letter that is inside
  // one, how far the one that reaches furthest from there goes. A name
  // found from `at` to `end` is hidden if that is `end` or beyond; which
  // such a word itself always is, so it is never taken.
  std::array<std::uint8_t, kCssMostFamilyLetters> nothingTo{};
  for (const CssKnownFamily& known : kKnown) {
    if (known.familyClass != CssFamilyClass::kUnset) {
      continue;
    }
    for (std::size_t at = all.find(known.name); at != std::string_view::npos; at = all.find(known.name, at + 1)) {
      const auto end = static_cast<std::uint8_t>(at + known.name.size());
      for (std::size_t letter = at; letter < end; ++letter) {
        nothingTo[letter] = std::max(nothingTo[letter], end);
      }
    }
  }
  const auto hidden = [&nothingTo](std::size_t at, std::size_t length) { return nothingTo[at] >= at + length; };

  // The longest name that is known and is found there, not hidden; of two
  // as long, the later.
  const CssKnownFamily* best = nullptr;
  std::size_t bestAt = 0;
  for (const CssKnownFamily& known : kKnown) {
    if (best != nullptr && known.name.size() < best->name.size()) {
      continue;
    }
    // The last place it is found that is not hidden. A whole word never
    // is: the names are checked for that where they are listed.
    std::size_t at = std::string_view::npos;
    if (known.wholeWord) {
      for (std::size_t word = wordCount; word-- > 0;) {
        if (Part(all, words[word].start, words[word].length) == known.name) {
          at = words[word].start;
          break;
        }
      }
    } else {
      at = all.rfind(known.name);
      while (at != std::string_view::npos && hidden(at, known.name.size())) {
        at = at == 0 ? std::string_view::npos : all.rfind(known.name, at - 1);
      }
    }
    if (at == std::string_view::npos) {
      continue;
    }
    if (best == nullptr || known.name.size() > best->name.size() || at > bestAt) {
      best = &known;
      bestAt = at;
    }
  }
  if (best == nullptr) {
    return CssFamilyClass::kUnset;
  }
  how = CssFamilyHow::kName;
  return best->familyClass;
}

CssFamilyClass CssClassOfFamilies(std::string_view families, CssFamilyHow& how) noexcept {
  how = CssFamilyHow::kNothing;
  std::array<char, kCssMostFamilyName> name{};
  std::string_view rest = families;
  for (std::string_view one = CssNextFamily(rest, name); !one.empty(); one = CssNextFamily(rest, name)) {
    CssFamilyHow said = CssFamilyHow::kNothing;
    const CssFamilyClass familyClass = CssClassOfFamily(one, said);
    if (familyClass != CssFamilyClass::kUnset) {
      how = said;
      return familyClass;
    }
  }
  return CssFamilyClass::kOther;
}

CssFamilyClass CssClassOfFamilies(std::string_view families) noexcept {
  CssFamilyHow how = CssFamilyHow::kNothing;
  return CssClassOfFamilies(families, how);
}

}  // namespace bn::epub
