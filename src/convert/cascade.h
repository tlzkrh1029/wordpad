// Property sets with "unset" values, used to resolve style inheritance in DOCX and ODT files.
#pragma once
#include "model.h"
#include <climits>
#include <cstdlib>

namespace conv {
namespace detail {

const int kUnset = INT_MIN;

struct RunStyle {
    bool hasFont = false, hasEaFont = false, hasColor = false, hasHighlight = false;
    std::string font, eaFont;
    Color color, highlight;
    int size = kUnset, bold = kUnset, italic = kUnset, underline = kUnset, strike = kUnset, script = kUnset, hidden = kUnset;

    void Merge(const RunStyle& o)
    {
        if (o.hasFont) { hasFont = true; font = o.font; }
        if (o.hasEaFont) { hasEaFont = true; eaFont = o.eaFont; }
        if (o.hasColor) { hasColor = true; color = o.color; }
        if (o.hasHighlight) { hasHighlight = true; highlight = o.highlight; }
        if (o.size != kUnset) size = o.size;
        if (o.bold != kUnset) bold = o.bold;
        if (o.italic != kUnset) italic = o.italic;
        if (o.underline != kUnset) underline = o.underline;
        if (o.strike != kUnset) strike = o.strike;
        if (o.script != kUnset) script = o.script;
        if (o.hidden != kUnset) hidden = o.hidden;
    }
};

struct ParaStyle {
    int align = kUnset, left = kUnset, right = kUnset, first = kUnset, before = kUnset, after = kUnset;
    int line = kUnset, lineRule = kUnset, numId = kUnset, ilvl = kUnset, pageBreakBefore = kUnset;
    bool hasTabs = false;
    std::vector<TabStop> tabs;
    std::vector<int> clearedTabs;

    void Merge(const ParaStyle& o)
    {
        if (o.align != kUnset) align = o.align;
        if (o.left != kUnset) left = o.left;
        if (o.right != kUnset) right = o.right;
        if (o.first != kUnset) first = o.first;
        if (o.before != kUnset) before = o.before;
        if (o.after != kUnset) after = o.after;
        if (o.line != kUnset) { line = o.line; lineRule = o.lineRule == kUnset ? 0 : o.lineRule; }
        if (o.numId != kUnset) numId = o.numId;
        if (o.ilvl != kUnset) ilvl = o.ilvl;
        if (o.pageBreakBefore != kUnset) pageBreakBefore = o.pageBreakBefore;
        if (o.hasTabs) {
            hasTabs = true;
            for (int pos : o.clearedTabs)
                for (size_t i = 0; i < tabs.size();)
                    if (std::abs(tabs[i].pos - pos) <= 20) tabs.erase(tabs.begin() + (ptrdiff_t)i); else ++i;
            for (const TabStop& t : o.tabs) {
                bool replaced = false;
                for (TabStop& mine : tabs) if (mine.pos == t.pos) { mine = t; replaced = true; }
                if (!replaced) tabs.push_back(t);
            }
        }
    }

    void ApplyTo(ParaProps& pp) const
    {
        pp.align = align == kUnset ? 0 : align;
        pp.left = left == kUnset ? 0 : left;
        pp.right = right == kUnset ? 0 : right;
        pp.first = first == kUnset ? 0 : first;
        pp.before = before == kUnset ? 0 : before;
        pp.after = after == kUnset ? 0 : after;
        pp.lineRule = lineRule == kUnset ? 0 : lineRule;
        pp.line = line == kUnset ? 0 : line;
        if (pp.lineRule == 0 && pp.line == 240) pp.line = 0;     // both mean single spacing
        pp.pageBreakBefore = pageBreakBefore == 1;
        pp.tabs = tabs;
        std::sort(pp.tabs.begin(), pp.tabs.end(), [](const TabStop& a, const TabStop& b) { return a.pos < b.pos; });
    }
};

// Numbers come straight from the document and may be absurd ("1e309", twenty digits): every
// conversion saturates instead of overflowing.
inline int ToInt(const std::string* s, int fallback)
{
    if (!s || s->empty()) return fallback;
    char* end = nullptr;
    double v = strtod(s->c_str(), &end);
    if (end == s->c_str()) return fallback;
    return RoundToInt(v);
}

// "12pt", "1.5in", "2.54cm", "10mm", "96px", "2pc" -> twips. Returns fallback for other input ("50%").
inline int LengthToTwips(const std::string& text, int fallback)
{
    char* end = nullptr;
    double v = strtod(text.c_str(), &end);
    if (end == text.c_str()) return fallback;
    std::string unit = end;
    while (!unit.empty() && unit[0] == ' ') unit.erase(0, 1);
    double factor;
    if (unit.compare(0, 2, "in") == 0) factor = 1440.0;
    else if (unit.compare(0, 2, "cm") == 0) factor = 1440.0 / 2.54;
    else if (unit.compare(0, 2, "mm") == 0) factor = 1440.0 / 25.4;
    else if (unit.compare(0, 2, "px") == 0) factor = 15.0;
    else if (unit.compare(0, 2, "pc") == 0) factor = 240.0;
    else if (unit.compare(0, 2, "pt") == 0 || unit.empty()) factor = 20.0;
    else return fallback;
    return RoundToInt(v * factor);
}

inline std::string DirName(const std::string& path)
{
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// Resolves a relative reference against the directory of the part that contains it.
inline std::string ResolvePath(const std::string& baseDir, const std::string& target)
{
    std::string path = !target.empty() && target[0] == '/' ? target.substr(1) : baseDir + target;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) out += '/'; out += parts[i]; }
    return out;
}

} // namespace detail
} // namespace conv
