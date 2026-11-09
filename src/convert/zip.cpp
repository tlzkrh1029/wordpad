// Minimal ZIP container support (stored and deflated entries) for DOCX and ODT packages.
#include "zip.h"
#include <cstring>

namespace conv {

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------
uint32_t Crc32(const uint8_t* data, size_t size)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Inflate
// ---------------------------------------------------------------------------
namespace {

struct BitReader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    uint32_t buf = 0;
    int count = 0;
    bool error = false;

    int Bits(int need)
    {
        while (count < need) {
            if (pos >= n) { error = true; return 0; }
            buf |= (uint32_t)p[pos++] << count;
            count += 8;
        }
        int v = (int)(buf & ((1u << need) - 1));
        buf >>= need;
        count -= need;
        return v;
    }
};

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[288];
};

// Builds canonical decoding tables. Returns false for an over-subscribed code.
bool Construct(Huffman& h, const uint16_t* length, int n)
{
    for (int i = 0; i < 16; ++i) h.count[i] = 0;
    for (int i = 0; i < n; ++i) h.count[length[i]]++;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = (uint16_t)(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i)
        if (length[i] != 0) h.symbol[offs[length[i]]++] = (uint16_t)i;
    return true;
}

int Decode(BitReader& s, const Huffman& h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= s.Bits(1);
        if (s.error) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const uint16_t kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
const uint16_t kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
const uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
const uint16_t kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

bool InflateCodes(BitReader& s, Bytes& out, const Huffman& lencode, const Huffman& distcode, size_t maxSize)
{
    for (;;) {
        int symbol = Decode(s, lencode);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (out.size() >= maxSize) return false;
            out.push_back((uint8_t)symbol);
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29) return false;
            int len = kLenBase[symbol] + s.Bits(kLenExtra[symbol]);
            int ds = Decode(s, distcode);
            if (ds < 0 || ds >= 30) return false;
            size_t dist = (size_t)kDistBase[ds] + (size_t)s.Bits(kDistExtra[ds]);
            if (s.error || dist > out.size() || out.size() + (size_t)len > maxSize) return false;
            size_t from = out.size() - dist;
            for (int i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
    }
}

} // namespace

bool Inflate(const uint8_t* src, size_t srcSize, Bytes& out, size_t maxSize)
{
    out.clear();
    BitReader s{ src, srcSize };
    static Huffman fixedLen, fixedDist;
    static bool fixedReady = false;
    if (!fixedReady) {
        uint16_t lengths[288];
        int i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        Construct(fixedLen, lengths, 288);
        for (i = 0; i < 30; ++i) lengths[i] = 5;
        Construct(fixedDist, lengths, 30);
        fixedReady = true;
    }

    int last;
    do {
        last = s.Bits(1);
        int type = s.Bits(2);
        if (s.error) return false;
        if (type == 0) {
            s.buf = 0;
            s.count = 0;
            if (s.pos + 4 > s.n) return false;
            unsigned len = s.p[s.pos] | (s.p[s.pos + 1] << 8);
            unsigned nlen = s.p[s.pos + 2] | (s.p[s.pos + 3] << 8);
            s.pos += 4;
            if ((len ^ 0xFFFF) != nlen || s.pos + len > s.n || out.size() + len > maxSize) return false;
            out.insert(out.end(), s.p + s.pos, s.p + s.pos + len);
            s.pos += len;
        } else if (type == 1) {
            if (!InflateCodes(s, out, fixedLen, fixedDist, maxSize)) return false;
        } else if (type == 2) {
            int nlen = s.Bits(5) + 257, ndist = s.Bits(5) + 1, ncode = s.Bits(4) + 4;
            if (s.error || nlen > 286 || ndist > 30) return false;
            static const int order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            uint16_t lengths[320];
            for (int i = 0; i < 19; ++i) lengths[i] = 0;
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = (uint16_t)s.Bits(3);
            Huffman codeLen;
            if (s.error || !Construct(codeLen, lengths, 19)) return false;
            int index = 0;
            while (index < nlen + ndist) {
                int symbol = Decode(s, codeLen);
                if (symbol < 0) return false;
                if (symbol < 16) {
                    lengths[index++] = (uint16_t)symbol;
                } else {
                    int repeat, value = 0;
                    if (symbol == 16) {
                        if (index == 0) return false;
                        value = lengths[index - 1];
                        repeat = 3 + s.Bits(2);
                    } else if (symbol == 17) {
                        repeat = 3 + s.Bits(3);
                    } else {
                        repeat = 11 + s.Bits(7);
                    }
                    if (s.error || index + repeat > nlen + ndist) return false;
                    while (repeat--) lengths[index++] = (uint16_t)value;
                }
            }
            if (lengths[256] == 0) return false;
            Huffman lencode, distcode;
            if (!Construct(lencode, lengths, nlen)) return false;
            // Incomplete distance codes are allowed (a single distance code is common).
            Construct(distcode, lengths + nlen, ndist);
            if (!InflateCodes(s, out, lencode, distcode, maxSize)) return false;
        } else {
            return false;
        }
    } while (!last);
    return !s.error;
}

// ---------------------------------------------------------------------------
// Deflate (fixed Huffman codes with LZ77 matching)
// ---------------------------------------------------------------------------
namespace {

struct BitWriter {
    Bytes& out;
    uint32_t buf = 0;
    int count = 0;

    void Bits(uint32_t value, int n)   // least significant bit first
    {
        buf |= value << count;
        count += n;
        while (count >= 8) {
            out.push_back((uint8_t)(buf & 0xFF));
            buf >>= 8;
            count -= 8;
        }
    }
    void Code(uint32_t code, int n)    // Huffman codes are sent most significant bit first
    {
        uint32_t rev = 0;
        for (int i = 0; i < n; ++i) rev |= ((code >> i) & 1u) << (n - 1 - i);
        Bits(rev, n);
    }
    void Flush()
    {
        if (count > 0) { out.push_back((uint8_t)(buf & 0xFF)); buf = 0; count = 0; }
    }
};

void PutLiteralOrLength(BitWriter& w, int symbol)
{
    if (symbol < 144) w.Code(0x30 + symbol, 8);
    else if (symbol < 256) w.Code(0x190 + (symbol - 144), 9);
    else if (symbol < 280) w.Code(symbol - 256, 7);
    else w.Code(0xC0 + (symbol - 280), 8);
}

void PutMatch(BitWriter& w, int length, int distance)
{
    int li = 28;
    while (li > 0 && kLenBase[li] > length) --li;
    PutLiteralOrLength(w, 257 + li);
    if (kLenExtra[li]) w.Bits((uint32_t)(length - kLenBase[li]), kLenExtra[li]);
    int di = 29;
    while (di > 0 && kDistBase[di] > distance) --di;
    w.Code((uint32_t)di, 5);
    if (kDistExtra[di]) w.Bits((uint32_t)(distance - kDistBase[di]), kDistExtra[di]);
}

} // namespace

void Deflate(const uint8_t* src, size_t n, Bytes& out)
{
    out.clear();
    out.reserve(n / 2 + 64);
    BitWriter w{ out };
    w.Bits(1, 1);    // final block
    w.Bits(1, 2);    // fixed Huffman codes

    const int kHashBits = 15, kHashSize = 1 << kHashBits, kWindow = 32768, kMaxChain = 48;
    std::vector<int> head((size_t)kHashSize, -1);
    std::vector<int> prev(n > 0 ? n : 1, -1);
    auto hashAt = [&](size_t i) -> int {
        return (int)((((uint32_t)src[i] << 10) ^ ((uint32_t)src[i + 1] << 5) ^ src[i + 2]) & (kHashSize - 1));
    };

    size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + 3 <= n) {
            int h = hashAt(i);
            int candidate = head[(size_t)h];
            int chain = 0;
            size_t maxLen = n - i < 258 ? n - i : 258;
            while (candidate >= 0 && (int)i - candidate <= kWindow && chain++ < kMaxChain) {
                const uint8_t* a = src + candidate;
                const uint8_t* b = src + i;
                if (a[bestLen] == b[bestLen] || bestLen == 0) {
                    size_t len = 0;
                    while (len < maxLen && a[len] == b[len]) ++len;
                    if ((int)len > bestLen) {
                        bestLen = (int)len;
                        bestDist = (int)i - candidate;
                        if (len == maxLen) break;
                    }
                }
                candidate = prev[(size_t)candidate];
            }
        }
        if (bestLen >= 3) {
            PutMatch(w, bestLen, bestDist);
            for (int k = 0; k < bestLen; ++k) {
                if (i + 3 <= n) {
                    int h = hashAt(i);
                    prev[i] = head[(size_t)h];
                    head[(size_t)h] = (int)i;
                }
                ++i;
            }
        } else {
            PutLiteralOrLength(w, src[i]);
            if (i + 3 <= n) {
                int h = hashAt(i);
                prev[i] = head[(size_t)h];
                head[(size_t)h] = (int)i;
            }
            ++i;
        }
    }
    PutLiteralOrLength(w, 256);
    w.Flush();
}

// ---------------------------------------------------------------------------
// ZIP reading
// ---------------------------------------------------------------------------
namespace {

uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

void Wr16(Bytes& out, uint16_t v) { out.push_back((uint8_t)(v & 0xFF)); out.push_back((uint8_t)(v >> 8)); }
void Wr32(Bytes& out, uint32_t v) { Wr16(out, (uint16_t)(v & 0xFFFF)); Wr16(out, (uint16_t)(v >> 16)); }

const size_t kMaxEntrySize = 512u * 1024u * 1024u;

} // namespace

bool ZipReader::Open(const Bytes& file)
{
    m_file = &file;
    m_entries.clear();
    size_t n = file.size();
    if (n < 22) return false;
    const uint8_t* d = file.data();

    // Locate the end-of-central-directory record (it may be followed by a comment).
    size_t eocd = (size_t)-1;
    size_t lowest = n > 22 + 65535 ? n - 22 - 65535 : 0;
    for (size_t i = n - 22;; --i) {
        if (Rd32(d + i) == 0x06054B50u) { eocd = i; break; }
        if (i == lowest) break;
    }
    if (eocd == (size_t)-1) return false;
    unsigned total = Rd16(d + eocd + 10);
    size_t cdSize = Rd32(d + eocd + 12), cdOffset = Rd32(d + eocd + 16);
    if (cdOffset > n || cdSize > n - cdOffset) return false;

    size_t p = cdOffset;
    for (unsigned i = 0; i < total; ++i) {
        if (p + 46 > n || Rd32(d + p) != 0x02014B50u) return false;
        Entry e;
        e.method = Rd16(d + p + 10);
        e.crc = Rd32(d + p + 16);
        e.compressedSize = Rd32(d + p + 20);
        e.size = Rd32(d + p + 24);
        unsigned nameLen = Rd16(d + p + 28), extraLen = Rd16(d + p + 30), commentLen = Rd16(d + p + 32);
        e.headerOffset = Rd32(d + p + 42);
        if (p + 46 + nameLen > n) return false;
        std::string name((const char*)d + p + 46, nameLen);
        for (char& c : name) if (c == '\\') c = '/';
        m_entries[name] = e;
        p += 46 + (size_t)nameLen + extraLen + commentLen;
    }
    return !m_entries.empty();
}

bool ZipReader::Has(const std::string& name) const { return m_entries.find(name) != m_entries.end(); }

std::vector<std::string> ZipReader::Names() const
{
    std::vector<std::string> names;
    for (const auto& kv : m_entries) names.push_back(kv.first);
    return names;
}

bool ZipReader::Read(const std::string& name, Bytes& out) const
{
    out.clear();
    auto it = m_entries.find(name);
    if (it == m_entries.end() || !m_file) return false;
    const Entry& e = it->second;
    const Bytes& f = *m_file;
    size_t n = f.size();
    if ((size_t)e.headerOffset + 30 > n || Rd32(f.data() + e.headerOffset) != 0x04034B50u) return false;
    size_t dataPos = (size_t)e.headerOffset + 30 + Rd16(f.data() + e.headerOffset + 26) + Rd16(f.data() + e.headerOffset + 28);
    if (dataPos > n || e.compressedSize > n - dataPos || e.size > kMaxEntrySize) return false;
    if (e.method == 0) {
        out.assign(f.begin() + (ptrdiff_t)dataPos, f.begin() + (ptrdiff_t)(dataPos + e.compressedSize));
        return true;
    }
    if (e.method == 8) {
        out.reserve(e.size);
        return Inflate(f.data() + dataPos, e.compressedSize, out, kMaxEntrySize) && out.size() == e.size;
    }
    return false;
}

bool ZipReader::ReadText(const std::string& name, std::string& out) const
{
    // Markup parts are parsed into a tree that needs many times their size: 64 MB of XML is far more
    // than any real document has, and keeps a "ZIP bomb" from exhausting the memory.
    const uint32_t kMaxTextSize = 64u * 1024u * 1024u;
    auto it = m_entries.find(name);
    if (it == m_entries.end() || it->second.size > kMaxTextSize) return false;
    Bytes data;
    if (!Read(name, data)) return false;
    out.assign(data.begin(), data.end());
    return true;
}

// ---------------------------------------------------------------------------
// ZIP writing
// ---------------------------------------------------------------------------
void ZipWriter::Add(const std::string& name, const uint8_t* data, size_t size, bool compress)
{
    Entry e;
    e.name = name;
    e.crc = Crc32(data, size);
    e.size = (uint32_t)size;
    e.headerOffset = (uint32_t)m_out.size();
    Bytes packed;
    if (compress && size > 32) {
        Deflate(data, size, packed);
        if (packed.size() >= size) compress = false;
    } else {
        compress = false;
    }
    e.method = compress ? 8 : 0;
    e.compressedSize = compress ? (uint32_t)packed.size() : (uint32_t)size;

    Wr32(m_out, 0x04034B50u);
    Wr16(m_out, 20);              // version needed
    Wr16(m_out, 0x0800);          // flags: UTF-8 names
    Wr16(m_out, e.method);
    Wr16(m_out, 0);               // time
    Wr16(m_out, 0x21);            // date: 1980-01-01
    Wr32(m_out, e.crc);
    Wr32(m_out, e.compressedSize);
    Wr32(m_out, e.size);
    Wr16(m_out, (uint16_t)name.size());
    Wr16(m_out, 0);               // no extra field
    m_out.insert(m_out.end(), name.begin(), name.end());
    if (compress) m_out.insert(m_out.end(), packed.begin(), packed.end());
    else if (size) m_out.insert(m_out.end(), data, data + size);
    m_entries.push_back(e);
}

void ZipWriter::Add(const std::string& name, const std::string& text, bool compress)
{
    Add(name, (const uint8_t*)text.data(), text.size(), compress);
}

void ZipWriter::Add(const std::string& name, const Bytes& data, bool compress)
{
    Add(name, data.data(), data.size(), compress);
}

Bytes ZipWriter::Finish()
{
    uint32_t cdOffset = (uint32_t)m_out.size();
    for (const Entry& e : m_entries) {
        Wr32(m_out, 0x02014B50u);
        Wr16(m_out, 20);          // version made by
        Wr16(m_out, 20);          // version needed
        Wr16(m_out, 0x0800);
        Wr16(m_out, e.method);
        Wr16(m_out, 0);
        Wr16(m_out, 0x21);
        Wr32(m_out, e.crc);
        Wr32(m_out, e.compressedSize);
        Wr32(m_out, e.size);
        Wr16(m_out, (uint16_t)e.name.size());
        Wr16(m_out, 0);           // extra
        Wr16(m_out, 0);           // comment
        Wr16(m_out, 0);           // disk number
        Wr16(m_out, 0);           // internal attributes
        Wr32(m_out, 0);           // external attributes
        Wr32(m_out, e.headerOffset);
        m_out.insert(m_out.end(), e.name.begin(), e.name.end());
    }
    uint32_t cdSize = (uint32_t)m_out.size() - cdOffset;
    Wr32(m_out, 0x06054B50u);
    Wr16(m_out, 0);
    Wr16(m_out, 0);
    Wr16(m_out, (uint16_t)m_entries.size());
    Wr16(m_out, (uint16_t)m_entries.size());
    Wr32(m_out, cdSize);
    Wr32(m_out, cdOffset);
    Wr16(m_out, 0);
    Bytes result;
    result.swap(m_out);
    m_entries.clear();
    return result;
}

} // namespace conv
