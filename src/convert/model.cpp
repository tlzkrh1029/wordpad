// Temporary stubs until the converters are implemented.
#include "convert.h"

namespace conv {

static PictureConverter g_pictureConverter = nullptr;

Container DetectContainer(const Bytes&) { return Container::Unknown; }
bool DocxToRtf(const Bytes&, std::string&, std::string*) { return false; }
bool OdtToRtf(const Bytes&, std::string&, std::string*) { return false; }
bool RtfToDocx(const std::string&, Bytes&, std::string*) { return false; }
bool RtfToOdt(const std::string&, Bytes&, std::string*) { return false; }
void SetPictureConverter(PictureConverter fn) { g_pictureConverter = fn; }
PictureConverter GetPictureConverter() { return g_pictureConverter; }

} // namespace conv
