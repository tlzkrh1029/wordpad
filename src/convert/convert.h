// Document conversion between RTF (the rich edit control's native format) and DOCX / ODT.
// This code is platform independent so it can be unit-tested natively on any OS.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace conv {

typedef std::vector<uint8_t> Bytes;

enum class Container { Unknown, Docx, Odt };

// Looks inside a ZIP package and tells which document type it is.
Container DetectContainer(const Bytes& file);

// DOCX / ODT -> RTF (for loading into the rich edit control).
bool DocxToRtf(const Bytes& file, std::string& rtf, std::string* error);
bool OdtToRtf(const Bytes& file, std::string& rtf, std::string* error);

// RTF (as written by the rich edit control) -> DOCX / ODT.
bool RtfToDocx(const std::string& rtf, Bytes& file, std::string* error);
bool RtfToOdt(const std::string& rtf, Bytes& file, std::string* error);

// Optional hook used to turn pictures that cannot be stored directly into PNG.
enum class PictureKind { Wmf, Emf, Dib, AnyImageFile };
struct PictureRequest {
    PictureKind kind;
    const uint8_t* data;
    size_t size;
    int widthPx, heightPx;      // desired raster size for metafiles
};
struct PictureResult {
    Bytes png;
    int widthPx = 0, heightPx = 0;
};
typedef bool (*PictureConverter)(const PictureRequest& request, PictureResult& result);
void SetPictureConverter(PictureConverter fn);
PictureConverter GetPictureConverter();

} // namespace conv
