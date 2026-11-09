// Minimal ZIP container support (stored and deflated entries) for DOCX and ODT packages.
#pragma once
#include "convert.h"
#include <map>

namespace conv {

uint32_t Crc32(const uint8_t* data, size_t size);
// Raw DEFLATE (RFC 1951). Inflate returns false on malformed input or if the output exceeds maxSize.
bool Inflate(const uint8_t* src, size_t srcSize, Bytes& out, size_t maxSize);
void Deflate(const uint8_t* src, size_t size, Bytes& out);

class ZipReader {
public:
    bool Open(const Bytes& file);
    bool Has(const std::string& name) const;
    bool Read(const std::string& name, Bytes& out) const;
    bool ReadText(const std::string& name, std::string& out) const;
    std::vector<std::string> Names() const;

private:
    struct Entry { uint16_t method; uint32_t crc, compressedSize, size, headerOffset; };
    const Bytes* m_file = nullptr;
    std::map<std::string, Entry> m_entries;
};

class ZipWriter {
public:
    // compress = false stores the data as is (used for already compressed pictures and the ODF mimetype).
    void Add(const std::string& name, const uint8_t* data, size_t size, bool compress = true);
    void Add(const std::string& name, const std::string& text, bool compress = true);
    void Add(const std::string& name, const Bytes& data, bool compress = true);
    Bytes Finish();

private:
    struct Entry { std::string name; uint16_t method; uint32_t crc, compressedSize, size, headerOffset; };
    Bytes m_out;
    std::vector<Entry> m_entries;
};

} // namespace conv
