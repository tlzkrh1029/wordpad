// OpenDocument Text (.odt) <-> document model.
#include "model.h"
#include "cascade.h"
#include "xml.h"
#include "zip.h"
#include <cstdio>
#include <cstring>
#include <map>

namespace conv {

using namespace detail;

namespace {

struct OdfStyle {
    std::string parent;
    std::string family;
    RunStyle run;
    ParaStyle para;
    int columnWidth = 0;        // table-column styles
    bool resolved = false;
};

struct ListLevel {
    int kind = 1;
    int start = 1;
    int left = kUnset, first = kUnset;
};

typedef std::map<int, ListLevel> ListStyle;     // level (1-based) -> definition

// ===========================================================================
// Reader
// ===========================================================================
class OdtReader {
public:
    OdtReader(const Bytes& file, Document& doc) : m_file(file), m_doc(doc) {}

    bool Read(std::string* error)
    {
        if (!m_zip.Open(m_file)) return Fail(error, "not a ZIP package");
        std::string xml;
        XmlNode content;
        if (!m_zip.ReadText("content.xml", xml) || !XmlParse(xml, content)) return Fail(error, "content.xml is missing or malformed");

        // styles.xml holds the named styles, content.xml the automatic ones.
        XmlNode styles;
        std::string stylesXml;
        if (m_zip.ReadText("styles.xml", stylesXml) && XmlParse(stylesXml, styles)) {
            LoadFonts(styles.Child("office:font-face-decls"));
            LoadStyles(styles.Child("office:styles"));
        }
        LoadFonts(content.Child("office:font-face-decls"));
        LoadStyles(content.Child("office:automatic-styles"));

        const XmlNode* body = content.Child("office:body");
        const XmlNode* text = body ? body->Child("office:text") : nullptr;
        if (!text) return Fail(error, "the document has no text body");
        WalkBlocks(*text, m_doc.blocks, std::string(), 0);

        RunStyle rs = m_defaultRun;
        if (const OdfStyle* standard = Resolve("paragraph", "Standard")) rs.Merge(standard->run);
        CharProps cp = ToCharProps(rs);
        m_doc.defaultFont = cp.font;
        m_doc.defaultHalfPoints = cp.halfPoints;
        return true;
    }

private:
    const Bytes& m_file;
    Document& m_doc;
    ZipReader m_zip;
    std::map<std::string, std::string> m_fontFaces;      // style:name -> family
    std::map<std::string, OdfStyle> m_styles;            // "family/name"
    std::map<std::string, ListStyle> m_listStyles;
    RunStyle m_defaultRun;
    ParaStyle m_defaultPara;
    std::map<std::string, int> m_pictureCache;
    std::vector<Paragraph> m_pending;                    // text-box paragraphs

    // white-space handling state for the paragraph being read
    bool m_lastWasSpace = true;

    // list numbering
    std::map<std::string, int> m_listCounters;           // "style/level" -> last number
    std::string m_lastListKey;
    int m_lastListStart = 1;
    bool m_previousWasListItem = false;

    static bool Fail(std::string* error, const char* message)
    {
        if (error) *error = message;
        return false;
    }

    static std::string CleanFontName(std::string name)
    {
        // "'Malgun Gothic', sans-serif" -> Malgun Gothic
        size_t comma = name.find(',');
        if (comma != std::string::npos) name = name.substr(0, comma);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\'' || name.back() == '"')) name.pop_back();
        size_t start = 0;
        while (start < name.size() && (name[start] == ' ' || name[start] == '\'' || name[start] == '"')) ++start;
        return name.substr(start);
    }

    void LoadFonts(const XmlNode* decls)
    {
        if (!decls) return;
        for (const XmlNode& f : decls->children) {
            if (f.name != "style:font-face") continue;
            std::string name = f.AttrOr("style:name");
            std::string family = CleanFontName(f.AttrOr("svg:font-family", name.c_str()));
            if (!name.empty()) m_fontFaces[name] = family.empty() ? name : family;
        }
    }

    std::string FontFamily(const std::string& styleFontName) const
    {
        auto it = m_fontFaces.find(styleFontName);
        return it != m_fontFaces.end() ? it->second : CleanFontName(styleFontName);
    }

    void ParseTextProps(const XmlNode& tp, RunStyle& s)
    {
        if (const std::string* v = tp.Attr("style:font-name")) { s.hasFont = true; s.font = FontFamily(*v); }
        else if (const std::string* v2 = tp.Attr("fo:font-family")) { s.hasFont = true; s.font = CleanFontName(*v2); }
        if (const std::string* v = tp.Attr("style:font-name-asian")) { s.hasEaFont = true; s.eaFont = FontFamily(*v); }
        else if (const std::string* v2 = tp.Attr("style:font-family-asian")) { s.hasEaFont = true; s.eaFont = CleanFontName(*v2); }
        if (const std::string* v = tp.Attr("fo:font-size")) {
            if (v->find('%') == std::string::npos) {
                int tw = LengthToTwips(*v, 0);
                if (tw > 0) s.size = (tw + 5) / 10;      // twips -> half points
            }
        }
        if (const std::string* v = tp.Attr("fo:font-weight")) s.bold = (*v == "bold" || atoi(v->c_str()) >= 600) ? 1 : 0;
        if (const std::string* v = tp.Attr("fo:font-style")) s.italic = (*v == "italic" || *v == "oblique") ? 1 : 0;
        if (const std::string* v = tp.Attr("style:text-underline-style")) s.underline = *v != "none" ? 1 : 0;
        if (const std::string* v = tp.Attr("style:text-line-through-style")) s.strike = *v != "none" ? 1 : 0;
        if (const std::string* v = tp.Attr("style:text-position")) {
            if (v->compare(0, 5, "super") == 0) s.script = 1;
            else if (v->compare(0, 3, "sub") == 0) s.script = 2;
            else {
                double pct = strtod(v->c_str(), nullptr);
                s.script = pct > 0 ? 1 : pct < 0 ? 2 : 0;
            }
        }
        if (const std::string* v = tp.Attr("fo:color")) { s.hasColor = true; s.color = Color(); ParseHexColor(*v, s.color); }
        if (const std::string* v = tp.Attr("fo:background-color")) { s.hasHighlight = true; s.highlight = Color(); ParseHexColor(*v, s.highlight); }
        if (const std::string* v = tp.Attr("text:display")) s.hidden = *v == "none" ? 1 : 0;
    }

    void ParseParagraphProps(const XmlNode& pp, ParaStyle& s)
    {
        if (const std::string* v = pp.Attr("fo:text-align")) {
            s.align = *v == "center" ? 1 : (*v == "end" || *v == "right") ? 2 : *v == "justify" ? 3 : 0;
        }
        if (const std::string* v = pp.Attr("fo:margin-left")) s.left = LengthToTwips(*v, 0);
        if (const std::string* v = pp.Attr("fo:margin-right")) s.right = LengthToTwips(*v, 0);
        if (const std::string* v = pp.Attr("fo:text-indent")) s.first = LengthToTwips(*v, 0);
        if (const std::string* v = pp.Attr("fo:margin-top")) s.before = LengthToTwips(*v, 0);
        if (const std::string* v = pp.Attr("fo:margin-bottom")) s.after = LengthToTwips(*v, 0);
        if (const std::string* v = pp.Attr("fo:line-height")) {
            if (v->find('%') != std::string::npos) {
                double pct = strtod(v->c_str(), nullptr);
                if (pct > 0) { s.line = (int)(pct * 2.4 + 0.5); s.lineRule = 0; }
            } else if (*v != "normal") {
                int tw = LengthToTwips(*v, 0);
                if (tw > 0) { s.line = tw; s.lineRule = 1; }
            }
        }
        if (const std::string* v = pp.Attr("style:line-height-at-least")) {
            int tw = LengthToTwips(*v, 0);
            if (tw > 0) { s.line = tw; s.lineRule = 2; }
        }
        if (const std::string* v = pp.Attr("fo:break-before")) s.pageBreakBefore = *v == "page" ? 1 : 0;
        if (const XmlNode* tabs = pp.Child("style:tab-stops")) {
            s.hasTabs = true;
            for (const XmlNode& t : tabs->children) {
                if (t.name != "style:tab-stop") continue;
                TabStop ts;
                ts.pos = LengthToTwips(t.AttrOr("style:position"), 0);
                std::string type = t.AttrOr("style:type", "left");
                ts.align = type == "center" ? 1 : type == "right" ? 2 : type == "char" ? 3 : 0;
                if (ts.pos > 0) s.tabs.push_back(ts);
            }
        }
    }

    void LoadStyles(const XmlNode* container)
    {
        if (!container) return;
        for (const XmlNode& s : container->children) {
            if (s.name == "style:default-style") {
                std::string family = s.AttrOr("style:family");
                if (family == "paragraph") {
                    if (const XmlNode* tp = s.Child("style:text-properties")) ParseTextProps(*tp, m_defaultRun);
                    if (const XmlNode* pp = s.Child("style:paragraph-properties")) ParseParagraphProps(*pp, m_defaultPara);
                }
            } else if (s.name == "style:style") {
                OdfStyle style;
                style.family = s.AttrOr("style:family");
                style.parent = s.AttrOr("style:parent-style-name");
                if (const XmlNode* tp = s.Child("style:text-properties")) ParseTextProps(*tp, style.run);
                if (const XmlNode* pp = s.Child("style:paragraph-properties")) ParseParagraphProps(*pp, style.para);
                if (const XmlNode* cp = s.Child("style:table-column-properties"))
                    style.columnWidth = LengthToTwips(cp->AttrOr("style:column-width"), 0);
                m_styles[style.family + "/" + s.AttrOr("style:name")] = style;
            } else if (s.name == "text:list-style") {
                ListStyle& list = m_listStyles[s.AttrOr("style:name")];
                for (const XmlNode& l : s.children) {
                    bool bullet = l.name == "text:list-level-style-bullet";
                    bool number = l.name == "text:list-level-style-number";
                    if (!bullet && !number) continue;
                    ListLevel level;
                    int n = ToInt(l.Attr("text:level"), 1);
                    if (bullet) level.kind = 1;
                    else {
                        std::string fmt = l.AttrOr("style:num-format", "1");
                        level.kind = fmt == "a" ? 3 : fmt == "A" ? 4 : fmt == "i" ? 5 : fmt == "I" ? 6 : fmt.empty() ? 0 : 2;
                        level.start = ToInt(l.Attr("text:start-value"), 1);
                    }
                    if (const XmlNode* props = l.Child("style:list-level-properties")) {
                        if (const XmlNode* align = props->Child("style:list-level-label-alignment")) {
                            if (const std::string* v = align->Attr("fo:margin-left")) level.left = LengthToTwips(*v, 0);
                            if (const std::string* v = align->Attr("fo:text-indent")) level.first = LengthToTwips(*v, 0);
                        } else {
                            // Older "label-width-and-position" model.
                            int space = LengthToTwips(props->AttrOr("text:space-before", "0"), 0);
                            int width = LengthToTwips(props->AttrOr("text:min-label-width", "0"), 0);
                            if (space || width) { level.left = space + width; level.first = -width; }
                        }
                    }
                    list[n] = level;
                }
            }
        }
    }

    const OdfStyle* Resolve(const std::string& family, const std::string& name, int depth = 0)
    {
        if (name.empty()) return nullptr;
        auto it = m_styles.find(family + "/" + name);
        if (it == m_styles.end()) return nullptr;
        OdfStyle& style = it->second;
        if (!style.resolved && depth < 24) {
            style.resolved = true;
            if (!style.parent.empty() && style.parent != name) {
                if (const OdfStyle* base = Resolve(family, style.parent, depth + 1)) {
                    RunStyle run = base->run;
                    run.Merge(style.run);
                    style.run = run;
                    ParaStyle para = base->para;
                    para.Merge(style.para);
                    style.para = para;
                }
            }
        }
        return &style;
    }

    CharProps ToCharProps(const RunStyle& rs) const
    {
        CharProps cp;
        cp.font = rs.hasFont ? rs.font : std::string("Liberation Serif");
        cp.halfPoints = rs.size != kUnset ? rs.size : 24;
        cp.bold = rs.bold == 1;
        cp.italic = rs.italic == 1;
        cp.underline = rs.underline == 1;
        cp.strike = rs.strike == 1;
        cp.script = rs.script == kUnset ? 0 : rs.script;
        if (rs.hasColor) cp.color = rs.color;
        if (rs.hasHighlight) cp.highlight = rs.highlight;
        return cp;
    }

    // -----------------------------------------------------------------------
    // listStyle / listLevel describe the enclosing text:list (level 0 = not in a list).
    void WalkBlocks(const XmlNode& container, std::vector<Block>& out, const std::string& listStyle, int listLevel,
                    bool listHeader = false, int itemStart = 0)
    {
        bool firstInItem = true;
        for (const XmlNode& c : container.children) {
            const std::string& n = c.name;
            if (n == "text:p" || n == "text:h") {
                Block block;
                // Only the first paragraph of a list item carries the label.
                bool labelled = listLevel > 0 && firstInItem && !listHeader;
                ParseParagraph(c, block.para, listStyle, listLevel, labelled, itemStart);
                firstInItem = false;
                out.push_back(std::move(block));
                FlushPending(out);
            } else if (n == "text:list") {
                std::string style = c.AttrOr("text:style-name", listStyle.c_str());
                bool continues = c.AttrOr("text:continue-numbering") == "true";
                if (listLevel == 0 && !continues) {
                    // A new top-level list restarts its numbering.
                    for (auto& kv : m_listCounters)
                        if (kv.first.compare(0, style.size() + 1, style + "/") == 0) kv.second = 0;
                }
                for (const XmlNode& item : c.children) {
                    if (item.name != "text:list-item" && item.name != "text:list-header") continue;
                    WalkBlocks(item, out, style, listLevel + 1, item.name == "text:list-header", ToInt(item.Attr("text:start-value"), 0));
                }
                firstInItem = false;
            } else if (n == "table:table") {
                Block block;
                block.isTable = true;
                ParseTable(c, block.rows);
                if (!block.rows.empty()) out.push_back(std::move(block));
                m_previousWasListItem = false;
            } else if (n == "text:section" || n == "text:index-body" || n == "text:table-of-content" ||
                       n == "text:alphabetical-index" || n == "text:illustration-index" || n == "text:user-index" ||
                       n == "text:bibliography") {
                WalkBlocks(c, out, listStyle, listLevel);
            } else if (n == "draw:frame") {
                // A frame anchored to the page or paragraph: give it a paragraph of its own.
                Block block;
                m_defaultPara.ApplyTo(block.para.props);
                ParseFrame(c, ToCharProps(m_defaultRun), block.para.items);
                if (!block.para.items.empty()) out.push_back(std::move(block));
                FlushPending(out);
            }
        }
    }

    void FlushPending(std::vector<Block>& out)
    {
        if (m_pending.empty()) return;
        std::vector<Paragraph> pending;
        pending.swap(m_pending);
        for (Paragraph& p : pending) {
            Block block;
            block.para = std::move(p);
            out.push_back(std::move(block));
        }
        m_previousWasListItem = false;
    }

    void ParseParagraph(const XmlNode& p, Paragraph& para, const std::string& listStyle, int listLevel, bool labelled, int itemStart)
    {
        std::string styleName = p.AttrOr("text:style-name", "Standard");
        const OdfStyle* style = Resolve("paragraph", styleName);
        ParaStyle ps = m_defaultPara;

        const ListLevel* level = nullptr;
        if (listLevel > 0) {
            auto ls = m_listStyles.find(listStyle);
            if (ls != m_listStyles.end()) {
                auto lv = ls->second.find(listLevel);
                if (lv != ls->second.end()) level = &lv->second;
            }
        }
        if (level) {
            if (level->left != kUnset) ps.left = level->left;
            if (level->first != kUnset) ps.first = level->first;
        }
        if (style) ps.Merge(style->para);
        ps.ApplyTo(para.props);

        if (level && level->kind != 0 && labelled) {
            para.props.list = level->kind;
            std::string key = listStyle + "/" + std::to_string(listLevel);
            for (auto& kv : m_listCounters) {
                // Deeper levels of the same list restart.
                size_t slash = kv.first.rfind('/');
                if (kv.first.compare(0, slash, listStyle) == 0 && slash == listStyle.size() && atoi(kv.first.c_str() + slash + 1) > listLevel)
                    kv.second = 0;
            }
            int& counter = m_listCounters[key];
            if (itemStart > 0) counter = itemStart;
            else counter = counter == 0 ? level->start : counter + 1;
            bool continues = m_previousWasListItem && m_lastListKey == key && itemStart == 0;
            para.props.listStart = continues ? m_lastListStart : counter;
            m_lastListKey = key;
            m_lastListStart = para.props.listStart;
            m_previousWasListItem = true;
        } else if (!(listLevel > 0 && !labelled)) {
            m_previousWasListItem = false;
        }

        RunStyle rs = m_defaultRun;
        if (style) rs.Merge(style->run);
        m_lastWasSpace = true;        // leading white space of a paragraph is dropped
        WalkInline(p, rs, std::string(), para.items);
    }

    void AddText(const std::string& raw, const RunStyle& rs, const std::string& link, std::vector<Inline>& out)
    {
        // ODF white-space rules: tabs and line ends count as spaces, runs of spaces collapse.
        std::string text;
        for (char ch : raw) {
            char c = (ch == '\t' || ch == '\r' || ch == '\n') ? ' ' : ch;
            if (c == ' ') {
                if (m_lastWasSpace) continue;
                m_lastWasSpace = true;
            } else {
                m_lastWasSpace = false;
            }
            text += c;
        }
        AddLiteral(text, rs, link, out);
    }

    void AddLiteral(const std::string& text, const RunStyle& rs, const std::string& link, std::vector<Inline>& out)
    {
        if (text.empty() || rs.hidden == 1) return;
        CharProps cp = ToCharProps(rs);
        cp.link = link;
        size_t before = out.size();
        SplitByScript(text, cp, rs.hasEaFont ? rs.eaFont : std::string(), out);
        if (before > 0 && out.size() > before && out[before - 1].kind == Inline::Text && out[before].kind == Inline::Text &&
            out[before - 1].props == out[before].props) {
            out[before - 1].text += out[before].text;
            out.erase(out.begin() + (ptrdiff_t)before);
        }
    }

    void WalkInline(const XmlNode& container, const RunStyle& rs, const std::string& link, std::vector<Inline>& out)
    {
        for (const XmlNode& c : container.children) {
            if (c.IsText()) { AddText(c.text, rs, link, out); continue; }
            const std::string& n = c.name;
            if (n == "text:span") {
                RunStyle inner = rs;
                if (const OdfStyle* style = Resolve("text", c.AttrOr("text:style-name"))) inner.Merge(style->run);
                WalkInline(c, inner, link, out);
            } else if (n == "text:a") {
                RunStyle inner = rs;
                if (const OdfStyle* style = Resolve("text", c.AttrOr("text:style-name"))) inner.Merge(style->run);
                WalkInline(c, inner, c.AttrOr("xlink:href", link.c_str()), out);
            } else if (n == "text:s") {
                int count = std::max(1, ToInt(c.Attr("text:c"), 1));
                AddLiteral(std::string((size_t)std::min(count, 1000), ' '), rs, link, out);
                m_lastWasSpace = true;
            } else if (n == "text:tab") {
                Inline i;
                i.kind = Inline::Tab;
                i.props = ToCharProps(rs);
                out.push_back(i);
                m_lastWasSpace = true;
            } else if (n == "text:line-break") {
                Inline i;
                i.kind = Inline::LineBreak;
                i.props = ToCharProps(rs);
                out.push_back(i);
                m_lastWasSpace = true;
            } else if (n == "draw:frame") {
                ParseFrame(c, ToCharProps(rs), out);
                m_lastWasSpace = false;
            } else if (n == "text:note" || n == "office:annotation" || n == "office:annotation-end" || n == "text:tracked-changes" ||
                       n == "text:bookmark" || n == "text:bookmark-start" || n == "text:bookmark-end" ||
                       n == "text:reference-mark" || n == "text:soft-page-break" || n == "text:change" ||
                       n == "text:change-start" || n == "text:change-end") {
                // not represented
            } else if (n == "text:ruby") {
                if (const XmlNode* base = c.Child("text:ruby-base")) WalkInline(*base, rs, link, out);
            } else {
                // Fields (page numbers, dates, ...) and unknown inline wrappers: keep their text.
                WalkInline(c, rs, link, out);
            }
        }
    }

    void ParseFrame(const XmlNode& frame, const CharProps& cp, std::vector<Inline>& out)
    {
        if (const XmlNode* box = frame.Child("draw:text-box")) {
            std::vector<Block> blocks;
            bool savedList = m_previousWasListItem;
            bool savedSpace = m_lastWasSpace;
            WalkBlocks(*box, blocks, std::string(), 0);
            m_previousWasListItem = savedList;
            m_lastWasSpace = savedSpace;
            for (Block& b : blocks) {
                if (!b.isTable) { m_pending.push_back(std::move(b.para)); continue; }
                for (Row& row : b.rows)
                    for (Cell& cell : row.cells)
                        for (Paragraph& para : cell.paras) m_pending.push_back(std::move(para));
            }
            return;
        }
        const XmlNode* image = frame.Child("draw:image");
        if (!image) return;
        std::string href = image->AttrOr("xlink:href");
        if (href.empty() || href.find("://") != std::string::npos) return;
        std::string path = ResolvePath("", href);
        int index;
        auto cached = m_pictureCache.find(path);
        if (cached != m_pictureCache.end()) {
            index = cached->second;
        } else {
            Bytes data;
            index = m_zip.Read(path, data) ? AddPicture(m_doc, data, PictureKind::AnyImageFile, 0, 0) : -1;
            m_pictureCache[path] = index;
        }
        if (index < 0) return;
        const Picture& pic = m_doc.pictures[(size_t)index];
        Inline item;
        item.kind = Inline::Picture;
        item.picture = index;
        item.props = cp;
        item.widthTwips = LengthToTwips(frame.AttrOr("svg:width"), 0);
        item.heightTwips = LengthToTwips(frame.AttrOr("svg:height"), 0);
        if (item.widthTwips <= 0 || item.heightTwips <= 0) { item.widthTwips = pic.widthPx * 15; item.heightTwips = pic.heightPx * 15; }
        out.push_back(std::move(item));
    }

    void ParseTable(const XmlNode& table, std::vector<Row>& rows)
    {
        std::vector<int> widths;
        CollectColumns(table, widths);
        CollectRows(table, widths, rows);
    }

    void CollectColumns(const XmlNode& container, std::vector<int>& widths)
    {
        for (const XmlNode& c : container.children) {
            if (c.name == "table:table-column") {
                int repeat = std::min(256, std::max(1, ToInt(c.Attr("table:number-columns-repeated"), 1)));
                const OdfStyle* style = Resolve("table-column", c.AttrOr("table:style-name"));
                for (int i = 0; i < repeat; ++i) widths.push_back(style ? style->columnWidth : 0);
            } else if (c.name == "table:table-columns" || c.name == "table:table-header-columns" || c.name == "table:table-column-group") {
                CollectColumns(c, widths);
            }
        }
    }

    void CollectRows(const XmlNode& container, const std::vector<int>& widths, std::vector<Row>& rows)
    {
        for (const XmlNode& c : container.children) {
            if (c.name == "table:table-rows" || c.name == "table:table-header-rows" || c.name == "table:table-row-group") {
                CollectRows(c, widths, rows);
                continue;
            }
            if (c.name != "table:table-row") continue;
            Row row;
            size_t column = 0;
            for (const XmlNode& tc : c.children) {
                if (tc.name == "table:covered-table-cell") { ++column; continue; }
                if (tc.name != "table:table-cell") continue;
                int span = std::max(1, ToInt(tc.Attr("table:number-columns-spanned"), 1));
                Cell cell;
                for (int i = 0; i < span; ++i)
                    if (column + (size_t)i < widths.size()) cell.width += widths[column + (size_t)i];
                if (cell.width <= 0) cell.width = 1440 * span;
                column += (size_t)span;
                std::vector<Block> blocks;
                m_previousWasListItem = false;
                WalkBlocks(tc, blocks, std::string(), 0);
                for (Block& b : blocks) {
                    if (!b.isTable) { cell.paras.push_back(std::move(b.para)); continue; }
                    for (Row& r : b.rows)
                        for (Cell& inner : r.cells)
                            for (Paragraph& p : inner.paras) cell.paras.push_back(std::move(p));
                }
                if (cell.paras.empty()) cell.paras.push_back(Paragraph());
                row.cells.push_back(std::move(cell));
            }
            if (!row.cells.empty()) rows.push_back(std::move(row));
        }
    }
};

// ===========================================================================
// Writer
// ===========================================================================
const char* const kOdfNamespaces =
    " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
    " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
    " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\""
    " xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\""
    " xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\""
    " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\""
    " xmlns:xlink=\"http://www.w3.org/1999/xlink\""
    " xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\""
    " office:version=\"1.2\"";

std::string Inches(int twips)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4fin", twips / 1440.0);
    return buf;
}

class OdtWriter {
public:
    explicit OdtWriter(const Document& doc) : m_doc(doc) {}

    bool Write(Bytes& out)
    {
        m_defaultFont = m_doc.defaultFont.empty() ? std::string("Calibri") : m_doc.defaultFont;
        m_defaultSize = m_doc.defaultHalfPoints > 0 ? m_doc.defaultHalfPoints : 22;
        UseFont(m_defaultFont);

        std::string body;
        WriteBlocks(m_doc.blocks, body);

        std::string content = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content";
        content += kOdfNamespaces;
        content += ">" + FontDecls() + "<office:automatic-styles>" + m_autoStyles + "</office:automatic-styles>"
                   "<office:body><office:text>" + body + "</office:text></office:body></office:document-content>";

        ZipWriter zip;
        zip.Add("mimetype", std::string("application/vnd.oasis.opendocument.text"), false);   // must be first and stored
        zip.Add("content.xml", content);
        zip.Add("styles.xml", Styles());
        zip.Add("meta.xml", std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
                "xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" office:version=\"1.2\">"
                "<office:meta><meta:generator>RichPad</meta:generator></office:meta></office:document-meta>"));
        std::string manifest = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
            "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"application/vnd.oasis.opendocument.text\"/>"
            "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
            "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
            "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>";
        for (size_t i = 0; i < m_doc.pictures.size(); ++i) {
            if (!m_pictureUsed.count((int)i)) continue;
            std::string name = PictureName((int)i);
            zip.Add(name, m_doc.pictures[i].data, false);
            manifest += "<manifest:file-entry manifest:full-path=\"" + name + "\" manifest:media-type=\"" +
                        (m_doc.pictures[i].jpeg ? "image/jpeg" : "image/png") + "\"/>";
        }
        manifest += "</manifest:manifest>";
        zip.Add("META-INF/manifest.xml", manifest);
        out = zip.Finish();
        return true;
    }

private:
    const Document& m_doc;
    std::string m_defaultFont;
    int m_defaultSize = 22;
    std::string m_autoStyles;
    std::map<std::string, std::string> m_paraStyles;    // serialized properties -> style name
    std::map<std::string, std::string> m_textStyles;
    std::vector<std::string> m_fonts;
    std::map<int, bool> m_listStyleWritten;             // key: kind * 100000 + start
    std::map<int, int> m_pictureUsed;
    bool m_graphicStyleWritten = false;
    int m_tableCount = 0, m_frameCount = 0;
    bool m_lastWasSpace = true;
    bool m_atParagraphEnd = false;      // the item being written is the last one of its paragraph

    void UseFont(const std::string& font)
    {
        for (const std::string& f : m_fonts) if (f == font) return;
        m_fonts.push_back(font);
    }

    std::string FontDecls() const
    {
        std::string out = "<office:font-face-decls>";
        for (const std::string& f : m_fonts) {
            std::string name = XmlEscape(f, true);
            bool quote = f.find(' ') != std::string::npos;
            out += "<style:font-face style:name=\"" + name + "\" svg:font-family=\"" + (quote ? "'" + name + "'" : name) + "\"/>";
        }
        out += "</office:font-face-decls>";
        return out;
    }

    std::string PictureName(int index) const
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Pictures/image%d.%s", index + 1, m_doc.pictures[(size_t)index].jpeg ? "jpg" : "png");
        return buf;
    }

    std::string TextProps(const CharProps& cp)
    {
        std::string out;
        const std::string& font = cp.font.empty() ? m_defaultFont : cp.font;
        if (font != m_defaultFont) {
            UseFont(font);
            std::string f = XmlEscape(font, true);
            out += " style:font-name=\"" + f + "\" style:font-name-asian=\"" + f + "\" style:font-name-complex=\"" + f + "\"";
        }
        int size = cp.halfPoints > 0 ? cp.halfPoints : m_defaultSize;
        if (size != m_defaultSize) {
            char buf[120];
            snprintf(buf, sizeof(buf), " fo:font-size=\"%gpt\" style:font-size-asian=\"%gpt\" style:font-size-complex=\"%gpt\"",
                     size / 2.0, size / 2.0, size / 2.0);
            out += buf;
        }
        if (cp.bold) out += " fo:font-weight=\"bold\" style:font-weight-asian=\"bold\" style:font-weight-complex=\"bold\"";
        if (cp.italic) out += " fo:font-style=\"italic\" style:font-style-asian=\"italic\" style:font-style-complex=\"italic\"";
        if (cp.underline) out += " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"";
        if (cp.strike) out += " style:text-line-through-style=\"solid\" style:text-line-through-type=\"single\"";
        if (cp.script == 1) out += " style:text-position=\"super 58%\"";
        else if (cp.script == 2) out += " style:text-position=\"sub 58%\"";
        if (cp.color.set) out += " fo:color=\"#" + HexColor(cp.color) + "\"";
        if (cp.highlight.set) out += " fo:background-color=\"#" + HexColor(cp.highlight) + "\"";
        return out;
    }

    std::string TextStyle(const CharProps& cp)
    {
        std::string props = TextProps(cp);
        if (props.empty()) return std::string();
        auto it = m_textStyles.find(props);
        if (it != m_textStyles.end()) return it->second;
        std::string name = "T" + std::to_string(m_textStyles.size() + 1);
        m_textStyles[props] = name;
        m_autoStyles += "<style:style style:name=\"" + name + "\" style:family=\"text\"><style:text-properties" + props + "/></style:style>";
        return name;
    }

    std::string ParaStyleName(const Paragraph& para)
    {
        const ParaProps& p = para.props;
        static const char* const kAlign[] = { "start", "center", "end", "justify" };
        std::string props = std::string(" fo:text-align=\"") + kAlign[p.align >= 0 && p.align <= 3 ? p.align : 0] + "\"";
        props += " fo:margin-left=\"" + Inches(p.left) + "\" fo:margin-right=\"" + Inches(p.right) + "\" fo:text-indent=\"" + Inches(p.first) + "\"";
        props += " fo:margin-top=\"" + Inches(p.before) + "\" fo:margin-bottom=\"" + Inches(p.after) + "\"";
        if (p.lineRule == 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), " fo:line-height=\"%d%%\"", p.line > 0 ? (p.line * 100 + 120) / 240 : 100);
            props += buf;
        } else if (p.lineRule == 1) {
            props += " fo:line-height=\"" + Inches(p.line) + "\"";
        } else {
            props += " style:line-height-at-least=\"" + Inches(p.line) + "\"";
        }
        if (p.pageBreakBefore) props += " fo:break-before=\"page\"";
        std::string tabs;
        if (!p.tabs.empty()) {
            tabs = "<style:tab-stops>";
            for (const TabStop& t : p.tabs) {
                static const char* const kType[] = { "left", "center", "right", "char" };
                tabs += "<style:tab-stop style:position=\"" + Inches(t.pos) + "\" style:type=\"" + kType[t.align >= 0 && t.align <= 3 ? t.align : 0] + "\"";
                if (t.align == 3) tabs += " style:char=\".\"";
                tabs += "/>";
            }
            tabs += "</style:tab-stops>";
        }
        std::string key = props + "|" + tabs;
        auto it = m_paraStyles.find(key);
        if (it != m_paraStyles.end()) return it->second;
        std::string name = "P" + std::to_string(m_paraStyles.size() + 1);
        m_paraStyles[key] = name;
        m_autoStyles += "<style:style style:name=\"" + name + "\" style:family=\"paragraph\" style:parent-style-name=\"Standard\">"
                        "<style:paragraph-properties" + props + (tabs.empty() ? "/>" : ">" + tabs + "</style:paragraph-properties>");
        m_autoStyles += "</style:style>";
        return name;
    }

    std::string ListStyleName(int kind, int start)
    {
        int key = kind * 100000 + std::min(start, 99999);
        std::string name = "L" + std::to_string(key);
        if (m_listStyleWritten[key]) return name;
        m_listStyleWritten[key] = true;
        static const char* const kFormat[] = { "", "", "1", "a", "A", "i", "I" };
        std::string level;
        if (kind == 1)
            level = "<text:list-level-style-bullet text:level=\"1\" text:bullet-char=\"\xE2\x80\xA2\">";
        else
            level = std::string("<text:list-level-style-number text:level=\"1\" style:num-suffix=\".\" style:num-format=\"") +
                    kFormat[kind >= 2 && kind <= 6 ? kind : 2] + "\" text:start-value=\"" + std::to_string(start) + "\">";
        level += "<style:list-level-properties text:list-level-position-and-space-mode=\"label-alignment\">"
                 "<style:list-level-label-alignment text:label-followed-by=\"listtab\" text:list-tab-stop-position=\"0.5in\" "
                 "fo:text-indent=\"-0.25in\" fo:margin-left=\"0.5in\"/></style:list-level-properties>";
        level += kind == 1 ? "</text:list-level-style-bullet>" : "</text:list-level-style-number>";
        m_autoStyles += "<text:list-style style:name=\"" + name + "\">" + level + "</text:list-style>";
        return name;
    }

    void WriteText(const std::string& text, std::string& out)
    {
        std::string chunk;
        size_t i = 0, n = text.size();
        while (i < n) {
            char c = text[i];
            if (c == ' ') {
                size_t j = i;
                while (j < n && text[j] == ' ') ++j;
                size_t count = j - i;
                // A single space may be written literally unless a reader would collapse it away
                // (after another space, or at the very end of the paragraph).
                bool trailing = m_atParagraphEnd && j == n;
                if (!m_lastWasSpace && !trailing) { chunk += ' '; --count; }
                out += XmlEscape(chunk);
                chunk.clear();
                if (count == 1) out += "<text:s/>";
                else if (count > 1) out += "<text:s text:c=\"" + std::to_string(count) + "\"/>";
                m_lastWasSpace = true;
                i = j;
                continue;
            }
            if (c == '\t') { out += XmlEscape(chunk); chunk.clear(); out += "<text:tab/>"; m_lastWasSpace = true; }
            else if (c == '\n') { out += XmlEscape(chunk); chunk.clear(); out += "<text:line-break/>"; m_lastWasSpace = true; }
            else if (c != '\r') { chunk += c; m_lastWasSpace = false; }
            ++i;
        }
        out += XmlEscape(chunk);
    }

    void WriteItem(const Inline& item, std::string& out)
    {
        if (item.kind == Inline::Picture) {
            if (item.picture < 0 || item.picture >= (int)m_doc.pictures.size()) return;
            const Picture& pic = m_doc.pictures[(size_t)item.picture];
            m_pictureUsed[item.picture] = 1;
            if (!m_graphicStyleWritten) {
                m_graphicStyleWritten = true;
                m_autoStyles += "<style:style style:name=\"fr1\" style:family=\"graphic\" style:parent-style-name=\"Graphics\">"
                                "<style:graphic-properties style:vertical-pos=\"top\" style:vertical-rel=\"baseline\" "
                                "fo:padding=\"0in\" fo:border=\"none\" style:wrap=\"none\"/></style:style>";
            }
            int w = item.widthTwips > 0 ? item.widthTwips : pic.widthPx * 15;
            int h = item.heightTwips > 0 ? item.heightTwips : pic.heightPx * 15;
            ++m_frameCount;
            out += "<draw:frame draw:style-name=\"fr1\" draw:name=\"Image" + std::to_string(m_frameCount) +
                   "\" text:anchor-type=\"as-char\" svg:width=\"" + Inches(w) + "\" svg:height=\"" + Inches(h) + "\" draw:z-index=\"0\">"
                   "<draw:image xlink:href=\"" + PictureName(item.picture) + "\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\"/>"
                   "</draw:frame>";
            m_lastWasSpace = false;
            return;
        }
        std::string inner;
        switch (item.kind) {
        case Inline::Text: WriteText(item.text, inner); break;
        case Inline::Tab: inner = "<text:tab/>"; m_lastWasSpace = true; break;
        case Inline::LineBreak: case Inline::PageBreak: inner = "<text:line-break/>"; m_lastWasSpace = true; break;
        default: break;
        }
        if (inner.empty()) return;
        std::string style = TextStyle(item.props);
        if (style.empty()) out += inner;
        else out += "<text:span text:style-name=\"" + style + "\">" + inner + "</text:span>";
    }

    void WriteParagraph(const Paragraph& para, std::string& out)
    {
        out += "<text:p text:style-name=\"" + ParaStyleName(para) + "\">";
        m_lastWasSpace = true;
        size_t i = 0, count = para.items.size();
        while (i < count) {
            const std::string& link = para.items[i].props.link;
            if (link.empty()) {
                m_atParagraphEnd = i + 1 == count;
                WriteItem(para.items[i], out);
                ++i;
                continue;
            }
            size_t end = i;
            while (end < count && para.items[end].props.link == link) ++end;
            out += "<text:a xlink:type=\"simple\" xlink:href=\"" + XmlEscape(link, true) + "\">";
            for (size_t k = i; k < end; ++k) {
                m_atParagraphEnd = k + 1 == count;
                WriteItem(para.items[k], out);
            }
            out += "</text:a>";
            i = end;
        }
        m_atParagraphEnd = false;
        out += "</text:p>";
    }

    void WriteParagraphs(const std::vector<const Paragraph*>& paras, std::string& out)
    {
        size_t i = 0;
        while (i < paras.size()) {
            const ParaProps& p = paras[i]->props;
            if (p.list <= 0) { WriteParagraph(*paras[i], out); ++i; continue; }
            // Consecutive items of the same kind (and start value) form one list.
            size_t end = i;
            while (end < paras.size() && paras[end]->props.list == p.list && paras[end]->props.listStart == p.listStart) ++end;
            out += "<text:list text:style-name=\"" + ListStyleName(p.list, std::max(1, p.listStart)) + "\">";
            for (size_t k = i; k < end; ++k) {
                out += "<text:list-item>";
                WriteParagraph(*paras[k], out);
                out += "</text:list-item>";
            }
            out += "</text:list>";
            i = end;
        }
    }

    void WriteBlocks(const std::vector<Block>& blocks, std::string& out)
    {
        std::vector<const Paragraph*> run;
        for (const Block& block : blocks) {
            if (!block.isTable) { run.push_back(&block.para); continue; }
            WriteParagraphs(run, out);
            run.clear();
            WriteTable(block, out);
        }
        WriteParagraphs(run, out);
    }

    void WriteTable(const Block& block, std::string& out)
    {
        size_t columns = 0;
        for (const Row& row : block.rows) columns = std::max(columns, row.cells.size());
        if (columns == 0) return;
        std::string name = "Table" + std::to_string(++m_tableCount);
        int total = 0;
        std::vector<int> widths(columns, 1440);
        for (size_t c = 0; c < columns; ++c) {
            for (const Row& row : block.rows)
                if (c < row.cells.size() && row.cells[c].width > 0) { widths[c] = row.cells[c].width; break; }
            total += widths[c];
        }
        m_autoStyles += "<style:style style:name=\"" + name + "\" style:family=\"table\"><style:table-properties style:width=\"" +
                        Inches(total) + "\" table:align=\"left\"/></style:style>";
        m_autoStyles += "<style:style style:name=\"" + name + ".Cell\" style:family=\"table-cell\"><style:table-cell-properties "
                        "fo:padding=\"0.04in\" fo:border=\"0.5pt solid #000000\"/></style:style>";
        out += "<table:table table:name=\"" + name + "\" table:style-name=\"" + name + "\">";
        for (size_t c = 0; c < columns; ++c) {
            std::string colStyle = name + ".C" + std::to_string(c + 1);
            m_autoStyles += "<style:style style:name=\"" + colStyle + "\" style:family=\"table-column\"><style:table-column-properties "
                            "style:column-width=\"" + Inches(widths[c]) + "\"/></style:style>";
            out += "<table:table-column table:style-name=\"" + colStyle + "\"/>";
        }
        for (const Row& row : block.rows) {
            out += "<table:table-row>";
            for (size_t c = 0; c < columns; ++c) {
                out += "<table:table-cell table:style-name=\"" + name + ".Cell\" office:value-type=\"string\">";
                if (c < row.cells.size() && !row.cells[c].paras.empty()) {
                    std::vector<const Paragraph*> paras;
                    for (const Paragraph& p : row.cells[c].paras) paras.push_back(&p);
                    WriteParagraphs(paras, out);
                } else {
                    out += "<text:p/>";
                }
                out += "</table:table-cell>";
            }
            out += "</table:table-row>";
        }
        out += "</table:table>";
    }

    std::string Styles() const
    {
        const PageSetup& page = GetExportPage();
        std::string font = XmlEscape(m_defaultFont, true);
        char size[160];
        snprintf(size, sizeof(size), " fo:font-size=\"%gpt\" style:font-size-asian=\"%gpt\" style:font-size-complex=\"%gpt\"",
                 m_defaultSize / 2.0, m_defaultSize / 2.0, m_defaultSize / 2.0);
        std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles";
        out += kOdfNamespaces;
        out += ">" + FontDecls() + "<office:styles>"
               "<style:default-style style:family=\"paragraph\"><style:paragraph-properties fo:margin-top=\"0in\" fo:margin-bottom=\"0in\"/>"
               "<style:text-properties style:font-name=\"" + font + "\" style:font-name-asian=\"" + font + "\" style:font-name-complex=\"" + font + "\"" +
               size + "/></style:default-style>"
               "<style:style style:name=\"Standard\" style:family=\"paragraph\" style:class=\"text\"/>"
               "<style:style style:name=\"Graphics\" style:family=\"graphic\"/>"
               "</office:styles><office:automatic-styles><style:page-layout style:name=\"pm1\"><style:page-layout-properties"
               " fo:page-width=\"" + Inches(page.width) + "\" fo:page-height=\"" + Inches(page.height) + "\""
               " style:print-orientation=\"" + (page.width > page.height ? "landscape" : "portrait") + "\""
               " fo:margin-top=\"" + Inches(page.top) + "\" fo:margin-bottom=\"" + Inches(page.bottom) + "\""
               " fo:margin-left=\"" + Inches(page.left) + "\" fo:margin-right=\"" + Inches(page.right) + "\"/></style:page-layout>"
               "</office:automatic-styles><office:master-styles><style:master-page style:name=\"Standard\" style:page-layout-name=\"pm1\"/>"
               "</office:master-styles></office:document-styles>";
        return out;
    }
};

} // namespace

bool OdtRead(const Bytes& file, Document& doc, std::string* error)
{
    doc = Document();
    OdtReader reader(file, doc);
    return reader.Read(error);
}

bool OdtWrite(const Document& doc, Bytes& file, std::string* error)
{
    (void)error;
    OdtWriter writer(doc);
    return writer.Write(file);
}

} // namespace conv
