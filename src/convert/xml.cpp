// A small non-validating XML parser and writer helpers, sufficient for OOXML and ODF parts.
#include <cstdint>
#include "xml.h"
#include <cstdlib>
#include <cstring>
#include <map>

namespace conv {

namespace {

struct KnownNamespace { const char* uri; const char* prefix; };

const KnownNamespace kNamespaces[] = {
    // Office Open XML (transitional and strict)
    { "http://schemas.openxmlformats.org/wordprocessingml/2006/main", "w" },
    { "http://purl.oclc.org/ooxml/wordprocessingml/main", "w" },
    { "http://schemas.openxmlformats.org/officeDocument/2006/relationships", "r" },
    { "http://purl.oclc.org/ooxml/officeDocument/relationships", "r" },
    { "http://schemas.openxmlformats.org/drawingml/2006/main", "a" },
    { "http://purl.oclc.org/ooxml/drawingml/main", "a" },
    { "http://schemas.openxmlformats.org/drawingml/2006/picture", "pic" },
    { "http://purl.oclc.org/ooxml/drawingml/picture", "pic" },
    { "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing", "wp" },
    { "http://purl.oclc.org/ooxml/drawingml/wordprocessingDrawing", "wp" },
    { "http://schemas.openxmlformats.org/markup-compatibility/2006", "mc" },
    { "http://schemas.openxmlformats.org/package/2006/relationships", "rel" },
    { "urn:schemas-microsoft-com:vml", "v" },
    { "urn:schemas-microsoft-com:office:office", "o" },
    { "http://schemas.microsoft.com/office/word/2010/wordprocessingShape", "wps" },
    // OpenDocument
    { "urn:oasis:names:tc:opendocument:xmlns:office:1.0", "office" },
    { "urn:oasis:names:tc:opendocument:xmlns:style:1.0", "style" },
    { "urn:oasis:names:tc:opendocument:xmlns:text:1.0", "text" },
    { "urn:oasis:names:tc:opendocument:xmlns:table:1.0", "table" },
    { "urn:oasis:names:tc:opendocument:xmlns:drawing:1.0", "draw" },
    { "urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0", "fo" },
    { "urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0", "svg" },
    { "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0", "manifest" },
    { "http://www.w3.org/1999/xlink", "xlink" },
};

const char* CanonicalPrefix(const std::string& uri)
{
    for (const KnownNamespace& ns : kNamespaces)
        if (uri == ns.uri) return ns.prefix;
    return nullptr;
}

bool IsCanonicalPrefix(const std::string& prefix)
{
    for (const KnownNamespace& ns : kNamespaces)
        if (prefix == ns.prefix) return true;
    return false;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool IsNameChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
           c == ':' || (unsigned char)c >= 0x80;
}

void DecodeEntities(const char* p, size_t n, std::string& out)
{
    size_t i = 0;
    while (i < n) {
        if (p[i] != '&') { out += p[i++]; continue; }
        size_t semi = i + 1;
        while (semi < n && semi - i < 12 && p[semi] != ';') ++semi;
        if (semi >= n || p[semi] != ';') { out += p[i++]; continue; }
        std::string name(p + i + 1, semi - i - 1);
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name.size() > 1 && name[0] == '#') {
            uint32_t cp = (name[1] == 'x' || name[1] == 'X') ? (uint32_t)strtoul(name.c_str() + 2, nullptr, 16)
                                                             : (uint32_t)strtoul(name.c_str() + 1, nullptr, 10);
            AppendUtf8(out, cp);
        } else {
            out.append(p + i, semi - i + 1);   // unknown entity: keep verbatim
        }
        i = semi + 1;
    }
}

typedef std::map<std::string, std::string> Scope;   // document prefix -> namespace URI ("" = default)

std::string Qualify(const std::string& raw, const std::vector<Scope>& scopes, bool isAttribute)
{
    size_t colon = raw.find(':');
    std::string prefix = colon == std::string::npos ? "" : raw.substr(0, colon);
    std::string local = colon == std::string::npos ? raw : raw.substr(colon + 1);
    if (prefix == "xml" || prefix == "xmlns") return raw;
    if (prefix.empty() && isAttribute) return raw;      // unprefixed attributes have no namespace
    for (size_t i = scopes.size(); i-- > 0;) {
        auto it = scopes[i].find(prefix);
        if (it == scopes[i].end()) continue;
        if (const char* canonical = CanonicalPrefix(it->second)) return std::string(canonical) + ":" + local;
        break;
    }
    // Unknown namespace: keep the document's prefix, but never let it collide with a canonical one.
    if (prefix.empty()) return local;
    if (IsCanonicalPrefix(prefix)) return "x-" + prefix + ":" + local;
    return raw;
}

} // namespace

void AppendUtf8(std::string& out, uint32_t cp)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

uint32_t NextCodePoint(const std::string& text, size_t& pos)
{
    size_t n = text.size();
    unsigned char c = (unsigned char)text[pos++];
    if (c < 0x80) return c;
    int extra = (c >= 0xF0 && c <= 0xF4) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC2 && c <= 0xDF) ? 1 : -1;
    if (extra < 0 || c > 0xF4) return 0xFFFD;
    uint32_t cp = c & (0x3F >> extra);
    for (int i = 0; i < extra; ++i) {
        if (pos >= n || ((unsigned char)text[pos] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((unsigned char)text[pos++] & 0x3F);
    }
    return cp;
}

const std::string* XmlNode::Attr(const char* attrName) const
{
    for (const auto& a : attrs)
        if (a.first == attrName) return &a.second;
    return nullptr;
}

std::string XmlNode::AttrOr(const char* attrName, const char* fallback) const
{
    const std::string* v = Attr(attrName);
    return v ? *v : std::string(fallback);
}

const XmlNode* XmlNode::Child(const char* childName) const
{
    for (const XmlNode& c : children)
        if (c.name == childName) return &c;
    return nullptr;
}

std::string XmlNode::InnerText() const
{
    if (IsText()) return text;
    std::string out;
    for (const XmlNode& c : children) out += c.InnerText();
    return out;
}

const size_t kMaxDepth = 256;

bool XmlParse(const std::string& xml, XmlNode& root, std::string* error)
{
    auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    root = XmlNode();
    const char* p = xml.data();
    size_t n = xml.size(), i = 0;
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) i = 3;

    std::vector<XmlNode*> stack;
    std::vector<Scope> scopes;
    bool haveRoot = false;

    while (i < n) {
        if (p[i] != '<') {
            size_t start = i;
            while (i < n && p[i] != '<') ++i;
            if (!stack.empty()) {
                XmlNode text;
                DecodeEntities(p + start, i - start, text.text);
                stack.back()->children.push_back(std::move(text));
            }
            continue;
        }
        if (n - i >= 4 && memcmp(p + i, "<!--", 4) == 0) {
            size_t end = xml.find("-->", i + 4);
            if (end == std::string::npos) return fail("unterminated comment");
            i = end + 3;
            continue;
        }
        if (n - i >= 9 && memcmp(p + i, "<![CDATA[", 9) == 0) {
            size_t end = xml.find("]]>", i + 9);
            if (end == std::string::npos) return fail("unterminated CDATA section");
            if (!stack.empty()) {
                XmlNode text;
                text.text.assign(p + i + 9, end - i - 9);
                stack.back()->children.push_back(std::move(text));
            }
            i = end + 3;
            continue;
        }
        if (i + 1 < n && (p[i + 1] == '?' || p[i + 1] == '!')) {
            // Processing instruction or DOCTYPE: skip (honouring an internal subset in brackets).
            int depth = 0;
            size_t j = i + 2;
            for (; j < n; ++j) {
                if (p[j] == '[') ++depth;
                else if (p[j] == ']') --depth;
                else if (p[j] == '>' && depth <= 0) break;
            }
            if (j >= n) return fail("unterminated declaration");
            i = j + 1;
            continue;
        }
        if (i + 1 < n && p[i + 1] == '/') {
            size_t end = xml.find('>', i + 2);
            if (end == std::string::npos || stack.empty()) return fail("unexpected end tag");
            stack.pop_back();
            scopes.pop_back();
            i = end + 1;
            continue;
        }

        // Start tag.
        size_t j = i + 1;
        size_t nameStart = j;
        while (j < n && IsNameChar(p[j])) ++j;
        if (j == nameStart) return fail("malformed start tag");
        std::string rawName(p + nameStart, j - nameStart);
        std::vector<std::pair<std::string, std::string>> rawAttrs;
        Scope scope;
        bool selfClosing = false;
        for (;;) {
            while (j < n && IsSpace(p[j])) ++j;
            if (j >= n) return fail("unterminated start tag");
            if (p[j] == '>') { ++j; break; }
            if (p[j] == '/' && j + 1 < n && p[j + 1] == '>') { selfClosing = true; j += 2; break; }
            size_t aStart = j;
            while (j < n && IsNameChar(p[j])) ++j;
            if (j == aStart) return fail("malformed attribute");
            std::string aName(p + aStart, j - aStart);
            while (j < n && IsSpace(p[j])) ++j;
            if (j >= n || p[j] != '=') return fail("attribute without value");
            ++j;
            while (j < n && IsSpace(p[j])) ++j;
            if (j >= n || (p[j] != '"' && p[j] != '\'')) return fail("unquoted attribute value");
            char quote = p[j++];
            size_t vStart = j;
            while (j < n && p[j] != quote) ++j;
            if (j >= n) return fail("unterminated attribute value");
            std::string value;
            DecodeEntities(p + vStart, j - vStart, value);
            ++j;
            if (aName == "xmlns") scope[""] = value;
            else if (aName.compare(0, 6, "xmlns:") == 0) scope[aName.substr(6)] = value;
            else rawAttrs.emplace_back(std::move(aName), std::move(value));
        }
        scopes.push_back(std::move(scope));

        XmlNode node;
        node.name = Qualify(rawName, scopes, false);
        for (auto& a : rawAttrs) node.attrs.emplace_back(Qualify(a.first, scopes, true), std::move(a.second));

        XmlNode* placed;
        if (stack.empty()) {
            if (haveRoot) return fail("more than one root element");
            root = std::move(node);
            haveRoot = true;
            placed = &root;
        } else {
            stack.back()->children.push_back(std::move(node));
            placed = &stack.back()->children.back();
        }
        if (selfClosing) scopes.pop_back();
        else {
            // The readers walk the tree recursively, so the depth is bounded (real documents stay below 50).
            if (stack.size() >= kMaxDepth) return fail("the document is nested too deeply");
            stack.push_back(placed);
        }
        i = j;
    }
    if (!haveRoot) return fail("no root element");
    if (!stack.empty()) return fail("unexpected end of document");
    return true;
}

std::string XmlEscape(const std::string& text, bool attribute)
{
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': if (attribute) out += "&quot;"; else out += c; break;
        case '\n': if (attribute) out += "&#10;"; else out += c; break;
        case '\t': if (attribute) out += "&#9;"; else out += c; break;
        default:
            // Control characters are not allowed in XML 1.0.
            if ((unsigned char)c < 0x20 && c != '\n' && c != '\t' && c != '\r') out += ' ';
            else out += c;
        }
    }
    return out;
}

} // namespace conv
