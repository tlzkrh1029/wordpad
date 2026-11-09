// Colours, fonts and Windows 11 dark-mode integration.
#pragma once
#include "app.h"

struct ThemeColors {
    bool dark = false;
    bool highContrast = false;
    COLORREF base = 0;          // menu row, ruler and status bar background
    COLORREF bar = 0;           // command bar background
    COLORREF line = 0;          // separators
    COLORREF text = 0;
    COLORREF textDim = 0;
    COLORREF textDisabled = 0;
    COLORREF hover = 0;
    COLORREF pressed = 0;
    COLORREF checked = 0;
    COLORREF checkedLine = 0;
    COLORREF accent = 0;
    COLORREF accentText = 0;
    COLORREF field = 0;         // text boxes / combo boxes
    COLORREF fieldLine = 0;
    COLORREF rulerBand = 0;     // printable width on the ruler
    COLORREF rulerBandLine = 0;
    COLORREF rulerTick = 0;
    COLORREF rulerMarker = 0;
    COLORREF rulerMarkerFill = 0;
    COLORREF popupBg = 0;
    COLORREF popupLine = 0;
    COLORREF workspace = 0;     // print preview backdrop
};

extern ThemeColors T;

void Theme_Init();
void Theme_Shutdown();
// Recomputes T from the user's setting and the system state. Returns true if the dark flag changed.
bool Theme_Refresh();
bool Theme_SystemUsesDark();
void Theme_ApplyTitleBar(HWND top);
// Applies the dark or light variant of a visual style ("Explorer", "CFD") to a control.
void Theme_ApplyControl(HWND ctl, const wchar_t* styleName);
void Theme_ApplyTooltip(HWND tip);

HFONT  Theme_Font(int dpi, bool bold = false, int deltaPt = 0);
void   Theme_FlushFonts();
HBRUSH Theme_Brush(COLORREF c);

COLORREF Blend(COLORREF a, COLORREF b, int percentA);
void FillSolid(HDC hdc, const RECT& rc, COLORREF c);
// Anti-aliased rounded rectangle. Pass CLR_NONE to skip the fill or the border.
void DrawRoundRect(HDC hdc, const RECT& rc, int radius, COLORREF fill, COLORREF border);
void DrawLineH(HDC hdc, int x1, int x2, int y, COLORREF c);
void DrawLineV(HDC hdc, int x, int y1, int y2, COLORREF c);
