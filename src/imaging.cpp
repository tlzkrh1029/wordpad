// Image decoding / conversion helpers built on GDI+.
#include "gfx.h"
#include "imaging.h"

using namespace Gdiplus;

namespace {

const CLSID kPngEncoder = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

bool SaveAsPng(Image& img, std::vector<BYTE>& out)
{
    out.clear();
    IStream* stm = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stm)) || !stm) return false;
    bool ok = false;
    if (img.Save(stm, &kPngEncoder, nullptr) == Ok) {
        STATSTG st = {};
        HGLOBAL hg = nullptr;
        if (SUCCEEDED(stm->Stat(&st, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(stm, &hg)) && hg) {
            if (void* p = GlobalLock(hg)) {
                out.assign((const BYTE*)p, (const BYTE*)p + (size_t)st.cbSize.QuadPart);
                GlobalUnlock(hg);
                ok = !out.empty();
            }
        }
    }
    stm->Release();
    return ok;
}

int SaneDpi(REAL dpi)
{
    int v = (int)(dpi + 0.5f);
    return (v >= 50 && v <= 1200) ? v : 96;
}

// Draws an image onto a new bitmap (white background for formats without alpha handling).
Bitmap* Rasterize(Image& img, int w, int h, PixelFormat format, bool whiteBackground)
{
    Bitmap* bmp = new Bitmap(w, h, format);
    if (bmp->GetLastStatus() != Ok) { delete bmp; return nullptr; }
    Graphics gfx(bmp);
    if (whiteBackground) gfx.Clear(Color(255, 255, 255, 255));
    else gfx.Clear(Color(0, 255, 255, 255));
    gfx.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    gfx.SetPixelOffsetMode(PixelOffsetModeHalf);
    gfx.DrawImage(&img, Rect(0, 0, w, h));
    return bmp;
}

} // namespace

bool Image_Normalize(const BYTE* data, size_t size, std::vector<BYTE>& converted, ImageInfo& info)
{
    converted.clear();
    if (!data || size < 8 || size > 0x7FFFFFFF) return false;
    IStream* stm = SHCreateMemStream(data, (UINT)size);
    if (!stm) return false;
    bool ok = false;
    {
        Image img(stm);
        if (img.GetLastStatus() == Ok && img.GetWidth() > 0 && img.GetHeight() > 0) {
            info.width = (int)img.GetWidth();
            info.height = (int)img.GetHeight();
            info.dpiX = SaneDpi(img.GetHorizontalResolution());
            info.dpiY = SaneDpi(img.GetVerticalResolution());
            GUID fmt = {};
            img.GetRawFormat(&fmt);
            bool isPng = IsEqualGUID(fmt, ImageFormatPNG) != 0;
            info.isJpeg = IsEqualGUID(fmt, ImageFormatJPEG) != 0;
            if (isPng || info.isJpeg) {
                ok = true;
            } else {
                // Limit absurdly large canvases (e.g. scalable metafiles) to something reasonable.
                int w = info.width, h = info.height;
                if (w > 8192 || h > 8192) {
                    double f = std::min(8192.0 / w, 8192.0 / h);
                    w = std::max(1, (int)(w * f));
                    h = std::max(1, (int)(h * f));
                }
                if (Bitmap* bmp = Rasterize(img, w, h, PixelFormat32bppARGB, false)) {
                    ok = SaveAsPng(*bmp, converted);
                    delete bmp;
                    info.width = w;
                    info.height = h;
                }
            }
        }
    }
    stm->Release();
    return ok;
}

bool Image_ToDib(const BYTE* data, size_t size, std::vector<BYTE>& dib)
{
    dib.clear();
    if (!data || size < 8 || size > 0x7FFFFFFF) return false;
    IStream* stm = SHCreateMemStream(data, (UINT)size);
    if (!stm) return false;
    bool ok = false;
    {
        Image img(stm);
        if (img.GetLastStatus() == Ok && img.GetWidth() > 0 && img.GetHeight() > 0) {
            int w = (int)img.GetWidth(), h = (int)img.GetHeight();
            if (Bitmap* bmp = Rasterize(img, w, h, PixelFormat24bppRGB, true)) {
                BitmapData bd = {};
                Rect rc(0, 0, w, h);
                if (bmp->LockBits(&rc, ImageLockModeRead, PixelFormat24bppRGB, &bd) == Ok) {
                    int stride = ((w * 3) + 3) & ~3;
                    BITMAPINFOHEADER bih = {};
                    bih.biSize = sizeof(bih);
                    bih.biWidth = w;
                    bih.biHeight = h;             // bottom-up
                    bih.biPlanes = 1;
                    bih.biBitCount = 24;
                    bih.biCompression = BI_RGB;
                    bih.biSizeImage = (DWORD)(stride * h);
                    dib.resize(sizeof(bih) + (size_t)stride * h, 0);
                    memcpy(dib.data(), &bih, sizeof(bih));
                    for (int y = 0; y < h; ++y) {
                        const BYTE* src = (const BYTE*)bd.Scan0 + (ptrdiff_t)bd.Stride * (h - 1 - y);
                        memcpy(dib.data() + sizeof(bih) + (size_t)stride * y, src, (size_t)w * 3);
                    }
                    bmp->UnlockBits(&bd);
                    ok = true;
                }
                delete bmp;
            }
        }
    }
    stm->Release();
    return ok;
}

bool Image_FromHBitmap(HBITMAP hbmp, std::vector<BYTE>& png)
{
    if (!hbmp) return false;
    Bitmap bmp(hbmp, nullptr);
    if (bmp.GetLastStatus() != Ok) return false;
    return SaveAsPng(bmp, png);
}

bool Image_DibToPng(const BYTE* dib, size_t size, std::vector<BYTE>& png, int* width, int* height)
{
    if (!dib || size < sizeof(BITMAPINFOHEADER)) return false;
    BITMAPINFOHEADER bih;
    memcpy(&bih, dib, sizeof(bih));
    if (bih.biSize < sizeof(BITMAPCOREHEADER) || bih.biSize > size) return false;
    // Wrap the packed DIB in a BMP file header so GDI+ can decode it.
    DWORD colors = 0;
    if (bih.biSize >= sizeof(BITMAPINFOHEADER)) {
        if (bih.biBitCount <= 8) colors = bih.biClrUsed ? bih.biClrUsed : (1u << bih.biBitCount);
        else if (bih.biCompression == BI_BITFIELDS && bih.biSize == sizeof(BITMAPINFOHEADER)) colors = 3;
    }
    BITMAPFILEHEADER bfh = {};
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = (DWORD)(sizeof(bfh) + bih.biSize + colors * (bih.biCompression == BI_BITFIELDS && bih.biBitCount > 8 ? 4 : 4));
    bfh.bfSize = (DWORD)(sizeof(bfh) + size);
    std::vector<BYTE> file(sizeof(bfh) + size);
    memcpy(file.data(), &bfh, sizeof(bfh));
    memcpy(file.data() + sizeof(bfh), dib, size);

    IStream* stm = SHCreateMemStream(file.data(), (UINT)file.size());
    if (!stm) return false;
    bool ok = false;
    {
        Bitmap bmp(stm);
        if (bmp.GetLastStatus() == Ok && bmp.GetWidth() > 0) {
            if (width) *width = (int)bmp.GetWidth();
            if (height) *height = (int)bmp.GetHeight();
            ok = SaveAsPng(bmp, png);
        }
    }
    stm->Release();
    return ok;
}

bool Image_MetafileToPng(const BYTE* data, size_t size, bool enhanced, int widthPx, int heightPx,
                         std::vector<BYTE>& png)
{
    if (!data || size < 18 || widthPx <= 0 || heightPx <= 0) return false;
    widthPx = std::min(widthPx, 8192);
    heightPx = std::min(heightPx, 8192);

    HENHMETAFILE hemf = nullptr;
    if (enhanced) {
        hemf = SetEnhMetaFileBits((UINT)size, data);
    } else {
        // RTF stores the raw metafile without a placeable header; play it into the target extent.
        METAFILEPICT mfp = {};
        mfp.mm = MM_ANISOTROPIC;
        mfp.xExt = MulDiv(widthPx, 2540, 96);
        mfp.yExt = MulDiv(heightPx, 2540, 96);
        HDC ref = GetDC(nullptr);
        hemf = SetWinMetaFileBits((UINT)size, data, ref, &mfp);
        ReleaseDC(nullptr, ref);
    }
    if (!hemf) return false;

    bool ok = false;
    {
        Bitmap bmp(widthPx, heightPx, PixelFormat32bppARGB);
        if (bmp.GetLastStatus() == Ok) {
            {
                Graphics gfx(&bmp);
                gfx.Clear(Color(255, 255, 255, 255));
                HDC hdc = gfx.GetHDC();
                RECT rc = { 0, 0, widthPx, heightPx };
                PlayEnhMetaFile(hdc, hemf, &rc);
                gfx.ReleaseHDC(hdc);
            }
            ok = SaveAsPng(bmp, png);
        }
    }
    DeleteEnhMetaFile(hemf);
    return ok;
}
