// The menu row and the ribbon-like command bar at the top of the window.
#include "commandbar.h"
#include "theme.h"
#include "icons.h"
#include "editor.h"
#include <imm.h>
#include <set>

namespace {

enum Kind : BYTE { K_BUTTON, K_DROPDOWN, K_SPLIT, K_GAP, K_FONTNAME, K_FONTSIZE };
enum Size : BYTE { SZ_ICON, SZ_ICONTEXT, SZ_LARGE };

struct Item {
    int id = 0;             // command sent to the main window
    int dropId = 0;         // command of the drop-down part of a split button
    int icon = ICON_NONE;
    int label = -1;         // string id of the visible caption
    int tip = -1;           // string id of the tooltip
    Kind kind = K_BUTTON;
    Size size = SZ_ICON;
    RECT rc = {};
    RECT rcDrop = {};       // drop-down part of a split button
    bool enabled = true;
    bool checked = false;
    bool hasSwatch = false;
    COLORREF swatch = CLR_NONE;
};

struct Row { std::vector<int> items; };
struct Column { std::vector<Row> rows; };
struct Group {
    int label = -1;
    std::vector<Column> cols;
    RECT rc = {};
    bool visible = true;
};

const wchar_t kClassName[] = L"RichPadCommandBar";
const int kMenuCount = 6;
const int kMenuStr[kMenuCount] = { S_MENU_FILE, S_MENU_EDIT, S_MENU_VIEW, S_MENU_INSERT, S_MENU_FORMAT, S_MENU_HELP };
const UINT WM_CB_APPLYFONT = WM_APP + 60;   // wParam: 0 = face, 1 = size

struct State {
    HWND hwnd = nullptr;
    HWND hTip = nullptr;
    HWND hFontName = nullptr;
    HWND hFontSize = nullptr;
    std::vector<Item> items;
    std::vector<Group> groups;
    std::vector<int> quick;             // quick-access buttons next to the menu titles
    RECT menuRc[kMenuCount] = {};
    int quickSepX = 0;
    int dpi = 96;
    int menuH = 0;

    int hot = -1; bool hotDrop = false;
    int pressed = -1; bool pressedDrop = false;
    int activeDrop = -1;                // item whose drop-down is currently open
    int hotMenu = -1;
    int openMenu = -1;
    int nextMenu = -1; bool nextByKeyboard = false;
    bool tracking = false;
    bool menuMode = false; int focusMenu = 0;
    bool showMnemonics = false;
    HHOOK hook = nullptr;
    bool subOpen = false, selIsPopup = false;
    POINT lastMenuPt = {};
    DWORD closedTick = 0; int closedMenu = -1; int closedItem = -1;

    bool dirty = false;
    bool mouseTracked = false;
    std::wstring fontFace; int fontSize = 0;    // current selection (empty / 0 = mixed)
    std::wstring shownFace; int shownSize = -2; // what the boxes currently display
    std::wstring pendingFace;
    int tipSignature = -1;
} s;

int Sc(int v) { return Scale(v, s.dpi); }

int MeasureText(HDC hdc, const std::wstring& text, bool prefix)
{
    RECT rc = { 0, 0, 0, 0 };
    DrawTextW(hdc, text.c_str(), (int)text.size(), &rc, DT_SINGLELINE | DT_CALCRECT | (prefix ? 0 : DT_NOPREFIX));
    return rc.right;
}

int AddItem(int id, int icon, int label, int tip, Kind kind, Size size, int dropId = 0)
{
    Item it;
    it.id = id; it.icon = icon; it.label = label; it.tip = tip; it.kind = kind; it.size = size; it.dropId = dropId;
    s.items.push_back(it);
    return (int)s.items.size() - 1;
}

int Gap() { return AddItem(0, ICON_NONE, -1, -1, K_GAP, SZ_ICON); }
int Btn(int id, int icon, int tip) { return AddItem(id, icon, -1, tip, K_BUTTON, SZ_ICON); }

void BuildItems()
{
    // Preserve dynamic state across rebuilds.
    std::vector<Item> old = s.items;
    s.items.clear();
    s.groups.clear();
    s.quick.clear();

    s.quick.push_back(Btn(ID_FILE_SAVE, ICON_SAVE, S_TIP_SAVE));
    s.quick.push_back(Btn(ID_EDIT_UNDO, ICON_UNDO, S_TIP_UNDO));
    s.quick.push_back(Btn(ID_EDIT_REDO, ICON_REDO, S_TIP_REDO));

    {   // Clipboard
        Group g; g.label = S_GROUP_CLIPBOARD;
        Column paste; paste.rows.push_back(Row{ { AddItem(ID_EDIT_PASTE, ICON_PASTE, S_BTN_PASTE, S_TIP_PASTE, K_SPLIT, SZ_LARGE, ID_EDIT_PASTE_MENU) } });
        Column stack;
        stack.rows.push_back(Row{ { AddItem(ID_EDIT_CUT, ICON_CUT, S_BTN_CUT, S_TIP_CUT, K_BUTTON, SZ_ICONTEXT) } });
        stack.rows.push_back(Row{ { AddItem(ID_EDIT_COPY, ICON_COPY, S_BTN_COPY, S_TIP_COPY, K_BUTTON, SZ_ICONTEXT) } });
        g.cols.push_back(paste); g.cols.push_back(stack);
        s.groups.push_back(g);
    }
    {   // Font
        Group g; g.label = S_GROUP_FONT;
        Column c;
        Row r1, r2;
        r1.items = { AddItem(ID_FORMAT_FONTNAME, ICON_NONE, -1, S_TIP_FONTNAME, K_FONTNAME, SZ_ICON),
                     AddItem(ID_FORMAT_FONTSIZE, ICON_NONE, -1, S_TIP_FONTSIZE, K_FONTSIZE, SZ_ICON),
                     Btn(ID_FORMAT_GROW, ICON_GROW, S_TIP_GROW),
                     Btn(ID_FORMAT_SHRINK, ICON_SHRINK, S_TIP_SHRINK) };
        r2.items = { Btn(ID_FORMAT_BOLD, ICON_BOLD, S_TIP_BOLD),
                     Btn(ID_FORMAT_ITALIC, ICON_ITALIC, S_TIP_ITALIC),
                     Btn(ID_FORMAT_UNDERLINE, ICON_UNDERLINE, S_TIP_UNDERLINE),
                     Btn(ID_FORMAT_STRIKE, ICON_STRIKE, S_TIP_STRIKE),
                     Btn(ID_FORMAT_SUBSCRIPT, ICON_SUBSCRIPT, S_TIP_SUBSCRIPT),
                     Btn(ID_FORMAT_SUPERSCRIPT, ICON_SUPERSCRIPT, S_TIP_SUPERSCRIPT),
                     Gap(),
                     AddItem(ID_FORMAT_COLOR, ICON_COLOR, -1, S_TIP_COLOR, K_SPLIT, SZ_ICON, ID_FORMAT_COLOR_PICK),
                     AddItem(ID_FORMAT_HIGHLIGHT, ICON_HIGHLIGHT, -1, S_TIP_HIGHLIGHT, K_SPLIT, SZ_ICON, ID_FORMAT_HIGHLIGHT_PICK) };
        c.rows = { r1, r2 };
        g.cols.push_back(c);
        s.groups.push_back(g);
    }
    {   // Paragraph
        Group g; g.label = S_GROUP_PARAGRAPH;
        Column c;
        Row r1, r2;
        r1.items = { Btn(ID_FORMAT_INDENT_DEC, ICON_INDENT_DEC, S_TIP_INDENT_DEC),
                     Btn(ID_FORMAT_INDENT_INC, ICON_INDENT_INC, S_TIP_INDENT_INC),
                     Gap(),
                     AddItem(ID_FORMAT_BULLETS, ICON_LIST, -1, S_TIP_LIST, K_SPLIT, SZ_ICON, ID_FORMAT_LIST_MENU),
                     AddItem(ID_FORMAT_SPACING_MENU, ICON_SPACING, -1, S_TIP_SPACING, K_DROPDOWN, SZ_ICON) };
        r2.items = { Btn(ID_FORMAT_ALIGN_LEFT, ICON_ALIGN_LEFT, S_TIP_ALIGN_LEFT),
                     Btn(ID_FORMAT_ALIGN_CENTER, ICON_ALIGN_CENTER, S_TIP_ALIGN_CENTER),
                     Btn(ID_FORMAT_ALIGN_RIGHT, ICON_ALIGN_RIGHT, S_TIP_ALIGN_RIGHT),
                     Btn(ID_FORMAT_ALIGN_JUSTIFY, ICON_ALIGN_JUSTIFY, S_TIP_ALIGN_JUSTIFY),
                     Gap(),
                     Btn(ID_FORMAT_PARAGRAPH, ICON_PARAGRAPH, S_TIP_PARAGRAPH) };
        c.rows = { r1, r2 };
        g.cols.push_back(c);
        s.groups.push_back(g);
    }
    {   // Insert
        Group g; g.label = S_GROUP_INSERT;
        const int defs[3][4] = {
            { ID_INSERT_PICTURE, ICON_PICTURE, S_BTN_PICTURE, S_TIP_PICTURE },
            { ID_INSERT_DATETIME, ICON_DATETIME, S_BTN_DATETIME, S_TIP_DATETIME },
            { ID_INSERT_OBJECT, ICON_OBJECT, S_BTN_OBJECT, S_TIP_OBJECT } };
        for (auto& d : defs) {
            Column c;
            c.rows.push_back(Row{ { AddItem(d[0], d[1], d[2], d[3], K_BUTTON, SZ_LARGE) } });
            g.cols.push_back(c);
        }
        s.groups.push_back(g);
    }
    {   // Editing
        Group g; g.label = S_GROUP_EDITING;
        Column c;
        c.rows.push_back(Row{ { AddItem(ID_EDIT_FIND, ICON_FIND, S_BTN_FIND, S_TIP_FIND, K_BUTTON, SZ_ICONTEXT) } });
        c.rows.push_back(Row{ { AddItem(ID_EDIT_REPLACE, ICON_REPLACE, S_BTN_REPLACE, S_TIP_REPLACE, K_BUTTON, SZ_ICONTEXT) } });
        c.rows.push_back(Row{ { AddItem(ID_EDIT_SELECTALL, ICON_SELECTALL, S_BTN_SELECTALL, S_TIP_SELECTALL, K_BUTTON, SZ_ICONTEXT) } });
        g.cols.push_back(c);
        s.groups.push_back(g);
    }

    for (Item& it : s.items) {
        for (const Item& o : old) {
            if (o.id == it.id && o.kind == it.kind && it.id != 0) {
                it.enabled = o.enabled; it.checked = o.checked; it.hasSwatch = o.hasSwatch; it.swatch = o.swatch;
                break;
            }
        }
    }
    s.hot = s.pressed = s.activeDrop = -1;
    s.tipSignature = -1;
}

int ItemWidth(HDC hdc, const Item& it)
{
    switch (it.kind) {
    case K_GAP: return Sc(9);
    case K_FONTNAME: return Sc(158);
    case K_FONTSIZE: return Sc(62);
    default: break;
    }
    int textW = it.label >= 0 ? MeasureText(hdc, S(it.label), false) : 0;
    if (it.size == SZ_LARGE) {
        int w = textW + Sc(14);
        if (it.kind != K_BUTTON) w += Sc(10);   // chevron
        return std::max(Sc(52), w);
    }
    if (it.size == SZ_ICONTEXT) return Sc(6) + Sc(16) + Sc(6) + textW + Sc(8);
    int w = Sc(28);
    if (it.kind == K_DROPDOWN) w += Sc(12);
    if (it.kind == K_SPLIT) w += Sc(14);
    return w;
}

void UpdateTooltips(int signature);

void Layout()
{
    if (!s.hwnd) return;
    RECT client;
    GetClientRect(s.hwnd, &client);
    s.menuH = Sc(32);

    HDC hdc = GetDC(s.hwnd);
    HFONT oldFont = (HFONT)SelectObject(hdc, Theme_Font(s.dpi));

    // ---- menu row ----
    int x = Sc(6);
    for (int i = 0; i < kMenuCount; ++i) {
        std::wstring t = s.showMnemonics ? std::wstring(S(kMenuStr[i])) : StripMnemonic(S(kMenuStr[i]));
        int w = MeasureText(hdc, t, s.showMnemonics) + Sc(20);
        s.menuRc[i] = { x, Sc(3), x + w, s.menuH - Sc(3) };
        x += w + Sc(1);
    }
    x += Sc(7);
    s.quickSepX = x;
    x += Sc(8);
    for (int idx : s.quick) {
        s.items[idx].rc = { x, Sc(3), x + Sc(32), s.menuH - Sc(3) };
        x += Sc(33);
    }

    // ---- groups ----
    int top = s.menuH + 1 + Sc(5);
    int contentH = Sc(66);
    int labelH = Sc(18);
    int comboH = 0;
    if (s.hFontName) { RECT r; GetWindowRect(s.hFontName, &r); comboH = r.bottom - r.top; }

    int gx = Sc(4);
    int visibleCount = 0;
    for (size_t gi = 0; gi < s.groups.size(); ++gi) {
        Group& grp = s.groups[gi];
        int cx = gx + Sc(7);
        for (Column& col : grp.cols) {
            int n = (int)col.rows.size();
            int colW = 0;
            bool large = (n == 1 && col.rows[0].items.size() == 1 && s.items[col.rows[0].items[0]].size == SZ_LARGE);
            if (large) {
                Item& it = s.items[col.rows[0].items[0]];
                colW = ItemWidth(hdc, it);
                it.rc = { cx, top, cx + colW, top + contentH };
                if (it.kind == K_SPLIT) it.rcDrop = { cx, top + Sc(42), cx + colW, top + contentH };
            } else {
                int rowH = n >= 3 ? Sc(22) : Sc(26);
                int gap = (contentH - n * rowH) / (n + 1);
                int y = top + gap;
                for (Row& row : col.rows) {
                    int rx = cx;
                    for (int idx : row.items) {
                        Item& it = s.items[idx];
                        int w = ItemWidth(hdc, it);
                        it.rc = { rx, y, rx + w, y + rowH };
                        if (it.kind == K_SPLIT) it.rcDrop = { rx + w - Sc(14), y, rx + w, y + rowH };
                        rx += w + ((it.kind == K_FONTNAME || it.kind == K_FONTSIZE) ? Sc(4) : Sc(1));
                    }
                    colW = std::max(colW, rx - cx);
                    y += rowH + gap;
                }
            }
            cx += colW + Sc(3);
        }
        grp.rc = { gx, top, cx + Sc(4), top + contentH + labelH };
        // The first two groups always stay; the others are dropped when the window is too narrow.
        grp.visible = (gi < 2) || (grp.rc.right <= client.right);
        if (gi > 0 && !s.groups[gi - 1].visible) grp.visible = false;
        if (grp.visible) ++visibleCount;
        gx = grp.rc.right + 1;
    }

    // Position the font boxes (they are real child windows).
    for (const Item& it : s.items) {
        HWND h = it.kind == K_FONTNAME ? s.hFontName : (it.kind == K_FONTSIZE ? s.hFontSize : nullptr);
        if (!h) continue;
        int y = it.rc.top + ((it.rc.bottom - it.rc.top) - comboH) / 2;
        SetWindowPos(h, nullptr, it.rc.left, y, it.rc.right - it.rc.left, Sc(420), SWP_NOZORDER | SWP_NOACTIVATE);
    }

    SelectObject(hdc, oldFont);
    ReleaseDC(s.hwnd, hdc);

    UpdateTooltips(visibleCount * 1000 + s.dpi + (s.showMnemonics ? 500000 : 0) + (Strings_IsKorean() ? 1000000 : 0));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool ItemVisible(int idx)
{
    for (const Group& grp : s.groups) {
        for (const Column& col : grp.cols)
            for (const Row& row : col.rows)
                for (int i : row.items)
                    if (i == idx) return grp.visible;
    }
    return true;   // quick-access buttons
}

void AddTool(UINT_PTR id, const RECT& rc)
{
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = s.hwnd;
    ti.uId = id;
    ti.rect = rc;
    ti.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(s.hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void UpdateTooltips(int signature)
{
    if (!s.hTip || signature == s.tipSignature) return;
    s.tipSignature = signature;
    // Remove every existing tool, then register the current rectangles.
    for (;;) {
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        if (!SendMessageW(s.hTip, TTM_ENUMTOOLSW, 0, (LPARAM)&ti)) break;
        SendMessageW(s.hTip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    for (size_t i = 0; i < s.items.size(); ++i) {
        const Item& it = s.items[i];
        if (it.tip < 0 || !ItemVisible((int)i)) continue;
        if (it.kind == K_FONTNAME || it.kind == K_FONTSIZE) {
            HWND h = it.kind == K_FONTNAME ? s.hFontName : s.hFontSize;
            TOOLINFOW ti = {};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
            ti.hwnd = s.hwnd;
            ti.uId = (UINT_PTR)h;
            ti.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(s.hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            continue;
        }
        if (it.kind == K_SPLIT) {
            RECT main = it.rc;
            if (it.size == SZ_LARGE) main.bottom = it.rcDrop.top; else main.right = it.rcDrop.left;
            AddTool(i + 1, main);
            AddTool(10000 + i, it.rcDrop);
        } else {
            AddTool(i + 1, it.rc);
        }
    }
}

int HitMenu(POINT pt)
{
    for (int i = 0; i < kMenuCount; ++i)
        if (PtInRect(&s.menuRc[i], pt)) return i;
    return -1;
}

int HitItem(POINT pt, bool* dropPart)
{
    for (size_t i = 0; i < s.items.size(); ++i) {
        const Item& it = s.items[i];
        if (it.kind == K_GAP || it.kind == K_FONTNAME || it.kind == K_FONTSIZE) continue;
        if (!PtInRect(&it.rc, pt) || !ItemVisible((int)i)) continue;
        if (dropPart) *dropPart = (it.kind == K_SPLIT && PtInRect(&it.rcDrop, pt));
        return (int)i;
    }
    return -1;
}

void InvalidateItem(int idx)
{
    if (idx >= 0 && idx < (int)s.items.size()) InvalidateRect(s.hwnd, &s.items[idx].rc, FALSE);
}

void InvalidateMenuRow()
{
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    rc.bottom = s.menuH;
    InvalidateRect(s.hwnd, &rc, FALSE);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void DrawItem(HDC hdc, int idx)
{
    const Item& it = s.items[idx];
    if (it.kind == K_GAP) {
        DrawLineV(hdc, (it.rc.left + it.rc.right) / 2, it.rc.top + Sc(4), it.rc.bottom - Sc(4), T.line);
        return;
    }
    if (it.kind == K_FONTNAME || it.kind == K_FONTSIZE) return;

    bool hot = it.enabled && s.hot == idx;
    bool down = it.enabled && s.pressed == idx;
    bool dropOpen = s.activeDrop == idx;
    bool split = it.kind == K_SPLIT;
    COLORREF fg = it.enabled ? T.text : T.textDisabled;
    int radius = Sc(4);

    RECT rMain = it.rc;
    if (split) {
        if (it.size == SZ_LARGE) rMain.bottom = it.rcDrop.top; else rMain.right = it.rcDrop.left;
    }

    if (it.checked && it.enabled)
        DrawRoundRect(hdc, split ? rMain : it.rc, radius, T.checked, T.checkedLine);
    if (hot || down || dropOpen) {
        if (!it.checked || split) DrawRoundRect(hdc, it.rc, radius, T.hover, CLR_NONE);
        if (split) {
            bool onDrop = dropOpen || (hot && s.hotDrop) || (down && s.pressedDrop);
            if (down || dropOpen) DrawRoundRect(hdc, onDrop ? it.rcDrop : rMain, radius, T.pressed, CLR_NONE);
            if (it.size == SZ_LARGE) DrawLineH(hdc, it.rc.left + Sc(5), it.rc.right - Sc(5), it.rcDrop.top, T.checkedLine);
            else DrawLineV(hdc, it.rcDrop.left, it.rc.top + Sc(5), it.rc.bottom - Sc(5), T.checkedLine);
        } else if (down || dropOpen) {
            DrawRoundRect(hdc, it.rc, radius, T.pressed, CLR_NONE);
        }
    }

    COLORREF bar = CLR_NONE;
    if (it.hasSwatch && it.swatch != CLR_NONE) bar = it.enabled ? it.swatch : Blend(it.swatch, T.bar, 35);
    SetTextColor(hdc, fg);

    if (it.size == SZ_LARGE) {
        int isz = Sc(32);
        Icon_Draw(hdc, it.icon, (it.rc.left + it.rc.right - isz) / 2, it.rc.top + Sc(6), isz, fg, bar);
        RECT rt = { it.rc.left, it.rc.top + Sc(42), it.rc.right, it.rc.bottom - Sc(4) };
        if (it.kind != K_BUTTON) {
            std::wstring label = S(it.label);
            int tw = MeasureText(hdc, label, false);
            int total = tw + Sc(11);
            RECT rl = rt;
            rl.left = (it.rc.left + it.rc.right - total) / 2;
            rl.right = rl.left + tw;
            DrawTextW(hdc, label.c_str(), -1, &rl, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            Icon_DrawChevron(hdc, rl.right + Sc(7), (rt.top + rt.bottom) / 2 + Sc(1), Sc(3), fg);
        } else {
            DrawTextW(hdc, S(it.label), -1, &rt, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }
    } else if (it.size == SZ_ICONTEXT) {
        int isz = Sc(16);
        int ix = it.rc.left + Sc(6);
        Icon_Draw(hdc, it.icon, ix, (it.rc.top + it.rc.bottom - isz) / 2, isz, fg, bar);
        RECT rt = { ix + isz + Sc(6), it.rc.top, it.rc.right - Sc(4), it.rc.bottom };
        DrawTextW(hdc, S(it.label), -1, &rt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    } else {
        int isz = Sc(16);
        RECT area = it.rc;
        if (split) area = rMain;
        else if (it.kind == K_DROPDOWN) area.right -= Sc(12);
        Icon_Draw(hdc, it.icon, (area.left + area.right - isz) / 2, (area.top + area.bottom - isz) / 2, isz, fg, bar);
        if (split)
            Icon_DrawChevron(hdc, (it.rcDrop.left + it.rcDrop.right) / 2, (it.rc.top + it.rc.bottom) / 2 + Sc(1), Sc(3), fg);
        else if (it.kind == K_DROPDOWN)
            Icon_DrawChevron(hdc, it.rc.right - Sc(9), (it.rc.top + it.rc.bottom) / 2 + Sc(1), Sc(3), fg);
    }
}

void Paint(HDC hdcWnd, const RECT& rcPaint)
{
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC hdc = CreateCompatibleDC(hdcWnd);
    HBITMAP bmp = CreateCompatibleBitmap(hdcWnd, rc.right, rc.bottom);
    HBITMAP oldBmp = (HBITMAP)SelectObject(hdc, bmp);
    HFONT oldFont = (HFONT)SelectObject(hdc, Theme_Font(s.dpi));
    SetBkMode(hdc, TRANSPARENT);

    RECT rMenu = { 0, 0, rc.right, s.menuH };
    FillSolid(hdc, rMenu, T.base);
    RECT rBar = { 0, s.menuH, rc.right, rc.bottom };
    FillSolid(hdc, rBar, T.bar);
    DrawLineH(hdc, 0, rc.right, s.menuH, T.line);
    DrawLineH(hdc, 0, rc.right, rc.bottom - 1, T.line);

    // ---- menu titles ----
    for (int i = 0; i < kMenuCount; ++i) {
        bool open = s.openMenu == i;
        bool hot = !s.tracking && (s.hotMenu == i || (s.menuMode && s.focusMenu == i));
        if (open) DrawRoundRect(hdc, s.menuRc[i], Sc(4), T.pressed, CLR_NONE);
        else if (hot) DrawRoundRect(hdc, s.menuRc[i], Sc(4), T.hover, CLR_NONE);
        std::wstring t = s.showMnemonics ? std::wstring(S(kMenuStr[i])) : StripMnemonic(S(kMenuStr[i]));
        SetTextColor(hdc, T.text);
        RECT rt = s.menuRc[i];
        DrawTextW(hdc, t.c_str(), -1, &rt, DT_SINGLELINE | DT_CENTER | DT_VCENTER | (s.showMnemonics ? 0 : DT_NOPREFIX));
    }
    DrawLineV(hdc, s.quickSepX, Sc(9), s.menuH - Sc(9), T.checkedLine);
    for (int idx : s.quick) DrawItem(hdc, idx);

    // ---- groups ----
    for (size_t gi = 0; gi < s.groups.size(); ++gi) {
        const Group& grp = s.groups[gi];
        if (!grp.visible) continue;
        for (const Column& col : grp.cols)
            for (const Row& row : col.rows)
                for (int idx : row.items) DrawItem(hdc, idx);
        RECT rl = { grp.rc.left, grp.rc.bottom - Sc(18), grp.rc.right, grp.rc.bottom };
        SetTextColor(hdc, T.textDim);
        DrawTextW(hdc, S(grp.label), -1, &rl, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        DrawLineV(hdc, grp.rc.right, grp.rc.top + Sc(4), grp.rc.bottom - Sc(4), T.line);
    }

    BitBlt(hdcWnd, rcPaint.left, rcPaint.top, rcPaint.right - rcPaint.left, rcPaint.bottom - rcPaint.top,
           hdc, rcPaint.left, rcPaint.top, SRCCOPY);
    SelectObject(hdc, oldFont);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

// ---------------------------------------------------------------------------
// Menu tracking
// ---------------------------------------------------------------------------
LRESULT CALLBACK MenuHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == MSGF_MENU && s.tracking) {
        MSG* m = (MSG*)lParam;
        switch (m->message) {
        case WM_MOUSEMOVE: {
            POINT pt = m->pt;
            if (pt.x == s.lastMenuPt.x && pt.y == s.lastMenuPt.y) break;
            s.lastMenuPt = pt;
            POINT c = pt;
            ScreenToClient(s.hwnd, &c);
            int idx = HitMenu(c);
            if (idx >= 0 && idx != s.openMenu) {
                s.nextMenu = idx;
                s.nextByKeyboard = false;
                EndMenu();
                return 1;
            }
            break;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            POINT c = m->pt;
            ScreenToClient(s.hwnd, &c);
            if (HitMenu(c) == s.openMenu) {   // clicking the open title closes its menu
                s.nextMenu = -1;
                EndMenu();
                return 1;
            }
            break;
        }
        case WM_KEYDOWN:
            if (m->wParam == VK_LEFT && !s.subOpen) {
                s.nextMenu = (s.openMenu + kMenuCount - 1) % kMenuCount;
                s.nextByKeyboard = true;
                EndMenu();
                return 1;
            }
            if (m->wParam == VK_RIGHT && !s.selIsPopup) {
                s.nextMenu = (s.openMenu + 1) % kMenuCount;
                s.nextByKeyboard = true;
                EndMenu();
                return 1;
            }
            break;
        }
    }
    return CallNextHookEx(s.hook, code, wParam, lParam);
}

void SuspendTooltips(bool suspend);

void SetMnemonics(bool show)
{
    if (s.showMnemonics != show) {
        s.showMnemonics = show;
        Layout();
    }
}

void TrackMenu(int index, bool keyboard)
{
    if (s.tracking || index < 0 || index >= kMenuCount) return;
    s.tracking = true;
    SuspendTooltips(true);
    int cur = index;
    while (cur >= 0 && cur < kMenuCount) {
        HMENU menu = g.hMenus[cur];
        if (!menu) break;
        s.openMenu = cur;
        s.nextMenu = -1;
        s.nextByKeyboard = false;
        s.subOpen = false;
        s.selIsPopup = false;
        SetMnemonics(keyboard || s.menuMode);
        InvalidateMenuRow();
        UpdateWindow(s.hwnd);

        RECT rc = s.menuRc[cur];
        MapWindowPoints(s.hwnd, nullptr, (POINT*)&rc, 2);
        TPMPARAMS tp = {};
        tp.cbSize = sizeof(tp);
        tp.rcExclude = rc;
        GetCursorPos(&s.lastMenuPt);
        s.hook = SetWindowsHookExW(WH_MSGFILTER, MenuHookProc, nullptr, GetCurrentThreadId());
        if (keyboard) PostMessageW(g.hMain, WM_KEYDOWN, VK_DOWN, 0);   // highlights the first item
        TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL, rc.left, rc.bottom, g.hMain, &tp);
        if (s.hook) { UnhookWindowsHookEx(s.hook); s.hook = nullptr; }
        s.closedTick = GetTickCount();
        s.closedMenu = cur;
        cur = s.nextMenu;
        keyboard = s.nextByKeyboard;
    }
    s.openMenu = -1;
    s.tracking = false;
    s.menuMode = false;
    SuspendTooltips(false);
    SetMnemonics(false);
    // The pointer may have moved while the menu loop owned the mouse.
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(s.hwnd, &pt);
    s.hotMenu = HitMenu(pt);
    InvalidateMenuRow();
    if (GetFocus() == s.hwnd && g.hEdit) SetFocus(g.hEdit);
}

void ExitMenuMode()
{
    if (!s.menuMode) return;
    s.menuMode = false;
    SetMnemonics(false);
    InvalidateMenuRow();
    if (GetFocus() == s.hwnd && g.hEdit) SetFocus(g.hEdit);
}

int MenuByMnemonic(wchar_t ch)
{
    wchar_t up = (wchar_t)(UINT_PTR)CharUpperW((LPWSTR)(UINT_PTR)ch);
    for (int i = 0; i < kMenuCount; ++i)
        if (GetMnemonic(S(kMenuStr[i])) == up) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Font boxes
// ---------------------------------------------------------------------------
std::wstring SizeToText(int twips)
{
    if (twips <= 0) return L"";
    if (twips % 20 == 0) return std::to_wstring(twips / 20);
    wchar_t buf[32];
    swprintf(buf, 32, L"%d.%d", twips / 20, (twips % 20) / 2);
    return buf;
}

void PopulateFonts()
{
    if (!s.hFontName) return;
    std::set<std::wstring> names;
    HDC hdc = GetDC(nullptr);
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(hdc, &lf, [](const LOGFONTW* plf, const TEXTMETRICW*, DWORD type, LPARAM lp) -> int {
        if (plf->lfFaceName[0] != L'@' && !(type & RASTER_FONTTYPE))
            ((std::set<std::wstring>*)lp)->insert(plf->lfFaceName);
        return 1;
    }, (LPARAM)&names, 0);
    ReleaseDC(nullptr, hdc);

    SendMessageW(s.hFontName, WM_SETREDRAW, FALSE, 0);
    SendMessageW(s.hFontName, CB_RESETCONTENT, 0, 0);
    for (const std::wstring& n : names) SendMessageW(s.hFontName, CB_ADDSTRING, 0, (LPARAM)n.c_str());
    SendMessageW(s.hFontName, WM_SETREDRAW, TRUE, 0);
    s.shownFace = L"\x01";   // force a refresh of the displayed text
}

void ShowFontInfo(bool force)
{
    HWND focus = GetFocus();
    bool nameBusy = focus && (focus == s.hFontName || IsChild(s.hFontName, focus));
    bool sizeBusy = focus && (focus == s.hFontSize || IsChild(s.hFontSize, focus));
    if (s.hFontName && !nameBusy && (force || s.shownFace != s.fontFace)) {
        s.shownFace = s.fontFace;
        int idx = s.fontFace.empty() ? -1 : (int)SendMessageW(s.hFontName, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)s.fontFace.c_str());
        SendMessageW(s.hFontName, CB_SETCURSEL, (WPARAM)idx, 0);
        if (idx < 0) SetWindowTextW(s.hFontName, s.fontFace.c_str());
    }
    if (s.hFontSize && !sizeBusy && (force || s.shownSize != s.fontSize)) {
        s.shownSize = s.fontSize;
        std::wstring t = SizeToText(s.fontSize);
        int idx = t.empty() ? -1 : (int)SendMessageW(s.hFontSize, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)t.c_str());
        SendMessageW(s.hFontSize, CB_SETCURSEL, (WPARAM)idx, 0);
        if (idx < 0) SetWindowTextW(s.hFontSize, t.c_str());
    }
}

LRESULT CALLBACK ComboEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
{
    HWND combo = GetParent(hwnd);
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN && !SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0)) {
            PostMessageW(s.hwnd, WM_CB_APPLYFONT, combo == s.hFontSize ? 1 : 0, 1);
            return 0;
        }
        if (wParam == VK_ESCAPE && !SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0)) {
            SetFocus(g.hEdit);
            return 0;
        }
        if (wParam == VK_TAB) {
            bool back = GetKeyState(VK_SHIFT) < 0;
            if (combo == s.hFontName) SetFocus(back ? g.hEdit : s.hFontSize);
            else SetFocus(back ? s.hFontName : g.hEdit);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam == L'\r' || wParam == 0x1B || wParam == L'\t') return 0;   // no beep
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ComboEditProc, id);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void CreateChildren()
{
    DWORD common = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_HASSTRINGS;
    s.hFontName = CreateWindowExW(0, WC_COMBOBOXW, L"", common | CBS_SORT, 0, 0, 100, 400, s.hwnd,
                                  (HMENU)(INT_PTR)ID_FORMAT_FONTNAME, g.hInst, nullptr);
    s.hFontSize = CreateWindowExW(0, WC_COMBOBOXW, L"", common, 0, 0, 60, 400, s.hwnd,
                                  (HMENU)(INT_PTR)ID_FORMAT_FONTSIZE, g.hInst, nullptr);
    static const int kSizes[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 };
    for (int v : kSizes) SendMessageW(s.hFontSize, CB_ADDSTRING, 0, (LPARAM)std::to_wstring(v).c_str());
    SendMessageW(s.hFontName, CB_LIMITTEXT, LF_FACESIZE - 1, 0);
    SendMessageW(s.hFontSize, CB_LIMITTEXT, 6, 0);
    SendMessageW(s.hFontName, CB_SETMINVISIBLE, 18, 0);
    SendMessageW(s.hFontSize, CB_SETMINVISIBLE, 16, 0);
    for (HWND h : { s.hFontName, s.hFontSize }) {
        COMBOBOXINFO cbi = {};
        cbi.cbSize = sizeof(cbi);
        if (GetComboBoxInfo(h, &cbi) && cbi.hwndItem) SetWindowSubclass(cbi.hwndItem, ComboEditProc, 1, 0);
    }
    PopulateFonts();

    s.hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, s.hwnd, nullptr, g.hInst, nullptr);
    SendMessageW(s.hTip, TTM_SETMAXTIPWIDTH, 0, 480);
}

void ApplyChildTheme()
{
    HFONT font = Theme_Font(s.dpi);
    for (HWND h : { s.hFontName, s.hFontSize }) {
        if (!h) continue;
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        Theme_ApplyControl(h, L"CFD");
        COMBOBOXINFO cbi = {};
        cbi.cbSize = sizeof(cbi);
        if (GetComboBoxInfo(h, &cbi) && cbi.hwndList) Theme_ApplyControl(cbi.hwndList, L"Explorer");
    }
    Theme_ApplyTooltip(s.hTip);
}

void SetHot(int item, bool drop, int menu)
{
    if (item != s.hot || drop != s.hotDrop) {
        InvalidateItem(s.hot);
        s.hot = item;
        s.hotDrop = drop;
        InvalidateItem(s.hot);
    }
    if (menu != s.hotMenu) {
        s.hotMenu = menu;
        InvalidateMenuRow();
    }
}

// Tooltips must not pop up over an open menu or palette.
void SuspendTooltips(bool suspend)
{
    if (!s.hTip) return;
    if (suspend) SendMessageW(s.hTip, TTM_POP, 0, 0);
    SendMessageW(s.hTip, TTM_ACTIVATE, suspend ? FALSE : TRUE, 0);
}

void OpenDropDown(int idx, int command)
{
    s.activeDrop = idx;
    InvalidateItem(idx);
    UpdateWindow(s.hwnd);
    SuspendTooltips(true);
    SendMessageW(g.hMain, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    SuspendTooltips(false);
    s.activeDrop = -1;
    s.closedTick = GetTickCount();
    s.closedItem = idx;
    InvalidateItem(idx);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        s.hwnd = hwnd;
        s.dpi = DpiOf(hwnd);
        // Menu mnemonics are plain letters; no input method composition while the menu row has focus.
        ImmAssociateContext(hwnd, nullptr);
        BuildItems();
        CreateChildren();
        ApplyChildTheme();
        Layout();
        return 0;

    case WM_SIZE:
        Layout();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Paint(hdc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        bool drop = false;
        int item = HitItem(pt, &drop);
        if (item >= 0 && !s.items[item].enabled) item = -1;
        if (s.pressed >= 0 && item != s.pressed) item = -1;
        SetHot(item, drop, s.pressed >= 0 ? -1 : HitMenu(pt));
        if (!s.mouseTracked) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            s.mouseTracked = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        s.mouseTracked = false;
        SetHot(-1, false, -1);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        // A click that dismissed a popup must not immediately reopen the same popup.
        bool dismissing = (DWORD)GetMessageTime() <= s.closedTick && GetTickCount() - s.closedTick < 500;
        int menu = HitMenu(pt);
        if (menu >= 0) {
            if (dismissing && menu == s.closedMenu) { s.closedMenu = -1; return 0; }
            TrackMenu(menu, false);
            return 0;
        }
        bool drop = false;
        int item = HitItem(pt, &drop);
        if (item < 0 || !s.items[item].enabled) return 0;
        const Item& it = s.items[item];
        if (it.kind == K_DROPDOWN || (it.kind == K_SPLIT && drop)) {
            if (dismissing && item == s.closedItem) { s.closedItem = -1; return 0; }
            OpenDropDown(item, it.kind == K_SPLIT ? it.dropId : it.id);
            return 0;
        }
        s.pressed = item;
        s.pressedDrop = drop;
        SetCapture(hwnd);
        InvalidateItem(item);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (s.pressed < 0) return 0;
        int item = s.pressed;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        s.pressed = -1;
        ReleaseCapture();
        InvalidateItem(item);
        if (HitItem(pt, nullptr) == item && s.items[item].enabled)
            SendMessageW(g.hMain, WM_COMMAND, MAKEWPARAM(s.items[item].id, 0), 0);
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (s.pressed >= 0) { InvalidateItem(s.pressed); s.pressed = -1; }
        return 0;

    case WM_COMMAND: {
        HWND ctl = (HWND)lParam;
        if (ctl == s.hFontName || ctl == s.hFontSize) {
            int which = ctl == s.hFontSize ? 1 : 0;
            switch (HIWORD(wParam)) {
            case CBN_SELENDOK: {
                // The edit text is not updated yet; remember the chosen list entry.
                int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    wchar_t buf[128] = L"";
                    if (SendMessageW(ctl, CB_GETLBTEXTLEN, sel, 0) < 127) SendMessageW(ctl, CB_GETLBTEXT, sel, (LPARAM)buf);
                    s.pendingFace = buf;
                    PostMessageW(hwnd, WM_CB_APPLYFONT, which, 0);
                }
                break;
            }
            case CBN_KILLFOCUS:
                ShowFontInfo(true);
                break;
            case CBN_DROPDOWN:
                SuspendTooltips(true);
                break;
            case CBN_CLOSEUP:
                SuspendTooltips(false);
                break;
            }
            return 0;
        }
        break;
    }

    case WM_CB_APPLYFONT: {
        // lParam 1: Enter was pressed in the edit field; use the typed text.
        HWND ctl = wParam == 1 ? s.hFontSize : s.hFontName;
        if (lParam == 1) {
            wchar_t buf[128] = L"";
            GetWindowTextW(ctl, buf, 128);
            s.pendingFace = buf;
            if (wParam == 0 && buf[0]) {
                // Complete a partially typed font name.
                int idx = (int)SendMessageW(ctl, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)buf);
                if (idx < 0) idx = (int)SendMessageW(ctl, CB_FINDSTRING, (WPARAM)-1, (LPARAM)buf);
                if (idx >= 0 && SendMessageW(ctl, CB_GETLBTEXTLEN, idx, 0) < 127) {
                    SendMessageW(ctl, CB_GETLBTEXT, idx, (LPARAM)buf);
                    s.pendingFace = buf;
                }
            }
        }
        SendMessageW(g.hMain, WM_APP_FONTAPPLY, wParam, 0);
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->hwndFrom == s.hTip && hdr->code == TTN_GETDISPINFOW) {
            NMTTDISPINFOW* di = (NMTTDISPINFOW*)lParam;
            int tip = -1;
            if (di->uFlags & TTF_IDISHWND) {
                HWND h = (HWND)hdr->idFrom;
                tip = (h == s.hFontSize) ? S_TIP_FONTSIZE : S_TIP_FONTNAME;
            } else {
                UINT_PTR id = hdr->idFrom;
                if (id >= 10000) {
                    size_t idx = id - 10000;
                    if (idx < s.items.size()) tip = s.items[idx].id == ID_EDIT_PASTE ? S_TIP_PASTE_MORE : s.items[idx].tip;
                } else if (id >= 1 && id - 1 < s.items.size()) {
                    tip = s.items[id - 1].tip;
                }
            }
            di->lpszText = (LPWSTR)(tip >= 0 ? S(tip) : L"");
            return 0;
        }
        break;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
        if (T.dark) {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, T.text);
            SetBkColor(hdc, T.field);
            return (LRESULT)Theme_Brush(T.field);
        }
        break;

    case WM_KEYDOWN:
        if (s.menuMode) {
            switch (wParam) {
            case VK_LEFT:  s.focusMenu = (s.focusMenu + kMenuCount - 1) % kMenuCount; InvalidateMenuRow(); break;
            case VK_RIGHT: s.focusMenu = (s.focusMenu + 1) % kMenuCount; InvalidateMenuRow(); break;
            case VK_DOWN: case VK_UP: case VK_RETURN: case VK_SPACE: TrackMenu(s.focusMenu, true); break;
            case VK_ESCAPE: ExitMenuMode(); break;
            }
            return 0;
        }
        break;

    case WM_CHAR:
        if (s.menuMode) {
            int idx = MenuByMnemonic((wchar_t)wParam);
            if (idx >= 0) TrackMenu(idx, true);
            else if (wParam >= 0x20) MessageBeep(MB_OK);
            return 0;
        }
        break;

    case WM_KILLFOCUS:
        if (s.menuMode && !s.tracking) {
            s.menuMode = false;
            SetMnemonics(false);
            InvalidateMenuRow();
        }
        return 0;

    case WM_FONTCHANGE:
        PopulateFonts();
        ShowFontInfo(true);
        return 0;

    case WM_DESTROY:
        s.hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

Item* FindItem(int id)
{
    for (Item& it : s.items)
        if (it.id == id && it.kind != K_GAP) return &it;
    return nullptr;
}

} // namespace

// ===========================================================================

bool CmdBar_Register()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g.hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc) != 0;
}

HWND CmdBar_Create(HWND parent)
{
    return CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                           0, 0, 100, 100, parent, nullptr, g.hInst, nullptr);
}

int CmdBar_GetHeight()
{
    int dpi = s.hwnd ? DpiOf(s.hwnd) : 96;
    // menu row + top line + padding + content + group label + padding + bottom line
    return Scale(32, dpi) + 1 + Scale(5, dpi) + Scale(66, dpi) + Scale(18, dpi) + Scale(3, dpi) + 1;
}

void CmdBar_SetEnabled(int id, bool enabled)
{
    for (Item& it : s.items) {
        if (it.id == id && it.enabled != enabled) {
            it.enabled = enabled;
            InvalidateRect(s.hwnd, &it.rc, FALSE);
        }
    }
    if (id == ID_FORMAT_FONTNAME && s.hFontName) EnableWindow(s.hFontName, enabled);
    if (id == ID_FORMAT_FONTSIZE && s.hFontSize) EnableWindow(s.hFontSize, enabled);
}

void CmdBar_SetChecked(int id, bool checked)
{
    for (Item& it : s.items) {
        if (it.id == id && it.checked != checked) {
            it.checked = checked;
            InvalidateRect(s.hwnd, &it.rc, FALSE);
        }
    }
}

void CmdBar_SetSwatch(int id, COLORREF color)
{
    if (Item* it = FindItem(id)) {
        if (!it->hasSwatch || it->swatch != color) {
            it->hasSwatch = true;
            it->swatch = color;
            InvalidateRect(s.hwnd, &it->rc, FALSE);
        }
    }
}

void CmdBar_SetFontInfo(const wchar_t* face, int sizeTwips)
{
    s.fontFace = face ? face : L"";
    s.fontSize = sizeTwips;
    ShowFontInfo(false);
}

void CmdBar_Commit()
{
    // Invalidation happens as states change; nothing else to do, but keep the hook for callers.
}

bool CmdBar_GetItemScreenRect(int id, RECT* rc)
{
    for (size_t i = 0; i < s.items.size(); ++i) {
        const Item& it = s.items[i];
        if ((it.id == id || (it.dropId == id && id != 0)) && it.kind != K_GAP && ItemVisible((int)i) && !IsRectEmpty(&it.rc)) {
            *rc = it.rc;
            MapWindowPoints(s.hwnd, nullptr, (POINT*)rc, 2);
            return true;
        }
    }
    return false;
}

std::wstring CmdBar_GetFontName() { return s.pendingFace; }

int CmdBar_GetFontSizeTwips()
{
    // Accepts "12", "10.5" and "10,5".
    std::wstring t = s.pendingFace;
    for (wchar_t& c : t) if (c == L',') c = L'.';
    wchar_t* end = nullptr;
    double v = wcstod(t.c_str(), &end);
    while (end && *end == L' ') ++end;
    if (!end || end == t.c_str() || *end != 0) return -1;
    if (v < 1.0 || v > 1638.0) return -1;
    return (int)(v * 2 + 0.5) * 10;   // rounded to half points
}

void CmdBar_RevertFontBoxes() { ShowFontInfo(true); }

void CmdBar_Rebuild()
{
    if (!s.hwnd) return;
    s.dpi = DpiOf(s.hwnd);
    BuildItems();
    ApplyChildTheme();
    Layout();
    ShowFontInfo(true);
    RedrawWindow(s.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

void CmdBar_RefreshFonts()
{
    PopulateFonts();
    ShowFontInfo(true);
}

bool CmdBar_OpenMenuByMnemonic(wchar_t ch)
{
    int idx = MenuByMnemonic(ch);
    if (idx < 0 || s.tracking) return false;
    TrackMenu(idx, true);
    return true;
}

void CmdBar_EnterMenuMode()
{
    if (s.tracking) return;
    if (s.menuMode) { ExitMenuMode(); return; }
    s.menuMode = true;
    s.focusMenu = 0;
    SetMnemonics(true);
    SetFocus(s.hwnd);
    InvalidateMenuRow();
}

bool CmdBar_IsChildFocused()
{
    HWND focus = GetFocus();
    return focus && s.hwnd && focus != s.hwnd && IsChild(s.hwnd, focus);
}

void CmdBar_OnMenuSelect(WPARAM wParam, LPARAM lParam)
{
    if (!s.tracking || s.openMenu < 0) return;
    UINT flags = HIWORD(wParam);
    if (flags == 0xFFFF && lParam == 0) return;   // menu closed
    bool inTop = (HMENU)lParam == g.hMenus[s.openMenu];
    s.selIsPopup = (flags & MF_POPUP) != 0;
    if (inTop && !s.selIsPopup) s.subOpen = false;
}

void CmdBar_OnInitMenuPopup(HMENU menu)
{
    if (s.tracking && s.openMenu >= 0 && menu != g.hMenus[s.openMenu]) s.subOpen = true;
}

void CmdBar_OnUninitMenuPopup(HMENU menu)
{
    if (s.tracking && s.openMenu >= 0 && menu != g.hMenus[s.openMenu]) s.subOpen = false;
}

void CmdBar_TrackItemMenu(int itemId, HMENU menu)
{
    RECT rc;
    POINT pt;
    TPMPARAMS tp = {};
    tp.cbSize = sizeof(tp);
    bool anchored = CmdBar_GetItemScreenRect(itemId, &rc);
    if (anchored) {
        tp.rcExclude = rc;
        pt.x = rc.left;
        pt.y = rc.bottom;
    } else {
        GetCursorPos(&pt);
    }
    TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL, pt.x, pt.y, g.hMain, anchored ? &tp : nullptr);
}
