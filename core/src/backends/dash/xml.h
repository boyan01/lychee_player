#ifndef LYCHEE_BACKENDS_DASH_XML_H_
#define LYCHEE_BACKENDS_DASH_XML_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lychee {

// An element of a small XML document (an MPEG-DASH manifest). Names are
// local: a namespace prefix is dropped ("cenc:pssh" is "pssh").
struct XmlElement {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<XmlElement> children;
  // The character data directly inside, entities and CDATA decoded.
  std::string text;

  // Null if absent.
  const std::string* Attribute(std::string_view attribute) const;
  // The first child named |child|, or null.
  const XmlElement* Child(std::string_view child) const;
  std::vector<const XmlElement*> Children(std::string_view child) const;
};

// Parses |text| (UTF-8, with or without a BOM) into its root element.
// Comments, processing instructions and a DOCTYPE are skipped; the
// predefined and numeric character references are decoded. False with
// |error| set if it is not well-formed, nests deeper than 64 elements, or
// has more than 200000 of them.
bool ParseXml(std::string_view text, XmlElement* root, std::string* error);

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_DASH_XML_H_
