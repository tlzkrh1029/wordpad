// RTF -> document model. The reader targets the RTF dialect written by the Windows rich edit
// control (and tolerates the usual extras found in files written by other word processors).
#include "model.h"
#include "xml.h"
#include <cstdlib>
#include <cstring>
#include <map>

namespace conv {

namespace {

struct FontEntry {
    std::string name;
    int charset = 1;
    int explicitCodePage = 0;
};

unsigned CharsetToCodePage(int charset)
{
    switch (charset) {
    case 0: return 1252;
    case 2: return 1252;     // symbol fonts: bytes are glyph indices, keep them as Latin-1
    case 77: return 10000;
    case 128: return 932;
    case 129: return 949;
    case 130: return 1361;
    case 134: return 936;
    case 136: return 950;
    case 161: return 1253;
    case 162: return 1254;
    case 163: return 1258;
    case 177: return 1255;
    case 178: return 1256;
    case 186: return 1257;
    case 204: return 1251;
    case 222: return 874;
    case 238: return 1250;
    case 254: return 437;
    case 255: return 850;
    default: return 0;       // use the document code page
    }
}

enum Dest { D_BODY, D_SKIP, D_FONTTBL, D_COLORTBL, D_PICT, D_PN, D_FLDINST };

// Groups nest a dozen levels deep in real documents; every level costs a copy of the formatting state.
const size_t kMaxGroupDepth = 2000;

struct State {
    Dest dest = D_BODY;
    CharProps cp;
    int font = -1;
    int uc = 1;
    bool star = false;          // "\*" seen: an unknown destination must be skipped
    bool pictRoot = false;
    bool fieldRoot = false;
    bool fldInstRoot = false;
};

struct PictData {
    int type = 0;               // 1 png, 2 jpeg, 3 emf, 4 wmf, 5 dib
    int w = 0, h = 0, wgoal = 0, hgoal = 0, scalex = 100, scaley = 100;
    std::string hex;
    Bytes bin;
};

class Reader {
public:
    Reader(const std::string& src, Document& doc) : m_src(src), m_doc(doc) {}

    bool Parse()
    {
        size_t n = m_src.size();
        if (n < 6 || m_src.compare(0, 5, "{\\rtf") != 0) return false;
        while (m_pos < n) {
            char c = m_src[m_pos];
            if (c == '{') {
                FlushBytes();
                m_skip = 0;
                if (m_stack.size() >= kMaxGroupDepth) return false;     // not a sane document
                m_stack.push_back(m_st);
                m_st.star = false;
                m_st.pictRoot = m_st.fieldRoot = m_st.fldInstRoot = false;
                ++m_pos;
            } else if (c == '}') {
                FlushBytes();
                m_skip = 0;
                GroupEnd();
                ++m_pos;
                if (m_stack.empty()) break;
                m_st = m_stack.back();
                m_stack.pop_back();
                if (m_stack.empty()) break;      // the document group is closed; ignore anything after it
            } else if (c == '\\') {
                Control();
            } else if (c == '\r' || c == '\n' || c == '\0') {
                ++m_pos;
            } else {
                Byte((unsigned char)c);
                ++m_pos;
            }
        }
        Finish();
        return true;
    }

private:
    const std::string& m_src;
    Document& m_doc;
    size_t m_pos = 0;
    State m_st;
    std::vector<State> m_stack;

    std::map<int, FontEntry> m_fonts;
    int m_defaultFont = 0;
    unsigned m_ansiCodePage = 1252;
    std::vector<Color> m_colors;

    ParaProps m_para;
    bool m_paraInTable = false;
    int m_tabAlign = 0;
    Paragraph m_cur;
    std::string m_bytes;        // text bytes waiting to be decoded with the current code page
    int m_skip = 0;             // characters to skip after \uN
    uint32_t m_highSurrogate = 0;

    // table under construction
    std::vector<int> m_cellx;
    int m_rowLeft = 0;          // \trleft: position of the left edge of the row
    Row m_row;
    Cell m_cell;
    bool m_tableOpen = false;

    PictData m_pict;
    FontEntry m_fontEntry;
    int m_fontIndex = -1;
    std::string m_fontName;     // UTF-8
    int m_red = -1, m_green = -1, m_blue = -1;
    std::string m_fieldInstr;
    std::vector<std::string> m_links;

    unsigned CurrentCodePage() const
    {
        const FontEntry* f = nullptr;
        auto it = m_fonts.find(m_st.dest == D_FONTTBL ? -1 : m_st.font);
        if (it != m_fonts.end()) f = &it->second;
        if (m_st.dest == D_FONTTBL) f = &m_fontEntry;
        if (f) {
            if (f->explicitCodePage) return (unsigned)f->explicitCodePage;
            unsigned cp = CharsetToCodePage(f->charset);
            if (cp) return cp;
        }
        return m_ansiCodePage;
    }

    void EmitText(const std::string& utf8)
    {
        if (utf8.empty()) return;
        switch (m_st.dest) {
        case D_BODY:
            if (!m_cur.items.empty() && m_cur.items.back().kind == Inline::Text && m_cur.items.back().props == m_st.cp) {
                m_cur.items.back().text += utf8;
            } else {
                Inline run;
                run.text = utf8;
                run.props = m_st.cp;
                m_cur.items.push_back(std::move(run));
            }
            break;
        case D_FONTTBL: m_fontName += utf8; break;
        case D_FLDINST: m_fieldInstr += utf8; break;
        default: break;
        }
    }

    void FlushBytes()
    {
        if (m_bytes.empty()) return;
        std::string utf8;
        DecodeCodePage(CurrentCodePage(), m_bytes, utf8);
        m_bytes.clear();
        EmitText(utf8);
    }

    void Unicode(uint32_t cp)
    {
        FlushBytes();
        std::string utf8;
        AppendUtf8(utf8, cp);
        EmitText(utf8);
    }

    void AddInline(Inline::Kind kind)
    {
        FlushBytes();
        if (m_st.dest != D_BODY) return;
        Inline item;
        item.kind = kind;
        item.props = m_st.cp;
        m_cur.items.push_back(std::move(item));
    }

    void Byte(unsigned char b)
    {
        if (m_skip > 0) { --m_skip; return; }
        switch (m_st.dest) {
        case D_SKIP: case D_PN:
            return;
        case D_FONTTBL:
            if (b == ';') FinishFont();
            else m_bytes += (char)b;
            return;
        case D_COLORTBL:
            if (b == ';') {
                Color c;
                if (m_red >= 0 || m_green >= 0 || m_blue >= 0) {
                    c.set = true;
                    c.r = (uint8_t)std::max(0, m_red);
                    c.g = (uint8_t)std::max(0, m_green);
                    c.b = (uint8_t)std::max(0, m_blue);
                }
                m_colors.push_back(c);
                m_red = m_green = m_blue = -1;
            }
            return;
        case D_PICT:
            if ((b >= '0' && b <= '9') || (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F')) m_pict.hex += (char)b;
            return;
        default:
            m_bytes += (char)b;
            return;
        }
    }

    void FinishFont()
    {
        FlushBytes();
        // Trim, and drop anything after an alternative-name separator.
        std::string name = m_fontName;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        size_t start = 0;
        while (start < name.size() && name[start] == ' ') ++start;
        name = name.substr(start);
        if (m_fontIndex >= 0) {
            m_fontEntry.name = name;
            m_fonts[m_fontIndex] = m_fontEntry;
        }
        m_fontEntry = FontEntry();
        m_fontIndex = -1;
        m_fontName.clear();
    }

    void EndParagraph()
    {
        FlushBytes();
        m_cur.props = m_para;
        if (m_paraInTable) {
            m_cell.paras.push_back(std::move(m_cur));
        } else {
            m_tableOpen = false;
            Block block;
            block.para = std::move(m_cur);
            m_doc.blocks.push_back(std::move(block));
        }
        m_cur = Paragraph();
    }

    void EndCell()
    {
        FlushBytes();
        m_cur.props = m_para;
        if (!m_cur.items.empty() || m_cell.paras.empty()) m_cell.paras.push_back(std::move(m_cur));
        m_cur = Paragraph();
        m_row.cells.push_back(std::move(m_cell));
        m_cell = Cell();
    }

    void EndRow()
    {
        if (m_row.cells.empty()) return;
        int previous = m_rowLeft;
        for (size_t i = 0; i < m_row.cells.size(); ++i) {
            int edge = i < m_cellx.size() ? m_cellx[i] : previous + 1440;
            m_row.cells[i].width = std::max(1, edge - previous);
            previous = edge;
        }
        if (!m_tableOpen || m_doc.blocks.empty() || !m_doc.blocks.back().isTable) {
            Block block;
            block.isTable = true;
            m_doc.blocks.push_back(std::move(block));
        }
        m_doc.blocks.back().rows.push_back(std::move(m_row));
        m_row = Row();
        m_tableOpen = true;
    }

    void FinishPicture()
    {
        PictData p;
        std::swap(p, m_pict);
        Bytes data;
        if (!p.bin.empty()) {
            data.swap(p.bin);
        } else {
            data.reserve(p.hex.size() / 2);
            auto nibble = [](char c) -> int { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
            for (size_t i = 0; i + 1 < p.hex.size(); i += 2)
                data.push_back((uint8_t)((nibble(p.hex[i]) << 4) | nibble(p.hex[i + 1])));
        }
        if (data.empty() || p.type == 0) return;

        // The numbers come from the file: compute in 64 bits and keep the result within the page limits.
        auto scaled = [](int goal, int pixels, int percent) {
            long long twips = goal > 0 ? goal : (long long)pixels * 15;
            twips = twips * std::max(1, percent) / 100;
            return (int)std::max(0LL, std::min<long long>(twips, kMaxTwips));
        };
        int widthTwips = scaled(p.wgoal, p.w, p.scalex);
        int heightTwips = scaled(p.hgoal, p.h, p.scaley);
        if (widthTwips <= 0 || heightTwips <= 0) { widthTwips = 1440; heightTwips = 1440; }
        // Raster size used when a metafile has to be converted (twice the screen resolution).
        int pxW = std::min(4096, std::max(16, widthTwips * 192 / 1440));
        int pxH = std::min(4096, std::max(16, heightTwips * 192 / 1440));
        PictureKind kind = p.type == 3 ? PictureKind::Emf : p.type == 4 ? PictureKind::Wmf :
                           p.type == 5 ? PictureKind::Dib : PictureKind::AnyImageFile;
        int index = AddPicture(m_doc, data, kind, pxW, pxH);
        if (index < 0) return;
        Inline item;
        item.kind = Inline::Picture;
        item.picture = index;
        item.props = m_st.cp;
        item.widthTwips = widthTwips;
        item.heightTwips = heightTwips;
        m_cur.items.push_back(std::move(item));
    }

    void FinishFieldInstruction()
    {
        // HYPERLINK "target"  (a "\l" switch marks an in-document bookmark, which is not kept)
        std::string instr;
        std::swap(instr, m_fieldInstr);
        size_t at = instr.find("HYPERLINK");
        if (at == std::string::npos || m_links.empty()) return;
        size_t q1 = instr.find('"', at);
        if (q1 == std::string::npos) return;
        std::string between = instr.substr(at + 9, q1 - at - 9);
        if (between.find("\\l") != std::string::npos) return;
        size_t q2 = instr.find('"', q1 + 1);
        if (q2 == std::string::npos) return;
        m_links.back() = instr.substr(q1 + 1, q2 - q1 - 1);
    }

    void GroupEnd()
    {
        if (m_st.pictRoot) FinishPicture();
        if (m_st.fldInstRoot) FinishFieldInstruction();
        if (m_st.fieldRoot && !m_links.empty()) m_links.pop_back();
    }

    void Finish()
    {
        FlushBytes();
        if (!m_row.cells.empty() || !m_cell.paras.empty()) {
            if (!m_cur.items.empty() || !m_cell.paras.empty()) EndCell();
            EndRow();
        } else if (!m_cur.items.empty()) {
            EndParagraph();
        }
        auto it = m_fonts.find(m_defaultFont);
        if (it != m_fonts.end()) m_doc.defaultFont = it->second.name;
    }

    void Control()
    {
        size_t n = m_src.size();
        ++m_pos;
        if (m_pos >= n) return;
        char c = m_src[m_pos];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            size_t start = m_pos;
            while (m_pos < n && ((m_src[m_pos] >= 'a' && m_src[m_pos] <= 'z') || (m_src[m_pos] >= 'A' && m_src[m_pos] <= 'Z'))) ++m_pos;
            std::string word = m_src.substr(start, m_pos - start);
            bool hasParam = false;
            long param = 0;
            if (m_pos < n && (m_src[m_pos] == '-' || (m_src[m_pos] >= '0' && m_src[m_pos] <= '9'))) {
                size_t ps = m_pos;
                if (m_src[m_pos] == '-') ++m_pos;
                while (m_pos < n && m_src[m_pos] >= '0' && m_src[m_pos] <= '9') ++m_pos;
                param = strtol(m_src.substr(ps, m_pos - ps).c_str(), nullptr, 10);
                // Keep later arithmetic (pixels to twips and the like) inside the range of an int.
                param = std::max(-100000000L, std::min(100000000L, param));
                hasParam = true;
            }
            if (m_pos < n && m_src[m_pos] == ' ') ++m_pos;
            Word(word, hasParam, (int)param);
            return;
        }
        ++m_pos;
        switch (c) {
        case '\'': {
            if (m_pos + 1 < n) {
                char hex[3] = { m_src[m_pos], m_src[m_pos + 1], 0 };
                m_pos += 2;
                Byte((unsigned char)strtoul(hex, nullptr, 16));
            }
            break;
        }
        case '*': m_st.star = true; break;
        case '~': if (m_skip > 0) --m_skip; else Unicode(0xA0); break;
        case '_': if (m_skip > 0) --m_skip; else Unicode(0x2011); break;
        case '-': break;                       // optional hyphen
        case '\\': case '{': case '}': Byte((unsigned char)c); break;
        case '\r': case '\n': Word("par", false, 0); break;
        default: break;
        }
    }

    void Word(const std::string& w, bool hasParam, int v)
    {
        // Binary data may contain braces and backslashes: outside of pictures it is skipped as a whole.
        if (w == "bin" && m_st.dest != D_PICT) {
            if (hasParam && v > 0) m_pos += std::min((size_t)v, m_src.size() - m_pos);
            return;
        }
        // "\u" is the only control word that takes part in the \ucN skipping.
        if (w == "u") {
            if (m_st.dest == D_SKIP || m_st.dest == D_PICT || m_st.dest == D_COLORTBL) { m_skip = m_st.uc; return; }
            uint32_t cp = (uint32_t)(v < 0 ? v + 65536 : v) & 0x1FFFFF;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                m_highSurrogate = cp;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                if (m_highSurrogate) Unicode(0x10000 + ((m_highSurrogate - 0xD800) << 10) + (cp - 0xDC00));
                m_highSurrogate = 0;
            } else {
                m_highSurrogate = 0;
                Unicode(cp);
            }
            m_skip = m_st.uc;
            return;
        }
        m_skip = 0;
        FlushBytes();

        if (m_st.star) {
            m_st.star = false;
            if (w == "pn") { m_st.dest = D_PN; return; }
            if (w == "fldinst") { m_st.dest = D_FLDINST; m_st.fldInstRoot = true; m_fieldInstr.clear(); return; }
            if (w == "shppict") return;                 // contains a regular \pict
            m_st.dest = D_SKIP;
            return;
        }

        switch (m_st.dest) {
        case D_SKIP:
            return;
        case D_FONTTBL:
            if (w == "f") { m_fontIndex = v; }
            else if (w == "fcharset") m_fontEntry.charset = v;
            else if (w == "cpg") m_fontEntry.explicitCodePage = v;
            else if (w == "falt" || w == "panose" || w == "fname" || w == "fontemb" || w == "fontfile") m_st.dest = D_SKIP;
            return;
        case D_COLORTBL:
            if (w == "red") m_red = v;
            else if (w == "green") m_green = v;
            else if (w == "blue") m_blue = v;
            return;
        case D_PICT:
            if (w == "pngblip") m_pict.type = 1;
            else if (w == "jpegblip") m_pict.type = 2;
            else if (w == "emfblip") m_pict.type = 3;
            else if (w == "wmetafile") m_pict.type = 4;
            else if (w == "dibitmap") m_pict.type = 5;
            else if (w == "picw") m_pict.w = v;
            else if (w == "pich") m_pict.h = v;
            else if (w == "picwgoal") m_pict.wgoal = v;
            else if (w == "pichgoal") m_pict.hgoal = v;
            else if (w == "picscalex") m_pict.scalex = v;
            else if (w == "picscaley") m_pict.scaley = v;
            else if (w == "bin" && hasParam && v > 0) {
                size_t count = std::min((size_t)v, m_src.size() - m_pos);
                m_pict.bin.insert(m_pict.bin.end(), m_src.begin() + (ptrdiff_t)m_pos, m_src.begin() + (ptrdiff_t)(m_pos + count));
                m_pos += count;
            }
            return;
        case D_PN:
            if (w == "pnlvlblt") m_para.list = 1;
            else if (w == "pndec") m_para.list = 2;
            else if (w == "pnlcltr") m_para.list = 3;
            else if (w == "pnucltr") m_para.list = 4;
            else if (w == "pnlcrm") m_para.list = 5;
            else if (w == "pnucrm") m_para.list = 6;
            else if (w == "pnlvlbody" || w == "pnlvlcont") { if (m_para.list == 0) m_para.list = 2; }
            else if (w == "pnstart") m_para.listStart = std::max(1, v);
            else if (w == "pntxtb" || w == "pntxta") m_st.dest = D_SKIP;
            return;
        case D_FLDINST:
            return;
        case D_BODY:
            break;
        }
        BodyWord(w, hasParam, v);
    }

    static bool OnOff(bool hasParam, int v) { return !hasParam || v != 0; }

    Color ColorAt(int index) const
    {
        if (index <= 0 || index >= (int)m_colors.size()) return Color();
        return m_colors[(size_t)index];
    }

    void BodyWord(const std::string& w, bool hasParam, int v)
    {
        CharProps& cp = m_st.cp;
        switch (w[0]) {
        case 'a':
            if (w == "ansicpg") m_ansiCodePage = (unsigned)v;
            break;
        case 'b':
            if (w == "b") cp.bold = OnOff(hasParam, v);
            else if (w == "bullet") Unicode(0x2022);
            break;
        case 'c':
            if (w == "cf") cp.color = ColorAt(v);
            else if (w == "cb" || w == "chcbpat") cp.highlight = ColorAt(v);
            else if (w == "cell") EndCell();
            else if (w == "cellx") m_cellx.push_back(v);
            else if (w == "colortbl") { m_st.dest = D_COLORTBL; m_colors.clear(); m_red = m_green = m_blue = -1; }
            break;
        case 'd':
            if (w == "deff") { m_defaultFont = v; if (m_st.font < 0) m_st.font = v; }
            break;
        case 'e':
            if (w == "emdash") Unicode(0x2014);
            else if (w == "endash") Unicode(0x2013);
            else if (w == "emspace") Unicode(0x2003);
            else if (w == "enspace") Unicode(0x2002);
            break;
        case 'f':
            if (w == "f") {
                m_st.font = v;
                auto it = m_fonts.find(v);
                cp.font = it != m_fonts.end() ? it->second.name : std::string();
            }
            else if (w == "fs") cp.halfPoints = v > 0 ? v : 0;
            else if (w == "fi") m_para.first = v;
            else if (w == "fonttbl") m_st.dest = D_FONTTBL;
            else if (w == "field") { m_st.fieldRoot = true; m_links.push_back(std::string()); }
            else if (w == "fldrslt") { if (!m_links.empty()) cp.link = m_links.back(); }
            else if (w == "footer" || w == "footerl" || w == "footerr" || w == "footerf" || w == "footnote") m_st.dest = D_SKIP;
            break;
        case 'h':
            if (w == "highlight") cp.highlight = ColorAt(v);
            else if (w == "header" || w == "headerl" || w == "headerr" || w == "headerf") m_st.dest = D_SKIP;
            break;
        case 'i':
            if (w == "i") cp.italic = OnOff(hasParam, v);
            else if (w == "intbl") m_paraInTable = true;
            else if (w == "info") m_st.dest = D_SKIP;
            break;
        case 'l':
            if (w == "li") m_para.left = v;
            else if (w == "line") AddInline(Inline::LineBreak);
            else if (w == "lquote") Unicode(0x2018);
            else if (w == "ldblquote") Unicode(0x201C);
            else if (w == "listtext") m_st.dest = D_SKIP;
            break;
        case 'n':
            if (w == "nosupersub") cp.script = 0;
            else if (w == "nonshppict") m_st.dest = D_SKIP;
            else if (w == "nestcell") AddInline(Inline::Tab);
            else if (w == "nestrow") { /* flattened */ }
            break;
        case 'p':
            if (w == "par") EndParagraph();
            else if (w == "pard") { m_para = ParaProps(); m_paraInTable = false; m_tabAlign = 0; }
            else if (w == "plain") {
                std::string link = cp.link;
                cp = CharProps();
                cp.link = link;
                m_st.font = m_defaultFont;
            }
            else if (w == "page") AddInline(Inline::PageBreak);
            else if (w == "pagebb") m_para.pageBreakBefore = true;
            else if (w == "pict") { m_st.dest = D_PICT; m_st.pictRoot = true; m_pict = PictData(); }
            else if (w == "pntext") m_st.dest = D_SKIP;
            break;
        case 'q':
            if (w == "ql") m_para.align = 0;
            else if (w == "qc") m_para.align = 1;
            else if (w == "qr") m_para.align = 2;
            else if (w == "qj") m_para.align = 3;
            else if (w == "qmspace") Unicode(0x2005);
            break;
        case 'r':
            if (w == "ri") m_para.right = v;
            else if (w == "row") EndRow();
            else if (w == "rquote") Unicode(0x2019);
            else if (w == "rdblquote") Unicode(0x201D);
            break;
        case 's':
            if (w == "sa") m_para.after = v;
            else if (w == "sb") m_para.before = v;
            else if (w == "sl") {
                // Positive: "at least" (or a multiple with \slmult1); negative: exact.
                if (v < 0) { m_para.lineRule = 1; m_para.line = -v; }
                else if (v == 0) { m_para.lineRule = 0; m_para.line = 0; }
                else { m_para.lineRule = 2; m_para.line = v; }
            }
            else if (w == "slmult") {
                if (v == 1 && m_para.line != 0) m_para.lineRule = 0;   // multiple, in 240ths of a line
            }
            else if (w == "strike" || w == "striked") cp.strike = OnOff(hasParam, v);
            else if (w == "super") cp.script = 1;
            else if (w == "sub") cp.script = 2;
            else if (w == "sect") EndParagraph();
            else if (w == "stylesheet") m_st.dest = D_SKIP;
            break;
        case 't':
            if (w == "tab") AddInline(Inline::Tab);
            else if (w == "tx") {
                TabStop t;
                t.pos = v;
                t.align = m_tabAlign;
                m_para.tabs.push_back(t);
                m_tabAlign = 0;
            }
            else if (w == "tqc") m_tabAlign = 1;
            else if (w == "tqr") m_tabAlign = 2;
            else if (w == "tqdec") m_tabAlign = 3;
            else if (w == "trowd") { m_cellx.clear(); m_rowLeft = 0; }
            else if (w == "trleft") m_rowLeft = v;
            break;
        case 'u':
            if (w == "ul") cp.underline = OnOff(hasParam, v);
            else if (w == "ulnone") cp.underline = false;
            else if (w == "uc") m_st.uc = std::max(0, v);
            else if (w.size() > 2 && w.compare(0, 2, "ul") == 0) cp.underline = OnOff(hasParam, v);   // \uld, \uldb, \ulw, ...
            break;
        case 'z':
            if (w == "zwj") Unicode(0x200D);
            else if (w == "zwnj") Unicode(0x200C);
            break;
        default:
            break;
        }
    }
};

} // namespace

bool RtfRead(const std::string& rtf, Document& doc, std::string* error)
{
    doc = Document();
    Reader reader(rtf, doc);
    if (!reader.Parse()) {
        if (error) *error = "not an RTF document";
        return false;
    }
    Sanitize(doc);
    return true;
}

} // namespace conv
