// Colour palette popup used by the text colour and highlight buttons.
#include "popups.h"
#include "theme.h"

namespace {

const wchar_t kClassName[] = L"RichPadColorPopup";

const COLORREF kTextColors[40] = {
    RGB(0,0,0),       RGB(153,51,0),    RGB(51,51,0),     RGB(0,51,0),      RGB(0,51,102),    RGB(0,0,128),     RGB(51,51,153),   RGB(51,51,51),
    RGB(128,0,0),     RGB(255,102,0),   RGB(128,128,0),   RGB(0,128,0),     RGB(0,128,128),   RGB(0,0,255),     RGB(102,102,153), RGB(128,128,128),
    RGB(255,0,0),     RGB(255,153,0),   RGB(153,204,0),   RGB(51,153,102),  RGB(51,204,204),  RGB(51,102,255),  RGB(128,0,128),   RGB(153,153,153),
    RGB(255,0,255),   RGB(255,204,0),   RGB(255,255,0),   RGB(0,255,0),     RGB(0,255,255),   RGB(0,204,255),   RGB(153,51,102),  RGB(192,192,192),
    RGB(255,153,204), RGB(255,204,153), RGB(255,255,153), RGB(204,255,204), RGB(204,255,255), RGB(153,204,255), RGB(204,153,255), RGB(255,255,255),
};

const COLORREF kHighlightColors[15] = {
    RGB(255,255,0), RGB(0,255,0),   RGB(0,255,255), RGB(255,0,255), RGB(0,0,255),
    RGB(255,0,0),   RGB(0,0,128),   RGB(0,128,128), RGB(0,128,0),   RGB(128,0,128),
    RGB(128,0,0),   RGB(128,128,0), RGB(128,128,128), RGB(192,192,192), RGB(0,0,0),
};

// Hot / selected index: >= 0 swatch, -2 = "Automatic / No colour" row, -3 = "More colours" row.
const int kNone = -1, kDefaultRow = -2, kMoreRow = -3;

struct State {
    HWND hwnd = nullptr;
    bool highlight = false;
    const COLORREF* colors = nullptr;
    int count = 0, cols = 0;
    int dpi = 96;
    int hot = kNone;
    bool done = false;
    int result = kNone;
    RECT rcDefault = {}, rcMore = {}, rcGrid = {};
    int cell = 0, gap = 0;
} s;

int Sc(int v) { return Scale(v, s.dpi); }

RECT SwatchRect(int i)
{
    int col = i % s.cols, row = i / s.cols;
    int x = s.rcGrid.left + col * (s.cell + s.gap);
    int y = s.rcGrid.top + row * (s.cell + s.gap);
    return RECT{ x, y, x + s.cell, y + s.cell };
}

SIZE ComputeLayout()
{
    int pad = Sc(8);
    s.cell = Sc(s.highlight ? 26 : 20);
    s.gap = Sc(4);
    int rows = (s.count + s.cols - 1) / s.cols;
    int gridW = s.cols * s.cell + (s.cols - 1) * s.gap;
    int gridH = rows * s.cell + (rows - 1) * s.gap;
    int rowH = Sc(28);
    int y = pad;
    s.rcDefault = { pad, y, pad + gridW, y + rowH };
    y += rowH + Sc(6);
    s.rcGrid = { pad, y, pad + gridW, y + gridH };
    y += gridH;
    if (!s.highlight) {
        y += Sc(6);
        s.rcMore = { pad, y, pad + gridW, y + rowH };
        y += rowH;
    } else {
        SetRectEmpty(&s.rcMore);
    }
    return SIZE{ gridW + 2 * pad, y + pad };
}

int HitTest(POINT pt)
{
    if (PtInRect(&s.rcDefault, pt)) return kDefaultRow;
    if (!IsRectEmpty(&s.rcMore) && PtInRect(&s.rcMore, pt)) return kMoreRow;
    for (int i = 0; i < s.count; ++i) {
        RECT r = SwatchRect(i);
        InflateRect(&r, s.gap / 2, s.gap / 2);
        if (PtInRect(&r, pt)) return i;
    }
    return kNone;
}

void Paint(HDC hdc)
{
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    FillSolid(hdc, rc, T.popupBg);
    HBRUSH frame = Theme_Brush(T.popupLine);
    FrameRect(hdc, &rc, frame);
    HFONT oldFont = (HFONT)SelectObject(hdc, Theme_Font(s.dpi));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, T.text);

    // "Automatic" / "No colour" row with a sample box.
    if (s.hot == kDefaultRow) DrawRoundRect(hdc, s.rcDefault, Sc(4), T.hover, CLR_NONE);
    RECT box = { s.rcDefault.left + Sc(6), s.rcDefault.top + Sc(6), s.rcDefault.left + Sc(6) + Sc(16), s.rcDefault.bottom - Sc(6) };
    if (s.highlight) {
        FillSolid(hdc, box, RGB(255, 255, 255));
        FrameRect(hdc, &box, Theme_Brush(T.fieldLine));
        // Diagonal stroke = "no colour".
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(220, 50, 47));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, box.left + 1, box.bottom - 2, nullptr);
        LineTo(hdc, box.right - 1, box.top);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    } else {
        FillSolid(hdc, box, RGB(0, 0, 0));
        FrameRect(hdc, &box, Theme_Brush(T.fieldLine));
    }
    RECT rt = { box.right + Sc(8), s.rcDefault.top, s.rcDefault.right, s.rcDefault.bottom };
    DrawTextW(hdc, S(s.highlight ? S_COLOR_NONE : S_COLOR_AUTO), -1, &rt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    for (int i = 0; i < s.count; ++i) {
        RECT r = SwatchRect(i);
        if (s.hot == i) {
            RECT o = r;
            InflateRect(&o, Sc(2), Sc(2));
            DrawRoundRect(hdc, o, Sc(3), CLR_NONE, T.text);
        }
        DrawRoundRect(hdc, r, Sc(2), s.colors[i], T.fieldLine);
    }

    if (!IsRectEmpty(&s.rcMore)) {
        if (s.hot == kMoreRow) DrawRoundRect(hdc, s.rcMore, Sc(4), T.hover, CLR_NONE);
        RECT rm = { s.rcMore.left + Sc(8), s.rcMore.top, s.rcMore.right, s.rcMore.bottom };
        DrawTextW(hdc, S(S_COLOR_MORE), -1, &rm, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    SelectObject(hdc, oldFont);
}

void SetHot(int hot)
{
    if (hot != s.hot) {
        s.hot = hot;
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

void MoveHot(int dx, int dy)
{
    int rows = (s.count + s.cols - 1) / s.cols;
    if (s.hot == kNone) { SetHot(kDefaultRow); return; }
    if (s.hot == kDefaultRow) { if (dy > 0) SetHot(0); return; }
    if (s.hot == kMoreRow) { if (dy < 0) SetHot((rows - 1) * s.cols); return; }
    int col = s.hot % s.cols, row = s.hot / s.cols;
    col = std::clamp(col + dx, 0, s.cols - 1);
    row += dy;
    if (row < 0) { SetHot(kDefaultRow); return; }
    if (row >= rows) { SetHot(s.highlight ? s.hot : kMoreRow); return; }
    SetHot(std::min(row * s.cols + col, s.count - 1));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Paint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (PtInRect(&rc, pt)) SetHot(HitTest(pt));
        else SetHot(kNone);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (!PtInRect(&rc, pt)) { s.result = kNone; s.done = true; }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hit = HitTest(pt);
        if (hit != kNone) { s.result = hit; s.done = true; }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lParam != hwnd && !s.done) { s.result = kNone; s.done = true; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool ColorPopup_Register()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW | CS_SAVEBITS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g.hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc) != 0;
}

ColorPopupResult ColorPopup_Show(HWND owner, const RECT& anchor, bool highlight, COLORREF* color)
{
    s = State();
    s.highlight = highlight;
    s.colors = highlight ? kHighlightColors : kTextColors;
    s.count = highlight ? (int)ARRAYSIZE(kHighlightColors) : (int)ARRAYSIZE(kTextColors);
    s.cols = highlight ? 5 : 8;
    s.dpi = DpiOf(owner);
    SIZE size = ComputeLayout();

    // Keep the popup inside the work area of the monitor that contains the button.
    int x = anchor.left, y = anchor.bottom + 1;
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (x + size.cx > mi.rcWork.right) x = mi.rcWork.right - size.cx;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y + size.cy > mi.rcWork.bottom) y = anchor.top - size.cy - 1;
    }

    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"", WS_POPUP,
                             x, y, size.cx, size.cy, owner, nullptr, g.hInst, nullptr);
    if (!s.hwnd) return COLORPOPUP_CANCEL;
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(s.hwnd);
    SetCapture(s.hwnd);

    MSG msg;
    while (!s.done) {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) { if (r == 0) PostQuitMessage((int)msg.wParam); break; }
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            switch (msg.wParam) {
            case VK_LEFT:  MoveHot(-1, 0); continue;
            case VK_RIGHT: MoveHot(1, 0); continue;
            case VK_UP:    MoveHot(0, -1); continue;
            case VK_DOWN:  MoveHot(0, 1); continue;
            case VK_RETURN: case VK_SPACE:
                s.result = s.hot;
                s.done = true;
                continue;
            default:
                s.result = kNone;
                s.done = true;
                continue;
            }
        }
        if (msg.message == WM_KEYUP || msg.message == WM_CHAR || msg.message == WM_SYSKEYUP) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (GetCapture() != s.hwnd) { if (!s.done) { s.result = kNone; s.done = true; } }
    }
    s.done = true;
    if (GetCapture() == s.hwnd) ReleaseCapture();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;

    if (s.result == kDefaultRow) return COLORPOPUP_DEFAULT;
    if (s.result >= 0 && s.result < s.count) {
        *color = s.colors[s.result];
        return COLORPOPUP_COLOR;
    }
    if (s.result == kMoreRow) {
        CHOOSECOLORW cc = {};
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = owner;
        cc.rgbResult = *color;
        cc.lpCustColors = g.cfg.customColors;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
        if (ChooseColorW(&cc)) {
            *color = cc.rgbResult;
            return COLORPOPUP_COLOR;
        }
    }
    return COLORPOPUP_CANCEL;
}
