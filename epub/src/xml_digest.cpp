#include "epub/xml_digest.h"

#include <array>
#include <cstddef>
#include <span>

#include "core/checksum.h"

namespace bn::epub {

void XmlEventDigest::Add(std::string_view bytes) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters as the bytes they are.
  checksum_ = core::Crc32({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}, checksum_);
  bytes_ += bytes.size();
}

void XmlEventDigest::AddEscaped(std::string_view raw) noexcept {
  // What needs no escaping goes in runs, as long as they come.
  std::size_t start = 0;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    std::string_view escape;
    switch (raw[i]) {
      case '\\':
        escape = "\\\\";
        break;
      case '\n':
        escape = "\\n";
        break;
      case '\t':
        escape = "\\t";
        break;
      case '\r':
        escape = "\\r";
        break;
      default:
        continue;
    }
    Add(std::string_view(raw.data() + start, i - start));  // Not substr(), which can throw.
    Add(escape);
    start = i + 1;
  }
  Add(std::string_view(raw.data() + start, raw.size() - start));
}

void XmlEventDigest::EndText() noexcept {
  if (inText_) {
    Add("\n");
    inText_ = false;
  }
}

void XmlEventDigest::OnStartElement(std::string_view name, const XmlAttributes& attributes) noexcept {
  EndText();
  Add("(");
  Add(name);
  Add("\n");
  ++events_;
  for (std::size_t i = 0; i < attributes.Count(); ++i) {
    Add("A");
    Add(attributes.Name(i));
    Add(" ");
    AddEscaped(attributes.Value(i));
    Add("\n");
    ++events_;
  }
}

void XmlEventDigest::OnEndElement(std::string_view name) noexcept {
  EndText();
  Add(")");
  Add(name);
  Add("\n");
  ++events_;
}

void XmlEventDigest::OnText(std::string_view text) noexcept {
  if (text.empty()) {
    return;
  }
  if (!inText_) {
    Add("-");
    inText_ = true;
    ++events_;
  }
  AddEscaped(text);
}

void XmlEventDigest::OnEntity(std::string_view name) noexcept {
  EndText();
  Add("&");
  Add(name);
  Add("\n");
  ++events_;
}

void XmlEventDigest::EndDocument() noexcept { EndText(); }

void XmlEventDigest::Reset() noexcept {
  checksum_ = 0;
  events_ = 0;
  bytes_ = 0;
  inText_ = false;
}

bool HasXmlFileEnding(std::string_view name) noexcept {
  constexpr std::array<std::string_view, 6> kEndings = {".xhtml", ".html", ".htm", ".xml", ".opf", ".ncx"};
  for (const std::string_view ending : kEndings) {
    if (name.size() < ending.size()) {
      continue;
    }
    const std::size_t start = name.size() - ending.size();
    bool same = true;
    for (std::size_t i = 0; i < ending.size() && same; ++i) {
      char ch = name[start + i];
      if (ch >= 'A' && ch <= 'Z') {
        ch = static_cast<char>(ch - 'A' + 'a');
      }
      same = ch == ending[i];
    }
    if (same) {
      return true;
    }
  }
  return false;
}

}  // namespace bn::epub
