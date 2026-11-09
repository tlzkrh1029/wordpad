// Image decoding / conversion helpers built on GDI+.
#pragma once
#include "app.h"

struct ImageInfo {
    int width = 0, height = 0;      // pixels
    int dpiX = 96, dpiY = 96;
    bool isJpeg = false;            // false: PNG
};

// Validates image bytes and reports their size. PNG and JPEG data is kept as is (converted stays
// empty); any other format (BMP, GIF, TIFF, ICO, ...) is re-encoded as PNG into `converted`.
bool Image_Normalize(const BYTE* data, size_t size, std::vector<BYTE>& converted, ImageInfo& info);

// Decodes any supported image into a packed 24-bit DIB (BITMAPINFOHEADER followed by the pixels).
bool Image_ToDib(const BYTE* data, size_t size, std::vector<BYTE>& dib);

bool Image_FromHBitmap(HBITMAP hbmp, std::vector<BYTE>& png);

// Packed DIB (BITMAPINFO + pixels, as stored in RTF \dibitmap0) -> PNG.
bool Image_DibToPng(const BYTE* dib, size_t size, std::vector<BYTE>& png, int* width, int* height);

// Windows metafile bits (as stored in RTF \wmetafile8 or \emfblip) rendered to a PNG of the given size.
bool Image_MetafileToPng(const BYTE* data, size_t size, bool enhanced, int widthPx, int heightPx,
                         std::vector<BYTE>& png);
