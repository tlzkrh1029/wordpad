// The horizontal ruler: indent markers, tab stops and the printable width.
#include "ruler.h"
#include "theme.h"
#include "editor.h"
#include <cmath>

namespace {

const wchar_t kClassName[] = L"RichPadRuler";

enum DragKind { DRAG_NONE, DRAG_FIRST, DRAG_HANGING, DRAG_LEFTBOX, DRAG_RIGHT, DRAG_TAB };

struct State {
    HWND hwnd = nullptr;
    PARAFORMAT2 pf = {};
    DragKind drag = DRAG_NONE;
    int dragTab = -1;          // index into pf.rgxTabs
    bool dragRemove = false;   // tab dragged off the ruler
    PARAFORMAT2 dragPf = {};   // live values while dragging
    int guideX = -1;           // XOR guide line currently drawn in the editor (-1 = none)
} s;

int Dpi() { return s.hwnd ? DpiOf(s.hwnd) : 96; }
int Sc(int v) { return Scale(v, Dpi()); }

// Width (twips) of the area in which paragraphs are laid out.
int ExtentTwips()
{
    if (g.cfg.wrapMode == WRAP_WINDOW && g.hEdit) {
        RECT rc = {};
        SendMessageW(g.hEdit, EM_GETRECT, 0, (LPARAM)&rc);
        double ppt = Editor_PixelsPerTwip();
        int px = (rc.right - rc.left) - g.textOriginDelta;
        if (ppt > 0 && px > 0) return std::max(720, (int)(px / ppt));
    }
    return Editor_PageWidthTwips();
}

int XFromTwips(int tw) { return Editor_OriginX() + (int)std::lround(tw * Editor_PixelsPerTwip()); }

int TwipsFromX(int x, bool snap)
{
    double ppt = Editor_PixelsPerTwip();
    if (ppt <= 0) return 0;
    double tw = (x - Editor_OriginX()) / ppt;
    if (snap) {
        double step = Measure_SnapStep();
        tw = std::floor(tw / step + 0.5) * step;
    }
    return (int)std::lround(tw);
}

void DrawGuide(int x)
{
    // XOR line across the editor so the user can see where the indent will land.
    if (!g.hEdit) return;
    RECT rc;
    GetClientRect(g.hEdit, &rc);
    HDC hdc = GetDC(g.hEdit);
    PatBlt(hdc, x, 0, 1, rc.bottom, DSTINVERT);
    ReleaseDC(g.hEdit, hdc);
}

void SetGuide(int x)
{
    if (x == s.guideX) return;
    if (s.guideX >= 0) DrawGuide(s.guideX);
    s.guideX = x;
    if (s.guideX >= 0) DrawGuide(s.guideX);
}

struct Geometry {
    int height, bandTop, bandBottom;
    int xFirst, xLeft, xRight, x0, x1;
};

Geometry GetGeometry(const PARAFORMAT2& pf)
{
    Geometry geo;
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    geo.height = rc.bottom;
    geo.bandTop = Sc(6);
    geo.bandBottom = rc.bottom - Sc(6);
    int extent = ExtentTwips();
    geo.x0 = XFromTwips(0);
    geo.x1 = XFromTwips(extent);
    geo.xFirst = XFromTwips(pf.dxStartIndent);
    geo.xLeft = XFromTwips(pf.dxStartIndent + pf.dxOffset);
    geo.xRight = XFromTwips(extent - pf.dxRightIndent);
    return geo;
}

void DrawMarker(HDC hdc, int x, int y, bool pointDown, bool withBox, bool dim)
{
    int hw = Sc(4), body = Sc(3), tip = Sc(4);
    COLORREF line = dim ? T.textDisabled : T.rulerMarker;
    HPEN pen = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, Theme_Brush(T.rulerMarkerFill));
    POINT pts[5];
    if (pointDown) {
        pts[0] = { x - hw, y }; pts[1] = { x + hw, y }; pts[2] = { x + hw, y + body };
        pts[3] = { x, y + body + tip }; pts[4] = { x - hw, y + body };
    } else {
        pts[0] = { x, y }; pts[1] = { x + hw, y + tip }; pts[2] = { x + hw, y + tip + body };
        pts[3] = { x - hw, y + tip + body }; pts[4] = { x - hw, y + tip };
    }
    Polygon(hdc, pts, 5);
    if (withBox) {
        int top = y + tip + body;
        Rectangle(hdc, x - hw, top, x + hw + 1, top + Sc(5));
    }
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void DrawTab(HDC hdc, int x, int bottom, int align, COLORREF color)
{
    int h = Sc(6), w = Sc(4), t = std::max(1, Sc(2));
    RECT v, hz;
    switch (align) {
    case 1:   // centre: inverted T
        v = { x - t / 2, bottom - h, x - t / 2 + t, bottom };
        hz = { x - w, bottom - t, x + w + 1, bottom };
        break;
    case 2:   // right: mirrored L
        v = { x - t + 1, bottom - h, x + 1, bottom };
        hz = { x - w, bottom - t, x + 1, bottom };
        break;
    default:  // left (and decimal): L
        v = { x, bottom - h, x + t, bottom };
        hz = { x, bottom - t, x + w + 1, bottom };
        break;
    }
    FillSolid(hdc, v, color);
    FillSolid(hdc, hz, color);
    if (align == 3) {
        RECT dot = { x + w, bottom - h, x + w + t, bottom - h + t };
        FillSolid(hdc, dot, color);
    }
}

void Paint(HDC hdcWnd)
{
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC hdc = CreateCompatibleDC(hdcWnd);
    HBITMAP bmp = CreateCompatibleBitmap(hdcWnd, rc.right, rc.bottom);
    HBITMAP oldBmp = (HBITMAP)SelectObject(hdc, bmp);
    FillSolid(hdc, rc, T.base);
    DrawLineH(hdc, 0, rc.right, rc.bottom - 1, T.line);

    const PARAFORMAT2& pf = (s.drag != DRAG_NONE) ? s.dragPf : s.pf;
    Geometry geo = GetGeometry(pf);

    // Printable band.
    RECT band = { geo.x0, geo.bandTop, geo.x1, geo.bandBottom };
    FillSolid(hdc, band, T.rulerBand);
    HBRUSH frame = Theme_Brush(T.rulerBandLine);
    FrameRect(hdc, &band, frame);

    // Ticks and numbers.
    double ppt = Editor_PixelsPerTwip();
    double major, minor;       // twips
    int minorPerMajor, midIndex, labelStep;
    switch (g.cfg.units) {
    case UNIT_INCH: major = 1440; minorPerMajor = 8; midIndex = 4; labelStep = 1; break;
    case UNIT_PT:   major = 1440; minorPerMajor = 6; midIndex = 3; labelStep = 72; break;
    case UNIT_PICA: major = 1440; minorPerMajor = 6; midIndex = 3; labelStep = 6; break;
    default:        major = 1440.0 / 2.54; minorPerMajor = 4; midIndex = 2; labelStep = 1; break;
    }
    minor = major / minorPerMajor;
    int extent = ExtentTwips();
    double majorPx = major * ppt;
    int labelEvery = 1;
    while (majorPx * labelEvery < Sc(24)) labelEvery *= 2;
    bool drawMinor = (minor * ppt) >= Sc(4);

    HFONT oldFont = (HFONT)SelectObject(hdc, Theme_Font(Dpi(), false, -1));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, T.rulerTick);
    int midY = (geo.bandTop + geo.bandBottom) / 2;
    int count = (int)(extent / minor) + 1;
    for (int i = 1; i <= count; ++i) {
        double tw = i * minor;
        if (tw > extent + 0.5) break;
        int x = geo.x0 + (int)std::lround(tw * ppt);
        if (x < -20 || x > rc.right + 20) continue;
        if (i % minorPerMajor == 0) {
            int n = i / minorPerMajor;
            if (n % labelEvery != 0) { DrawLineV(hdc, x, midY - Sc(1), midY + Sc(2), T.rulerTick); continue; }
            wchar_t buf[16];
            wsprintfW(buf, L"%d", n * labelStep);
            RECT rt = { x - Sc(20), geo.bandTop, x + Sc(20), geo.bandBottom };
            DrawTextW(hdc, buf, -1, &rt, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        } else if (drawMinor) {
            int half = (i % minorPerMajor == midIndex) ? Sc(3) : Sc(1);
            DrawLineV(hdc, x, midY - half, midY + half + 1, T.rulerTick);
        }
    }
    SelectObject(hdc, oldFont);

    // Tab stops.
    bool dimTabs = !(pf.dwMask & PFM_TABSTOPS);
    for (int i = 0; i < pf.cTabCount && i < MAX_TAB_STOPS; ++i) {
        if (s.drag == DRAG_TAB && i == s.dragTab && s.dragRemove) continue;
        int pos = pf.rgxTabs[i] & 0x00FFFFFF;
        int align = (pf.rgxTabs[i] >> 24) & 0x0F;
        DrawTab(hdc, XFromTwips(pos), geo.bandBottom - 1, align, dimTabs ? T.textDisabled : T.rulerMarker);
    }

    // Indent markers.
    bool dim = !(pf.dwMask & PFM_STARTINDENT) || !(pf.dwMask & PFM_OFFSET);
    DrawMarker(hdc, geo.xFirst, Sc(1), true, false, dim);
    DrawMarker(hdc, geo.xLeft, geo.bandBottom - Sc(8), false, true, dim);
    DrawMarker(hdc, geo.xRight, geo.bandBottom - Sc(8), false, false, !(pf.dwMask & PFM_RIGHTINDENT));

    BitBlt(hdcWnd, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

void SortTabs(PARAFORMAT2& pf)
{
    int n = std::min<int>(pf.cTabCount, MAX_TAB_STOPS);
    std::sort(pf.rgxTabs, pf.rgxTabs + n, [](LONG a, LONG b) { return (a & 0x00FFFFFF) < (b & 0x00FFFFFF); });
}

DragKind HitTest(POINT pt, int* tabIndex)
{
    Geometry geo = GetGeometry(s.pf);
    int tol = Sc(5);
    int mid = (geo.bandTop + geo.bandBottom) / 2;
    if (pt.y < mid && std::abs(pt.x - geo.xFirst) <= tol) return DRAG_FIRST;
    if (pt.y >= geo.bandBottom - Sc(1) && std::abs(pt.x - geo.xLeft) <= tol) return DRAG_LEFTBOX;
    if (pt.y >= mid - Sc(1) && std::abs(pt.x - geo.xLeft) <= tol) return DRAG_HANGING;
    if (pt.y >= mid - Sc(1) && std::abs(pt.x - geo.xRight) <= tol) return DRAG_RIGHT;
    if (pt.y >= mid - Sc(2)) {
        for (int i = 0; i < s.pf.cTabCount && i < MAX_TAB_STOPS; ++i) {
            int x = XFromTwips(s.pf.rgxTabs[i] & 0x00FFFFFF);
            if (std::abs(pt.x - x) <= Sc(4)) { *tabIndex = i; return DRAG_TAB; }
        }
    }
    return DRAG_NONE;
}

void UpdateDrag(POINT pt)
{
    int extent = ExtentTwips();
    int tw = std::clamp(TwipsFromX(pt.x, true), 0, extent);
    PARAFORMAT2& d = s.dragPf;
    LONG first = s.pf.dxStartIndent, left = s.pf.dxStartIndent + s.pf.dxOffset;
    const int minWidth = 360;
    switch (s.drag) {
    case DRAG_FIRST:
        tw = std::min(tw, extent - (int)s.pf.dxRightIndent - minWidth);
        d.dxStartIndent = std::max(0, tw);
        d.dxOffset = left - d.dxStartIndent;
        break;
    case DRAG_HANGING:
        tw = std::min(tw, extent - (int)s.pf.dxRightIndent - minWidth);
        d.dxOffset = std::max(0, tw) - first;
        break;
    case DRAG_LEFTBOX: {
        LONG delta = tw - left;
        LONG lowest = std::min(first, left);
        if (lowest + delta < 0) delta = -lowest;
        d.dxStartIndent = first + delta;
        d.dxOffset = s.pf.dxOffset;
        tw = left + delta;
        break;
    }
    case DRAG_RIGHT: {
        int minX = (int)std::max(first, left) + minWidth;
        tw = std::max(tw, minX);
        d.dxRightIndent = std::max(0, extent - tw);
        break;
    }
    case DRAG_TAB: {
        RECT rc;
        GetClientRect(s.hwnd, &rc);
        s.dragRemove = pt.y < -Sc(14) || pt.y > rc.bottom + Sc(14);
        if (s.dragTab >= 0 && s.dragTab < MAX_TAB_STOPS)
            d.rgxTabs[s.dragTab] = (s.pf.rgxTabs[s.dragTab] & 0xFF000000) | (tw & 0x00FFFFFF);
        break;
    }
    default: break;
    }
    SetGuide(s.dragRemove ? -1 : XFromTwips(tw));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void FinishDrag(bool apply)
{
    DragKind kind = s.drag;
    s.drag = DRAG_NONE;
    SetGuide(-1);
    ReleaseCapture();
    if (apply) {
        PARAFORMAT2 pf = {};
        pf.cbSize = sizeof(pf);
        switch (kind) {
        case DRAG_FIRST: case DRAG_HANGING: case DRAG_LEFTBOX:
            pf.dwMask = PFM_STARTINDENT | PFM_OFFSET;
            pf.dxStartIndent = s.dragPf.dxStartIndent;
            pf.dxOffset = s.dragPf.dxOffset;
            break;
        case DRAG_RIGHT:
            pf.dwMask = PFM_RIGHTINDENT;
            pf.dxRightIndent = s.dragPf.dxRightIndent;
            break;
        case DRAG_TAB: {
            PARAFORMAT2 t = s.dragPf;
            if (s.dragRemove && s.dragTab >= 0 && s.dragTab < t.cTabCount) {
                for (int i = s.dragTab; i + 1 < t.cTabCount; ++i) t.rgxTabs[i] = t.rgxTabs[i + 1];
                --t.cTabCount;
            }
            SortTabs(t);
            pf.dwMask = PFM_TABSTOPS;
            pf.cTabCount = t.cTabCount;
            memcpy(pf.rgxTabs, t.rgxTabs, sizeof(pf.rgxTabs));
            break;
        }
        default: break;
        }
        if (pf.dwMask) {
            Editor_SetParaFormat(pf);
            PostMessageW(g.hMain, WM_APP_RULERCHANGED, 0, 0);
        }
    }
    s.dragRemove = false;
    if (g.hEdit) InvalidateRect(g.hEdit, nullptr, FALSE);
    Ruler_Update();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        s.hwnd = hwnd;
        s.pf.cbSize = sizeof(s.pf);
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
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        Editor_GetParaFormat(s.pf);
        int tab = -1;
        DragKind kind = HitTest(pt, &tab);
        s.dragPf = s.pf;
        if (kind == DRAG_NONE) {
            // Clicking the lower half of the band adds a left tab stop there.
            Geometry geo = GetGeometry(s.pf);
            int mid = (geo.bandTop + geo.bandBottom) / 2;
            if (pt.y < mid - Sc(2) || pt.x < geo.x0 || pt.x > geo.x1 || s.pf.cTabCount >= MAX_TAB_STOPS) return 0;
            int tw = std::clamp(TwipsFromX(pt.x, true), 0, ExtentTwips());
            if (tw <= 0) return 0;
            tab = s.dragPf.cTabCount;
            s.dragPf.rgxTabs[tab] = tw;
            s.dragPf.cTabCount = (SHORT)(tab + 1);
            s.pf = s.dragPf;          // the new stop behaves like an existing one while dragging
            kind = DRAG_TAB;
        }
        s.drag = kind;
        s.dragTab = tab;
        s.dragRemove = false;
        SetCapture(hwnd);
        UpdateDrag(pt);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (s.drag != DRAG_NONE) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            UpdateDrag(pt);
        }
        return 0;
    case WM_LBUTTONUP:
        if (s.drag != DRAG_NONE) FinishDrag(true);
        return 0;
    case WM_CAPTURECHANGED:
        if (s.drag != DRAG_NONE && (HWND)lParam != hwnd) {
            s.drag = DRAG_NONE;
            SetGuide(-1);
            s.dragRemove = false;
            Ruler_Update();
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (s.drag != DRAG_NONE) FinishDrag(false);
        PostMessageW(g.hMain, WM_COMMAND, ID_FORMAT_PARAGRAPH, 0);
        return 0;
    case WM_DESTROY:
        s.hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool Ruler_Register()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g.hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc) != 0;
}

HWND Ruler_Create(HWND parent)
{
    return CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 100, 26,
                           parent, nullptr, g.hInst, nullptr);
}

int Ruler_GetHeight() { return Scale(27, s.hwnd ? DpiOf(s.hwnd) : 96); }

void Ruler_Update()
{
    if (!s.hwnd || s.drag != DRAG_NONE) return;
    if (g.hEdit) Editor_GetParaFormat(s.pf);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------
double Measure_SnapStep()
{
    switch (g.cfg.units) {
    case UNIT_INCH: return 90.0;              // 1/16"
    case UNIT_PT:   return 120.0;             // 6 pt
    case UNIT_PICA: return 120.0;             // half a pica
    default:        return 1440.0 / 2.54 / 4; // 0.25 cm
    }
}

static std::wstring TrimNumber(double v)
{
    wchar_t buf[48];
    swprintf(buf, 48, L"%.2f", v);
    std::wstring t = buf;
    while (!t.empty() && t.back() == L'0') t.pop_back();
    if (!t.empty() && t.back() == L'.') t.pop_back();
    if (t == L"-0") t = L"0";
    return t;
}

std::wstring Measure_Format(int twips)
{
    switch (g.cfg.units) {
    case UNIT_INCH: return TrimNumber(twips / 1440.0) + L"\"";
    case UNIT_PT:   return TrimNumber(twips / 20.0) + L" pt";
    case UNIT_PICA: return TrimNumber(twips / 240.0) + L" pi";
    default:        return TrimNumber(twips * 2.54 / 1440.0) + L" cm";
    }
}

bool Measure_Parse(const wchar_t* text, int* twips)
{
    std::wstring t = text ? text : L"";
    for (wchar_t& c : t) if (c == L',') c = L'.';
    const wchar_t* p = t.c_str();
    while (*p == L' ' || *p == L'\t') ++p;
    wchar_t* end = nullptr;
    double v = wcstod(p, &end);
    if (end == p) return false;
    while (*end == L' ' || *end == L'\t') ++end;
    std::wstring unit = end;
    while (!unit.empty() && (unit.back() == L' ' || unit.back() == L'\t')) unit.pop_back();
    CharLowerBuffW(&unit[0], (DWORD)unit.size());
    double factor;
    if (unit.empty()) {
        switch (g.cfg.units) {
        case UNIT_INCH: factor = 1440.0; break;
        case UNIT_PT:   factor = 20.0; break;
        case UNIT_PICA: factor = 240.0; break;
        default:        factor = 1440.0 / 2.54; break;
        }
    } else if (unit == L"\"" || unit == L"in" || unit == L"inch" || unit == L"inches" || unit == L"인치") factor = 1440.0;
    else if (unit == L"cm" || unit == L"센티미터") factor = 1440.0 / 2.54;
    else if (unit == L"mm" || unit == L"밀리미터") factor = 1440.0 / 25.4;
    else if (unit == L"pt" || unit == L"포인트") factor = 20.0;
    else if (unit == L"pi" || unit == L"pica" || unit == L"picas" || unit == L"파이카") factor = 240.0;
    else return false;
    double tw = v * factor;
    if (tw < -31680 || tw > 31680) return false;   // +/- 22 inches
    *twips = (int)std::lround(tw);
    return true;
}
