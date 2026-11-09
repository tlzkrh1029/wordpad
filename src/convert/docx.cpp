// Office Open XML (.docx) <-> document model.
#include "model.h"
#include "cascade.h"
#include "xml.h"
#include "zip.h"
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace conv {

using namespace detail;

namespace {

struct NamedColor { const char* name; uint8_t r, g, b; };
const NamedColor kHighlightColors[] = {
    { "black", 0, 0, 0 }, { "blue", 0, 0, 255 }, { "cyan", 0, 255, 255 }, { "green", 0, 255, 0 },
    { "magenta", 255, 0, 255 }, { "red", 255, 0, 0 }, { "yellow", 255, 255, 0 }, { "white", 255, 255, 255 },
    { "darkBlue", 0, 0, 128 }, { "darkCyan", 0, 128, 128 }, { "darkGreen", 0, 128, 0 }, { "darkMagenta", 128, 0, 128 },
    { "darkRed", 128, 0, 0 }, { "darkYellow", 128, 128, 0 }, { "darkGray", 128, 128, 128 }, { "lightGray", 192, 192, 192 },
};

bool OnOff(const XmlNode& n)
{
    const std::string* v = n.Attr("w:val");
    return !v || (*v != "0" && *v != "false" && *v != "off");
}

struct Style {
    std::string basedOn;
    bool paragraph = false;
    RunStyle run;
    ParaStyle para;
    bool resolved = false;
};

struct NumLevel {
    int kind = 2;
    int start = 1;
    int left = kUnset, first = kUnset;
};

struct NumInstance {
    int abstractId = -1;
    std::map<int, int> startOverride;
};

// ===========================================================================
// Reader
// ===========================================================================
class DocxReader {
public:
    DocxReader(const Bytes& file, Document& doc) : m_file(file), m_doc(doc) {}

    bool Read(std::string* error)
    {
        if (!m_zip.Open(m_file)) return Fail(error, "not a ZIP package");
        std::string mainPart = "word/document.xml";
        {
            std::string xml;
            XmlNode rels;
            if (m_zip.ReadText("_rels/.rels", xml) && XmlParse(xml, rels)) {
                for (const XmlNode& r : rels.children) {
                    const std::string* type = r.Attr("Type");
                    const std::string* target = r.Attr("Target");
                    if (type && target && type->size() >= 15 && type->compare(type->size() - 15, 15, "/officeDocument") == 0)
                        mainPart = ResolvePath("", *target);
                }
            }
        }
        std::string xml;
        if (!m_zip.ReadText(mainPart, xml)) return Fail(error, "the main document part is missing");
        XmlNode root;
        std::string parseError;
        if (!XmlParse(xml, root, &parseError)) return Fail(error, "the document XML is malformed");
        m_baseDir = DirName(mainPart);
        m_relsPath = m_baseDir + "_rels/" + mainPart.substr(m_baseDir.size()) + ".rels";
        LoadRelationships(m_relsPath);
        LoadTheme();
        LoadStyles();
        LoadNumbering();

        const XmlNode* body = root.Child("w:body");
        if (!body) return Fail(error, "the document has no body");
        WalkBlocks(*body, m_doc.blocks);

        // Document defaults = what an unformatted paragraph looks like.
        RunStyle rs = m_defaultRun;
        if (const Style* normal = Resolve(m_defaultParaStyle)) rs.Merge(normal->run);
        CharProps cp = ToCharProps(rs);
        m_doc.defaultFont = cp.font;
        m_doc.defaultHalfPoints = cp.halfPoints;
        return true;
    }

private:
    const Bytes& m_file;
    Document& m_doc;
    ZipReader m_zip;
    std::string m_baseDir;
    std::string m_relsPath;

    struct Relationship { std::string target; bool external = false; };
    std::map<std::string, Relationship> m_rels;
    std::map<std::string, Style> m_styles;
    std::string m_defaultParaStyle;
    RunStyle m_defaultRun;
    ParaStyle m_defaultPara;
    std::map<int, std::map<int, NumLevel>> m_abstractNums;
    std::map<int, NumInstance> m_nums;
    std::string m_minorLatin, m_majorLatin, m_minorEa, m_majorEa;
    std::map<std::string, std::string> m_themeScriptFonts;   // "Hang" -> typeface (minor font)
    std::string m_eastAsiaLang;
    std::map<std::string, int> m_pictureCache;

    std::map<std::pair<int, int>, int> m_counters;            // (numId, level) -> last number used
    int m_lastListNum = 0, m_lastListLevel = -1, m_lastListStart = 1;
    bool m_previousWasListItem = false;

    std::vector<Paragraph> m_pending;       // text-box paragraphs to place after the current one
    std::string m_fieldInstr;
    std::string m_fieldLink;
    int m_fieldState = 0;                   // 0 none, 1 collecting the instruction, 2 inside the result

    static bool Fail(std::string* error, const char* message)
    {
        if (error) *error = message;
        return false;
    }

    bool ParsePart(const std::string& path, XmlNode& root)
    {
        std::string xml;
        return m_zip.ReadText(path, xml) && XmlParse(xml, root);
    }

    void LoadRelationships(const std::string& path)
    {
        XmlNode root;
        if (!ParsePart(path, root)) return;
        for (const XmlNode& r : root.children) {
            const std::string* id = r.Attr("Id");
            const std::string* target = r.Attr("Target");
            if (!id || !target) continue;
            Relationship rel;
            rel.target = *target;
            rel.external = r.AttrOr("TargetMode") == "External";
            m_rels[*id] = rel;
        }
    }

    std::string PartByType(const char* suffix, const char* fallback)
    {
        // The relationships file names the parts; the conventional names are a fallback.
        XmlNode root;
        if (ParsePart(m_relsPath, root)) {
            size_t n = strlen(suffix);
            for (const XmlNode& r : root.children) {
                const std::string* type = r.Attr("Type");
                const std::string* target = r.Attr("Target");
                if (type && target && type->size() >= n && type->compare(type->size() - n, n, suffix) == 0)
                    return ResolvePath(m_baseDir, *target);
            }
        }
        return m_baseDir + fallback;
    }

    void LoadTheme()
    {
        XmlNode root;
        if (!ParsePart(PartByType("/theme", "theme/theme1.xml"), root)) return;
        const XmlNode* elements = root.Child("a:themeElements");
        const XmlNode* scheme = elements ? elements->Child("a:fontScheme") : nullptr;
        if (!scheme) return;
        auto face = [](const XmlNode* font, const char* child) -> std::string {
            const XmlNode* n = font ? font->Child(child) : nullptr;
            return n ? n->AttrOr("typeface") : std::string();
        };
        const XmlNode* minor = scheme->Child("a:minorFont");
        const XmlNode* major = scheme->Child("a:majorFont");
        m_minorLatin = face(minor, "a:latin");
        m_majorLatin = face(major, "a:latin");
        m_minorEa = face(minor, "a:ea");
        m_majorEa = face(major, "a:ea");
        if (minor) {
            for (const XmlNode& f : minor->children)
                if (f.name == "a:font") m_themeScriptFonts[f.AttrOr("script")] = f.AttrOr("typeface");
        }
    }

    std::string ThemeFont(const std::string& key) const
    {
        bool major = key.compare(0, 5, "major") == 0;
        if (key.find("EastAsia") != std::string::npos) {
            const std::string& ea = major ? m_majorEa : m_minorEa;
            if (!ea.empty()) return ea;
            const char* script = nullptr;
            if (m_eastAsiaLang.compare(0, 2, "ko") == 0) script = "Hang";
            else if (m_eastAsiaLang.compare(0, 2, "ja") == 0) script = "Jpan";
            else if (m_eastAsiaLang == "zh-TW" || m_eastAsiaLang == "zh-HK") script = "Hant";
            else if (m_eastAsiaLang.compare(0, 2, "zh") == 0) script = "Hans";
            if (script) {
                auto it = m_themeScriptFonts.find(script);
                if (it != m_themeScriptFonts.end()) return it->second;
            }
            return std::string();
        }
        return major ? m_majorLatin : m_minorLatin;
    }

    void ParseRunProps(const XmlNode& rpr, RunStyle& s)
    {
        for (const XmlNode& c : rpr.children) {
            const std::string& n = c.name;
            if (n == "w:rFonts") {
                const std::string* ascii = c.Attr("w:ascii");
                if (!ascii) ascii = c.Attr("w:hAnsi");
                if (ascii && !ascii->empty()) { s.hasFont = true; s.font = *ascii; }
                else if (const std::string* theme = c.Attr("w:asciiTheme")) {
                    std::string f = ThemeFont(*theme);
                    if (!f.empty()) { s.hasFont = true; s.font = f; }
                }
                const std::string* ea = c.Attr("w:eastAsia");
                if (ea && !ea->empty()) { s.hasEaFont = true; s.eaFont = *ea; }
                else if (const std::string* theme = c.Attr("w:eastAsiaTheme")) {
                    std::string f = ThemeFont(*theme);
                    if (!f.empty()) { s.hasEaFont = true; s.eaFont = f; }
                }
            }
            else if (n == "w:b") s.bold = OnOff(c);
            else if (n == "w:i") s.italic = OnOff(c);
            else if (n == "w:strike" || n == "w:dstrike") s.strike = OnOff(c);
            else if (n == "w:u") s.underline = c.AttrOr("w:val", "single") != "none";
            else if (n == "w:vanish") s.hidden = OnOff(c);
            else if (n == "w:vertAlign") {
                std::string v = c.AttrOr("w:val");
                s.script = v == "superscript" ? 1 : v == "subscript" ? 2 : 0;
            }
            else if (n == "w:sz") { int v = ToInt(c.Attr("w:val"), 0); if (v > 0) s.size = v; }
            else if (n == "w:color") {
                s.hasColor = true;
                s.color = Color();
                ParseHexColor(c.AttrOr("w:val"), s.color);
            }
            else if (n == "w:highlight") {
                std::string v = c.AttrOr("w:val");
                s.hasHighlight = true;
                s.highlight = Color();
                for (const NamedColor& nc : kHighlightColors)
                    if (v == nc.name) { s.highlight.set = true; s.highlight.r = nc.r; s.highlight.g = nc.g; s.highlight.b = nc.b; }
            }
            else if (n == "w:shd") {
                Color fill;
                if (ParseHexColor(c.AttrOr("w:fill"), fill) && !(s.hasHighlight && s.highlight.set)) {
                    s.hasHighlight = true;
                    s.highlight = fill;
                }
            }
            else if (n == "w:lang") {
                if (const std::string* ea = c.Attr("w:eastAsia")) if (m_eastAsiaLang.empty()) m_eastAsiaLang = *ea;
            }
        }
    }

    void ParseParaProps(const XmlNode& ppr, ParaStyle& s)
    {
        for (const XmlNode& c : ppr.children) {
            const std::string& n = c.name;
            if (n == "w:jc") {
                std::string v = c.AttrOr("w:val");
                s.align = v == "center" ? 1 : (v == "right" || v == "end") ? 2 : (v == "both" || v == "distribute") ? 3 : 0;
            }
            else if (n == "w:ind") {
                const std::string* left = c.Attr("w:left");
                if (!left) left = c.Attr("w:start");
                const std::string* right = c.Attr("w:right");
                if (!right) right = c.Attr("w:end");
                if (left) s.left = ToInt(left, 0);
                if (right) s.right = ToInt(right, 0);
                if (const std::string* hanging = c.Attr("w:hanging")) s.first = -ToInt(hanging, 0);
                else if (const std::string* firstLine = c.Attr("w:firstLine")) s.first = ToInt(firstLine, 0);
            }
            else if (n == "w:spacing") {
                if (const std::string* v = c.Attr("w:before")) s.before = ToInt(v, 0);
                if (const std::string* v = c.Attr("w:after")) s.after = ToInt(v, 0);
                if (const std::string* v = c.Attr("w:line")) {
                    s.line = ToInt(v, 240);
                    std::string rule = c.AttrOr("w:lineRule", "auto");
                    s.lineRule = rule == "exact" ? 1 : rule == "atLeast" ? 2 : 0;
                }
            }
            else if (n == "w:tabs") {
                s.hasTabs = true;
                for (const XmlNode& t : c.children) {
                    if (t.name != "w:tab") continue;
                    std::string v = t.AttrOr("w:val", "left");
                    int pos = ToInt(t.Attr("w:pos"), 0);
                    if (v == "clear") { s.clearedTabs.push_back(pos); continue; }
                    if (v == "bar" || pos <= 0) continue;
                    TabStop ts;
                    ts.pos = pos;
                    ts.align = v == "center" ? 1 : (v == "right" || v == "end") ? 2 : v == "decimal" ? 3 : 0;
                    s.tabs.push_back(ts);
                }
            }
            else if (n == "w:numPr") {
                if (const XmlNode* id = c.Child("w:numId")) s.numId = ToInt(id->Attr("w:val"), 0);
                if (const XmlNode* lvl = c.Child("w:ilvl")) s.ilvl = ToInt(lvl->Attr("w:val"), 0);
            }
            else if (n == "w:pageBreakBefore") s.pageBreakBefore = OnOff(c);
        }
    }

    void LoadStyles()
    {
        // The East Asian language decides which theme font applies to CJK text; it has to be known
        // before the first w:rFonts element is resolved.
        XmlNode settings;
        if (ParsePart(PartByType("/settings", "settings.xml"), settings))
            if (const XmlNode* lang = settings.Child("w:themeFontLang")) m_eastAsiaLang = lang->AttrOr("w:eastAsia");
        XmlNode root;
        if (!ParsePart(PartByType("/styles", "styles.xml"), root)) return;
        if (m_eastAsiaLang.empty())
            if (const XmlNode* defaults = root.Child("w:docDefaults"))
                if (const XmlNode* r = defaults->Child("w:rPrDefault"))
                    if (const XmlNode* rpr = r->Child("w:rPr"))
                        if (const XmlNode* lang = rpr->Child("w:lang")) m_eastAsiaLang = lang->AttrOr("w:eastAsia");
        if (const XmlNode* defaults = root.Child("w:docDefaults")) {
            if (const XmlNode* r = defaults->Child("w:rPrDefault"))
                if (const XmlNode* rpr = r->Child("w:rPr")) ParseRunProps(*rpr, m_defaultRun);
            if (const XmlNode* p = defaults->Child("w:pPrDefault"))
                if (const XmlNode* ppr = p->Child("w:pPr")) ParseParaProps(*ppr, m_defaultPara);
        }
        for (const XmlNode& s : root.children) {
            if (s.name != "w:style") continue;
            std::string id = s.AttrOr("w:styleId");
            if (id.empty()) continue;
            Style style;
            style.paragraph = s.AttrOr("w:type", "paragraph") == "paragraph";
            if (const XmlNode* based = s.Child("w:basedOn")) style.basedOn = based->AttrOr("w:val");
            if (const XmlNode* rpr = s.Child("w:rPr")) ParseRunProps(*rpr, style.run);
            if (const XmlNode* ppr = s.Child("w:pPr")) ParseParaProps(*ppr, style.para);
            std::string isDefault = s.AttrOr("w:default");
            if (style.paragraph && (isDefault == "1" || isDefault == "true")) m_defaultParaStyle = id;
            m_styles[id] = style;
        }
    }

    // Returns the style with the properties of its base styles merged in.
    const Style* Resolve(const std::string& id, int depth = 0)
    {
        auto it = m_styles.find(id);
        if (it == m_styles.end()) return nullptr;
        Style& style = it->second;
        if (!style.resolved && depth < 24) {
            style.resolved = true;
            if (!style.basedOn.empty() && style.basedOn != id) {
                if (const Style* base = Resolve(style.basedOn, depth + 1)) {
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

    void LoadNumbering()
    {
        XmlNode root;
        if (!ParsePart(PartByType("/numbering", "numbering.xml"), root)) return;
        for (const XmlNode& n : root.children) {
            if (n.name == "w:abstractNum") {
                int id = ToInt(n.Attr("w:abstractNumId"), -1);
                for (const XmlNode& l : n.children) {
                    if (l.name != "w:lvl") continue;
                    NumLevel level;
                    int ilvl = ToInt(l.Attr("w:ilvl"), 0);
                    if (const XmlNode* fmt = l.Child("w:numFmt")) {
                        std::string v = fmt->AttrOr("w:val");
                        level.kind = v == "bullet" ? 1 : v == "lowerLetter" ? 3 : v == "upperLetter" ? 4 :
                                     v == "lowerRoman" ? 5 : v == "upperRoman" ? 6 : v == "none" ? 0 : 2;
                    }
                    if (const XmlNode* start = l.Child("w:start")) level.start = ToInt(start->Attr("w:val"), 1);
                    if (const XmlNode* ppr = l.Child("w:pPr")) {
                        ParaStyle ps;
                        ParseParaProps(*ppr, ps);
                        level.left = ps.left;
                        level.first = ps.first;
                    }
                    m_abstractNums[id][ilvl] = level;
                }
            } else if (n.name == "w:num") {
                NumInstance inst;
                int id = ToInt(n.Attr("w:numId"), -1);
                if (const XmlNode* a = n.Child("w:abstractNumId")) inst.abstractId = ToInt(a->Attr("w:val"), -1);
                for (const XmlNode& o : n.children) {
                    if (o.name != "w:lvlOverride") continue;
                    if (const XmlNode* so = o.Child("w:startOverride"))
                        inst.startOverride[ToInt(o.Attr("w:ilvl"), 0)] = ToInt(so->Attr("w:val"), 1);
                }
                m_nums[id] = inst;
            }
        }
    }

    CharProps ToCharProps(const RunStyle& rs) const
    {
        CharProps cp;
        cp.font = rs.hasFont ? rs.font : (!m_minorLatin.empty() ? m_minorLatin : std::string("Times New Roman"));
        cp.halfPoints = rs.size != kUnset ? rs.size : 20;
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
    void WalkBlocks(const XmlNode& container, std::vector<Block>& out)
    {
        for (const XmlNode& c : container.children) {
            const std::string& n = c.name;
            if (n == "w:p") {
                Block block;
                ParseParagraph(c, block.para);
                out.push_back(std::move(block));
                FlushPending(out);
            } else if (n == "w:tbl") {
                Block block;
                block.isTable = true;
                ParseTable(c, block.rows);
                if (!block.rows.empty()) out.push_back(std::move(block));
                m_previousWasListItem = false;
            } else if (n == "w:sdt") {
                if (const XmlNode* content = c.Child("w:sdtContent")) WalkBlocks(*content, out);
            } else if (n == "w:ins" || n == "w:customXml" || n == "w:smartTag" || n == "w:moveTo") {
                WalkBlocks(c, out);
            } else if (n == "mc:AlternateContent") {
                const XmlNode* pick = c.Child("mc:Choice");
                if (!pick) pick = c.Child("mc:Fallback");
                if (pick) WalkBlocks(*pick, out);
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

    void ParseParagraph(const XmlNode& p, Paragraph& para)
    {
        const XmlNode* ppr = p.Child("w:pPr");
        std::string styleId = m_defaultParaStyle;
        if (ppr) if (const XmlNode* ps = ppr->Child("w:pStyle")) styleId = ps->AttrOr("w:val", styleId.c_str());
        const Style* style = Resolve(styleId);

        ParaStyle base = m_defaultPara;
        if (style) base.Merge(style->para);
        ParaStyle direct;
        if (ppr) ParseParaProps(*ppr, direct);

        // Numbering sits between the style and the direct formatting.
        int numId = direct.numId != kUnset ? direct.numId : base.numId;
        int ilvl = direct.ilvl != kUnset ? direct.ilvl : (base.ilvl != kUnset ? base.ilvl : 0);
        const NumLevel* level = nullptr;
        NumLevel fallbackLevel;
        if (numId != kUnset && numId > 0) {
            auto ni = m_nums.find(numId);
            if (ni != m_nums.end()) {
                auto ai = m_abstractNums.find(ni->second.abstractId);
                if (ai != m_abstractNums.end()) {
                    auto li = ai->second.find(ilvl);
                    if (li != ai->second.end()) level = &li->second;
                }
                if (!level) level = &fallbackLevel;
            }
        }
        ParaStyle ps = base;
        if (level) {
            if (level->left != kUnset) ps.left = level->left;
            if (level->first != kUnset) ps.first = level->first;
        }
        ps.Merge(direct);

        ParaProps& pp = para.props;
        ps.ApplyTo(pp);

        if (level && level->kind != 0) {
            pp.list = level->kind;
            // Deeper levels restart whenever a shallower item appears.
            for (auto& kv : m_counters)
                if (kv.first.first == numId && kv.first.second > ilvl) kv.second = 0;
            int start = level->start;
            auto ni = m_nums.find(numId);
            if (ni != m_nums.end()) {
                auto so = ni->second.startOverride.find(ilvl);
                if (so != ni->second.startOverride.end()) start = so->second;
            }
            int& counter = m_counters[std::make_pair(numId, ilvl)];
            counter = counter == 0 ? start : counter + 1;
            bool continues = m_previousWasListItem && m_lastListNum == numId && m_lastListLevel == ilvl;
            pp.listStart = continues ? m_lastListStart : counter;
            m_lastListNum = numId;
            m_lastListLevel = ilvl;
            m_lastListStart = pp.listStart;
            m_previousWasListItem = true;
        } else {
            m_previousWasListItem = false;
        }

        RunStyle baseRun = m_defaultRun;
        if (style) baseRun.Merge(style->run);
        WalkInline(p, baseRun, std::string(), para.items);
    }

    void WalkInline(const XmlNode& container, const RunStyle& baseRun, const std::string& link, std::vector<Inline>& out)
    {
        for (const XmlNode& c : container.children) {
            const std::string& n = c.name;
            if (n == "w:r") {
                ParseRun(c, baseRun, link, out);
            } else if (n == "w:hyperlink") {
                std::string target;
                if (const std::string* id = c.Attr("r:id")) {
                    auto it = m_rels.find(*id);
                    if (it != m_rels.end()) target = it->second.target;
                }
                WalkInline(c, baseRun, target.empty() ? link : target, out);
            } else if (n == "w:ins" || n == "w:smartTag" || n == "w:fldSimple" || n == "w:customXml" || n == "w:dir" ||
                       n == "w:bdo" || n == "w:moveTo") {
                WalkInline(c, baseRun, link, out);
            } else if (n == "w:sdt") {
                if (const XmlNode* content = c.Child("w:sdtContent")) WalkInline(*content, baseRun, link, out);
            } else if (n == "mc:AlternateContent") {
                const XmlNode* pick = c.Child("mc:Choice");
                if (!pick) pick = c.Child("mc:Fallback");
                if (pick) WalkInline(*pick, baseRun, link, out);
            }
        }
    }

    // Without an explicit East Asian font the theme's font for the script of the text is used.
    std::string ScriptFont(const std::string& text) const
    {
        if (m_themeScriptFonts.empty()) return std::string();
        size_t pos = 0;
        const char* script = nullptr;
        while (pos < text.size() && !script) {
            uint32_t cp = NextCodePoint(text, pos);
            if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F)) script = "Hang";
            else if (cp >= 0x3040 && cp <= 0x30FF) script = "Jpan";
        }
        if (!script) return std::string();
        auto it = m_themeScriptFonts.find(script);
        return it != m_themeScriptFonts.end() ? it->second : std::string();
    }

    void AddText(const std::string& text, const CharProps& cp, const std::string& explicitEaFont, std::vector<Inline>& out)
    {
        size_t before = out.size();
        std::string eaFont = explicitEaFont.empty() ? ScriptFont(text) : explicitEaFont;
        SplitByScript(text, cp, eaFont, out);
        // Merge with the previous run when the formatting is identical.
        if (before > 0 && out.size() > before && out[before - 1].kind == Inline::Text && out[before].kind == Inline::Text &&
            out[before - 1].props == out[before].props) {
            out[before - 1].text += out[before].text;
            out.erase(out.begin() + (ptrdiff_t)before);
        }
    }

    void ParseRun(const XmlNode& r, const RunStyle& baseRun, const std::string& link, std::vector<Inline>& out)
    {
        RunStyle rs = baseRun;
        if (const XmlNode* rpr = r.Child("w:rPr")) {
            if (const XmlNode* rstyle = rpr->Child("w:rStyle"))
                if (const Style* cs = Resolve(rstyle->AttrOr("w:val"))) rs.Merge(cs->run);
            ParseRunProps(*rpr, rs);
        }
        CharProps cp = ToCharProps(rs);
        cp.link = !link.empty() ? link : (m_fieldState == 2 ? m_fieldLink : std::string());
        std::string eaFont = rs.hasEaFont ? rs.eaFont : std::string();
        bool hidden = rs.hidden == 1;

        for (const XmlNode& c : r.children) {
            const std::string& n = c.name;
            if (n == "w:fldChar") {
                std::string type = c.AttrOr("w:fldCharType");
                if (type == "begin") { m_fieldState = 1; m_fieldInstr.clear(); m_fieldLink.clear(); }
                else if (type == "separate") {
                    m_fieldState = 2;
                    size_t at = m_fieldInstr.find("HYPERLINK");
                    if (at != std::string::npos) {
                        size_t q1 = m_fieldInstr.find('"', at);
                        size_t q2 = q1 == std::string::npos ? q1 : m_fieldInstr.find('"', q1 + 1);
                        if (q2 != std::string::npos && m_fieldInstr.substr(at, q1 - at).find("\\l") == std::string::npos)
                            m_fieldLink = m_fieldInstr.substr(q1 + 1, q2 - q1 - 1);
                    }
                    cp.link = !link.empty() ? link : m_fieldLink;
                }
                else { m_fieldState = 0; m_fieldLink.clear(); cp.link = link; }
                continue;
            }
            if (n == "w:instrText") { if (m_fieldState == 1) m_fieldInstr += c.InnerText(); continue; }
            if (m_fieldState == 1 || hidden) continue;

            if (n == "w:t") AddText(c.InnerText(), cp, eaFont, out);
            else if (n == "w:tab") { Inline i; i.kind = Inline::Tab; i.props = cp; out.push_back(i); }
            else if (n == "w:br") {
                std::string type = c.AttrOr("w:type");
                if (type == "column") continue;
                Inline i;
                i.kind = type == "page" ? Inline::PageBreak : Inline::LineBreak;
                i.props = cp;
                out.push_back(i);
            }
            else if (n == "w:cr") { Inline i; i.kind = Inline::LineBreak; i.props = cp; out.push_back(i); }
            else if (n == "w:noBreakHyphen") AddText("\xE2\x80\x91", cp, eaFont, out);
            else if (n == "w:sym") {
                unsigned code = (unsigned)strtoul(c.AttrOr("w:char", "0").c_str(), nullptr, 16);
                if (code >= 0xF000 && code <= 0xF0FF) code -= 0xF000;     // symbol fonts live in the private use area
                if (code >= 0x20) {
                    CharProps sym = cp;
                    std::string font = c.AttrOr("w:font");
                    if (!font.empty()) sym.font = font;
                    std::string text;
                    AppendUtf8(text, code);
                    Inline i;
                    i.text = text;
                    i.props = sym;
                    out.push_back(i);
                }
            }
            else if (n == "w:drawing" || n == "w:pict" || n == "w:object") ParseGraphic(c, cp, out);
            else if (n == "mc:AlternateContent") {
                const XmlNode* pick = c.Child("mc:Choice");
                if (!pick) pick = c.Child("mc:Fallback");
                if (pick)
                    for (const XmlNode& g : pick->children)
                        if (g.name == "w:drawing" || g.name == "w:pict") ParseGraphic(g, cp, out);
            }
        }
    }

    static const XmlNode* FindDescendant(const XmlNode& node, const char* name)
    {
        for (const XmlNode& c : node.children) {
            if (c.name == name) return &c;
            if (const XmlNode* found = FindDescendant(c, name)) return found;
        }
        return nullptr;
    }

    void CollectTextBoxes(const XmlNode& node)
    {
        for (const XmlNode& c : node.children) {
            if (c.name == "w:txbxContent") {
                std::vector<Block> blocks;
                bool savedList = m_previousWasListItem;
                WalkBlocks(c, blocks);
                m_previousWasListItem = savedList;
                for (Block& b : blocks) {
                    if (!b.isTable) { m_pending.push_back(std::move(b.para)); continue; }
                    for (Row& row : b.rows)
                        for (Cell& cell : row.cells)
                            for (Paragraph& para : cell.paras) m_pending.push_back(std::move(para));
                }
            } else {
                CollectTextBoxes(c);
            }
        }
    }

    void ParseGraphic(const XmlNode& node, const CharProps& cp, std::vector<Inline>& out)
    {
        CollectTextBoxes(node);
        std::string relId;
        int widthTwips = 0, heightTwips = 0;
        if (const XmlNode* blip = FindDescendant(node, "a:blip")) {
            relId = blip->AttrOr("r:embed");
            if (relId.empty()) relId = blip->AttrOr("r:link");
            if (const XmlNode* extent = FindDescendant(node, "wp:extent")) {
                widthTwips = RoundToInt(strtod(extent->AttrOr("cx", "0").c_str(), nullptr) / 635.0);
                heightTwips = RoundToInt(strtod(extent->AttrOr("cy", "0").c_str(), nullptr) / 635.0);
            }
        } else if (const XmlNode* image = FindDescendant(node, "v:imagedata")) {
            relId = image->AttrOr("r:id");
            if (const XmlNode* shape = FindDescendant(node, "v:shape")) {
                std::string style = shape->AttrOr("style");
                size_t w = style.find("width:"), h = style.find("height:");
                if (w != std::string::npos) widthTwips = LengthToTwips(style.substr(w + 6), 0);
                if (h != std::string::npos) heightTwips = LengthToTwips(style.substr(h + 7), 0);
            }
        }
        if (relId.empty()) return;
        auto rel = m_rels.find(relId);
        if (rel == m_rels.end() || rel->second.external) return;
        std::string path = ResolvePath(m_baseDir, rel->second.target);

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
        if (widthTwips <= 0 || heightTwips <= 0) { widthTwips = pic.widthPx * 15; heightTwips = pic.heightPx * 15; }
        Inline item;
        item.kind = Inline::Picture;
        item.picture = index;
        item.props = cp;
        item.widthTwips = widthTwips;
        item.heightTwips = heightTwips;
        out.push_back(std::move(item));
    }

    void ParseTable(const XmlNode& tbl, std::vector<Row>& rows)
    {
        std::vector<int> grid;
        if (const XmlNode* g = tbl.Child("w:tblGrid"))
            for (const XmlNode& col : g->children)
                if (col.name == "w:gridCol") grid.push_back(ToInt(col.Attr("w:w"), 0));
        CollectRows(tbl, grid, rows);
    }

    void CollectRows(const XmlNode& container, const std::vector<int>& grid, std::vector<Row>& rows)
    {
        for (const XmlNode& c : container.children) {
            if (c.name == "w:tr") {
                Row row;
                size_t gridIndex = 0;
                CollectCells(c, grid, gridIndex, row);
                if (!row.cells.empty()) rows.push_back(std::move(row));
            } else if (c.name == "w:sdt") {
                if (const XmlNode* content = c.Child("w:sdtContent")) CollectRows(*content, grid, rows);
            } else if (c.name == "w:ins" || c.name == "w:customXml") {
                CollectRows(c, grid, rows);
            }
        }
    }

    void CollectCells(const XmlNode& container, const std::vector<int>& grid, size_t& gridIndex, Row& row)
    {
        for (const XmlNode& c : container.children) {
            if (c.name == "w:sdt") {
                if (const XmlNode* content = c.Child("w:sdtContent")) CollectCells(*content, grid, gridIndex, row);
                continue;
            }
            if (c.name != "w:tc") continue;
            Cell cell;
            int span = 1;
            if (const XmlNode* tcPr = c.Child("w:tcPr")) {
                if (const XmlNode* gs = tcPr->Child("w:gridSpan")) span = std::max(1, ToInt(gs->Attr("w:val"), 1));
                if (const XmlNode* w = tcPr->Child("w:tcW"))
                    if (w->AttrOr("w:type", "dxa") == "dxa") cell.width = ToInt(w->Attr("w:w"), 0);
            }
            if (cell.width <= 0) {
                for (int i = 0; i < span; ++i)
                    if (gridIndex + (size_t)i < grid.size()) cell.width += grid[gridIndex + (size_t)i];
            }
            if (cell.width <= 0) cell.width = 1440;
            gridIndex += (size_t)span;

            std::vector<Block> blocks;
            m_previousWasListItem = false;
            WalkBlocks(c, blocks);
            for (Block& b : blocks) {
                if (!b.isTable) { cell.paras.push_back(std::move(b.para)); continue; }
                // Nested tables are flattened into the paragraphs of the outer cell.
                for (Row& r : b.rows)
                    for (Cell& inner : r.cells)
                        for (Paragraph& p : inner.paras) cell.paras.push_back(std::move(p));
            }
            if (cell.paras.empty()) cell.paras.push_back(Paragraph());
            row.cells.push_back(std::move(cell));
        }
    }
};

// ===========================================================================
// Writer
// ===========================================================================
const char* const kNsW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
const char* const kNsR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const char* const kXmlHeader = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";

class DocxWriter {
public:
    explicit DocxWriter(const Document& doc) : m_doc(doc) {}

    bool Write(Bytes& out)
    {
        m_defaultFont = m_doc.defaultFont.empty() ? std::string("Calibri") : m_doc.defaultFont;
        m_defaultSize = m_doc.defaultHalfPoints > 0 ? m_doc.defaultHalfPoints : 22;

        std::string body;
        const ParaProps* previousList = nullptr;
        for (size_t i = 0; i < m_doc.blocks.size(); ++i) {
            const Block& block = m_doc.blocks[i];
            if (block.isTable) {
                WriteTable(block, body);
                previousList = nullptr;
            } else {
                WriteParagraph(block.para, previousList, body);
                previousList = block.para.props.list > 0 ? &block.para.props : nullptr;
            }
        }
        // A document must not end with a table.
        if (m_doc.blocks.empty() || m_doc.blocks.back().isTable) body += "<w:p/>";

        const PageSetup& page = GetExportPage();
        char sect[400];
        snprintf(sect, sizeof(sect),
                 "<w:sectPr><w:pgSz w:w=\"%d\" w:h=\"%d\"%s/><w:pgMar w:top=\"%d\" w:right=\"%d\" w:bottom=\"%d\" w:left=\"%d\" "
                 "w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/></w:sectPr>",
                 page.width, page.height, page.width > page.height ? " w:orient=\"landscape\"" : "", page.top, page.right,
                 page.bottom, page.left);

        std::string document = kXmlHeader;
        document += "<w:document xmlns:w=\"" + std::string(kNsW) + "\" xmlns:r=\"" + kNsR + "\" "
                    "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
                    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                    "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><w:body>";
        document += body;
        document += sect;
        document += "</w:body></w:document>";

        ZipWriter zip;
        zip.Add("[Content_Types].xml", ContentTypes());
        zip.Add("_rels/.rels", std::string(kXmlHeader) +
                "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
                "Target=\"word/document.xml\"/></Relationships>");
        zip.Add("word/document.xml", document);
        zip.Add("word/styles.xml", Styles());
        if (!m_listInstances.empty()) zip.Add("word/numbering.xml", Numbering());
        zip.Add("word/_rels/document.xml.rels", Relationships());
        for (size_t i = 0; i < m_doc.pictures.size(); ++i) {
            if (m_pictureRel.find((int)i) == m_pictureRel.end()) continue;
            zip.Add("word/media/" + PictureName((int)i), m_doc.pictures[i].data, false);
        }
        out = zip.Finish();
        return true;
    }

private:
    const Document& m_doc;
    std::string m_defaultFont;
    int m_defaultSize = 22;
    int m_nextRelId = 10;
    int m_drawingId = 1;
    std::map<int, std::string> m_pictureRel;                       // picture index -> relationship id
    std::vector<std::pair<std::string, std::string>> m_linkRels;   // relationship id, target
    struct ListInstance { int kind; int start; };
    std::vector<ListInstance> m_listInstances;                     // numId = index + 1

    std::string PictureName(int index) const
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "image%d.%s", index + 1, m_doc.pictures[(size_t)index].jpeg ? "jpeg" : "png");
        return buf;
    }

    std::string NewRelId()
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "rId%d", m_nextRelId++);
        return buf;
    }

    static void Attr(std::string& out, const char* name, int value)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), " %s=\"%d\"", name, value);
        out += buf;
    }

    std::string RunProps(const CharProps& cp) const
    {
        std::string out;
        const std::string& font = cp.font.empty() ? m_defaultFont : cp.font;
        if (font != m_defaultFont) {
            std::string f = XmlEscape(font, true);
            out += "<w:rFonts w:ascii=\"" + f + "\" w:hAnsi=\"" + f + "\" w:eastAsia=\"" + f + "\" w:cs=\"" + f + "\"/>";
        }
        if (cp.bold) out += "<w:b/>";
        if (cp.italic) out += "<w:i/>";
        if (cp.strike) out += "<w:strike/>";
        if (cp.color.set) out += "<w:color w:val=\"" + HexColor(cp.color) + "\"/>";
        int size = cp.halfPoints > 0 ? cp.halfPoints : m_defaultSize;
        if (size != m_defaultSize) {
            out += "<w:sz";
            Attr(out, "w:val", size);
            out += "/><w:szCs";
            Attr(out, "w:val", size);
            out += "/>";
        }
        if (cp.highlight.set) {
            const char* named = nullptr;
            for (const NamedColor& nc : kHighlightColors)
                if (nc.r == cp.highlight.r && nc.g == cp.highlight.g && nc.b == cp.highlight.b) named = nc.name;
            if (named) out += std::string("<w:highlight w:val=\"") + named + "\"/>";
        }
        if (cp.underline) out += "<w:u w:val=\"single\"/>";
        if (cp.highlight.set) {
            bool named = false;
            for (const NamedColor& nc : kHighlightColors)
                if (nc.r == cp.highlight.r && nc.g == cp.highlight.g && nc.b == cp.highlight.b) named = true;
            if (!named) out += "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"" + HexColor(cp.highlight) + "\"/>";
        }
        if (cp.script == 1) out += "<w:vertAlign w:val=\"superscript\"/>";
        else if (cp.script == 2) out += "<w:vertAlign w:val=\"subscript\"/>";
        return out.empty() ? out : "<w:rPr>" + out + "</w:rPr>";
    }

    void WritePicture(const Inline& item, std::string& out)
    {
        if (item.picture < 0 || item.picture >= (int)m_doc.pictures.size()) return;
        const Picture& pic = m_doc.pictures[(size_t)item.picture];
        std::string relId;
        auto it = m_pictureRel.find(item.picture);
        if (it == m_pictureRel.end()) { relId = NewRelId(); m_pictureRel[item.picture] = relId; }
        else relId = it->second;
        long long cx = (long long)(item.widthTwips > 0 ? item.widthTwips : pic.widthPx * 15) * 635;
        long long cy = (long long)(item.heightTwips > 0 ? item.heightTwips : pic.heightPx * 15) * 635;
        int id = m_drawingId++;
        char buf[1600];
        snprintf(buf, sizeof(buf),
                 "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
                 "<wp:extent cx=\"%lld\" cy=\"%lld\"/><wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>"
                 "<wp:docPr id=\"%d\" name=\"Picture %d\"/><wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>"
                 "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                 "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"%s\"/><pic:cNvPicPr/></pic:nvPicPr>"
                 "<pic:blipFill><a:blip r:embed=\"%s\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                 "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm>"
                 "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic>"
                 "</wp:inline></w:drawing></w:r>",
                 cx, cy, id, id, id, PictureName(item.picture).c_str(), relId.c_str(), cx, cy);
        out += buf;
    }

    void WriteRun(const Inline& item, std::string& out)
    {
        if (item.kind == Inline::Picture) { WritePicture(item, out); return; }
        if (item.kind == Inline::Text && item.text.empty()) return;
        out += "<w:r>";
        out += RunProps(item.props);
        switch (item.kind) {
        case Inline::Text: {
            // Tabs and line feeds inside the text become their own elements.
            std::string chunk;
            auto flush = [&]() {
                if (chunk.empty()) return;
                out += "<w:t xml:space=\"preserve\">" + XmlEscape(chunk) + "</w:t>";
                chunk.clear();
            };
            for (char c : item.text) {
                if (c == '\t') { flush(); out += "<w:tab/>"; }
                else if (c == '\n') { flush(); out += "<w:br/>"; }
                else if (c != '\r') chunk += c;
            }
            flush();
            break;
        }
        case Inline::Tab: out += "<w:tab/>"; break;
        case Inline::LineBreak: out += "<w:br/>"; break;
        case Inline::PageBreak: out += "<w:br w:type=\"page\"/>"; break;
        default: break;
        }
        out += "</w:r>";
    }

    void WriteItems(const std::vector<Inline>& items, std::string& out)
    {
        size_t i = 0;
        while (i < items.size()) {
            const std::string& link = items[i].props.link;
            if (link.empty()) { WriteRun(items[i], out); ++i; continue; }
            size_t end = i;
            while (end < items.size() && items[end].props.link == link) ++end;
            std::string relId = NewRelId();
            m_linkRels.emplace_back(relId, link);
            out += "<w:hyperlink r:id=\"" + relId + "\">";
            for (size_t k = i; k < end; ++k) WriteRun(items[k], out);
            out += "</w:hyperlink>";
            i = end;
        }
    }

    // previousList: properties of the preceding paragraph if it was a list item of the same list.
    void WriteParagraph(const Paragraph& para, const ParaProps* previousList, std::string& out)
    {
        const ParaProps& p = para.props;
        std::string ppr;
        if (p.pageBreakBefore) ppr += "<w:pageBreakBefore/>";
        if (p.list > 0) {
            bool continues = previousList && previousList->list == p.list && previousList->listStart == p.listStart &&
                             !m_listInstances.empty();
            if (!continues) m_listInstances.push_back({ p.list, std::max(1, p.listStart) });
            ppr += "<w:numPr><w:ilvl w:val=\"0\"/><w:numId";
            Attr(ppr, "w:val", (int)m_listInstances.size());
            ppr += "/></w:numPr>";
        }
        if (!p.tabs.empty()) {
            ppr += "<w:tabs>";
            for (const TabStop& t : p.tabs) {
                static const char* const kKind[] = { "left", "center", "right", "decimal" };
                ppr += std::string("<w:tab w:val=\"") + kKind[t.align >= 0 && t.align <= 3 ? t.align : 0] + "\"";
                Attr(ppr, "w:pos", t.pos);
                ppr += "/>";
            }
            ppr += "</w:tabs>";
        }
        {
            // Always written: the document defaults carry no spacing of their own.
            ppr += "<w:spacing";
            Attr(ppr, "w:before", p.before);
            Attr(ppr, "w:after", p.after);
            int line = p.line > 0 ? p.line : 240;
            Attr(ppr, "w:line", line);
            ppr += std::string(" w:lineRule=\"") + (p.lineRule == 1 ? "exact" : p.lineRule == 2 ? "atLeast" : "auto") + "\"/>";
        }
        if (p.left || p.right || p.first) {
            ppr += "<w:ind";
            Attr(ppr, "w:left", p.left);
            if (p.right) Attr(ppr, "w:right", p.right);
            if (p.first < 0) Attr(ppr, "w:hanging", -p.first);
            else if (p.first > 0) Attr(ppr, "w:firstLine", p.first);
            ppr += "/>";
        }
        if (p.align >= 1 && p.align <= 3) {
            static const char* const kJc[] = { "left", "center", "right", "both" };
            ppr += std::string("<w:jc w:val=\"") + kJc[p.align] + "\"/>";
        }
        // Formatting of the paragraph mark keeps empty lines at the right height.
        if (para.items.empty()) { /* nothing known */ }
        else {
            std::string mark = RunProps(para.items.back().props);
            ppr += mark;
        }
        out += "<w:p><w:pPr>" + ppr + "</w:pPr>";
        WriteItems(para.items, out);
        out += "</w:p>";
    }

    void WriteTable(const Block& block, std::string& out)
    {
        size_t columns = 0;
        for (const Row& row : block.rows) columns = std::max(columns, row.cells.size());
        if (columns == 0) return;
        const char* border = " w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>";
        out += "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/><w:tblBorders>";
        for (const char* side : { "top", "left", "bottom", "right", "insideH", "insideV" })
            out += std::string("<w:") + side + border;
        out += "</w:tblBorders></w:tblPr><w:tblGrid>";
        for (size_t c = 0; c < columns; ++c) {
            int width = 1440;
            for (const Row& row : block.rows)
                if (c < row.cells.size() && row.cells[c].width > 0) { width = row.cells[c].width; break; }
            out += "<w:gridCol";
            Attr(out, "w:w", width);
            out += "/>";
        }
        out += "</w:tblGrid>";
        for (const Row& row : block.rows) {
            out += "<w:tr>";
            for (const Cell& cell : row.cells) {
                out += "<w:tc><w:tcPr><w:tcW";
                Attr(out, "w:w", cell.width > 0 ? cell.width : 1440);
                out += " w:type=\"dxa\"/></w:tcPr>";
                if (cell.paras.empty()) out += "<w:p/>";
                const ParaProps* previousList = nullptr;
                for (const Paragraph& para : cell.paras) {
                    WriteParagraph(para, previousList, out);
                    previousList = para.props.list > 0 ? &para.props : nullptr;
                }
                out += "</w:tc>";
            }
            out += "</w:tr>";
        }
        out += "</w:tbl>";
    }

    std::string ContentTypes() const
    {
        std::string out = kXmlHeader;
        out += "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
               "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
               "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
               "<Default Extension=\"png\" ContentType=\"image/png\"/>"
               "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
               "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
               "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>";
        if (!m_listInstances.empty())
            out += "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>";
        out += "</Types>";
        return out;
    }

    std::string Relationships() const
    {
        std::string base = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
        std::string out = kXmlHeader;
        out += "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
        out += "<Relationship Id=\"rId1\" Type=\"" + base + "styles\" Target=\"styles.xml\"/>";
        if (!m_listInstances.empty()) out += "<Relationship Id=\"rId2\" Type=\"" + base + "numbering\" Target=\"numbering.xml\"/>";
        for (const auto& kv : m_pictureRel)
            out += "<Relationship Id=\"" + kv.second + "\" Type=\"" + base + "image\" Target=\"media/" + PictureName(kv.first) + "\"/>";
        for (const auto& link : m_linkRels)
            out += "<Relationship Id=\"" + link.first + "\" Type=\"" + base + "hyperlink\" Target=\"" + XmlEscape(link.second, true) +
                   "\" TargetMode=\"External\"/>";
        out += "</Relationships>";
        return out;
    }

    std::string Styles() const
    {
        std::string font = XmlEscape(m_defaultFont, true);
        char size[96];
        snprintf(size, sizeof(size), "<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/>", m_defaultSize, m_defaultSize);
        std::string out = kXmlHeader;
        out += "<w:styles xmlns:w=\"" + std::string(kNsW) + "\"><w:docDefaults><w:rPrDefault><w:rPr>"
               "<w:rFonts w:ascii=\"" + font + "\" w:hAnsi=\"" + font + "\" w:eastAsia=\"" + font + "\" w:cs=\"" + font + "\"/>" +
               size + "</w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault>"
               "</w:docDefaults>"
               "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
               "</w:styles>";
        return out;
    }

    std::string Numbering() const
    {
        static const char* const kFormat[] = { "", "bullet", "decimal", "lowerLetter", "upperLetter", "lowerRoman", "upperRoman" };
        std::string out = kXmlHeader;
        out += "<w:numbering xmlns:w=\"" + std::string(kNsW) + "\">";
        // One abstract definition per list kind (ids 1..6), then one instance per list in the text.
        for (int kind = 1; kind <= 6; ++kind) {
            bool used = false;
            for (const ListInstance& li : m_listInstances) if (li.kind == kind) used = true;
            if (!used) continue;
            char buf[512];
            snprintf(buf, sizeof(buf),
                     "<w:abstractNum w:abstractNumId=\"%d\"><w:multiLevelType w:val=\"hybridMultilevel\"/>"
                     "<w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"%s\"/><w:lvlText w:val=\"%s\"/><w:lvlJc w:val=\"left\"/>"
                     "<w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr></w:lvl></w:abstractNum>",
                     kind, kFormat[kind], kind == 1 ? "\xE2\x80\xA2" : "%1.");
            out += buf;
        }
        for (size_t i = 0; i < m_listInstances.size(); ++i) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "<w:num w:numId=\"%d\"><w:abstractNumId w:val=\"%d\"/><w:lvlOverride w:ilvl=\"0\"><w:startOverride w:val=\"%d\"/>"
                     "</w:lvlOverride></w:num>",
                     (int)i + 1, m_listInstances[i].kind, m_listInstances[i].start);
            out += buf;
        }
        out += "</w:numbering>";
        return out;
    }
};

} // namespace

bool DocxRead(const Bytes& file, Document& doc, std::string* error)
{
    doc = Document();
    DocxReader reader(file, doc);
    if (!reader.Read(error)) return false;
    Sanitize(doc);
    return true;
}

bool DocxWrite(const Document& doc, Bytes& file, std::string* error)
{
    (void)error;
    DocxWriter writer(doc);
    return writer.Write(file);
}

} // namespace conv
