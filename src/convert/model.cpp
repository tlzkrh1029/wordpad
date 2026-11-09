// Shared helpers of the document converters and the public entry points.
#include "model.h"
#include "xml.h"
#include "zip.h"
#include <cstdio>
#include <cstring>

namespace conv {

namespace {

PictureConverter g_pictureConverter = nullptr;
CodePageDecoder g_decode = nullptr;
CodePageEncoder g_encode = nullptr;
PageSetup g_page;

// Windows-1252 code points 0x80..0x9F.
const uint16_t kCp1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178 };

uint32_t Be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

} // namespace

void SetPictureConverter(PictureConverter fn) { g_pictureConverter = fn; }
PictureConverter GetPictureConverter() { return g_pictureConverter; }
void SetExportPage(const PageSetup& page) { g_page = page; }
const PageSetup& GetExportPage() { return g_page; }

void SetCodePageHooks(CodePageDecoder decode, CodePageEncoder encode)
{
    g_decode = decode;
    g_encode = encode;
}

bool DecodeCodePage(unsigned codePage, const std::string& bytes, std::string& utf8)
{
    utf8.clear();
    bool ascii = true;
    for (char c : bytes) if ((unsigned char)c >= 0x80) { ascii = false; break; }
    if (ascii) { utf8 = bytes; return true; }
    if (codePage == 65001) { utf8 = bytes; return true; }
    if (g_decode && g_decode(codePage, bytes, utf8)) return true;
    // Portable fallback: Windows-1252.
    utf8.clear();
    for (char ch : bytes) {
        unsigned char c = (unsigned char)ch;
        if (c < 0x80) utf8 += ch;
        else if (c < 0xA0) AppendUtf8(utf8, kCp1252[c - 0x80]);
        else AppendUtf8(utf8, c);
    }
    return codePage == 1252 || codePage == 0;
}

bool EncodeCodePage(unsigned codePage, const std::string& utf8, std::string& bytes)
{
    bytes.clear();
    bool ascii = true;
    for (char c : utf8) if ((unsigned char)c >= 0x80) { ascii = false; break; }
    if (ascii) { bytes = utf8; return true; }
    return g_encode && g_encode(codePage, utf8, bytes);
}

bool IsEastAsian(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x11FF) ||      // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0x9FFF) ||      // CJK radicals, kana, ideographs
           (cp >= 0xA960 && cp <= 0xA97F) ||      // Hangul Jamo Extended-A
           (cp >= 0xAC00 && cp <= 0xD7FF) ||      // Hangul syllables and Jamo Extended-B
           (cp >= 0xF900 && cp <= 0xFAFF) ||      // CJK compatibility ideographs
           (cp >= 0xFE30 && cp <= 0xFE4F) ||      // CJK compatibility forms
           (cp >= 0xFF00 && cp <= 0xFFEF) ||      // full-width forms
           (cp >= 0x20000 && cp <= 0x3FFFF);      // supplementary ideographs
}

void SplitByScript(const std::string& text, const CharProps& props, const std::string& eastAsiaFont,
                   std::vector<Inline>& out)
{
    if (text.empty()) return;
    if (eastAsiaFont.empty() || eastAsiaFont == props.font) {
        Inline run;
        run.text = text;
        run.props = props;
        out.push_back(std::move(run));
        return;
    }
    size_t pos = 0, start = 0;
    int current = -1;    // 0 = other, 1 = East Asian
    auto flush = [&](size_t end) {
        if (end <= start) return;
        Inline run;
        run.text = text.substr(start, end - start);
        run.props = props;
        if (current == 1) run.props.font = eastAsiaFont;
        out.push_back(std::move(run));
        start = end;
    };
    while (pos < text.size()) {
        size_t at = pos;
        uint32_t cp = NextCodePoint(text, pos);
        // Spaces and common punctuation stay with whatever run they are in.
        bool neutral = cp == ' ' || cp == 0xA0 || (cp < 0x80 && !((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')));
        int kind = IsEastAsian(cp) ? 1 : 0;
        if (neutral && current >= 0) kind = current;
        if (current >= 0 && kind != current) flush(at);
        current = kind;
    }
    flush(text.size());
}

std::string HexColor(const Color& c)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

bool ParseHexColor(const std::string& text, Color& c)
{
    const char* p = text.c_str();
    if (*p == '#') ++p;
    if (strlen(p) != 6) return false;
    unsigned v = 0;
    for (int i = 0; i < 6; ++i) {
        char ch = p[i];
        unsigned d;
        if (ch >= '0' && ch <= '9') d = (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = (unsigned)(ch - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    c.set = true;
    c.r = (uint8_t)(v >> 16);
    c.g = (uint8_t)((v >> 8) & 0xFF);
    c.b = (uint8_t)(v & 0xFF);
    return true;
}

bool ImageSize(const Bytes& data, bool* jpeg, int* width, int* height)
{
    const uint8_t* d = data.data();
    size_t n = data.size();
    if (n >= 24 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
        if (jpeg) *jpeg = false;
        if (width) *width = (int)Be32(d + 16);
        if (height) *height = (int)Be32(d + 20);
        return true;
    }
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        // Walk the JPEG segments up to the start-of-frame marker.
        size_t i = 2;
        while (i + 9 < n) {
            if (d[i] != 0xFF) { ++i; continue; }
            uint8_t marker = d[i + 1];
            if (marker == 0xFF) { ++i; continue; }
            if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { i += 2; continue; }
            size_t len = ((size_t)d[i + 2] << 8) | d[i + 3];
            if ((marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                if (jpeg) *jpeg = true;
                if (height) *height = (d[i + 5] << 8) | d[i + 6];
                if (width) *width = (d[i + 7] << 8) | d[i + 8];
                return true;
            }
            if (len < 2) break;
            i += 2 + len;
        }
    }
    return false;
}

int AddPicture(Document& doc, const Bytes& data, PictureKind kind, int widthPx, int heightPx)
{
    Picture pic;
    int w = 0, h = 0;
    bool jpeg = false;
    if (kind == PictureKind::AnyImageFile && ImageSize(data, &jpeg, &w, &h)) {
        pic.jpeg = jpeg;
        pic.data = data;
        pic.widthPx = w;
        pic.heightPx = h;
    } else {
        if (!g_pictureConverter) return -1;
        PictureRequest request = { kind, data.data(), data.size(), widthPx, heightPx };
        PictureResult result;
        if (!g_pictureConverter(request, result) || result.png.empty()) return -1;
        pic.jpeg = false;
        pic.data.swap(result.png);
        pic.widthPx = result.widthPx;
        pic.heightPx = result.heightPx;
        if ((pic.widthPx <= 0 || pic.heightPx <= 0) && !ImageSize(pic.data, nullptr, &pic.widthPx, &pic.heightPx)) return -1;
    }
    if (pic.widthPx <= 0 || pic.heightPx <= 0) return -1;
    doc.pictures.push_back(std::move(pic));
    return (int)doc.pictures.size() - 1;
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
Container DetectContainer(const Bytes& file)
{
    ZipReader zip;
    if (!zip.Open(file)) return Container::Unknown;
    if (zip.Has("word/document.xml")) return Container::Docx;
    std::string mime;
    if (zip.ReadText("mimetype", mime) && mime.find("opendocument.text") != std::string::npos) return Container::Odt;
    if (zip.Has("content.xml") && zip.Has("META-INF/manifest.xml") && mime.empty()) return Container::Odt;
    return Container::Unknown;
}

bool DocxToRtf(const Bytes& file, std::string& rtf, std::string* error)
{
    Document doc;
    if (!DocxRead(file, doc, error)) return false;
    rtf = RtfWrite(doc);
    return true;
}

bool OdtToRtf(const Bytes& file, std::string& rtf, std::string* error)
{
    Document doc;
    if (!OdtRead(file, doc, error)) return false;
    rtf = RtfWrite(doc);
    return true;
}

bool RtfToDocx(const std::string& rtf, Bytes& file, std::string* error)
{
    Document doc;
    if (!RtfRead(rtf, doc, error)) return false;
    return DocxWrite(doc, file, error);
}

bool RtfToOdt(const std::string& rtf, Bytes& file, std::string* error)
{
    Document doc;
    if (!RtfRead(rtf, doc, error)) return false;
    return OdtWrite(doc, file, error);
}

} // namespace conv
