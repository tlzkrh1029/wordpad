// Mutation fuzzer for the document converters (robustness against damaged or hostile files).
//
//   convert_fuzz <iterations> <seed> file.docx file.odt file.rtf ...
//
// Every iteration takes one of the given files, damages it and runs it through the readers and,
// when the document could still be read, through all writers.  The DOCX and ODT packages are damaged
// both as raw bytes (ZIP structure, compressed data) and as XML text inside a freshly built package.
// Build it with sanitizers (see tests/run_convert_fuzz.sh); a crash or a hang leaves the offending
// input in fuzz-crash.bin.
#include "convert/model.h"
#include "convert/zip.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <unistd.h>

using namespace conv;

#if defined(__SANITIZE_ADDRESS__) || defined(__has_feature)
extern "C" void __sanitizer_set_death_callback(void (*callback)(void));
#define HAVE_DEATH_CALLBACK 1
#endif

namespace {

Bytes g_current;
unsigned long g_iteration = 0;

void DumpCurrent()
{
    FILE* f = fopen("fuzz-crash.bin", "wb");
    if (f) {
        fwrite(g_current.data(), 1, g_current.size(), f);
        fclose(f);
    }
    fprintf(stderr, "\nfuzz: failure in iteration %lu, input saved to fuzz-crash.bin (%zu bytes)\n", g_iteration, g_current.size());
}

void OnAlarm(int)
{
    fprintf(stderr, "\nfuzz: iteration %lu did not finish in time\n", g_iteration);
    DumpCurrent();
    _exit(3);
}

std::mt19937_64 g_rng;
size_t Rand(size_t n) { return n ? (size_t)(g_rng() % n) : 0; }
bool Chance(int percent) { return (int)Rand(100) < percent; }

const char* const kNumbers[] = { "0", "-1", "1", "2147483647", "-2147483648", "4294967295", "4294967296", "99999999999999999999",
                                 "-99999999999999999999", "65535", "65536", "32767", "1e309", "0x7fffffff", "", "NaN", "0.0000001",
                                 "100000000", "-100000000", "31680", "255", "256" };

// Damages a buffer without looking at its structure.
void MutateBytes(Bytes& d)
{
    int rounds = 1 + (int)Rand(4);
    for (int r = 0; r < rounds; ++r) {
        if (d.empty()) { d.push_back((uint8_t)Rand(256)); continue; }
        switch (Rand(8)) {
        case 0: d[Rand(d.size())] ^= (uint8_t)(1u << Rand(8)); break;
        case 1: d[Rand(d.size())] = (uint8_t)Rand(256); break;
        case 2: {   // interesting 32-bit values
            static const uint32_t kValues[] = { 0, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0xFFFFu, 0x10000u, 1 };
            uint32_t v = kValues[Rand(7)];
            size_t pos = Rand(d.size());
            for (size_t i = 0; i < 4 && pos + i < d.size(); ++i) d[pos + i] = (uint8_t)(v >> (8 * i));
            break;
        }
        case 3: {   // delete a range
            size_t pos = Rand(d.size()), len = 1 + Rand(std::min<size_t>(64, d.size() - pos));
            d.erase(d.begin() + (long)pos, d.begin() + (long)(pos + len));
            break;
        }
        case 4: {   // duplicate a range
            size_t pos = Rand(d.size()), len = 1 + Rand(std::min<size_t>(256, d.size() - pos));
            Bytes part(d.begin() + (long)pos, d.begin() + (long)(pos + len));
            d.insert(d.begin() + (long)Rand(d.size()), part.begin(), part.end());
            break;
        }
        case 5: {   // insert random bytes
            size_t len = 1 + Rand(16);
            Bytes part(len);
            for (uint8_t& b : part) b = (uint8_t)Rand(256);
            d.insert(d.begin() + (long)Rand(d.size()), part.begin(), part.end());
            break;
        }
        case 6: d.resize(Rand(d.size()) + 1); break;      // truncate
        case 7: {   // overwrite a range with one value
            size_t pos = Rand(d.size()), len = 1 + Rand(std::min<size_t>(32, d.size() - pos));
            memset(d.data() + pos, (int)Rand(256), len);
            break;
        }
        }
    }
}

// Damages markup (XML or RTF) in ways that keep most of it parseable.
void MutateText(std::string& t, bool rtf)
{
    int rounds = 1 + (int)Rand(4);
    for (int r = 0; r < rounds; ++r) {
        if (t.size() < 8) { t += rtf ? "{\\b x}" : "<a/>"; continue; }
        switch (Rand(10)) {
        case 0: {   // replace a number
            size_t pos = Rand(t.size());
            while (pos < t.size() && !(t[pos] >= '0' && t[pos] <= '9')) ++pos;
            size_t end = pos;
            while (end < t.size() && ((t[end] >= '0' && t[end] <= '9') || t[end] == '.')) ++end;
            if (pos < t.size()) t.replace(pos, end - pos, kNumbers[Rand(sizeof(kNumbers) / sizeof(kNumbers[0]))]);
            break;
        }
        case 1: {   // delete a tag or control word
            size_t pos = t.find(rtf ? '\\' : '<', Rand(t.size()));
            if (pos == std::string::npos) break;
            size_t end = rtf ? t.find_first_of(" \\{}", pos + 1) : t.find('>', pos);
            if (end == std::string::npos) break;
            t.erase(pos, end - pos + (rtf ? 0 : 1));
            break;
        }
        case 2: {   // duplicate a stretch of markup
            size_t pos = Rand(t.size()), len = 1 + Rand(std::min<size_t>(400, t.size() - pos));
            std::string part = t.substr(pos, len);
            t.insert(Rand(t.size()), part);
            break;
        }
        case 3: {   // move a stretch of markup
            size_t pos = Rand(t.size()), len = 1 + Rand(std::min<size_t>(200, t.size() - pos));
            std::string part = t.substr(pos, len);
            t.erase(pos, len);
            t.insert(Rand(t.size() + 1), part);
            break;
        }
        case 4: t.resize(Rand(t.size()) + 1); break;
        case 5: {   // deep nesting
            size_t depth = 1 + Rand(Chance(10) ? 200000 : 600);
            std::string open, close;
            for (size_t i = 0; i < depth; ++i) {
                if (rtf) { open += "{\\b "; close += "}"; }
                else { open += "<w:tbl><w:tr><w:tc>"; close += "</w:tc></w:tr></w:tbl>"; }
            }
            size_t pos = Rand(t.size());
            t.insert(pos, Chance(50) ? open + close : open);
            break;
        }
        case 6: {   // single character changes
            size_t pos = Rand(t.size());
            static const char kChars[] = "<>/\"'&;{}\\ =\0\n";
            t[pos] = kChars[Rand(sizeof(kChars) - 1)];
            break;
        }
        case 7: {   // very long attribute value or text
            size_t pos = Rand(t.size());
            t.insert(pos, std::string(1 + Rand(100000), (char)('a' + Rand(26))));
            break;
        }
        case 8: {   // entity / escape trouble
            static const char* const kXml[] = { "&#xFFFFFFFF;", "&#0;", "&#55357;", "&amp", "&;", "&#x110000;", "<![CDATA[", "<!--", "<?", "]]>" };
            static const char* const kRtf[] = { "\\u-1 ", "\\u65536?", "\\'zz", "\\'8", "\\bin999999 ", "\\uc99999 ", "\\ansicpg0 ", "\\f99999 ",
                                                "\\cf99999 ", "\\trowd\\cellx1\\cell\\row ", "\\pict\\pngblip ", "\\fs0 ", "\\li-99999999 " };
            t.insert(Rand(t.size()), rtf ? kRtf[Rand(13)] : kXml[Rand(10)]);
            break;
        }
        case 9: {   // swap two attribute values / words
            size_t a = t.find('"', Rand(t.size())), b = t.find('"', Rand(t.size()));
            if (a == std::string::npos || b == std::string::npos || a == b) break;
            std::swap(t[a + 1 < t.size() ? a + 1 : a], t[b + 1 < t.size() ? b + 1 : b]);
            break;
        }
        }
    }
}

// Rebuilds a package with damaged XML parts.
bool MutatePackage(const Bytes& seed, Bytes& out)
{
    ZipReader zip;
    if (!zip.Open(seed)) return false;
    std::vector<std::string> names = zip.Names();
    std::vector<std::string> xml;
    for (const std::string& n : names)
        if (n.size() > 4 && (n.compare(n.size() - 4, 4, ".xml") == 0 || n.compare(n.size() - 5, 5, ".rels") == 0)) xml.push_back(n);
    if (xml.empty()) return false;
    std::vector<std::string> victims;
    int count = 1 + (int)Rand(2);
    for (int i = 0; i < count; ++i) {
        // The main parts get most of the attention.
        std::string pick = xml[Rand(xml.size())];
        if (Chance(60)) {
            for (const std::string& n : xml)
                if (n == "word/document.xml" || n == "content.xml") pick = n;
        }
        victims.push_back(pick);
    }
    ZipWriter writer;
    for (const std::string& n : names) {
        Bytes data;
        if (!zip.Read(n, data)) continue;
        bool hit = false;
        for (const std::string& v : victims) hit = hit || v == n;
        if (hit) {
            std::string text(data.begin(), data.end());
            MutateText(text, false);
            writer.Add(n, text, Chance(80));
        } else if (Chance(3)) {
            continue;                                   // drop a part
        } else {
            if (Chance(2)) MutateBytes(data);           // damaged pictures and the like
            writer.Add(n, data, n != "mimetype");
        }
    }
    out = writer.Finish();
    return true;
}

void Exercise(const Bytes& input, int kind)
{
    Document doc;
    std::string error;
    bool ok = false;
    DetectContainer(input);
    if (kind == 0) ok = DocxRead(input, doc, &error);
    else if (kind == 1) ok = OdtRead(input, doc, &error);
    else ok = RtfRead(std::string(input.begin(), input.end()), doc, &error);
    if (!ok) return;
    // Whatever was read must be writable in every format, and the RTF must be readable again.
    std::string rtf = RtfWrite(doc);
    Document again;
    RtfRead(rtf, again, &error);
    Bytes file;
    DocxWrite(doc, file, &error);
    Bytes file2;
    OdtWrite(doc, file2, &error);
    if (g_iteration % 16 == 0) {
        Document back;
        DocxRead(file, back, &error);
        OdtRead(file2, back, &error);
    }
}

bool ReadFile(const char* path, Bytes& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool EndsWith(const std::string& s, const char* suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: convert_fuzz <iterations> <seed> files...\n");
        return 2;
    }
    unsigned long iterations = strtoul(argv[1], nullptr, 10);
    g_rng.seed(strtoull(argv[2], nullptr, 10));
    struct Seed { Bytes data; int kind; };
    std::vector<Seed> seeds;
    for (int i = 3; i < argc; ++i) {
        Seed s;
        std::string name = argv[i];
        s.kind = EndsWith(name, ".docx") ? 0 : EndsWith(name, ".odt") ? 1 : 2;
        if (ReadFile(argv[i], s.data)) seeds.push_back(std::move(s));
        else fprintf(stderr, "cannot read %s\n", argv[i]);
    }
    if (seeds.empty()) return 2;

#ifdef HAVE_DEATH_CALLBACK
    __sanitizer_set_death_callback(DumpCurrent);
#endif
    signal(SIGALRM, OnAlarm);

    unsigned long readable = 0;
    for (g_iteration = 1; g_iteration <= iterations; ++g_iteration) {
        const Seed& seed = seeds[Rand(seeds.size())];
        g_current = seed.data;
        if (seed.kind == 2) {
            if (Chance(25)) MutateBytes(g_current);
            else {
                std::string text(g_current.begin(), g_current.end());
                MutateText(text, true);
                g_current.assign(text.begin(), text.end());
            }
        } else if (Chance(35) || !MutatePackage(seed.data, g_current)) {
            g_current = seed.data;
            MutateBytes(g_current);
        }
        alarm(60);
        int kind = seed.kind;
        if (Chance(3)) kind = (int)Rand(3);        // the wrong reader must cope as well
        Document probe;
        std::string error;
        Exercise(g_current, kind);
        alarm(0);
        (void)probe;
        ++readable;
        if (g_iteration % 500 == 0) { fprintf(stderr, "\r%lu / %lu", g_iteration, iterations); fflush(stderr); }
    }
    fprintf(stderr, "\n");
    printf("fuzz: %lu iterations completed without a crash\n", readable);
    return 0;
}
