// The neutral document model shared by the RTF, DOCX and ODT readers and writers.
#pragma once
#include "convert.h"
#include <algorithm>

namespace conv {

struct Color {
    bool set = false;          // false: automatic text colour / no highlight
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Color& o) const { return set == o.set && (!set || (r == o.r && g == o.g && b == o.b)); }
    bool operator!=(const Color& o) const { return !(*this == o); }
};

struct CharProps {
    std::string font;          // UTF-8 family name; empty = document default
    int halfPoints = 0;        // 0 = document default
    bool bold = false, italic = false, underline = false, strike = false;
    int script = 0;            // 1 = superscript, 2 = subscript
    Color color, highlight;
    std::string link;          // hyperlink target, empty if none

    bool operator==(const CharProps& o) const
    {
        return font == o.font && halfPoints == o.halfPoints && bold == o.bold && italic == o.italic &&
               underline == o.underline && strike == o.strike && script == o.script && color == o.color &&
               highlight == o.highlight && link == o.link;
    }
    bool operator!=(const CharProps& o) const { return !(*this == o); }
};

struct TabStop {
    int pos = 0;               // twips
    int align = 0;             // 0 left, 1 centre, 2 right, 3 decimal
    bool operator==(const TabStop& o) const { return pos == o.pos && align == o.align; }
};

struct ParaProps {
    int align = 0;             // 0 left, 1 centre, 2 right, 3 justified
    int left = 0;              // twips: indent of the paragraph body
    int first = 0;             // twips: first line relative to `left` (negative = hanging)
    int right = 0;
    int before = 0, after = 0; // twips
    int lineRule = 0;          // 0 multiple of single spacing, 1 exact, 2 at least
    int line = 0;              // rule 0: 240ths of a line (0 or 240 = single); rules 1/2: twips
    int list = 0;              // 0 none, 1 bullet, 2 decimal, 3 lower letter, 4 upper letter, 5 lower roman, 6 upper roman
    int listStart = 1;
    bool pageBreakBefore = false;
    std::vector<TabStop> tabs;

    bool operator==(const ParaProps& o) const
    {
        return align == o.align && left == o.left && first == o.first && right == o.right && before == o.before &&
               after == o.after && lineRule == o.lineRule && line == o.line && list == o.list &&
               listStart == o.listStart && pageBreakBefore == o.pageBreakBefore && tabs == o.tabs;
    }
    bool operator!=(const ParaProps& o) const { return !(*this == o); }
};

struct Inline {
    enum Kind { Text, Tab, LineBreak, PageBreak, Picture };
    Kind kind = Text;
    std::string text;          // UTF-8 (Text only)
    CharProps props;
    int picture = -1;          // index into Document::pictures (Picture only)
    int widthTwips = 0, heightTwips = 0;
};

struct Paragraph {
    ParaProps props;
    std::vector<Inline> items;
};

struct Cell {
    int width = 0;             // twips
    std::vector<Paragraph> paras;
};

struct Row { std::vector<Cell> cells; };

struct Block {
    bool isTable = false;
    Paragraph para;            // when !isTable
    std::vector<Row> rows;     // when isTable
};

struct Picture {
    bool jpeg = false;         // false: PNG
    Bytes data;
    int widthPx = 0, heightPx = 0;
};

struct Document {
    std::vector<Block> blocks;
    std::vector<Picture> pictures;
    std::string defaultFont;   // empty = unspecified
    int defaultHalfPoints = 0; // 0 = unspecified
};

// Page geometry written into exported documents (twips).
struct PageSetup {
    int width = 11906, height = 16838;
    int left = 1800, top = 1440, right = 1800, bottom = 1440;
};
void SetExportPage(const PageSetup& page);
const PageSetup& GetExportPage();

// Code page conversion is supplied by the host (the portable fallback only knows Windows-1252).
typedef bool (*CodePageDecoder)(unsigned codePage, const std::string& bytes, std::string& utf8);
typedef bool (*CodePageEncoder)(unsigned codePage, const std::string& utf8, std::string& bytes);
void SetCodePageHooks(CodePageDecoder decode, CodePageEncoder encode);
bool DecodeCodePage(unsigned codePage, const std::string& bytes, std::string& utf8);
bool EncodeCodePage(unsigned codePage, const std::string& utf8, std::string& bytes);

// Individual conversion steps (also used directly by the unit tests).
bool RtfRead(const std::string& rtf, Document& doc, std::string* error);
std::string RtfWrite(const Document& doc);
bool DocxRead(const Bytes& file, Document& doc, std::string* error);
bool DocxWrite(const Document& doc, Bytes& file, std::string* error);
bool OdtRead(const Bytes& file, Document& doc, std::string* error);
bool OdtWrite(const Document& doc, Bytes& file, std::string* error);

// Brings every number of a freshly read document into the range that the writers and the rich edit
// control can handle (damaged or hostile files contain arbitrary values). Called by all readers.
void Sanitize(Document& doc);
// Rounds to the nearest integer; NaN becomes 0 and values beyond +-1e9 saturate.
int RoundToInt(double value);
const int kMaxTwips = 31680;       // 22 inches: the largest measure Word and the rich edit control accept

// Helpers shared by the readers and writers.
bool IsEastAsian(uint32_t codePoint);
// Splits text runs so that East Asian characters use eastAsiaFont and other text keeps its font.
void SplitByScript(const std::string& text, const CharProps& props, const std::string& eastAsiaFont,
                   std::vector<Inline>& out);
std::string HexColor(const Color& c);                 // "RRGGBB"
bool ParseHexColor(const std::string& text, Color& c); // accepts "RRGGBB" and "#RRGGBB"
// Decodes picture bytes of any supported kind into PNG/JPEG and adds them to the document.
// Returns the picture index, or -1 when the data cannot be used.
int AddPicture(Document& doc, const Bytes& data, PictureKind kind, int widthPx, int heightPx);
bool ImageSize(const Bytes& data, bool* jpeg, int* width, int* height);

} // namespace conv
