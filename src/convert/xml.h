// A small non-validating XML parser and writer helpers, sufficient for OOXML and ODF parts.
#pragma once
#include <string>
#include <vector>

namespace conv {

struct XmlNode {
    // Qualified name. Prefixes of well-known namespaces are normalised to the conventional ones
    // ("w:p", "text:p", ...) whatever prefix the document itself declares. Empty for text nodes.
    std::string name;
    std::string text;      // text nodes only
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;

    bool IsText() const { return name.empty(); }
    const std::string* Attr(const char* attrName) const;
    std::string AttrOr(const char* attrName, const char* fallback = "") const;
    const XmlNode* Child(const char* childName) const;
    // Concatenated text of all descendant text nodes.
    std::string InnerText() const;
};

bool XmlParse(const std::string& xml, XmlNode& root, std::string* error = nullptr);
std::string XmlEscape(const std::string& text, bool attribute = false);

// UTF-8 helpers shared by the converters.
void AppendUtf8(std::string& out, uint32_t codePoint);
// Decodes one code point starting at text[pos]; advances pos. Invalid bytes decode as U+FFFD.
uint32_t NextCodePoint(const std::string& text, size_t& pos);

} // namespace conv
