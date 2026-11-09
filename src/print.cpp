// Page setup, printing and print preview.
#include "print.h"
#include "editor.h"
#include "theme.h"
#include "ruler.h"
#include "imaging.h"
#include "fileio.h"
#include <winspool.h>

namespace {

// Where a document is laid out: a printer, or the screen when no printer is installed.
struct Target {
    HDC hdc = nullptr;
    bool printer = false;
    int dpiX = 96, dpiY = 96;
    SIZE paper = {};       // sheet size in twips
    POINT offset = {};     // unprintable left / top edge in twips
    RECT text = {};        // text area in twips, relative to the top-left corner of the sheet
};

void FillTarget(Target& t, HDC hdc, bool printer)
{
    t.hdc = hdc;
    t.printer = printer;
    t.dpiX = std::max(1, GetDeviceCaps(hdc, LOGPIXELSX));
    t.dpiY = std::max(1, GetDeviceCaps(hdc, LOGPIXELSY));
    if (printer) {
        t.paper.cx = MulDiv(GetDeviceCaps(hdc, PHYSICALWIDTH), 1440, t.dpiX);
        t.paper.cy = MulDiv(GetDeviceCaps(hdc, PHYSICALHEIGHT), 1440, t.dpiY);
        t.offset.x = MulDiv(GetDeviceCaps(hdc, PHYSICALOFFSETX), 1440, t.dpiX);
        t.offset.y = MulDiv(GetDeviceCaps(hdc, PHYSICALOFFSETY), 1440, t.dpiY);
        if (t.paper.cx <= 0 || t.paper.cy <= 0) t.paper = g.cfg.paper;
    } else {
        t.paper = g.cfg.paper;
        t.offset = { 0, 0 };
    }
    const RECT& m = g.cfg.margins;
    t.text = { m.left, m.top, t.paper.cx - m.right, t.paper.cy - m.bottom };
    // Margins that leave no room for text fall back to half an inch.
    if (t.text.right - t.text.left < 720) { t.text.left = 720; t.text.right = std::max(1440L, t.paper.cx - 720); }
    if (t.text.bottom - t.text.top < 720) { t.text.top = 720; t.text.bottom = std::max(1440L, t.paper.cy - 720); }
}

HDC CreatePrinterDC()
{
    if (!g.hDevNames) {
        PRINTDLGW pd = {};
        pd.lStructSize = sizeof(pd);
        pd.Flags = PD_RETURNDEFAULT;
        if (!PrintDlgW(&pd)) return nullptr;
        g.hDevMode = pd.hDevMode;
        g.hDevNames = pd.hDevNames;
    }
    if (!g.hDevNames) return nullptr;
    DEVNAMES* dn = (DEVNAMES*)GlobalLock(g.hDevNames);
    if (!dn) return nullptr;
    DEVMODEW* dm = g.hDevMode ? (DEVMODEW*)GlobalLock(g.hDevMode) : nullptr;
    HDC hdc = CreateDCW((LPCWSTR)dn + dn->wDriverOffset, (LPCWSTR)dn + dn->wDeviceOffset, nullptr, dm);
    if (dm) GlobalUnlock(g.hDevMode);
    GlobalUnlock(g.hDevNames);
    return hdc;
}

bool OpenLayoutTarget(Target& t)
{
    if (HDC hdc = CreatePrinterDC()) {
        FillTarget(t, hdc, true);
        return true;
    }
    HDC hdc = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
    if (!hdc) return false;
    FillTarget(t, hdc, false);
    return true;
}

void CloseTarget(Target& t)
{
    if (t.hdc) DeleteDC(t.hdc);
    t.hdc = nullptr;
}

// The zoom factor must not leak into the printed layout.
struct ZoomGuard {
    bool active = false;
    POINT scroll = {};
    ZoomGuard()
    {
        SendMessageW(g.hEdit, EM_GETSCROLLPOS, 0, (LPARAM)&scroll);
        SendMessageW(g.hEdit, WM_SETREDRAW, FALSE, 0);
        active = Editor_SuspendZoom();
        if (!active) SendMessageW(g.hEdit, WM_SETREDRAW, TRUE, 0);
    }
    ~ZoomGuard()
    {
        if (!active) return;
        Editor_ResumeZoom();
        SendMessageW(g.hEdit, EM_SETSCROLLPOS, 0, (LPARAM)&scroll);
        SendMessageW(g.hEdit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g.hEdit, nullptr, TRUE);
    }
};

RECT TextRectForDevice(const Target& t)
{
    // A printer DC has its origin at the top-left corner of the printable area.
    RECT rc = t.text;
    OffsetRect(&rc, -t.offset.x, -t.offset.y);
    return rc;
}

void Paginate(const Target& t, LONG cpMin, LONG cpMax, std::vector<LONG>& starts)
{
    starts.clear();
    LONG end = cpMax < 0 ? Editor_GetTextLength() : cpMax;
    FORMATRANGE fr = {};
    fr.hdc = t.hdc;
    fr.hdcTarget = t.hdc;
    fr.rcPage = { 0, 0, t.paper.cx, t.paper.cy };
    LONG cp = cpMin;
    while (cp < end && starts.size() < 100000) {
        starts.push_back(cp);
        fr.rc = TextRectForDevice(t);
        fr.chrg.cpMin = cp;
        fr.chrg.cpMax = end;
        LONG next = (LONG)SendMessageW(g.hEdit, EM_FORMATRANGE, FALSE, (LPARAM)&fr);
        if (next <= cp) break;
        cp = next;
    }
    SendMessageW(g.hEdit, EM_FORMATRANGE, 0, 0);
    if (starts.empty()) starts.push_back(cpMin);
}

// Records one page into an enhanced metafile that covers the whole sheet.
HENHMETAFILE RenderPage(const Target& t, LONG cpMin, LONG cpMax)
{
    int pxW = MulDiv(t.paper.cx, t.dpiX, 1440), pxH = MulDiv(t.paper.cy, t.dpiY, 1440);
    int hs = GetDeviceCaps(t.hdc, HORZSIZE), hr = std::max(1, GetDeviceCaps(t.hdc, HORZRES));
    int vs = GetDeviceCaps(t.hdc, VERTSIZE), vr = std::max(1, GetDeviceCaps(t.hdc, VERTRES));
    // Frame in 0.01 mm, expressed through the reference device so device units map 1:1.
    RECT frame = { 0, 0, MulDiv(pxW, hs * 100, hr), MulDiv(pxH, vs * 100, vr) };
    HDC meta = CreateEnhMetaFileW(t.hdc, nullptr, &frame, nullptr);
    if (!meta) return nullptr;
    FORMATRANGE fr = {};
    fr.hdc = meta;
    fr.hdcTarget = t.hdc;
    fr.rcPage = { 0, 0, t.paper.cx, t.paper.cy };
    fr.rc = t.text;
    fr.chrg.cpMin = cpMin;
    fr.chrg.cpMax = cpMax;
    SendMessageW(g.hEdit, EM_FORMATRANGE, TRUE, (LPARAM)&fr);
    SendMessageW(g.hEdit, EM_FORMATRANGE, 0, 0);
    return CloseEnhMetaFile(meta);
}

// A rendered page: normally an enhanced metafile (sharp at every zoom level); a bitmap is used
// when the control did not record anything into the metafile.
struct PageImage {
    HENHMETAFILE emf = nullptr;
    HBITMAP bmp = nullptr;
    SIZE bmpSize = {};
};

void FreePage(PageImage& img)
{
    if (img.emf) DeleteEnhMetaFile(img.emf);
    if (img.bmp) DeleteObject(img.bmp);
    img = PageImage();
}

bool EmfHasContent(HENHMETAFILE emf)
{
    ENHMETAHEADER hd = {};
    if (!emf || !GetEnhMetaFileHeader(emf, sizeof(hd), &hd)) return false;
    return hd.nRecords > 2;   // more than the header and the end-of-file record
}

HBITMAP RenderPageBitmap(const Target& t, LONG cpMin, LONG cpMax, SIZE* size)
{
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    int dpiX = std::max(1, GetDeviceCaps(mem, LOGPIXELSX)), dpiY = std::max(1, GetDeviceCaps(mem, LOGPIXELSY));
    size->cx = std::max(1, MulDiv(t.paper.cx, dpiX, 1440));
    size->cy = std::max(1, MulDiv(t.paper.cy, dpiY, 1440));
    HBITMAP bmp = CreateCompatibleBitmap(screen, size->cx, size->cy);
    ReleaseDC(nullptr, screen);
    if (!bmp) { DeleteDC(mem); return nullptr; }
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    RECT all = { 0, 0, size->cx, size->cy };
    FillSolid(mem, all, RGB(255, 255, 255));
    FORMATRANGE fr = {};
    fr.hdc = mem;
    fr.hdcTarget = t.hdc;
    fr.rcPage = { 0, 0, t.paper.cx, t.paper.cy };
    fr.rc = t.text;
    fr.chrg.cpMin = cpMin;
    fr.chrg.cpMax = cpMax;
    SendMessageW(g.hEdit, EM_FORMATRANGE, TRUE, (LPARAM)&fr);
    SendMessageW(g.hEdit, EM_FORMATRANGE, 0, 0);
    SelectObject(mem, old);
    DeleteDC(mem);
    return bmp;
}

PageImage RenderPageImage(const Target& t, LONG cpMin, LONG cpMax)
{
    PageImage img;
    img.emf = RenderPage(t, cpMin, cpMax);
    if (!EmfHasContent(img.emf) && cpMax > cpMin) {
        if (img.emf) { DeleteEnhMetaFile(img.emf); img.emf = nullptr; }
        img.bmp = RenderPageBitmap(t, cpMin, cpMax, &img.bmpSize);
    }
    return img;
}

void DrawPageImage(HDC hdc, const PageImage& img, const RECT& r)
{
    if (img.emf) {
        PlayEnhMetaFile(hdc, img.emf, &r);
    } else if (img.bmp) {
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP old = (HBITMAP)SelectObject(mem, img.bmp);
        int mode = SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, nullptr);
        StretchBlt(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top, mem, 0, 0, img.bmpSize.cx, img.bmpSize.cy, SRCCOPY);
        SetStretchBltMode(hdc, mode);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
}

// Counts the pixels of a rendered page that are not white (a cheap "did anything get drawn" test).
long CountInk(const PageImage& img, int widthPx, int heightPx)
{
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = widthPx;
    bi.bmiHeader.biHeight = -heightPx;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    long ink = 0;
    if (dib && bits) {
        HBITMAP old = (HBITMAP)SelectObject(mem, dib);
        RECT rc = { 0, 0, widthPx, heightPx };
        FillSolid(mem, rc, RGB(255, 255, 255));
        DrawPageImage(mem, img, rc);
        GdiFlush();
        const DWORD* px = (const DWORD*)bits;
        for (int i = 0; i < widthPx * heightPx; ++i)
            if ((px[i] & 0x00FFFFFF) != 0x00FFFFFF) ++ink;
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    return ink;
}

bool InRanges(int page, const PRINTPAGERANGE* ranges, int count)
{
    if (!ranges || count <= 0) return true;
    for (int i = 0; i < count; ++i)
        if (page >= (int)ranges[i].nFromPage && page <= (int)ranges[i].nToPage) return true;
    return false;
}

bool PrintJob(HDC hdc, const std::wstring& docName, const wchar_t* outputFile, LONG cpMin, LONG cpMax,
              const PRINTPAGERANGE* ranges, int rangeCount, int* pagesPrinted)
{
    Target t;
    FillTarget(t, hdc, true);
    ZoomGuard zoomGuard;

    DOCINFOW di = {};
    di.cbSize = sizeof(di);
    di.lpszDocName = docName.c_str();
    di.lpszOutput = outputFile;
    if (StartDocW(hdc, &di) <= 0) return false;

    int lastWanted = 0;
    for (int i = 0; i < rangeCount; ++i) lastWanted = std::max(lastWanted, (int)ranges[i].nToPage);

    FORMATRANGE fr = {};
    fr.hdc = hdc;
    fr.hdcTarget = hdc;
    fr.rcPage = { 0, 0, t.paper.cx, t.paper.cy };
    LONG end = cpMax < 0 ? Editor_GetTextLength() : cpMax;
    LONG cp = cpMin;
    int page = 0, printed = 0;
    bool ok = true;
    do {
        ++page;
        if (rangeCount > 0 && page > lastWanted) break;
        bool wanted = InRanges(page, ranges, rangeCount);
        if (wanted && StartPage(hdc) <= 0) { ok = false; break; }
        fr.rc = TextRectForDevice(t);
        fr.chrg.cpMin = cp;
        fr.chrg.cpMax = end;
        LONG next = (LONG)SendMessageW(g.hEdit, EM_FORMATRANGE, wanted ? TRUE : FALSE, (LPARAM)&fr);
        if (wanted) {
            if (EndPage(hdc) <= 0) { ok = false; break; }
            ++printed;
        }
        if (next <= cp) break;
        cp = next;
    } while (cp < end);
    SendMessageW(g.hEdit, EM_FORMATRANGE, 0, 0);

    if (ok) EndDoc(hdc); else AbortDoc(hdc);
    if (pagesPrinted) *pagesPrinted = printed;
    return ok;
}

int TwipsToDialogUnits(int twips, bool metric) { return metric ? MulDiv(twips, 2540, 1440) : MulDiv(twips, 1000, 1440); }
int DialogUnitsToTwips(int v, bool metric) { return metric ? MulDiv(v, 1440, 2540) : MulDiv(v, 1440, 1000); }

// ---------------------------------------------------------------------------
// Print preview window
// ---------------------------------------------------------------------------
const wchar_t kPreviewClass[] = L"RichPadPreview";
const wchar_t kCanvasClass[] = L"RichPadPreviewCanvas";

enum { BTN_PRINT = 100, BTN_PREV, BTN_NEXT, BTN_PAGES, BTN_ZOOMIN, BTN_ZOOMOUT, BTN_CLOSE, BTN_COUNT_END };
const int kButtonIds[] = { BTN_PRINT, BTN_PREV, BTN_NEXT, BTN_PAGES, BTN_ZOOMIN, BTN_ZOOMOUT, BTN_CLOSE };
const double kZoomLevels[] = { 1.0, 1.5, 2.0, 3.0, 4.0 };

struct Preview {
    HWND hwnd = nullptr;
    HWND hCanvas = nullptr;
    HWND buttons[ARRAYSIZE(kButtonIds)] = {};
    Target target;
    std::vector<LONG> starts;
    std::vector<PageImage> pages;
    std::vector<bool> rendered;
    int page = 0;
    bool twoPages = false;
    int zoom = 0;                 // index into kZoomLevels (0 = whole page)
    POINT scroll = {};
    SIZE content = {};
    bool done = false;
    bool wantPrint = false;
    int barHeight = 0;
    int wheelAccum = 0;
} pv;

int PvDpi() { return pv.hwnd ? DpiOf(pv.hwnd) : 96; }
int PvSc(int v) { return Scale(v, PvDpi()); }
int PageCount() { return (int)pv.starts.size(); }

const PageImage* GetPage(int index)
{
    if (index < 0 || index >= PageCount()) return nullptr;
    if (!pv.rendered[index]) {
        LONG end = index + 1 < PageCount() ? pv.starts[index + 1] : Editor_GetTextLength();
        pv.pages[index] = RenderPageImage(pv.target, pv.starts[index], end);
        pv.rendered[index] = true;
    }
    return &pv.pages[index];
}

void UpdatePreviewButtons()
{
    int shown = pv.twoPages ? 2 : 1;
    EnableWindow(pv.buttons[1], pv.page > 0);
    EnableWindow(pv.buttons[2], pv.page + shown < PageCount());
    EnableWindow(pv.buttons[3], PageCount() > 1);
    EnableWindow(pv.buttons[4], pv.zoom + 1 < (int)ARRAYSIZE(kZoomLevels));
    EnableWindow(pv.buttons[5], pv.zoom > 0);
    SetWindowTextW(pv.buttons[3], S(pv.twoPages ? S_PREVIEW_ONEPAGE : S_PREVIEW_TWOPAGES));
    InvalidateRect(pv.hwnd, nullptr, FALSE);
}

// Computes the on-screen rectangles of the visible pages (canvas coordinates, before scrolling).
void ComputePageRects(RECT rects[2], int* count)
{
    RECT rc;
    GetClientRect(pv.hCanvas, &rc);
    int pad = PvSc(20), gap = PvSc(20);
    int shown = (pv.twoPages && PageCount() > 1) ? 2 : 1;
    double aspect = (double)pv.target.paper.cy / std::max<LONG>(1, pv.target.paper.cx);
    int availW = std::max(50, (int)rc.right - 2 * pad - (shown - 1) * gap) / shown;
    int availH = std::max(50, (int)rc.bottom - 2 * pad);
    int w = availW, h = (int)(w * aspect);
    if (h > availH) { h = availH; w = (int)(h / aspect); }
    double z = kZoomLevels[pv.zoom];
    w = (int)(w * z);
    h = (int)(h * z);
    int totalW = shown * w + (shown - 1) * gap;
    pv.content = { totalW + 2 * pad, h + 2 * pad };
    int x0 = std::max(pad, ((int)rc.right - totalW) / 2);
    int y0 = std::max(pad, ((int)rc.bottom - h) / 2);
    for (int i = 0; i < shown; ++i) rects[i] = { x0 + i * (w + gap), y0, x0 + i * (w + gap) + w, y0 + h };
    *count = shown;
}

void UpdateScrollBars()
{
    RECT rects[2];
    int count;
    RECT rc;
    // Showing a scroll bar changes the client size, so iterate until the layout is stable.
    for (int pass = 0; pass < 3; ++pass) {
        GetClientRect(pv.hCanvas, &rc);
        ComputePageRects(rects, &count);
        SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
        si.nMin = 0;
        si.nMax = pv.content.cx - 1;
        si.nPage = rc.right;
        pv.scroll.x = std::clamp((int)pv.scroll.x, 0, std::max(0, (int)pv.content.cx - (int)rc.right));
        si.nPos = pv.scroll.x;
        SetScrollInfo(pv.hCanvas, SB_HORZ, &si, TRUE);
        si.nMax = pv.content.cy - 1;
        si.nPage = rc.bottom;
        pv.scroll.y = std::clamp((int)pv.scroll.y, 0, std::max(0, (int)pv.content.cy - (int)rc.bottom));
        si.nPos = pv.scroll.y;
        SetScrollInfo(pv.hCanvas, SB_VERT, &si, TRUE);
        RECT after;
        GetClientRect(pv.hCanvas, &after);
        if (EqualRect(&rc, &after)) break;
    }
    InvalidateRect(pv.hCanvas, nullptr, FALSE);
}

void GoToPage(int page)
{
    int shown = pv.twoPages ? 2 : 1;
    page = std::clamp(page, 0, std::max(0, PageCount() - shown));
    if (pv.twoPages) page -= page % 2;
    pv.page = std::max(0, page);
    pv.scroll.y = 0;
    UpdateScrollBars();
    UpdatePreviewButtons();
}

void SetPreviewZoom(int zoom)
{
    zoom = std::clamp(zoom, 0, (int)ARRAYSIZE(kZoomLevels) - 1);
    if (zoom == pv.zoom) return;
    pv.zoom = zoom;
    if (zoom == 0) pv.scroll = { 0, 0 };
    UpdateScrollBars();
    UpdatePreviewButtons();
}

void PaintCanvas(HDC hdcWnd)
{
    RECT rc;
    GetClientRect(pv.hCanvas, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC hdc = CreateCompatibleDC(hdcWnd);
    HBITMAP bmp = CreateCompatibleBitmap(hdcWnd, rc.right, rc.bottom);
    HBITMAP oldBmp = (HBITMAP)SelectObject(hdc, bmp);
    FillSolid(hdc, rc, T.workspace);

    RECT rects[2];
    int count = 0;
    ComputePageRects(rects, &count);
    for (int i = 0; i < count; ++i) {
        RECT r = rects[i];
        OffsetRect(&r, -pv.scroll.x, -pv.scroll.y);
        RECT shadow = r;
        OffsetRect(&shadow, PvSc(3), PvSc(3));
        FillSolid(hdc, shadow, Blend(RGB(0, 0, 0), T.workspace, 22));
        FillSolid(hdc, r, RGB(255, 255, 255));
        if (const PageImage* img = GetPage(pv.page + i)) {
            int saved = SaveDC(hdc);
            IntersectClipRect(hdc, r.left, r.top, r.right, r.bottom);
            DrawPageImage(hdc, *img, r);
            RestoreDC(hdc, saved);
        }
        FrameRect(hdc, &r, Theme_Brush(T.dark ? RGB(90, 90, 90) : RGB(170, 170, 170)));
    }
    BitBlt(hdcWnd, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

void ScrollCanvas(int bar, int code, int thumb)
{
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    GetScrollInfo(pv.hCanvas, bar, &si);
    int pos = si.nPos, line = PvSc(40), pg = (int)si.nPage;
    switch (code) {
    case SB_LINEUP: pos -= line; break;
    case SB_LINEDOWN: pos += line; break;
    case SB_PAGEUP: pos -= pg; break;
    case SB_PAGEDOWN: pos += pg; break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
    case SB_TOP: pos = 0; break;
    case SB_BOTTOM: pos = si.nMax; break;
    }
    (void)thumb;
    pos = std::clamp(pos, 0, std::max(0, si.nMax - (int)si.nPage + 1));
    if (bar == SB_VERT) pv.scroll.y = pos; else pv.scroll.x = pos;
    UpdateScrollBars();
}

LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintCanvas(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (pv.hCanvas) UpdateScrollBars();
        return 0;
    case WM_VSCROLL: ScrollCanvas(SB_VERT, LOWORD(wParam), HIWORD(wParam)); return 0;
    case WM_HSCROLL: ScrollCanvas(SB_HORZ, LOWORD(wParam), HIWORD(wParam)); return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        SetPreviewZoom(pv.zoom == 0 ? 2 : 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void LayoutPreview()
{
    RECT rc;
    GetClientRect(pv.hwnd, &rc);
    int dpi = PvDpi();
    pv.barHeight = Scale(48, dpi);
    HFONT font = Theme_Font(dpi);
    HDC hdc = GetDC(pv.hwnd);
    HFONT old = (HFONT)SelectObject(hdc, font);
    int x = Scale(10, dpi), h = Scale(30, dpi), y = (pv.barHeight - h) / 2;
    for (size_t i = 0; i < ARRAYSIZE(kButtonIds); ++i) {
        wchar_t text[96];
        GetWindowTextW(pv.buttons[i], text, ARRAYSIZE(text));
        int w = Scale(60, dpi);
        // The page-mode button changes its caption, so reserve room for the longer one.
        const wchar_t* candidates[2] = { text, nullptr };
        if (kButtonIds[i] == BTN_PAGES) { candidates[0] = S(S_PREVIEW_ONEPAGE); candidates[1] = S(S_PREVIEW_TWOPAGES); }
        for (const wchar_t* c : candidates) {
            if (!c) continue;
            std::wstring plain = StripMnemonic(c);
            RECT calc = { 0, 0, 0, 0 };
            DrawTextW(hdc, plain.c_str(), -1, &calc, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
            w = std::max(w, (int)calc.right + Scale(28, dpi));
        }
        SendMessageW(pv.buttons[i], WM_SETFONT, (WPARAM)font, TRUE);
        if (kButtonIds[i] == BTN_CLOSE) {
            SetWindowPos(pv.buttons[i], nullptr, std::max(x, (int)rc.right - w - Scale(10, dpi)), y, w, h, SWP_NOZORDER);
        } else {
            SetWindowPos(pv.buttons[i], nullptr, x, y, w, h, SWP_NOZORDER);
            x += w + Scale(6, dpi);
            if (kButtonIds[i] == BTN_PRINT || kButtonIds[i] == BTN_NEXT || kButtonIds[i] == BTN_PAGES) x += Scale(10, dpi);
        }
    }
    SelectObject(hdc, old);
    ReleaseDC(pv.hwnd, hdc);
    SetWindowPos(pv.hCanvas, nullptr, 0, pv.barHeight, rc.right, std::max(0, (int)rc.bottom - pv.barHeight), SWP_NOZORDER);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        pv.hwnd = hwnd;
        const int captions[] = { S_PREVIEW_PRINT, S_PREVIEW_PREV, S_PREVIEW_NEXT, S_PREVIEW_TWOPAGES,
                                 S_PREVIEW_ZOOMIN, S_PREVIEW_ZOOMOUT, S_PREVIEW_CLOSE };
        for (size_t i = 0; i < ARRAYSIZE(kButtonIds); ++i) {
            pv.buttons[i] = CreateWindowExW(0, L"BUTTON", S(captions[i]), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)kButtonIds[i], g.hInst, nullptr);
            Theme_ApplyControl(pv.buttons[i], L"Explorer");
        }
        pv.hCanvas = CreateWindowExW(0, kCanvasClass, L"", WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL,
                                     0, 0, 10, 10, hwnd, nullptr, g.hInst, nullptr);
        Theme_ApplyControl(pv.hCanvas, L"Explorer");
        Theme_ApplyTitleBar(hwnd);
        return 0;
    }
    case WM_SIZE:
        LayoutPreview();
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.bottom = pv.barHeight;
        FillSolid((HDC)wParam, rc, T.base);
        DrawLineH((HDC)wParam, 0, rc.right, pv.barHeight - 1, T.line);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        // "Page x of y" between the buttons and the close button.
        RECT rc, rClose, rZoom;
        GetClientRect(hwnd, &rc);
        GetWindowRect(pv.buttons[6], &rClose);
        GetWindowRect(pv.buttons[5], &rZoom);
        MapWindowPoints(nullptr, hwnd, (POINT*)&rClose, 2);
        MapWindowPoints(nullptr, hwnd, (POINT*)&rZoom, 2);
        RECT rt = { rZoom.right + PvSc(12), 0, rClose.left - PvSc(12), pv.barHeight };
        FillSolid(hdc, rt, T.base);
        if (rt.right > rt.left) {
            HFONT old = (HFONT)SelectObject(hdc, Theme_Font(PvDpi()));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, T.textDim);
            std::wstring label = Fmt(S(S_PREVIEW_PAGE), pv.page + 1, PageCount());
            DrawTextW(hdc, label.c_str(), -1, &rt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(hdc, old);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case BTN_PRINT: pv.wantPrint = true; pv.done = true; break;
        case BTN_PREV: GoToPage(pv.page - (pv.twoPages ? 2 : 1)); break;
        case BTN_NEXT: GoToPage(pv.page + (pv.twoPages ? 2 : 1)); break;
        case BTN_PAGES: pv.twoPages = !pv.twoPages; GoToPage(pv.page); break;
        case BTN_ZOOMIN: SetPreviewZoom(pv.zoom + 1); break;
        case BTN_ZOOMOUT: SetPreviewZoom(pv.zoom - 1); break;
        case BTN_CLOSE: case IDCANCEL: pv.done = true; break;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        pv.wheelAccum += GET_WHEEL_DELTA_WPARAM(wParam);
        int steps = pv.wheelAccum / WHEEL_DELTA;
        if (!steps) return 0;
        pv.wheelAccum -= steps * WHEEL_DELTA;
        RECT rc;
        GetClientRect(pv.hCanvas, &rc);
        if (pv.content.cy > rc.bottom) {
            int maxY = pv.content.cy - rc.bottom;
            int next = pv.scroll.y - steps * PvSc(80);
            if (next < 0 && pv.scroll.y == 0) GoToPage(pv.page - (pv.twoPages ? 2 : 1));
            else if (next > maxY && pv.scroll.y >= maxY) GoToPage(pv.page + (pv.twoPages ? 2 : 1));
            else { pv.scroll.y = std::clamp(next, 0, maxY); UpdateScrollBars(); }
        } else {
            GoToPage(pv.page - steps * (pv.twoPages ? 2 : 1));
        }
        return 0;
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wParam, T.base);
        return (LRESULT)Theme_Brush(T.base);
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = PvSc(640);
        mmi->ptMinTrackSize.y = PvSc(400);
        return 0;
    }
    case WM_CLOSE:
        pv.done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool PreviewKey(const MSG& msg)
{
    if (msg.message != WM_KEYDOWN) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    int step = pv.twoPages ? 2 : 1;
    switch (msg.wParam) {
    case VK_ESCAPE: pv.done = true; return true;
    case VK_PRIOR: case VK_LEFT: GoToPage(pv.page - step); return true;
    case VK_NEXT: case VK_RIGHT: GoToPage(pv.page + step); return true;
    case VK_HOME: GoToPage(0); return true;
    case VK_END: GoToPage(PageCount() - 1); return true;
    case VK_UP: ScrollCanvas(SB_VERT, SB_LINEUP, 0); return true;
    case VK_DOWN: ScrollCanvas(SB_VERT, SB_LINEDOWN, 0); return true;
    case VK_ADD: case VK_OEM_PLUS: SetPreviewZoom(pv.zoom + 1); return true;
    case VK_SUBTRACT: case VK_OEM_MINUS: SetPreviewZoom(pv.zoom - 1); return true;
    case 'P': if (ctrl) { pv.wantPrint = true; pv.done = true; return true; } break;
    }
    return false;
}

std::wstring PrintDocName() { return DocDisplayName() + L" - " + APP_NAME; }

} // namespace

// ===========================================================================

bool Print_Register()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PreviewProc;
    wc.hInstance = g.hInst;
    wc.hIcon = LoadIconW(g.hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPreviewClass;
    if (!RegisterClassExW(&wc)) return false;
    WNDCLASSEXW wcc = {};
    wcc.cbSize = sizeof(wcc);
    wcc.lpfnWndProc = CanvasProc;
    wcc.hInstance = g.hInst;
    wcc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcc.lpszClassName = kCanvasClass;
    return RegisterClassExW(&wcc) != 0;
}

void Print_PageSetup(HWND owner)
{
    bool metric = g.cfg.units != UNIT_INCH;
    PAGESETUPDLGW psd = {};
    psd.lStructSize = sizeof(psd);
    psd.hwndOwner = owner;
    psd.hDevMode = g.hDevMode;
    psd.hDevNames = g.hDevNames;
    psd.Flags = PSD_MARGINS | (metric ? PSD_INHUNDREDTHSOFMILLIMETERS : PSD_INTHOUSANDTHSOFINCHES);
    psd.rtMargin = { TwipsToDialogUnits(g.cfg.margins.left, metric), TwipsToDialogUnits(g.cfg.margins.top, metric),
                     TwipsToDialogUnits(g.cfg.margins.right, metric), TwipsToDialogUnits(g.cfg.margins.bottom, metric) };
    if (!PageSetupDlgW(&psd)) return;
    g.hDevMode = psd.hDevMode;
    g.hDevNames = psd.hDevNames;
    g.cfg.margins = { DialogUnitsToTwips(psd.rtMargin.left, metric), DialogUnitsToTwips(psd.rtMargin.top, metric),
                      DialogUnitsToTwips(psd.rtMargin.right, metric), DialogUnitsToTwips(psd.rtMargin.bottom, metric) };
    if (psd.ptPaperSize.x > 0 && psd.ptPaperSize.y > 0)
        g.cfg.paper = { DialogUnitsToTwips(psd.ptPaperSize.x, metric), DialogUnitsToTwips(psd.ptPaperSize.y, metric) };
    Editor_ApplyWrap();
    Ruler_Update();
    Settings_Save();
}

void Print_Print(HWND owner, bool showDialog)
{
    CHARRANGE sel;
    Editor_GetSel(sel);
    bool hasSel = sel.cpMin != sel.cpMax;

    if (!showDialog) {
        HDC hdc = CreatePrinterDC();
        if (!hdc) { MsgBox(owner, S(S_MSG_NOPRINTER), MB_OK | MB_ICONWARNING); return; }
        bool ok = PrintJob(hdc, PrintDocName(), nullptr, 0, -1, nullptr, 0, nullptr);
        DeleteDC(hdc);
        if (!ok) MsgBox(owner, S(S_MSG_PRINTFAILED), MB_OK | MB_ICONERROR);
        return;
    }

    PRINTPAGERANGE ranges[8] = {};
    PRINTDLGEXW pd = {};
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = owner;
    pd.hDevMode = g.hDevMode;
    pd.hDevNames = g.hDevNames;
    pd.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOCURRENTPAGE | PD_ALLPAGES | (hasSel ? 0 : PD_NOSELECTION);
    pd.nMaxPageRanges = ARRAYSIZE(ranges);
    pd.lpPageRanges = ranges;
    pd.nMinPage = 1;
    pd.nMaxPage = 9999;
    pd.nCopies = 1;
    pd.nStartPage = START_PAGE_GENERAL;
    HRESULT hr = PrintDlgExW(&pd);
    HDC hdc = nullptr;
    DWORD flags = 0;
    int rangeCount = 0;
    if (hr == S_OK) {
        g.hDevMode = pd.hDevMode;
        g.hDevNames = pd.hDevNames;
        if (pd.dwResultAction != PD_RESULT_PRINT) { if (pd.hDC) DeleteDC(pd.hDC); return; }
        hdc = pd.hDC;
        flags = pd.Flags;
        rangeCount = (int)pd.nPageRanges;
    } else {
        // Fall back to the classic dialog (for example when the property-sheet dialog is unavailable).
        PRINTDLGW old = {};
        old.lStructSize = sizeof(old);
        old.hwndOwner = owner;
        old.hDevMode = g.hDevMode;
        old.hDevNames = g.hDevNames;
        old.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_ALLPAGES | (hasSel ? 0 : PD_NOSELECTION);
        old.nMinPage = 1;
        old.nMaxPage = 9999;
        old.nFromPage = 1;
        old.nToPage = 1;
        old.nCopies = 1;
        if (!PrintDlgW(&old)) {
            DWORD e = CommDlgExtendedError();
            if (e == PDERR_NODEFAULTPRN || e == PDERR_NODEVICES) MsgBox(owner, S(S_MSG_NOPRINTER), MB_OK | MB_ICONWARNING);
            return;
        }
        g.hDevMode = old.hDevMode;
        g.hDevNames = old.hDevNames;
        hdc = old.hDC;
        flags = old.Flags;
        if (flags & PD_PAGENUMS) {
            ranges[0].nFromPage = old.nFromPage;
            ranges[0].nToPage = old.nToPage;
            rangeCount = 1;
        }
    }
    if (!hdc) { MsgBox(owner, S(S_MSG_PRINTFAILED), MB_OK | MB_ICONERROR); return; }

    bool selection = (flags & PD_SELECTION) && hasSel;
    bool usePages = (flags & PD_PAGENUMS) && rangeCount > 0;
    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = PrintJob(hdc, PrintDocName(), nullptr, selection ? sel.cpMin : 0, selection ? sel.cpMax : -1,
                       usePages ? ranges : nullptr, usePages ? rangeCount : 0, nullptr);
    SetCursor(oldCursor);
    DeleteDC(hdc);
    if (!ok) MsgBox(owner, S(S_MSG_PRINTFAILED), MB_OK | MB_ICONERROR);
}

bool Print_ToPrinter(const wchar_t* printerName, const wchar_t* outputFile, int* pages)
{
    HDC hdc = nullptr;
    if (printerName && *printerName) hdc = CreateDCW(L"WINSPOOL", printerName, nullptr, nullptr);
    else hdc = CreatePrinterDC();
    if (!hdc) return false;
    bool ok = PrintJob(hdc, PrintDocName(), outputFile, 0, -1, nullptr, 0, pages);
    DeleteDC(hdc);
    return ok;
}

int Print_CountPages()
{
    Target t;
    if (!OpenLayoutTarget(t)) return 0;
    std::vector<LONG> starts;
    {
        ZoomGuard zoomGuard;
        Paginate(t, 0, -1, starts);
    }
    CloseTarget(t);
    return (int)starts.size();
}

bool Print_RenderPagePng(int pageIndex, int widthPx, const wchar_t* pngPath)
{
    Target t;
    if (!OpenLayoutTarget(t)) return false;
    bool ok = false;
    {
        ZoomGuard zoomGuard;
        std::vector<LONG> starts;
        Paginate(t, 0, -1, starts);
        if (pageIndex >= 0 && pageIndex < (int)starts.size()) {
            LONG end = pageIndex + 1 < (int)starts.size() ? starts[pageIndex + 1] : Editor_GetTextLength();
            PageImage img = RenderPageImage(t, starts[pageIndex], end);
            int heightPx = MulDiv(widthPx, t.paper.cy, std::max<LONG>(1, t.paper.cx));
            HDC screen = GetDC(nullptr);
            HDC mem = CreateCompatibleDC(screen);
            HBITMAP bmp = CreateCompatibleBitmap(screen, widthPx, heightPx);
            HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
            RECT rc = { 0, 0, widthPx, heightPx };
            FillSolid(mem, rc, RGB(255, 255, 255));
            DrawPageImage(mem, img, rc);
            SelectObject(mem, old);
            std::vector<BYTE> png;
            if (Image_FromHBitmap(bmp, png)) ok = File_WriteAll(pngPath, png.data(), png.size(), nullptr);
            DeleteObject(bmp);
            DeleteDC(mem);
            ReleaseDC(nullptr, screen);
            FreePage(img);
        }
    }
    CloseTarget(t);
    return ok;
}

bool Print_Probe(PrintProbe& out)
{
    out = PrintProbe();
    Target t;
    if (!OpenLayoutTarget(t)) return false;
    {
        ZoomGuard zoomGuard;
        std::vector<LONG> starts;
        Paginate(t, 0, -1, starts);
        out.pages = (int)starts.size();
        out.usesPrinter = t.printer;
        out.dpi = t.dpiX;
        out.paper = t.paper;
        LONG end = starts.size() > 1 ? starts[1] : Editor_GetTextLength();
        int w = 400, h = MulDiv(w, t.paper.cy, std::max<LONG>(1, t.paper.cx));
        PageImage emf;
        emf.emf = RenderPage(t, starts[0], end);
        ENHMETAHEADER hd = {};
        if (emf.emf && GetEnhMetaFileHeader(emf.emf, sizeof(hd), &hd)) out.emfRecords = (int)hd.nRecords;
        out.inkEmf = CountInk(emf, w, h);
        FreePage(emf);
        PageImage bmp;
        bmp.bmp = RenderPageBitmap(t, starts[0], end, &bmp.bmpSize);
        out.inkBitmap = CountInk(bmp, w, h);
        FreePage(bmp);
    }
    CloseTarget(t);
    return true;
}

void Print_Preview(HWND owner)
{
    if (g.hPreview) return;
    pv = Preview();
    if (!OpenLayoutTarget(pv.target)) return;

    bool wantPrint = false;
    {
        ZoomGuard zoomGuard;
        HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        Paginate(pv.target, 0, -1, pv.starts);
        pv.pages.assign(pv.starts.size(), PageImage());
        pv.rendered.assign(pv.starts.size(), false);
        SetCursor(oldCursor);

        RECT rc;
        GetWindowRect(owner, &rc);
        bool maximized = IsZoomed(owner) != 0;
        std::wstring title = DocDisplayName() + L" - " + S(S_PREVIEW_TITLE);
        HWND hwnd = CreateWindowExW(0, kPreviewClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                    rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, owner, nullptr, g.hInst, nullptr);
        if (hwnd) {
            g.hPreview = hwnd;
            EnableWindow(owner, FALSE);
            ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
            LayoutPreview();
            GoToPage(0);
            SetFocus(pv.hCanvas);

            MSG msg;
            while (!pv.done) {
                BOOL r = GetMessageW(&msg, nullptr, 0, 0);
                if (r <= 0) { if (r == 0) PostQuitMessage((int)msg.wParam); break; }
                if (PreviewKey(msg)) continue;
                if (IsDialogMessageW(hwnd, &msg)) continue;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            EnableWindow(owner, TRUE);
            SetForegroundWindow(owner);
            DestroyWindow(hwnd);
            g.hPreview = nullptr;
        }
        for (PageImage& img : pv.pages) FreePage(img);
        wantPrint = pv.wantPrint;
        CloseTarget(pv.target);
        pv = Preview();
    }
    if (wantPrint) Print_Print(owner, true);
}
