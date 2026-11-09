// Colours, fonts and Windows 11 dark-mode integration.
#include "gfx.h"
#include "theme.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <map>

ThemeColors T;

namespace {

ULONG_PTR g_gdipToken = 0;

// Undocumented uxtheme.dll exports used by every Win32 app that supports dark mode
// (context menus, scroll bars and combo boxes have no documented dark variants).
typedef void (WINAPI* FnRefreshImmersiveColorPolicyState)();
typedef BOOL (WINAPI* FnAllowDarkModeForWindow)(HWND, BOOL);
typedef int  (WINAPI* FnSetPreferredAppMode)(int);
typedef void (WINAPI* FnFlushMenuThemes)();

FnRefreshImmersiveColorPolicyState pRefreshImmersiveColorPolicyState = nullptr;
FnAllowDarkModeForWindow pAllowDarkModeForWindow = nullptr;
FnSetPreferredAppMode pSetPreferredAppMode = nullptr;
FnFlushMenuThemes pFlushMenuThemes = nullptr;

DWORD g_osBuild = 0;

struct FontKey {
    int dpi; bool bold; int delta;
    bool operator<(const FontKey& o) const {
        if (dpi != o.dpi) return dpi < o.dpi;
        if (bold != o.bold) return bold < o.bold;
        return delta < o.delta;
    }
};
std::map<FontKey, HFONT> g_fonts;
std::map<COLORREF, HBRUSH> g_brushes;

DWORD ReadDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD def)
{
    DWORD value = def, size = sizeof(value), type = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, &type, &value, &size) != ERROR_SUCCESS) return def;
    return value;
}

bool IsHighContrast()
{
    HIGHCONTRASTW hc = { sizeof(hc) };
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

COLORREF SystemAccent()
{
    // Stored as 0xAABBGGRR, which matches COLORREF once the alpha byte is removed.
    DWORD v = ReadDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFFFFFFF);
    if (v == 0xFFFFFFFF) return RGB(0, 103, 192);
    return v & 0x00FFFFFF;
}

} // namespace

COLORREF Blend(COLORREF a, COLORREF b, int pa)
{
    int pb = 100 - pa;
    return RGB((GetRValue(a) * pa + GetRValue(b) * pb) / 100,
               (GetGValue(a) * pa + GetGValue(b) * pb) / 100,
               (GetBValue(a) * pa + GetBValue(b) * pb) / 100);
}

void Theme_Init()
{
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdipToken, &input, nullptr);

    // RtlGetNtVersionNumbers reports the real build even without a compatibility manifest.
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        typedef void (WINAPI* FnVer)(DWORD*, DWORD*, DWORD*);
        if (auto fn = (FnVer)(void*)GetProcAddress(ntdll, "RtlGetNtVersionNumbers")) {
            DWORD major = 0, minor = 0, build = 0;
            fn(&major, &minor, &build);
            g_osBuild = build & 0x0FFFFFFF;
        }
    }
    if (g_osBuild >= 17763) {
        if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            pRefreshImmersiveColorPolicyState = (FnRefreshImmersiveColorPolicyState)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(104));
            pAllowDarkModeForWindow = (FnAllowDarkModeForWindow)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(133));
            pSetPreferredAppMode = (FnSetPreferredAppMode)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
            pFlushMenuThemes = (FnFlushMenuThemes)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
        }
    }
    Theme_Refresh();
}

void Theme_Shutdown()
{
    Theme_FlushFonts();
    for (auto& kv : g_brushes) DeleteObject(kv.second);
    g_brushes.clear();
    if (g_gdipToken) Gdiplus::GdiplusShutdown(g_gdipToken);
    g_gdipToken = 0;
}

bool Theme_SystemUsesDark()
{
    return ReadDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", 1) == 0;
}

bool Theme_Refresh()
{
    bool wasDark = T.dark;
    bool hc = IsHighContrast();
    bool dark = !hc && (g.cfg.theme == THEME_DARK || (g.cfg.theme == THEME_SYSTEM && Theme_SystemUsesDark()));

    ThemeColors c;
    c.dark = dark;
    c.highContrast = hc;
    c.accent = SystemAccent();

    if (hc) {
        COLORREF win = GetSysColor(COLOR_WINDOW), txt = GetSysColor(COLOR_WINDOWTEXT);
        COLORREF hi = GetSysColor(COLOR_HIGHLIGHT);
        c.base = c.bar = c.field = c.popupBg = c.rulerBand = c.workspace = win;
        c.text = c.textDim = c.rulerTick = c.rulerMarker = txt;
        c.textDisabled = GetSysColor(COLOR_GRAYTEXT);
        c.line = c.fieldLine = c.popupLine = c.rulerBandLine = c.checkedLine = txt;
        c.hover = Blend(hi, win, 35);
        c.pressed = Blend(hi, win, 55);
        c.checked = Blend(hi, win, 45);
        c.accent = hi;
        c.accentText = GetSysColor(COLOR_HIGHLIGHTTEXT);
        c.rulerMarkerFill = win;
    } else if (dark) {
        c.base = RGB(32, 32, 32);
        c.bar = RGB(43, 43, 43);
        c.line = RGB(62, 62, 62);
        c.text = RGB(255, 255, 255);
        c.textDim = RGB(200, 200, 200);
        c.textDisabled = RGB(118, 118, 118);
        c.hover = RGB(61, 61, 61);
        c.pressed = RGB(52, 52, 52);
        c.checked = RGB(74, 74, 74);
        c.checkedLine = RGB(104, 104, 104);
        c.accent = Blend(c.accent, RGB(255, 255, 255), 55);   // lighter accent reads better on dark
        c.accentText = RGB(0, 0, 0);
        c.field = RGB(45, 45, 45);
        c.fieldLine = RGB(92, 92, 92);
        c.rulerBand = RGB(58, 58, 58);
        c.rulerBandLine = RGB(84, 84, 84);
        c.rulerTick = RGB(190, 190, 190);
        c.rulerMarker = RGB(225, 225, 225);
        c.rulerMarkerFill = RGB(90, 90, 90);
        c.popupBg = RGB(44, 44, 44);
        c.popupLine = RGB(72, 72, 72);
        c.workspace = RGB(24, 24, 24);
    } else {
        c.base = RGB(243, 243, 243);
        c.bar = RGB(249, 249, 249);
        c.line = RGB(225, 225, 225);
        c.text = RGB(27, 27, 27);
        c.textDim = RGB(96, 96, 96);
        c.textDisabled = RGB(160, 160, 160);
        c.hover = RGB(232, 232, 232);
        c.pressed = RGB(222, 222, 222);
        c.checked = RGB(218, 218, 218);
        c.checkedLine = RGB(190, 190, 190);
        c.accentText = RGB(255, 255, 255);
        c.field = RGB(255, 255, 255);
        c.fieldLine = RGB(200, 200, 200);
        c.rulerBand = RGB(255, 255, 255);
        c.rulerBandLine = RGB(204, 204, 204);
        c.rulerTick = RGB(110, 110, 110);
        c.rulerMarker = RGB(70, 70, 70);
        c.rulerMarkerFill = RGB(255, 255, 255);
        c.popupBg = RGB(249, 249, 249);
        c.popupLine = RGB(204, 204, 204);
        c.workspace = RGB(232, 232, 232);
    }
    T = c;

    if (pSetPreferredAppMode) {
        // 0 Default, 1 AllowDark (follow system), 2 ForceDark, 3 ForceLight
        int mode = hc ? 0 : (g.cfg.theme == THEME_DARK ? 2 : (g.cfg.theme == THEME_LIGHT ? 3 : 1));
        if (g_osBuild < 18362) mode = (mode != 0 && mode != 3) ? 1 : 0;   // 1809: BOOL AllowDarkModeForApp
        pSetPreferredAppMode(mode);
        if (pRefreshImmersiveColorPolicyState) pRefreshImmersiveColorPolicyState();
        if (pFlushMenuThemes) pFlushMenuThemes();
    }
    return wasDark != dark;
}

void Theme_ApplyTitleBar(HWND top)
{
    if (!top) return;
    BOOL dark = T.dark ? TRUE : FALSE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (19 on Windows 10 builds before 20H1)
    if (FAILED(DwmSetWindowAttribute(top, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(top, 19, &dark, sizeof(dark));
    if (!T.highContrast) {
        // 35 = DWMWA_CAPTION_COLOR (Windows 11): blend the title bar into the menu row.
        COLORREF caption = T.base;
        DwmSetWindowAttribute(top, 35, &caption, sizeof(caption));
    } else {
        COLORREF def = 0xFFFFFFFF;   // DWMWA_COLOR_DEFAULT
        DwmSetWindowAttribute(top, 35, &def, sizeof(def));
    }
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(top, dark);
}

void Theme_ApplyControl(HWND ctl, const wchar_t* styleName)
{
    if (!ctl) return;
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(ctl, T.dark);
    std::wstring name = T.dark ? (std::wstring(L"DarkMode_") + styleName) : std::wstring(styleName);
    SetWindowTheme(ctl, name.c_str(), nullptr);
    SendMessageW(ctl, WM_THEMECHANGED, 0, 0);
}

void Theme_ApplyTooltip(HWND tip)
{
    if (!tip) return;
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(tip, T.dark);
    SetWindowTheme(tip, T.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

HFONT Theme_Font(int dpi, bool bold, int deltaPt)
{
    FontKey key = { dpi, bold, deltaPt };
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    LOGFONTW lf = {};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        lf = ncm.lfMessageFont;
        int sysDpi = (int)GetDpiForSystem();
        if (sysDpi <= 0) sysDpi = 96;
        lf.lfHeight = MulDiv(lf.lfHeight, dpi, sysDpi);
    } else {
        lf.lfHeight = -MulDiv(9, dpi, 72);
        lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
    }
    if (deltaPt) lf.lfHeight -= MulDiv(deltaPt, dpi, 72);   // lfHeight is negative for character height
    if (bold) lf.lfWeight = FW_SEMIBOLD;
    lf.lfQuality = CLEARTYPE_QUALITY;
    HFONT font = CreateFontIndirectW(&lf);
    g_fonts[key] = font;
    return font;
}

void Theme_FlushFonts()
{
    for (auto& kv : g_fonts) DeleteObject(kv.second);
    g_fonts.clear();
}

HBRUSH Theme_Brush(COLORREF c)
{
    auto it = g_brushes.find(c);
    if (it != g_brushes.end()) return it->second;
    if (g_brushes.size() > 64) {
        for (auto& kv : g_brushes) DeleteObject(kv.second);
        g_brushes.clear();
    }
    HBRUSH b = CreateSolidBrush(c);
    g_brushes[c] = b;
    return b;
}

void FillSolid(HDC hdc, const RECT& rc, COLORREF c)
{
    COLORREF old = SetBkColor(hdc, c);
    ExtTextOutW(hdc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);
    SetBkColor(hdc, old);
}

void DrawLineH(HDC hdc, int x1, int x2, int y, COLORREF c)
{
    RECT rc = { x1, y, x2, y + 1 };
    FillSolid(hdc, rc, c);
}

void DrawLineV(HDC hdc, int x, int y1, int y2, COLORREF c)
{
    RECT rc = { x, y1, x + 1, y2 };
    FillSolid(hdc, rc, c);
}

void Gfx_RoundRectPath(Gdiplus::GraphicsPath& path, Gdiplus::REAL x, Gdiplus::REAL y,
                       Gdiplus::REAL w, Gdiplus::REAL h, Gdiplus::REAL r)
{
    using namespace Gdiplus;
    if (r <= 0.01f) { path.AddRectangle(RectF(x, y, w, h)); return; }
    REAL d = std::min(r * 2, std::min(w, h));
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

void DrawRoundRect(HDC hdc, const RECT& rc, int radius, COLORREF fill, COLORREF border)
{
    using namespace Gdiplus;
    Graphics gfx(hdc);
    gfx.SetSmoothingMode(SmoothingModeAntiAlias);
    gfx.SetPixelOffsetMode(PixelOffsetModeHalf);
    REAL x = (REAL)rc.left, y = (REAL)rc.top, w = (REAL)(rc.right - rc.left), h = (REAL)(rc.bottom - rc.top);
    if (fill != CLR_NONE) {
        GraphicsPath path;
        Gfx_RoundRectPath(path, x, y, w, h, (REAL)radius);
        SolidBrush brush(GpColor(fill));
        gfx.FillPath(&brush, &path);
    }
    if (border != CLR_NONE) {
        GraphicsPath path;
        Gfx_RoundRectPath(path, x + 0.5f, y + 0.5f, w - 1, h - 1, (REAL)radius);
        Pen pen(GpColor(border), 1.0f);
        gfx.DrawPath(&pen, &path);
    }
}
