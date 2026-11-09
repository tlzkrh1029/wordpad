// Unit tests for the document converters (RTF <-> DOCX / ODT).
//
// The converter code is platform independent, so this program builds and runs natively:
//   g++ -std=c++17 -Isrc tests/convert_test.cpp src/convert/*.cpp -o convert_test && ./convert_test [output-dir]
// With an output directory the sample documents are also written to disk so they can be opened
// in other word processors.
#include "convert/model.h"
#include "convert/xml.h"
#include "convert/zip.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <iconv.h>
#endif

using namespace conv;

static int g_passed = 0, g_failed = 0;

#define CHECK(cond) \
    do { if (cond) ++g_passed; else { ++g_failed; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_EQ(a, b) \
    do { \
        auto va = (a); auto vb = (b); \
        if (va == vb) ++g_passed; \
        else { ++g_failed; std::ostringstream os; os << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #a << " == " << #b \
               << "\n  left:  " << va << "\n  right: " << vb << "\n"; printf("%s", os.str().c_str()); } \
    } while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
#ifndef _WIN32
static bool IconvConvert(const char* to, const char* from, const std::string& in, std::string& out)
{
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) return false;
    std::string buf(in.size() * 4 + 16, '\0');
    char* src = const_cast<char*>(in.data());
    size_t srcLeft = in.size();
    char* dst = &buf[0];
    size_t dstLeft = buf.size();
    size_t r = iconv(cd, &src, &srcLeft, &dst, &dstLeft);
    iconv_close(cd);
    if (r == (size_t)-1) return false;
    out.assign(buf.data(), buf.size() - dstLeft);
    return true;
}

static bool TestDecode(unsigned cp, const std::string& bytes, std::string& utf8)
{
    char name[32];
    snprintf(name, sizeof(name), "CP%u", cp);
    return IconvConvert("UTF-8", name, bytes, utf8);
}

static bool TestEncode(unsigned cp, const std::string& utf8, std::string& bytes)
{
    char name[32];
    snprintf(name, sizeof(name), "CP%u", cp);
    return IconvConvert(name, "UTF-8", utf8, bytes);
}
#endif

static uint32_t Adler32(const Bytes& data)
{
    uint32_t a = 1, b = 0;
    for (uint8_t c : data) { a = (a + c) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

static void Be32(Bytes& out, uint32_t v)
{
    out.push_back((uint8_t)(v >> 24)); out.push_back((uint8_t)(v >> 16)); out.push_back((uint8_t)(v >> 8)); out.push_back((uint8_t)v);
}

static void PngChunk(Bytes& out, const char* type, const Bytes& data)
{
    Be32(out, (uint32_t)data.size());
    Bytes crcInput(type, type + 4);
    crcInput.insert(crcInput.end(), data.begin(), data.end());
    out.insert(out.end(), crcInput.begin(), crcInput.end());
    Be32(out, Crc32(crcInput.data(), crcInput.size()));
}

// A valid RGB PNG filled with one colour.
static Bytes MakePng(int width, int height, uint8_t r, uint8_t g, uint8_t b)
{
    Bytes raw;
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < width; ++x) { raw.push_back(r); raw.push_back(g); raw.push_back(b); }
    }
    Bytes deflated;
    Deflate(raw.data(), raw.size(), deflated);
    Bytes zlib = { 0x78, 0x01 };
    zlib.insert(zlib.end(), deflated.begin(), deflated.end());
    Be32(zlib, Adler32(raw));

    Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    Bytes ihdr;
    Be32(ihdr, (uint32_t)width);
    Be32(ihdr, (uint32_t)height);
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    PngChunk(png, "IHDR", ihdr);
    PngChunk(png, "IDAT", zlib);
    PngChunk(png, "IEND", Bytes());
    return png;
}

static std::string DumpChar(const CharProps& c)
{
    std::ostringstream os;
    os << "[" << c.font << " " << c.halfPoints;
    if (c.bold) os << " b";
    if (c.italic) os << " i";
    if (c.underline) os << " u";
    if (c.strike) os << " s";
    if (c.script) os << " script" << c.script;
    if (c.color.set) os << " color=" << HexColor(c.color);
    if (c.highlight.set) os << " hl=" << HexColor(c.highlight);
    if (!c.link.empty()) os << " link=" << c.link;
    os << "]";
    return os.str();
}

static std::string DumpPara(const Paragraph& p, const Document& doc)
{
    std::ostringstream os;
    const ParaProps& pp = p.props;
    os << "P align=" << pp.align << " left=" << pp.left << " first=" << pp.first << " right=" << pp.right << " before=" << pp.before
       << " after=" << pp.after << " line=" << pp.lineRule << ":" << pp.line << " list=" << pp.list;
    if (pp.list) os << " start=" << pp.listStart;
    if (pp.pageBreakBefore) os << " pagebreak";
    for (const TabStop& t : pp.tabs) os << " tab=" << t.pos << "/" << t.align;
    os << "\n";
    for (const Inline& i : p.items) {
        switch (i.kind) {
        case Inline::Text: os << "  T " << DumpChar(i.props) << " \"" << i.text << "\"\n"; break;
        case Inline::Tab: os << "  TAB\n"; break;
        case Inline::LineBreak: os << "  BR\n"; break;
        case Inline::PageBreak: os << "  PAGE\n"; break;
        case Inline::Picture: {
            const Picture& pic = doc.pictures[(size_t)i.picture];
            os << "  PIC " << (pic.jpeg ? "jpeg " : "png ") << pic.widthPx << "x" << pic.heightPx << " " << i.widthTwips << "x" << i.heightTwips
               << "tw bytes=" << pic.data.size() << "\n";
            break;
        }
        }
    }
    return os.str();
}

static std::string Dump(const Document& doc)
{
    std::ostringstream os;
    os << "default " << doc.defaultFont << " " << doc.defaultHalfPoints << "\n";
    for (const Block& b : doc.blocks) {
        if (!b.isTable) { os << DumpPara(b.para, doc); continue; }
        os << "TABLE\n";
        for (const Row& row : b.rows) {
            os << " ROW\n";
            for (const Cell& cell : row.cells) {
                os << " CELL width=" << cell.width << "\n";
                for (const Paragraph& p : cell.paras) os << DumpPara(p, doc);
            }
        }
        os << "ENDTABLE\n";
    }
    return os.str();
}

// Compares two dumps line by line and reports only the lines that differ.
static void CheckDump(const char* what, const std::string& actual, const std::string& expected)
{
    if (actual == expected) { ++g_passed; return; }
    ++g_failed;
    printf("FAIL dump differs: %s\n", what);
    std::istringstream a(actual), e(expected);
    std::string la, le;
    int shown = 0, line = 0;
    for (;;) {
        bool ha = (bool)std::getline(a, la), he = (bool)std::getline(e, le);
        if (!ha && !he) break;
        ++line;
        if (!ha) la = "<missing>";
        if (!he) le = "<missing>";
        if (la != le && shown++ < 12) printf("  line %d\n    actual:   %s\n    expected: %s\n", line, la.c_str(), le.c_str());
    }
}

static std::string PlainText(const Document& doc)
{
    std::string out;
    for (const Block& b : doc.blocks) {
        if (b.isTable) {
            for (const Row& row : b.rows)
                for (const Cell& cell : row.cells)
                    for (const Paragraph& p : cell.paras) {
                        for (const Inline& i : p.items) if (i.kind == Inline::Text) out += i.text;
                        out += "|";
                    }
            out += "\n";
            continue;
        }
        for (const Inline& i : b.para.items) {
            if (i.kind == Inline::Text) out += i.text;
            else if (i.kind == Inline::Tab) out += "\t";
            else if (i.kind == Inline::Picture) out += "<pic>";
        }
        out += "\n";
    }
    return out;
}

static Inline Text(const std::string& text, const CharProps& props)
{
    Inline i;
    i.text = text;
    i.props = props;
    return i;
}

static Document MakeSample()
{
    Document doc;
    doc.defaultFont = "Calibri";
    doc.defaultHalfPoints = 22;
    CharProps base;
    base.font = "Calibri";
    base.halfPoints = 22;

    auto addPara = [&](const ParaProps& pp, std::vector<Inline> items) {
        Block b;
        b.para.props = pp;
        b.para.items = std::move(items);
        doc.blocks.push_back(std::move(b));
    };

    ParaProps normal;
    normal.after = 200;
    normal.line = 276;

    CharProps title = base;
    title.halfPoints = 40;
    title.bold = true;
    title.color.set = true; title.color.r = 0x25; title.color.g = 0x63; title.color.b = 0xEB;
    ParaProps centered = normal;
    centered.align = 1;
    addPara(centered, { Text("Sample \xEB\xAC\xB8\xEC\x84\x9C & <title>", title) });

    CharProps bold = base; bold.bold = true;
    CharProps italic = base; italic.italic = true;
    CharProps under = base; under.underline = true;
    CharProps strike = base; strike.strike = true;
    CharProps sup = base; sup.script = 1;
    CharProps sub = base; sub.script = 2;
    CharProps hl = base; hl.highlight.set = true; hl.highlight.r = 255; hl.highlight.g = 255; hl.highlight.b = 0;
    CharProps custom = base; custom.highlight.set = true; custom.highlight.r = 0x12; custom.highlight.g = 0xAB; custom.highlight.b = 0xCD;
    CharProps korean = base; korean.font = "\xEB\xA7\x91\xEC\x9D\x80 \xEA\xB3\xA0\xEB\x94\x95";   // 맑은 고딕
    addPara(normal, { Text("Plain ", base), Text("bold", bold), Text(" ", base), Text("italic", italic), Text(" ", base),
                      Text("underline", under), Text(" ", base), Text("strike", strike), Text(" x", base), Text("2", sup),
                      Text(" H", base), Text("2", sub), Text("O ", base), Text("yellow", hl), Text(" ", base),
                      Text("custom", custom), Text(" ", base), Text("\xED\x95\x9C\xEA\xB8\x80 \xED\x85\x8D\xEC\x8A\xA4\xED\x8A\xB8", korean) });

    ParaProps bullets = normal;
    bullets.list = 1;
    bullets.left = 720;
    bullets.first = -360;
    addPara(bullets, { Text("First bullet", base) });
    addPara(bullets, { Text("Second bullet", base) });

    ParaProps numbers = bullets;
    numbers.list = 2;
    addPara(numbers, { Text("Number one", base) });
    addPara(numbers, { Text("Number two", base) });
    addPara(numbers, { Text("Number three", base) });

    ParaProps indented = normal;
    indented.left = 1440;
    indented.right = 720;
    indented.first = 360;
    indented.align = 3;
    indented.before = 120;
    indented.lineRule = 0;
    indented.line = 480;
    addPara(indented, { Text("Justified, double spaced and indented paragraph with  two spaces and a trailing space ", base) });

    ParaProps tabs = normal;
    tabs.tabs.push_back({ 2880, 0 });
    tabs.tabs.push_back({ 5760, 2 });
    Inline tab;
    tab.kind = Inline::Tab;
    tab.props = base;
    Inline br;
    br.kind = Inline::LineBreak;
    br.props = base;
    addPara(tabs, { Text("Name", base), tab, Text("Value", base), tab, Text("Right", base), br, Text("second line", base) });

    CharProps link = base;
    link.link = "https://example.com/?a=1&b=2";
    link.underline = true;
    link.color.set = true; link.color.b = 255;
    addPara(normal, { Text("Visit ", base), Text("the example site", link), Text(" today.", base) });

    Picture pic;
    pic.data = MakePng(40, 20, 37, 99, 235);
    pic.widthPx = 40;
    pic.heightPx = 20;
    doc.pictures.push_back(pic);
    Inline image;
    image.kind = Inline::Picture;
    image.picture = 0;
    image.props = base;
    image.widthTwips = 1200;
    image.heightTwips = 600;
    addPara(normal, { Text("Picture: ", base), image });

    Block table;
    table.isTable = true;
    for (int r = 0; r < 2; ++r) {
        Row row;
        for (int c = 0; c < 3; ++c) {
            Cell cell;
            cell.width = c == 0 ? 1440 : 2880;
            Paragraph p;
            p.items.push_back(Text("r" + std::to_string(r + 1) + "c" + std::to_string(c + 1), r == 0 ? bold : base));
            cell.paras.push_back(p);
            row.cells.push_back(cell);
        }
        table.rows.push_back(row);
    }
    doc.blocks.push_back(table);

    addPara(normal, {});                      // empty paragraph
    ParaProps right = normal;
    right.align = 2;
    addPara(right, { Text("The end \xF0\x9F\x98\x80", base) });     // emoji outside the BMP
    return doc;
}

static void WriteFile(const std::string& path, const void* data, size_t size)
{
    std::ofstream f(path, std::ios::binary);
    f.write((const char*)data, (std::streamsize)size);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static void TestZip(const std::string& outDir)
{
    CHECK_EQ(Crc32((const uint8_t*)"123456789", 9), 0xCBF43926u);

    std::vector<std::string> inputs = { "", "a", "hello hello hello hello hello hello", std::string(100000, 'x') };
    std::string mixed;
    uint32_t seed = 12345;
    for (int i = 0; i < 200000; ++i) {
        seed = seed * 1103515245u + 12345u;
        mixed += (char)((seed >> 16) % 7 == 0 ? 'A' + (seed >> 8) % 26 : "the quick brown fox "[i % 20]);
    }
    inputs.push_back(mixed);
    std::string binary;
    for (int i = 0; i < 70000; ++i) { seed = seed * 1103515245u + 12345u; binary += (char)(seed >> 16); }
    inputs.push_back(binary);
    for (const std::string& in : inputs) {
        Bytes packed, unpacked;
        Deflate((const uint8_t*)in.data(), in.size(), packed);
        CHECK(Inflate(packed.data(), packed.size(), unpacked, 1u << 30));
        CHECK(std::string(unpacked.begin(), unpacked.end()) == in);
    }
    {
        Bytes packed;
        Deflate((const uint8_t*)mixed.data(), mixed.size(), packed);
        CHECK(packed.size() < mixed.size() / 2);
        Bytes out;
        CHECK(!Inflate(packed.data(), packed.size() / 2, out, 1u << 30));      // truncated input
        CHECK(!Inflate(packed.data(), packed.size(), out, 100));              // output limit
    }

    ZipWriter writer;
    writer.Add("mimetype", std::string("application/test"), false);
    writer.Add("dir/text.txt", mixed);
    writer.Add("empty.bin", std::string());
    writer.Add("\xED\x95\x9C\xEA\xB8\x80.txt", std::string("korean name"));
    Bytes zip = writer.Finish();
    ZipReader reader;
    CHECK(reader.Open(zip));
    CHECK(reader.Has("dir/text.txt") && reader.Has("mimetype") && !reader.Has("missing"));
    std::string text;
    CHECK(reader.ReadText("dir/text.txt", text) && text == mixed);
    CHECK(reader.ReadText("mimetype", text) && text == "application/test");
    CHECK(reader.ReadText("empty.bin", text) && text.empty());
    CHECK_EQ(reader.Names().size(), (size_t)4);
    // The mimetype entry must be stored right at the start (ODF requirement).
    CHECK(memcmp(zip.data() + 30, "mimetypeapplication/test", 24) == 0);
    if (!outDir.empty()) WriteFile(outDir + "/test.zip", zip.data(), zip.size());

    Bytes garbage(100, 0x41);
    ZipReader bad;
    CHECK(!bad.Open(garbage));
}

static void TestXml()
{
    const char* xml =
        "<?xml version=\"1.0\"?>\n<!-- comment -->\n"
        "<x:document xmlns:x=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" xmlns:rr=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<x:body><x:p x:val='a &amp; b' rr:id=\"rId1\" plain=\"1\"><x:t xml:space=\"preserve\"> A &lt;&#65;&#x42;&gt; <![CDATA[<raw>]]></x:t><x:br/></x:p>"
        "<other:thing xmlns:other=\"urn:unknown\" other:attr=\"v\"/></x:body></x:document>";
    XmlNode root;
    std::string error;
    CHECK(XmlParse(xml, root, &error));
    CHECK_EQ(root.name, std::string("w:document"));
    const XmlNode* body = root.Child("w:body");
    CHECK(body != nullptr);
    if (body) {
        const XmlNode* p = body->Child("w:p");
        CHECK(p != nullptr);
        if (p) {
            CHECK_EQ(p->AttrOr("w:val"), std::string("a & b"));
            CHECK_EQ(p->AttrOr("r:id"), std::string("rId1"));
            CHECK_EQ(p->AttrOr("plain"), std::string("1"));
            const XmlNode* t = p->Child("w:t");
            CHECK(t && t->InnerText() == " A <AB> <raw>");
            CHECK(p->Child("w:br") != nullptr);
        }
        CHECK(body->Child("other:thing") != nullptr);
    }
    XmlNode bad;
    CHECK(!XmlParse("<a><b></a>", bad) || true);     // lenient parser: must not crash
    CHECK(!XmlParse("<a", bad));
    CHECK(!XmlParse("", bad));
    CHECK_EQ(XmlEscape("a<b>&\"c\"", true), std::string("a&lt;b&gt;&amp;&quot;c&quot;"));

    // A default namespace is mapped to its conventional prefix as well.
    XmlNode rels;
    CHECK(XmlParse("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"a\" Target=\"b\"/></Relationships>", rels));
    CHECK_EQ(rels.name, std::string("rel:Relationships"));
    CHECK(rels.children.size() == 1 && rels.children[0].AttrOr("Target") == "b");
}

static void TestRtfRead()
{
    // Shaped like the output of the Windows rich edit control (Korean system).
    const char* rtf =
        "{\\rtf1\\ansi\\ansicpg949\\deff0\\nouicompat\\deflang1033\\deflangfe1042{\\fonttbl{\\f0\\fnil\\fcharset129 \\'b8\\'bc\\'c0\\'ba \\'b0\\'ed\\'b5\\'f1;}"
        "{\\f1\\fnil\\fcharset0 Calibri;}{\\f2\\fnil\\fcharset2 Symbol;}}\r\n"
        "{\\colortbl ;\\red255\\green0\\blue0;\\red255\\green255\\blue0;}\r\n"
        "{\\*\\generator Riched20 10.0.26100}\\viewkind4\\uc1 \r\n"
        "\\pard\\sa200\\sl276\\slmult1\\b\\f0\\fs22\\lang18 \\'c7\\'d1\\'b1\\'db\\b0  text \\cf1 red\\cf0  \\highlight2 hl\\highlight0\\par\r\n"
        "\\pard{\\pntext\\f2\\'B7\\tab}{\\*\\pn\\pnlvlblt\\pnf2\\pnindent0{\\pntxtb\\'B7}}\\fi-360\\li720\\sa200\\sl276\\slmult1 item one\\par\r\n"
        "{\\pntext\\f2\\'B7\\tab}item two\\par\r\n"
        "\\pard{\\pntext\\f0 3.\\tab}{\\*\\pn\\pnlvlbody\\pnf0\\pnindent0\\pnstart3\\pndec{\\pntxta.}}\\fi-360\\li720 third\\par\r\n"
        "\\pard\\sa200\\sl276\\slmult1\\qc\\f1 centered \\u8364?\\~x\\{\\}\\\\ \\u-10179?\\u-8704?\\par\r\n"
        "\\pard\\tx1440\\tqr\\tx4320 a\\tab b\\line c{\\field{\\*\\fldinst{HYPERLINK \"http://example.com\"}}{\\fldrslt{\\ul\\cf1 link}}}\\par\r\n"
        "\\trowd\\trgaph108\\cellx2000\\cellx5000\\pard\\intbl one\\cell two\\par more\\cell\\row\r\n"
        "\\pard after\\par\r\n"
        "}\r\n";
    Document doc;
    std::string error;
    CHECK(RtfRead(rtf, doc, &error));
#ifndef _WIN32
    CHECK_EQ(doc.defaultFont, std::string("\xEB\xA7\x91\xEC\x9D\x80 \xEA\xB3\xA0\xEB\x94\x95"));
#endif
    CHECK_EQ(doc.blocks.size(), (size_t)8);
    if (doc.blocks.size() != 8) { printf("%s\n", Dump(doc).c_str()); return; }

    const Paragraph& p1 = doc.blocks[0].para;
    CHECK_EQ(p1.props.after, 200);
    CHECK(p1.props.lineRule == 0 && p1.props.line == 276);
    CHECK_EQ(p1.items.size(), (size_t)5);
    if (p1.items.size() == 5) {
#ifndef _WIN32
        CHECK_EQ(p1.items[0].text, std::string("\xED\x95\x9C\xEA\xB8\x80"));
#endif
        CHECK(p1.items[0].props.bold && p1.items[0].props.halfPoints == 22);
        CHECK(!p1.items[1].props.bold && p1.items[1].text == " text ");
        CHECK(p1.items[2].props.color.set && p1.items[2].props.color.r == 255 && p1.items[2].text == "red");
        CHECK(!p1.items[3].props.color.set);
        CHECK(p1.items[4].props.highlight.set && p1.items[4].props.highlight.g == 255 && p1.items[4].text == "hl");
    }
    const Paragraph& p2 = doc.blocks[1].para;
    CHECK(p2.props.list == 1 && p2.props.left == 720 && p2.props.first == -360);
    CHECK(p2.items.size() == 1 && p2.items[0].text == "item one");
    const Paragraph& p3 = doc.blocks[2].para;
    CHECK(p3.props.list == 1 && p3.items.size() == 1 && p3.items[0].text == "item two");
    const Paragraph& p4 = doc.blocks[3].para;
    CHECK(p4.props.list == 2 && p4.props.listStart == 3 && p4.props.after == 0);
    const Paragraph& p5 = doc.blocks[4].para;
    CHECK_EQ(p5.props.align, 1);
    CHECK(p5.props.list == 0);
    CHECK(p5.items.size() == 1 && p5.items[0].props.font == "Calibri");
    if (p5.items.size() == 1)
        CHECK_EQ(p5.items[0].text, std::string("centered \xE2\x82\xAC\xC2\xA0x{}\\ \xF0\x9F\x98\x80"));
    const Paragraph& p6 = doc.blocks[5].para;
    CHECK(p6.props.tabs.size() == 2 && p6.props.tabs[0].pos == 1440 && p6.props.tabs[1].pos == 4320 && p6.props.tabs[1].align == 2);
    CHECK_EQ(p6.items.size(), (size_t)6);
    if (p6.items.size() == 6) {
        CHECK(p6.items[1].kind == Inline::Tab && p6.items[3].kind == Inline::LineBreak);
        CHECK(p6.items[5].props.link == "http://example.com" && p6.items[5].text == "link" && p6.items[5].props.underline);
        CHECK(p6.items[4].props.link.empty());
    }
    CHECK(doc.blocks[6].isTable);
    if (doc.blocks[6].isTable) {
        const std::vector<Row>& rows = doc.blocks[6].rows;
        CHECK(rows.size() == 1 && rows[0].cells.size() == 2);
        if (rows.size() == 1 && rows[0].cells.size() == 2) {
            CHECK(rows[0].cells[0].width == 2000 && rows[0].cells[1].width == 3000);
            CHECK(rows[0].cells[0].paras.size() == 1 && rows[0].cells[1].paras.size() == 2);
        }
    }
    CHECK(!doc.blocks[7].isTable && doc.blocks[7].para.items.size() == 1 && doc.blocks[7].para.items[0].text == "after");

    // Text after the closing brace (some writers append a NUL) does not become content.
    Document trailing;
    CHECK(RtfRead(std::string("{\\rtf1 one\\par}\0 junk", 22), trailing, nullptr));
    CHECK_EQ(trailing.blocks.size(), (size_t)1);

    Document bad;
    CHECK(!RtfRead("not rtf", bad, nullptr));
    // Truncated input must not crash.
    std::string truncated(rtf);
    for (size_t cut = 0; cut < truncated.size(); cut += 37) {
        Document d;
        RtfRead(truncated.substr(0, cut), d, nullptr);
    }
    ++g_passed;
}

static void TestRoundTrips(const std::string& outDir)
{
    Document sample = MakeSample();
    std::string expected = Dump(sample);

    // RTF
    std::string rtf = RtfWrite(sample);
    Document fromRtf;
    CHECK(RtfRead(rtf, fromRtf, nullptr));
    CHECK_EQ(fromRtf.defaultHalfPoints, 0);          // RTF carries no default size
    fromRtf.defaultHalfPoints = sample.defaultHalfPoints;
    CheckDump("RTF round trip", Dump(fromRtf), expected);
    if (!outDir.empty()) WriteFile(outDir + "/sample.rtf", rtf.data(), rtf.size());

    // DOCX
    Bytes docx;
    CHECK(DocxWrite(sample, docx, nullptr));
    CHECK_EQ((int)DetectContainer(docx), (int)Container::Docx);
    Document fromDocx;
    std::string error;
    CHECK(DocxRead(docx, fromDocx, &error));
    CheckDump("DOCX round trip", Dump(fromDocx), expected);
    if (!outDir.empty()) WriteFile(outDir + "/sample.docx", docx.data(), docx.size());

    // ODT
    Bytes odt;
    CHECK(OdtWrite(sample, odt, nullptr));
    CHECK_EQ((int)DetectContainer(odt), (int)Container::Odt);
    Document fromOdt;
    CHECK(OdtRead(odt, fromOdt, &error));
    // ODT has no inline page breaks and merges nothing else, so the dump must match as well.
    CheckDump("ODT round trip", Dump(fromOdt), expected);
    if (!outDir.empty()) WriteFile(outDir + "/sample.odt", odt.data(), odt.size());

    // The public one-step entry points.
    Bytes viaApi;
    CHECK(RtfToDocx(rtf, viaApi, nullptr));
    std::string back;
    CHECK(DocxToRtf(viaApi, back, nullptr));
    Document again;
    CHECK(RtfRead(back, again, nullptr));
    again.defaultHalfPoints = sample.defaultHalfPoints;
    CheckDump("RTF -> DOCX -> RTF", Dump(again), expected);
    CHECK(RtfToOdt(rtf, viaApi, nullptr) && OdtToRtf(viaApi, back, nullptr));

    // Corrupt packages are rejected without crashing.
    Bytes broken = docx;
    for (size_t i = 100; i < broken.size(); i += 97) broken[i] ^= 0x5A;
    Document junk;
    DocxRead(broken, junk, nullptr);
    Bytes truncated(docx.begin(), docx.begin() + (ptrdiff_t)(docx.size() / 2));
    CHECK(!DocxRead(truncated, junk, nullptr));
    CHECK_EQ((int)DetectContainer(truncated), (int)Container::Unknown);
}

// Documents written by other applications: only readable properties are checked.
static void TestForeignDocx()
{
    // Styles, theme fonts, numbering through a paragraph style, nested structure, text boxes.
    ZipWriter zip;
    zip.Add("[Content_Types].xml", std::string("<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"/>"));
    zip.Add("_rels/.rels", std::string(
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
        "</Relationships>"));
    zip.Add("word/_rels/document.xml.rels", std::string(
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId5\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" Target=\"https://example.org/\" TargetMode=\"External\"/>"
        "<Relationship Id=\"rId6\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/image1.png\"/>"
        "</Relationships>"));
    Bytes png = MakePng(8, 4, 1, 2, 3);
    zip.Add("word/media/image1.png", png, false);
    zip.Add("word/theme/theme1.xml", std::string(
        "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:themeElements><a:fontScheme name=\"Office\">"
        "<a:majorFont><a:latin typeface=\"Calibri Light\"/><a:ea typeface=\"\"/></a:majorFont>"
        "<a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:font script=\"Hang\" typeface=\"Malgun Gothic\"/></a:minorFont>"
        "</a:fontScheme></a:themeElements></a:theme>"));
    zip.Add("word/styles.xml", std::string(
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:asciiTheme=\"minorHAnsi\" w:eastAsiaTheme=\"minorEastAsia\" w:hAnsiTheme=\"minorHAnsi\"/>"
        "<w:sz w:val=\"22\"/><w:lang w:val=\"en-US\" w:eastAsia=\"ko-KR\"/></w:rPr></w:rPrDefault>"
        "<w:pPrDefault><w:pPr><w:spacing w:after=\"160\" w:line=\"259\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:spacing w:before=\"240\" w:after=\"0\"/></w:pPr><w:rPr><w:rFonts w:asciiTheme=\"majorHAnsi\"/><w:color w:val=\"2F5496\"/><w:sz w:val=\"32\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:basedOn w:val=\"Normal\"/><w:pPr><w:ind w:left=\"720\"/></w:pPr></w:style>"
        "<w:style w:type=\"character\" w:styleId=\"Strong\"><w:rPr><w:b/></w:rPr></w:style>"
        "</w:styles>"));
    zip.Add("word/numbering.xml", std::string(
        "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr></w:lvl>"
        "<w:lvl w:ilvl=\"1\"><w:start w:val=\"1\"/><w:numFmt w:val=\"lowerLetter\"/><w:pPr><w:ind w:left=\"1440\" w:hanging=\"360\"/></w:pPr></w:lvl></w:abstractNum>"
        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num></w:numbering>"));
    zip.Add("word/document.xml", std::string(
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><w:body>"
        "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/></w:pPr><w:r><w:t>Title</w:t></w:r></w:p>"
        "<w:p><w:r><w:t xml:space=\"preserve\">Mixed \xED\x95\x9C\xEA\xB8\x80 text </w:t></w:r><w:r><w:rPr><w:rStyle w:val=\"Strong\"/></w:rPr><w:t>strong</w:t></w:r>"
        "<w:hyperlink r:id=\"rId5\"><w:r><w:t>link</w:t></w:r></w:hyperlink><w:del><w:r><w:delText>gone</w:delText></w:r></w:del>"
        "<w:r><w:rPr><w:vanish/></w:rPr><w:t>hidden</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>one</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>two</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/><w:numPr><w:ilvl w:val=\"1\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>nested</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>between</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>three</w:t></w:r></w:p>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"914400\" cy=\"457200\"/><a:graphic><a:graphicData><pic:pic><pic:blipFill><a:blip r:embed=\"rId6\"/></pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>"
        "<w:tbl><w:tblGrid><w:gridCol w:w=\"3000\"/><w:gridCol w:w=\"2000\"/></w:tblGrid>"
        "<w:tr><w:tc><w:p><w:r><w:t>A</w:t></w:r></w:p></w:tc><w:tc><w:tcPr><w:tcW w:w=\"2500\" w:type=\"dxa\"/></w:tcPr><w:p><w:r><w:t>B</w:t></w:r></w:p><w:p><w:r><w:t>B2</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
        "<w:p><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText> PAGE </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>"
        "<w:r><w:t>7</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r><w:r><w:tab/><w:t>tail</w:t><w:br w:type=\"page\"/></w:r></w:p>"
        "<w:sectPr/></w:body></w:document>"));
    Bytes file = zip.Finish();

    Document doc;
    std::string error;
    CHECK(DocxRead(file, doc, &error));
    CHECK_EQ(doc.defaultFont, std::string("Calibri"));
    CHECK_EQ(doc.defaultHalfPoints, 22);
    CHECK_EQ(doc.blocks.size(), (size_t)10);
    if (doc.blocks.size() != 10) { printf("%s\n", Dump(doc).c_str()); return; }

    const Paragraph& title = doc.blocks[0].para;
    CHECK(title.props.before == 240 && title.props.after == 0 && title.props.line == 259);
    CHECK(title.items.size() == 1 && title.items[0].props.font == "Calibri Light" && title.items[0].props.halfPoints == 32 &&
          title.items[0].props.color.set && title.items[0].props.color.r == 0x2F);

    const Paragraph& mixed = doc.blocks[1].para;
    CHECK(mixed.props.after == 160);
    // "Mixed " | "한글 " (East Asian font) | "text " | "strong" (bold) | "link"
    CHECK_EQ(mixed.items.size(), (size_t)5);
    if (mixed.items.size() == 5) {
        CHECK(mixed.items[0].text == "Mixed " && mixed.items[0].props.font == "Calibri");
        CHECK(mixed.items[1].props.font == "Malgun Gothic");
        CHECK(mixed.items[2].text == "text " && mixed.items[2].props.font == "Calibri");
        CHECK(mixed.items[3].text == "strong" && mixed.items[3].props.bold);
        CHECK(mixed.items[4].text == "link" && mixed.items[4].props.link == "https://example.org/");
    }
    CHECK(doc.blocks[2].para.props.list == 2 && doc.blocks[2].para.props.listStart == 1 && doc.blocks[2].para.props.left == 720 &&
          doc.blocks[2].para.props.first == -360);
    CHECK(doc.blocks[3].para.props.list == 2 && doc.blocks[3].para.props.listStart == 1);
    CHECK(doc.blocks[4].para.props.list == 3 && doc.blocks[4].para.props.left == 1440);
    CHECK(doc.blocks[5].para.props.list == 0);
    // The list continues at 3 after the interruption.
    CHECK(doc.blocks[6].para.props.list == 2 && doc.blocks[6].para.props.listStart == 3);
    const Paragraph& picture = doc.blocks[7].para;
    CHECK(picture.items.size() == 1 && picture.items[0].kind == Inline::Picture && picture.items[0].widthTwips == 1440 &&
          picture.items[0].heightTwips == 720);
    CHECK(doc.pictures.size() == 1 && doc.pictures[0].widthPx == 8 && doc.pictures[0].heightPx == 4);
    CHECK(doc.blocks[8].isTable && doc.blocks[8].rows.size() == 1 && doc.blocks[8].rows[0].cells.size() == 2);
    if (doc.blocks[8].isTable && doc.blocks[8].rows.size() == 1 && doc.blocks[8].rows[0].cells.size() == 2) {
        CHECK(doc.blocks[8].rows[0].cells[0].width == 3000 && doc.blocks[8].rows[0].cells[1].width == 2500);
        CHECK(doc.blocks[8].rows[0].cells[1].paras.size() == 2);
    }
    const Paragraph& field = doc.blocks[9].para;
    CHECK_EQ(PlainText(doc).substr(PlainText(doc).rfind("7")), std::string("7\ttail\n"));
    CHECK(!field.items.empty() && field.items.back().kind == Inline::PageBreak);

    // And the whole thing converts to RTF that reads back with the same text.
    std::string rtf = RtfWrite(doc);
    Document again;
    CHECK(RtfRead(rtf, again, nullptr));
    CHECK_EQ(PlainText(again), PlainText(doc));
}

static void TestForeignOdt()
{
    ZipWriter zip;
    zip.Add("mimetype", std::string("application/vnd.oasis.opendocument.text"), false);
    Bytes png = MakePng(10, 10, 9, 9, 9);
    zip.Add("Pictures/pic.png", png, false);
    const char* ns =
        " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
        " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\""
        " xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\""
        " xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\"";
    zip.Add("styles.xml", std::string("<office:document-styles") + ns + ">"
        "<office:font-face-decls><style:font-face style:name=\"Liberation Sans\" svg:font-family=\"&apos;Liberation Sans&apos;\"/></office:font-face-decls>"
        "<office:styles><style:default-style style:family=\"paragraph\"><style:text-properties style:font-name=\"Liberation Sans\" fo:font-size=\"11pt\"/></style:default-style>"
        "<style:style style:name=\"Standard\" style:family=\"paragraph\"/>"
        "<style:style style:name=\"Heading_20_1\" style:family=\"paragraph\" style:parent-style-name=\"Standard\">"
        "<style:paragraph-properties fo:margin-top=\"0.1665in\" fo:margin-bottom=\"0.0835in\"/><style:text-properties fo:font-size=\"18pt\" fo:font-weight=\"bold\"/></style:style>"
        "</office:styles></office:document-styles>");
    zip.Add("content.xml", std::string("<office:document-content") + ns + ">"
        "<office:automatic-styles>"
        "<style:style style:name=\"P1\" style:family=\"paragraph\" style:parent-style-name=\"Standard\"><style:paragraph-properties fo:text-align=\"center\" fo:margin-left=\"1cm\" fo:line-height=\"150%\"/></style:style>"
        "<style:style style:name=\"T1\" style:family=\"text\"><style:text-properties fo:font-style=\"italic\" fo:color=\"#ff0000\" style:text-underline-style=\"solid\"/></style:style>"
        "<text:list-style style:name=\"L1\"><text:list-level-style-number text:level=\"1\" style:num-format=\"I\" text:start-value=\"4\">"
        "<style:list-level-properties><style:list-level-label-alignment fo:margin-left=\"0.5in\" fo:text-indent=\"-0.25in\"/></style:list-level-properties>"
        "</text:list-level-style-number><text:list-level-style-bullet text:level=\"2\" text:bullet-char=\"-\"/></text:list-style>"
        "<style:style style:name=\"Tbl.A\" style:family=\"table-column\"><style:table-column-properties style:column-width=\"2in\"/></style:style>"
        "</office:automatic-styles><office:body><office:text>"
        "<text:h text:style-name=\"Heading_20_1\" text:outline-level=\"1\">Heading</text:h>\n"
        "<text:p text:style-name=\"P1\">  Centered   <text:span text:style-name=\"T1\">red italic</text:span><text:s text:c=\"2\"/>end<text:tab/>x<text:line-break/>y</text:p>"
        "<text:list text:style-name=\"L1\"><text:list-item><text:p>four</text:p></text:list-item>"
        "<text:list-item><text:p>five</text:p><text:list><text:list-item><text:p>sub</text:p></text:list-item></text:list></text:list-item></text:list>"
        "<text:p><text:a xlink:href=\"https://example.net\">a link</text:a><draw:frame svg:width=\"1in\" svg:height=\"0.5in\"><draw:image xlink:href=\"Pictures/pic.png\"/></draw:frame></text:p>"
        "<table:table><table:table-column table:style-name=\"Tbl.A\" table:number-columns-repeated=\"2\"/>"
        "<table:table-row><table:table-cell><text:p>c1</text:p></table:table-cell><table:table-cell><text:p>c2</text:p></table:table-cell></table:table-row></table:table>"
        "</office:text></office:body></office:document-content>");
    Bytes file = zip.Finish();

    Document doc;
    std::string error;
    CHECK(OdtRead(file, doc, &error));
    CHECK_EQ(doc.defaultFont, std::string("Liberation Sans"));
    CHECK_EQ(doc.defaultHalfPoints, 22);
    CHECK_EQ(doc.blocks.size(), (size_t)7);
    if (doc.blocks.size() != 7) { printf("%s\n", Dump(doc).c_str()); return; }
    const Paragraph& h = doc.blocks[0].para;
    CHECK(h.items.size() == 1 && h.items[0].props.bold && h.items[0].props.halfPoints == 36 && h.props.before == 240 && h.props.after == 120);
    const Paragraph& p = doc.blocks[1].para;
    CHECK(p.props.align == 1 && p.props.left == 567 && p.props.line == 360);
    CHECK_EQ(p.items.size(), (size_t)7);
    if (p.items.size() == 7) {
        CHECK_EQ(p.items[0].text, std::string("Centered "));
        CHECK(p.items[1].text == "red italic" && p.items[1].props.italic && p.items[1].props.underline && p.items[1].props.color.r == 255);
        CHECK_EQ(p.items[2].text, std::string("  end"));
        CHECK(p.items[3].kind == Inline::Tab && p.items[5].kind == Inline::LineBreak && p.items[6].text == "y");
    }
    CHECK(doc.blocks[2].para.props.list == 6 && doc.blocks[2].para.props.listStart == 4 && doc.blocks[2].para.props.left == 720 &&
          doc.blocks[2].para.props.first == -360);
    CHECK(doc.blocks[3].para.props.list == 6 && doc.blocks[3].para.props.listStart == 4);
    CHECK(doc.blocks[4].para.props.list == 1);
    const Paragraph& link = doc.blocks[5].para;
    CHECK(link.items.size() == 2 && link.items[0].props.link == "https://example.net" && link.items[1].kind == Inline::Picture &&
          link.items[1].widthTwips == 1440 && link.items[1].heightTwips == 720);
    CHECK(doc.blocks[6].isTable && doc.blocks[6].rows.size() == 1 && doc.blocks[6].rows[0].cells.size() == 2 &&
          doc.blocks[6].rows[0].cells[0].width == 2880);
}

// Damaged or hostile files must be rejected or tamed, never crash or overflow.
static void TestHostileInput()
{
    // Absurd numbers in RTF: saturated while parsing, clamped in the model.
    Document doc;
    std::string error;
    CHECK(RtfRead("{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Arial;}}\\pard\\li99999999999999999999\\fi-99999999999\\sb2147483647"
                  "\\sl999999999\\slmult1\\tx4000000000\\fs99999999 Text\\par}", doc, &error));
    CHECK_EQ(doc.blocks.size(), (size_t)1);
    if (!doc.blocks.empty()) {
        const ParaProps& pp = doc.blocks[0].para.props;
        CHECK(pp.left <= kMaxTwips && pp.left >= -kMaxTwips);
        CHECK(pp.first <= kMaxTwips && pp.first >= -kMaxTwips);
        CHECK(pp.before >= 0 && pp.before <= kMaxTwips);
        CHECK(pp.line >= 0 && pp.line <= kMaxTwips);
        for (const TabStop& t : pp.tabs) CHECK(t.pos >= 0 && t.pos <= kMaxTwips);
        if (!doc.blocks[0].para.items.empty()) CHECK(doc.blocks[0].para.items[0].props.halfPoints <= 3276);
    }
    // Every writer copes with the result.
    Bytes file;
    CHECK(DocxWrite(doc, file, &error));
    CHECK(OdtWrite(doc, file, &error));
    CHECK(!RtfWrite(doc).empty());

    // A picture whose header claims a gigantic size.
    Bytes png = MakePng(4, 4, 9, 9, 9);
    png[16] = 0x7F; png[17] = 0xFF; png[18] = 0xFF; png[19] = 0xFF;     // width in the IHDR chunk
    Document pictures;
    int index = AddPicture(pictures, png, PictureKind::AnyImageFile, 0, 0);
    if (index >= 0) CHECK(pictures.pictures[(size_t)index].widthPx <= 100000);

    // Unbalanced and very deep nesting.
    std::string deep = "{\\rtf1 ";
    for (int i = 0; i < 100000; ++i) deep += "{\\b ";
    CHECK(!RtfRead(deep, doc, &error));
    std::string xml = "<a>";
    for (int i = 0; i < 100000; ++i) xml += "<b>";
    XmlNode root;
    CHECK(!XmlParse(xml, root, &error));
    xml = "<a>";
    for (int i = 0; i < 100; ++i) xml += "<b>";
    for (int i = 0; i < 100; ++i) xml += "</b>";
    xml += "</a>";
    CHECK(XmlParse(xml, root, &error));

    // Saturating number conversion.
    CHECK_EQ(RoundToInt(1e300), 1000000000);
    CHECK_EQ(RoundToInt(-1e300), -1000000000);
    CHECK_EQ(RoundToInt(2.5), 3);
    CHECK_EQ(RoundToInt(-2.5), -3);
    double zero = 0.0;
    CHECK_EQ(RoundToInt(zero / zero), 0);

    // A DOCX with absurd values in the main part.
    ZipWriter zip;
    zip.Add("[Content_Types].xml", std::string("<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"/>"));
    zip.Add("word/document.xml", std::string(
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>"
        "<w:p><w:pPr><w:ind w:left=\"99999999999999999999\" w:hanging=\"1e309\"/><w:spacing w:before=\"-5\" w:line=\"4294967296\"/>"
        "<w:tabs><w:tab w:val=\"left\" w:pos=\"NaN\"/></w:tabs></w:pPr>"
        "<w:r><w:rPr><w:sz w:val=\"2147483647\"/></w:rPr><w:t>Big</w:t></w:r></w:p></w:body></w:document>"));
    Bytes docx = zip.Finish();
    Document hostile;
    CHECK(DocxRead(docx, hostile, &error));
    if (!hostile.blocks.empty()) {
        const ParaProps& pp = hostile.blocks[0].para.props;
        CHECK(pp.left <= kMaxTwips && pp.left >= -kMaxTwips && pp.first <= kMaxTwips && pp.first >= -kMaxTwips);
        CHECK(pp.before >= 0 && pp.line >= 0 && pp.line <= kMaxTwips);
        CHECK(!RtfWrite(hostile).empty());
        CHECK(OdtWrite(hostile, file, &error));
    }
}

int main(int argc, char** argv)
{
    std::string outDir = argc > 1 ? argv[1] : "";
#ifndef _WIN32
    SetCodePageHooks(TestDecode, TestEncode);
#endif
    TestZip(outDir);
    TestXml();
    TestRtfRead();
    TestRoundTrips(outDir);
    TestForeignDocx();
    TestForeignOdt();
    TestHostileInput();
    printf("%d checks passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
