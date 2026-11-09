// Command line front end of the document converters, for manual testing:
//   convert_cli input.{rtf,docx,odt} output.{rtf,docx,odt,txt}
// "txt" writes an outline of the parsed document model.
#include "convert/model.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

using namespace conv;

static bool EndsWith(const std::string& s, const char* suffix)
{
    std::string suf = suffix;
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static std::string Outline(const Document& doc)
{
    std::ostringstream os;
    os << "default font: " << doc.defaultFont << " " << doc.defaultHalfPoints / 2.0 << "pt, pictures: " << doc.pictures.size() << "\n";
    auto para = [&](const Paragraph& p, const char* indent) {
        const ParaProps& pp = p.props;
        os << indent << "P align=" << pp.align << " left=" << pp.left << " first=" << pp.first << " before=" << pp.before << " after=" << pp.after
           << " line=" << pp.lineRule << ":" << pp.line;
        if (pp.list) os << " list=" << pp.list << " start=" << pp.listStart;
        os << "\n";
        for (const Inline& i : p.items) {
            os << indent << "  ";
            switch (i.kind) {
            case Inline::Text:
                os << "[" << i.props.font << " " << i.props.halfPoints / 2.0 << (i.props.bold ? " b" : "") << (i.props.italic ? " i" : "")
                   << (i.props.underline ? " u" : "") << (i.props.color.set ? " #" + HexColor(i.props.color) : "")
                   << (i.props.link.empty() ? "" : " link=" + i.props.link) << "] " << i.text << "\n";
                break;
            case Inline::Tab: os << "<tab>\n"; break;
            case Inline::LineBreak: os << "<br>\n"; break;
            case Inline::PageBreak: os << "<page>\n"; break;
            case Inline::Picture: os << "<picture " << i.widthTwips << "x" << i.heightTwips << " twips>\n"; break;
            }
        }
    };
    for (const Block& b : doc.blocks) {
        if (!b.isTable) { para(b.para, ""); continue; }
        os << "TABLE " << b.rows.size() << " rows\n";
        for (const Row& row : b.rows) {
            os << "  ROW\n";
            for (const Cell& cell : row.cells) {
                os << "    CELL " << cell.width << "\n";
                for (const Paragraph& p : cell.paras) para(p, "      ");
            }
        }
    }
    return os.str();
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: convert_cli input output\n"); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    Bytes data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    Document doc;
    std::string error;
    bool ok;
    if (data.size() > 5 && std::string(data.begin(), data.begin() + 5) == "{\\rtf") {
        ok = RtfRead(std::string(data.begin(), data.end()), doc, &error);
    } else {
        Container kind = DetectContainer(data);
        if (kind == Container::Docx) ok = DocxRead(data, doc, &error);
        else if (kind == Container::Odt) ok = OdtRead(data, doc, &error);
        else { fprintf(stderr, "unknown input format\n"); return 1; }
    }
    if (!ok) { fprintf(stderr, "read failed: %s\n", error.c_str()); return 1; }

    std::string outPath = argv[2];
    Bytes out;
    if (EndsWith(outPath, ".docx")) ok = DocxWrite(doc, out, &error);
    else if (EndsWith(outPath, ".odt")) ok = OdtWrite(doc, out, &error);
    else if (EndsWith(outPath, ".rtf")) { std::string rtf = RtfWrite(doc); out.assign(rtf.begin(), rtf.end()); }
    else { std::string text = Outline(doc); out.assign(text.begin(), text.end()); }
    if (!ok) { fprintf(stderr, "write failed: %s\n", error.c_str()); return 1; }
    std::ofstream f(outPath, std::ios::binary);
    f.write((const char*)out.data(), (std::streamsize)out.size());
    return 0;
}
