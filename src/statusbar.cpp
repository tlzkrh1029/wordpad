// Status bar with document statistics and the zoom slider.
#include "statusbar.h"
#include "theme.h"
#include "icons.h"
#include "gfx.h"

namespace {

const wchar_t kClassName[] = L"RichPadStatusBar";
const int kZoomMin = 10, kZoomMax = 500;

enum Zone { Z_NONE, Z_MINUS, Z_PLUS, Z_TRACK, Z_PERCENT };

struct State {
    HWND hwnd = nullptr;
    HWND hTip = nullptr;
    std::vector<std::wstring> parts;
    int zoom = 100;
    Zone hot = Z_NONE;
    Zone pressed = Z_NONE;
    bool tracking = false;
    RECT rcMinus = {}, rcPlus = {}, rcTrack = {}, rcPercent = {};
} s;

int Dpi() { return s.hwnd ? DpiOf(s.hwnd) : 96; }
int Sc(int v) { return Scale(v, Dpi()); }

double FracFromZoom(int zoom)
{
    zoom = std::clamp(zoom, kZoomMin, kZoomMax);
    if (zoom <= 100) return 0.5 * (zoom - kZoomMin) / (100.0 - kZoomMin);
    return 0.5 + 0.5 * (zoom - 100) / (kZoomMax - 100.0);
}

int ZoomFromFrac(double f)
{
    f = std::clamp(f, 0.0, 1.0);
    if (f > 0.47 && f < 0.53) return 100;   // snap to the centre
    double z = f <= 0.5 ? kZoomMin + (100.0 - kZoomMin) * (f / 0.5) : 100 + (kZoomMax - 100.0) * ((f - 0.5) / 0.5);
    int step = z < 100 ? 5 : 10;
    return std::clamp(((int)(z / step + 0.5)) * step, kZoomMin, kZoomMax);
}

void Layout()
{
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    int btn = Sc(22), y = (rc.bottom - btn) / 2;
    int x = rc.right - Sc(10);
    s.rcPlus = { x - btn, y, x, y + btn };
    x -= btn + Sc(4);
    s.rcTrack = { x - Sc(110), y, x, y + btn };
    x -= Sc(110) + Sc(4);
    s.rcMinus = { x - btn, y, x, y + btn };
    x -= btn + Sc(6);
    s.rcPercent = { x - Sc(48), y, x, y + btn };

    if (s.hTip) {
        struct { UINT_PTR id; const RECT* rc; } tools[] = { { 1, &s.rcMinus }, { 2, &s.rcPlus }, { 3, &s.rcPercent } };
        for (auto& t : tools) {
            TOOLINFOW ti = {};
            ti.cbSize = sizeof(ti);
            ti.hwnd = s.hwnd;
            ti.uId = t.id;
            ti.rect = *t.rc;
            SendMessageW(s.hTip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
    }
}

Zone HitTest(POINT pt)
{
    if (PtInRect(&s.rcMinus, pt)) return Z_MINUS;
    if (PtInRect(&s.rcPlus, pt)) return Z_PLUS;
    if (PtInRect(&s.rcTrack, pt)) return Z_TRACK;
    if (PtInRect(&s.rcPercent, pt)) return Z_PERCENT;
    return Z_NONE;
}

void Paint(HDC hdcWnd)
{
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC hdc = CreateCompatibleDC(hdcWnd);
    HBITMAP bmp = CreateCompatibleBitmap(hdcWnd, rc.right, rc.bottom);
    HBITMAP oldBmp = (HBITMAP)SelectObject(hdc, bmp);
    HFONT oldFont = (HFONT)SelectObject(hdc, Theme_Font(Dpi()));
    SetBkMode(hdc, TRANSPARENT);
    FillSolid(hdc, rc, T.base);
    DrawLineH(hdc, 0, rc.right, 0, T.line);

    // Left: document statistics.
    int x = Sc(12);
    int limit = s.rcPercent.left - Sc(12);
    SetTextColor(hdc, T.textDim);
    for (size_t i = 0; i < s.parts.size(); ++i) {
        RECT rt = { x, 1, limit, rc.bottom };
        if (x >= limit) break;
        RECT calc = { 0, 0, 0, 0 };
        DrawTextW(hdc, s.parts[i].c_str(), -1, &calc, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
        DrawTextW(hdc, s.parts[i].c_str(), -1, &rt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        x += calc.right + Sc(11);
        if (i + 1 < s.parts.size() && x < limit) {
            DrawLineV(hdc, x, Sc(8), rc.bottom - Sc(7), T.checkedLine);
            x += Sc(12);
        }
    }

    // Right: zoom controls.
    wchar_t buf[16];
    wsprintfW(buf, L"%d%%", s.zoom);
    if (s.hot == Z_PERCENT) DrawRoundRect(hdc, s.rcPercent, Sc(4), s.pressed == Z_PERCENT ? T.pressed : T.hover, CLR_NONE);
    SetTextColor(hdc, T.text);
    RECT rp = s.rcPercent;
    DrawTextW(hdc, buf, -1, &rp, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);

    struct { Zone z; const RECT* r; int icon; } buttons[] = { { Z_MINUS, &s.rcMinus, ICON_ZOOM_OUT }, { Z_PLUS, &s.rcPlus, ICON_ZOOM_IN } };
    for (auto& b : buttons) {
        if (s.hot == b.z) DrawRoundRect(hdc, *b.r, Sc(4), s.pressed == b.z ? T.pressed : T.hover, CLR_NONE);
        int isz = Sc(16);
        Icon_Draw(hdc, b.icon, (b.r->left + b.r->right - isz) / 2, (b.r->top + b.r->bottom - isz) / 2, isz, T.text);
    }

    // Slider.
    int pad = Sc(8);
    int tx0 = s.rcTrack.left + pad, tx1 = s.rcTrack.right - pad;
    int midY = (s.rcTrack.top + s.rcTrack.bottom) / 2;
    RECT track = { tx0, midY - Sc(2), tx1, midY + Sc(2) };
    DrawRoundRect(hdc, track, Sc(2), T.checkedLine, CLR_NONE);
    int cx = (tx0 + tx1) / 2;
    DrawLineV(hdc, cx, midY - Sc(6), midY - Sc(3), T.textDim);
    DrawLineV(hdc, cx, midY + Sc(3), midY + Sc(6), T.textDim);
    int thumbX = tx0 + (int)((tx1 - tx0) * FracFromZoom(s.zoom) + 0.5);
    {
        Gdiplus::Graphics gfx(hdc);
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        float r = (float)Sc(s.tracking || s.hot == Z_TRACK ? 7 : 6);
        Gdiplus::SolidBrush ring(GpColor(T.dark ? RGB(69, 69, 69) : RGB(255, 255, 255)));
        Gdiplus::Pen edge(GpColor(T.checkedLine), 1.0f);
        gfx.FillEllipse(&ring, thumbX - r - 2, midY - r - 2, 2 * r + 4, 2 * r + 4);
        gfx.DrawEllipse(&edge, thumbX - r - 2, midY - r - 2, 2 * r + 4, 2 * r + 4);
        Gdiplus::SolidBrush dot(GpColor(T.accent));
        float ir = r - 1.5f;
        gfx.FillEllipse(&dot, thumbX - ir, midY - ir, 2 * ir, 2 * ir);
    }

    BitBlt(hdcWnd, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldFont);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

void RequestZoom(int percent)
{
    percent = std::clamp(percent, kZoomMin, kZoomMax);
    SendMessageW(g.hMain, WM_APP_SETZOOM, (WPARAM)percent, 0);
}

void ZoomFromPoint(int x)
{
    int pad = Sc(8);
    int tx0 = s.rcTrack.left + pad, tx1 = s.rcTrack.right - pad;
    if (tx1 <= tx0) return;
    RequestZoom(ZoomFromFrac((double)(x - tx0) / (tx1 - tx0)));
}

void CreateTooltips()
{
    s.hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, s.hwnd, nullptr, g.hInst, nullptr);
    for (UINT_PTR id = 1; id <= 3; ++id) {
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = s.hwnd;
        ti.uId = id;
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(s.hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    Theme_ApplyTooltip(s.hTip);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        s.hwnd = hwnd;
        CreateTooltips();
        return 0;
    case WM_SIZE:
        Layout();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Paint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (s.tracking) { ZoomFromPoint(pt.x); return 0; }
        Zone z = HitTest(pt);
        if (z != s.hot) {
            s.hot = z;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (s.hot != Z_NONE) { s.hot = Z_NONE; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        Zone z = HitTest(pt);
        s.pressed = z;
        if (z == Z_TRACK) {
            s.tracking = true;
            SetCapture(hwnd);
            ZoomFromPoint(pt.x);
        } else if (z == Z_MINUS) {
            RequestZoom(((s.zoom + 9) / 10 - 1) * 10);
        } else if (z == Z_PLUS) {
            RequestZoom((s.zoom / 10 + 1) * 10);
        } else if (z == Z_PERCENT) {
            RequestZoom(100);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (s.tracking) { s.tracking = false; ReleaseCapture(); }
        s.pressed = Z_NONE;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_CAPTURECHANGED:
        s.tracking = false;
        s.pressed = Z_NONE;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->hwndFrom == s.hTip && hdr->code == TTN_GETDISPINFOW) {
            NMTTDISPINFOW* di = (NMTTDISPINFOW*)lParam;
            int id = (int)hdr->idFrom;
            di->lpszText = (LPWSTR)S(id == 1 ? S_TIP_ZOOMOUT : (id == 2 ? S_TIP_ZOOMIN : S_TIP_ZOOMRESET));
            return 0;
        }
        break;
    }
    case WM_DESTROY:
        s.hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool StatusBar_Register()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g.hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc) != 0;
}

HWND StatusBar_Create(HWND parent)
{
    return CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 100, 28,
                           parent, nullptr, g.hInst, nullptr);
}

int StatusBar_GetHeight() { return Scale(28, s.hwnd ? DpiOf(s.hwnd) : 96); }

void StatusBar_SetParts(const std::vector<std::wstring>& parts)
{
    if (parts == s.parts) return;
    s.parts = parts;
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

std::wstring StatusBar_GetText()
{
    std::wstring text;
    for (const std::wstring& part : s.parts) {
        if (!text.empty()) text += L" | ";
        text += part;
    }
    return text;
}

void StatusBar_SetZoom(int percent)
{
    if (percent == s.zoom) return;
    s.zoom = percent;
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void StatusBar_Rebuild()
{
    if (!s.hwnd) return;
    Theme_ApplyTooltip(s.hTip);
    Layout();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
