// Document model -> RTF, written for the Windows rich edit control.
#include "model.h"
#include "xml.h"
#include <cstdio>

namespace conv {

namespace {

class Writer {
public:
    explicit Writer(const Document& doc) : m_doc(doc) {}

    std::string Run()
    {
        std::string defaultFont = m_doc.defaultFont.empty() ? std::string("Calibri") : m_doc.defaultFont;
        m_defaultFont = FontIndex(defaultFont);
        m_symbolFont = FontIndex("Symbol");
        m_defaultSize = m_doc.defaultHalfPoints > 0 ? m_doc.defaultHalfPoints : 22;

        // The body is produced first so that the font and colour tables are complete.
        std::string body;
        const ParaProps* previous = nullptr;
        for (const Block& block : m_doc.blocks) {
            if (block.isTable) {
                WriteTable(block, body);
                previous = nullptr;
            } else {
                WriteParagraph(block.para, false, previous, body);
                body += "\\par\r\n";
                previous = &block.para.props;
            }
        }

        std::string out = "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1";
        out += "{\\fonttbl";
        for (size_t i = 0; i < m_fonts.size(); ++i) {
            char head[64];
            snprintf(head, sizeof(head), "{\\f%d\\fnil\\fcharset%d ", (int)i, Charset(m_fonts[i]));
            out += head;
            out += FontName(m_fonts[i]);
            out += ";}";
        }
        out += "}\r\n{\\colortbl ;";
        for (const Color& c : m_colors) {
            char buf[64];
            snprintf(buf, sizeof(buf), "\\red%d\\green%d\\blue%d;", c.r, c.g, c.b);
            out += buf;
        }
        out += "}\r\n\\viewkind4\\uc1 ";
        out += body;
        out += "}\r\n";
        return out;
    }

private:
    const Document& m_doc;
    std::vector<std::string> m_fonts;
    std::vector<Color> m_colors;
    int m_defaultFont = 0, m_symbolFont = 1, m_defaultSize = 22;

    int FontIndex(const std::string& name)
    {
        for (size_t i = 0; i < m_fonts.size(); ++i)
            if (m_fonts[i] == name) return (int)i;
        m_fonts.push_back(name);
        return (int)m_fonts.size() - 1;
    }

    int ColorIndex(const Color& c)
    {
        if (!c.set) return 0;
        for (size_t i = 0; i < m_colors.size(); ++i)
            if (m_colors[i] == c) return (int)i + 1;
        m_colors.push_back(c);
        return (int)m_colors.size();
    }

    // Character set of a font, guessed from the script of its name (the text itself is Unicode).
    static int Charset(const std::string& name)
    {
        if (name == "Symbol" || name == "Wingdings" || name == "Webdings") return 2;
        size_t pos = 0;
        bool hangul = false, kana = false, han = false;
        while (pos < name.size()) {
            uint32_t cp = NextCodePoint(name, pos);
            if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F)) hangul = true;
            else if (cp >= 0x3040 && cp <= 0x30FF) kana = true;
            else if (cp >= 0x4E00 && cp <= 0x9FFF) han = true;
        }
        if (hangul) return 129;
        if (kana) return 128;
        if (han) return 134;
        return 0;
    }

    static unsigned CodePageOfCharset(int charset)
    {
        switch (charset) {
        case 128: return 932;
        case 129: return 949;
        case 134: return 936;
        default: return 1252;
        }
    }

    // Font names are written in the code page of their character set (as the rich edit control
    // does); if that is not possible they fall back to \u escapes.
    static std::string FontName(const std::string& name)
    {
        bool ascii = true;
        for (char c : name) if ((unsigned char)c >= 0x80) ascii = false;
        if (ascii) return Escape(name);
        std::string bytes;
        if (EncodeCodePage(CodePageOfCharset(Charset(name)), name, bytes)) {
            std::string out;
            for (char ch : bytes) {
                unsigned char c = (unsigned char)ch;
                if (c >= 0x80 || c == '\\' || c == '{' || c == '}' || c == ';') {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\'%02x", c);
                    out += buf;
                } else {
                    out += ch;
                }
            }
            return out;
        }
        return Escape(name);
    }

    static std::string Escape(const std::string& text)
    {
        std::string out;
        out.reserve(text.size() + 16);
        size_t pos = 0;
        while (pos < text.size()) {
            uint32_t cp = NextCodePoint(text, pos);
            if (cp == '\\' || cp == '{' || cp == '}') { out += '\\'; out += (char)cp; }
            else if (cp == '\t') out += "\\tab ";
            else if (cp == '\n') out += "\\line ";
            else if (cp == '\r') { /* dropped */ }
            else if (cp == 0xA0) out += "\\~";
            else if (cp < 0x20) out += ' ';
            else if (cp < 0x80) out += (char)cp;
            else {
                char buf[32];
                if (cp >= 0x10000) {
                    uint32_t v = cp - 0x10000;
                    int hi = (int)(0xD800 + (v >> 10)), lo = (int)(0xDC00 + (v & 0x3FF));
                    snprintf(buf, sizeof(buf), "\\u%d?\\u%d?", hi - 65536, lo - 65536);
                } else {
                    snprintf(buf, sizeof(buf), "\\u%d?", cp >= 0x8000 ? (int)cp - 65536 : (int)cp);
                }
                out += buf;
            }
        }
        return out;
    }

    static void Num(std::string& out, const char* keyword, int value)
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "\\%s%d", keyword, value);
        out += buf;
    }

    std::string CharCodes(const CharProps& cp)
    {
        // \plain first: every run states its complete formatting, nothing leaks from the previous one.
        std::string out = "\\plain";
        Num(out, "f", cp.font.empty() ? m_defaultFont : FontIndex(cp.font));
        Num(out, "fs", cp.halfPoints > 0 ? cp.halfPoints : m_defaultSize);
        if (cp.bold) out += "\\b";
        if (cp.italic) out += "\\i";
        if (cp.underline) out += "\\ul";
        if (cp.strike) out += "\\strike";
        if (cp.script == 1) out += "\\super";
        else if (cp.script == 2) out += "\\sub";
        if (cp.color.set) Num(out, "cf", ColorIndex(cp.color));
        if (cp.highlight.set) Num(out, "highlight", ColorIndex(cp.highlight));
        return out;
    }

    void WritePicture(const Inline& item, std::string& out)
    {
        if (item.picture < 0 || item.picture >= (int)m_doc.pictures.size()) return;
        const Picture& pic = m_doc.pictures[(size_t)item.picture];
        int wTw = item.widthTwips > 0 ? item.widthTwips : pic.widthPx * 15;
        int hTw = item.heightTwips > 0 ? item.heightTwips : pic.heightPx * 15;
        char head[160];
        snprintf(head, sizeof(head), "{\\pict\\%s\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d\r\n", pic.jpeg ? "jpegblip" : "pngblip",
                 pic.widthPx, pic.heightPx, wTw, hTw);
        out += head;
        static const char kHex[] = "0123456789abcdef";
        out.reserve(out.size() + pic.data.size() * 2 + pic.data.size() / 32 + 8);
        for (size_t i = 0; i < pic.data.size(); ++i) {
            out += kHex[pic.data[i] >> 4];
            out += kHex[pic.data[i] & 15];
            if ((i & 63) == 63) out += "\r\n";
        }
        out += "}";
    }

    void WriteItems(const std::vector<Inline>& items, std::string& out)
    {
        size_t i = 0;
        while (i < items.size()) {
            const std::string& link = items[i].props.link;
            size_t end = i;
            if (!link.empty()) {
                while (end < items.size() && items[end].props.link == link) ++end;
                std::string target;
                for (char c : link) { if (c == '"') continue; target += c; }
                out += "{\\field{\\*\\fldinst{HYPERLINK \"" + Escape(target) + "\"}}{\\fldrslt{";
                for (size_t k = i; k < end; ++k) WriteItem(items[k], out);
                out += "}}}";
                i = end;
            } else {
                WriteItem(items[i], out);
                ++i;
            }
        }
    }

    void WriteItem(const Inline& item, std::string& out)
    {
        switch (item.kind) {
        case Inline::Text:
            if (item.text.empty()) return;
            out += "{" + CharCodes(item.props) + " " + Escape(item.text) + "}";
            break;
        case Inline::Tab: out += "{" + CharCodes(item.props) + "\\tab}"; break;
        case Inline::LineBreak: out += "{" + CharCodes(item.props) + "\\line}"; break;
        case Inline::PageBreak: out += "\\page "; break;
        case Inline::Picture: WritePicture(item, out); break;
        }
    }

    std::string ListCodes(const ParaProps& p)
    {
        if (p.list <= 0) return std::string();
        char buf[200];
        if (p.list == 1) {
            snprintf(buf, sizeof(buf), "{\\pntext\\f%d\\'B7\\tab}{\\*\\pn\\pnlvlblt\\pnf%d\\pnindent360{\\pntxtb\\'B7}}",
                     m_symbolFont, m_symbolFont);
            return buf;
        }
        const char* kind = p.list == 3 ? "pnlcltr" : p.list == 4 ? "pnucltr" : p.list == 5 ? "pnlcrm" : p.list == 6 ? "pnucrm" : "pndec";
        snprintf(buf, sizeof(buf), "{\\*\\pn\\pnlvlbody\\pnf%d\\pnindent360\\pnstart%d\\%s{\\pntxta.}}", m_defaultFont,
                 std::max(1, p.listStart), kind);
        return buf;
    }

    std::string ParaCodes(const ParaProps& p, bool inTable)
    {
        std::string out = "\\pard";
        if (inTable) out += "\\intbl";
        out += ListCodes(p);
        if (p.pageBreakBefore) out += "\\pagebb";
        static const char* const kAlign[] = { "", "\\qc", "\\qr", "\\qj" };
        out += kAlign[p.align >= 0 && p.align <= 3 ? p.align : 0];
        if (p.first) Num(out, "fi", p.first);
        if (p.left) Num(out, "li", p.left);
        if (p.right) Num(out, "ri", p.right);
        if (p.before) Num(out, "sb", p.before);
        if (p.after) Num(out, "sa", p.after);
        if (p.lineRule == 0) {
            if (p.line > 0 && p.line != 240) { Num(out, "sl", p.line); out += "\\slmult1"; }
        } else if (p.line > 0) {
            Num(out, "sl", p.lineRule == 1 ? -p.line : p.line);
            out += "\\slmult0";
        }
        for (const TabStop& t : p.tabs) {
            if (t.align == 1) out += "\\tqc";
            else if (t.align == 2) out += "\\tqr";
            else if (t.align == 3) out += "\\tqdec";
            Num(out, "tx", t.pos);
        }
        return out;
    }

    // previous: properties of the paragraph written just before (nullptr when there is none).
    void WriteParagraph(const Paragraph& para, bool inTable, const ParaProps* previous, std::string& out)
    {
        // Consecutive items of one list share the paragraph settings, as the control writes them;
        // repeating \pn would restart the numbering.
        bool continuesList = previous && para.props.list > 0 && *previous == para.props && !inTable;
        if (continuesList) {
            if (para.props.list == 1) {
                char buf[64];
                snprintf(buf, sizeof(buf), "{\\pntext\\f%d\\'B7\\tab}", m_symbolFont);
                out += buf;
            }
        } else {
            out += ParaCodes(para.props, inTable);
        }
        // Default run settings for empty paragraphs and for the paragraph mark.
        CharProps base;
        if (!para.items.empty()) {
            base = para.items.back().props;
            base.link.clear();
            base.script = 0;
            base.highlight = Color();
        }
        out += CharCodes(base);
        out += " ";
        WriteItems(para.items, out);
    }

    void WriteTable(const Block& block, std::string& out)
    {
        for (const Row& row : block.rows) {
            if (row.cells.empty()) continue;
            out += "\\trowd\\trgaph108\\trleft-108";
            int edge = -108;
            for (const Cell& cell : row.cells) {
                edge += cell.width > 0 ? cell.width : 1440;
                out += "\\clbrdrt\\brdrs\\brdrw10\\clbrdrl\\brdrs\\brdrw10\\clbrdrb\\brdrs\\brdrw10\\clbrdrr\\brdrs\\brdrw10";
                Num(out, "cellx", edge);
            }
            out += "\r\n";
            for (const Cell& cell : row.cells) {
                if (cell.paras.empty()) {
                    out += "\\pard\\intbl";
                } else {
                    for (size_t i = 0; i < cell.paras.size(); ++i) {
                        if (i > 0) out += "\\par\r\n";
                        WriteParagraph(cell.paras[i], true, nullptr, out);
                    }
                }
                out += "\\cell\r\n";
            }
            out += "\\row\r\n";
        }
    }
};

} // namespace

std::string RtfWrite(const Document& doc)
{
    Writer writer(doc);
    return writer.Run();
}

} // namespace conv
