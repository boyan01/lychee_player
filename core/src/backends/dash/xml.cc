#include "backends/dash/xml.h"

#include <cstdint>

namespace lychee {

namespace {

constexpr int kMaxDepth = 64;
constexpr size_t kMaxElements = 200000;

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view LocalName(std::string_view name) {
  const size_t colon = name.rfind(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

void AppendUtf8(uint32_t code, std::string* out) {
  if (code < 0x80) {
    out->push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out->push_back(static_cast<char>(0xc0 | (code >> 6)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else if (code < 0x10000) {
    out->push_back(static_cast<char>(0xe0 | (code >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3f)));
  } else {
    out->push_back(static_cast<char>(0xf0 | (code >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3f)));
  }
}

// Appends |raw| to |out| with its character references decoded; an
// unknown or malformed one is kept as it is.
void Decode(std::string_view raw, std::string* out) {
  size_t i = 0;
  while (i < raw.size()) {
    const size_t amp = raw.find('&', i);
    if (amp == std::string_view::npos) {
      out->append(raw.substr(i));
      return;
    }
    out->append(raw.substr(i, amp - i));
    const size_t semicolon = raw.find(';', amp);
    if (semicolon == std::string_view::npos || semicolon - amp > 12) {
      out->push_back('&');
      i = amp + 1;
      continue;
    }
    const std::string_view entity = raw.substr(amp + 1, semicolon - amp - 1);
    bool known = true;
    if (entity == "lt") {
      out->push_back('<');
    } else if (entity == "gt") {
      out->push_back('>');
    } else if (entity == "amp") {
      out->push_back('&');
    } else if (entity == "quot") {
      out->push_back('"');
    } else if (entity == "apos") {
      out->push_back('\'');
    } else if (entity.size() >= 2 && entity[0] == '#') {
      const bool hex = entity[1] == 'x' || entity[1] == 'X';
      uint32_t code = 0;
      size_t digits = 0;
      for (size_t k = hex ? 2 : 1; k < entity.size(); ++k, ++digits) {
        const char c = entity[k];
        uint32_t value;
        if (c >= '0' && c <= '9') {
          value = static_cast<uint32_t>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
          value = static_cast<uint32_t>(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
          value = static_cast<uint32_t>(c - 'A' + 10);
        } else {
          digits = 0;
          break;
        }
        code = code * (hex ? 16 : 10) + value;
        if (code > 0x10ffff) {
          digits = 0;
          break;
        }
      }
      known = digits > 0 && code > 0;
      if (known) {
        AppendUtf8(code, out);
      }
    } else {
      known = false;
    }
    if (known) {
      i = semicolon + 1;
    } else {
      out->push_back('&');
      i = amp + 1;
    }
  }
}

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  bool Document(XmlElement* root, std::string* error) {
    if (text_.substr(0, 3) == "\xef\xbb\xbf") {
      pos_ = 3;
    }
    bool have_root = false;
    for (;;) {
      SkipSpace();
      if (pos_ >= text_.size()) {
        break;
      }
      if (text_[pos_] != '<') {
        return Fail("text outside the root element", error);
      }
      if (StartsWith("<?")) {
        if (!SkipPast("?>")) return Fail("unterminated <?", error);
      } else if (StartsWith("<!--")) {
        if (!SkipPast("-->")) return Fail("unterminated comment", error);
      } else if (StartsWith("<!")) {
        if (!SkipDeclaration()) return Fail("unterminated <!", error);
      } else if (have_root) {
        return Fail("a second root element", error);
      } else {
        if (!Element(root, 1, error)) {
          return false;
        }
        have_root = true;
      }
    }
    return have_root || Fail("no root element", error);
  }

 private:
  bool Fail(const char* why, std::string* error) {
    *error = "xml: ";
    *error += why;
    *error += " at byte " + std::to_string(pos_);
    return false;
  }

  bool StartsWith(std::string_view prefix) const {
    return text_.substr(pos_, prefix.size()) == prefix;
  }

  void SkipSpace() {
    while (pos_ < text_.size() && IsSpace(text_[pos_])) {
      ++pos_;
    }
  }

  bool SkipPast(std::string_view end) {
    const size_t found = text_.find(end, pos_);
    if (found == std::string_view::npos) {
      return false;
    }
    pos_ = found + end.size();
    return true;
  }

  // <!DOCTYPE ...> with an internal subset in brackets, or another <!...>.
  bool SkipDeclaration() {
    int brackets = 0;
    for (; pos_ < text_.size(); ++pos_) {
      const char c = text_[pos_];
      if (c == '[') {
        ++brackets;
      } else if (c == ']') {
        --brackets;
      } else if (c == '>' && brackets <= 0) {
        ++pos_;
        return true;
      }
    }
    return false;
  }

  std::string_view Name() {
    const size_t start = pos_;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (IsSpace(c) || c == '/' || c == '>' || c == '=' || c == '<' ||
          c == '"' || c == '\'') {
        break;
      }
      ++pos_;
    }
    return text_.substr(start, pos_ - start);
  }

  // At '<' of a start tag.
  bool Element(XmlElement* element, int depth, std::string* error) {
    if (depth > kMaxDepth) {
      return Fail("elements nested too deep", error);
    }
    if (++elements_ > kMaxElements) {
      return Fail("too many elements", error);
    }
    ++pos_;
    const std::string_view name = Name();
    if (name.empty()) {
      return Fail("an element without a name", error);
    }
    element->name = LocalName(name);
    for (;;) {
      SkipSpace();
      if (pos_ >= text_.size()) {
        return Fail("unterminated start tag", error);
      }
      if (StartsWith("/>")) {
        pos_ += 2;
        return true;
      }
      if (text_[pos_] == '>') {
        ++pos_;
        break;
      }
      const std::string_view attribute = Name();
      if (attribute.empty()) {
        return Fail("a malformed attribute", error);
      }
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != '=') {
        return Fail("an attribute without a value", error);
      }
      ++pos_;
      SkipSpace();
      if (pos_ >= text_.size() || (text_[pos_] != '"' && text_[pos_] != '\'')) {
        return Fail("an unquoted attribute value", error);
      }
      const char quote = text_[pos_++];
      const size_t end = text_.find(quote, pos_);
      if (end == std::string_view::npos) {
        return Fail("an unterminated attribute value", error);
      }
      std::string value;
      Decode(text_.substr(pos_, end - pos_), &value);
      pos_ = end + 1;
      element->attributes.emplace_back(std::string(LocalName(attribute)),
                                       std::move(value));
    }
    // Content.
    for (;;) {
      const size_t lt = text_.find('<', pos_);
      if (lt == std::string_view::npos) {
        return Fail("an unterminated element", error);
      }
      Decode(text_.substr(pos_, lt - pos_), &element->text);
      pos_ = lt;
      if (StartsWith("</")) {
        pos_ += 2;
        const std::string_view closing = Name();
        if (LocalName(closing) != element->name) {
          return Fail("a mismatched end tag", error);
        }
        SkipSpace();
        if (pos_ >= text_.size() || text_[pos_] != '>') {
          return Fail("a malformed end tag", error);
        }
        ++pos_;
        return true;
      }
      if (StartsWith("<!--")) {
        if (!SkipPast("-->")) return Fail("unterminated comment", error);
      } else if (StartsWith("<![CDATA[")) {
        pos_ += 9;
        const size_t end = text_.find("]]>", pos_);
        if (end == std::string_view::npos) {
          return Fail("unterminated CDATA", error);
        }
        element->text.append(text_.substr(pos_, end - pos_));
        pos_ = end + 3;
      } else if (StartsWith("<?")) {
        if (!SkipPast("?>")) return Fail("unterminated <?", error);
      } else if (StartsWith("<!")) {
        if (!SkipDeclaration()) return Fail("unterminated <!", error);
      } else {
        element->children.emplace_back();
        if (!Element(&element->children.back(), depth + 1, error)) {
          return false;
        }
      }
    }
  }

  std::string_view text_;
  size_t pos_ = 0;
  size_t elements_ = 0;
};

}  // namespace

const std::string* XmlElement::Attribute(std::string_view attribute) const {
  for (const auto& [key, value] : attributes) {
    if (key == attribute) {
      return &value;
    }
  }
  return nullptr;
}

const XmlElement* XmlElement::Child(std::string_view child) const {
  for (const XmlElement& element : children) {
    if (element.name == child) {
      return &element;
    }
  }
  return nullptr;
}

std::vector<const XmlElement*> XmlElement::Children(
    std::string_view child) const {
  std::vector<const XmlElement*> found;
  for (const XmlElement& element : children) {
    if (element.name == child) {
      found.push_back(&element);
    }
  }
  return found;
}

bool ParseXml(std::string_view text, XmlElement* root, std::string* error) {
  *root = {};
  return Parser(text).Document(root, error);
}

}  // namespace lychee
