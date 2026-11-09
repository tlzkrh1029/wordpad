// Dialog boxes: paragraph, tabs, date and time, font, find / replace, about.
#include "dialogs.h"
#include "editor.h"
#include "ruler.h"
#include "theme.h"

namespace {

struct CtlText { int id; int str; };

void CenterOnOwner(HWND dlg)
{
    HWND owner = GetWindow(dlg, GW_OWNER);
    RECT rd, ro;
    GetWindowRect(dlg, &rd);
    if (!owner || !GetWindowRect(owner, &ro)) return;
    int w = rd.right - rd.left, h = rd.bottom - rd.top;
    int x = ro.left + ((ro.right - ro.left) - w) / 2;
    int y = ro.top + ((ro.bottom - ro.top) - h) / 2;
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi)) {
        x = std::clamp(x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - w));
        y = std::clamp(y, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - h));
    }
    SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Localize(HWND dlg, int title, std::initializer_list<CtlText> items)
{
    SetWindowTextW(dlg, S(title));
    for (const CtlText& it : items) SetDlgItemTextW(dlg, it.id, S(it.str));
    CenterOnOwner(dlg);
}

std::wstring GetText(HWND dlg, int id)
{
    wchar_t buf[256] = L"";
    GetDlgItemTextW(dlg, id, buf, ARRAYSIZE(buf));
    std::wstring t = buf;
    while (!t.empty() && t.back() == L' ') t.pop_back();
    size_t start = 0;
    while (start < t.size() && t[start] == L' ') ++start;
    return t.substr(start);
}

void InvalidField(HWND dlg, int id)
{
    MessageBoxW(dlg, S(S_MSG_INVALIDNUMBER), APP_NAME, MB_OK | MB_ICONWARNING);
    HWND ctl = GetDlgItem(dlg, id);
    SetFocus(ctl);
    SendMessageW(ctl, EM_SETSEL, 0, -1);
}

// ---------------------------------------------------------------------------
// Paragraph
// ---------------------------------------------------------------------------
struct ParagraphInit { int spacingIndex; int alignIndex; bool spaceAfter; } g_paraInit;

INT_PTR CALLBACK ParagraphProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM)
{
    static const int kSpacing[] = { 100, 115, 150, 200 };
    static const WORD kAlign[] = { PFA_LEFT, PFA_CENTER, PFA_RIGHT, PFA_JUSTIFY };
    switch (msg) {
    case WM_INITDIALOG: {
        Localize(dlg, S_DLG_PARAGRAPH, {
            { IDC_GROUP_1, S_DLG_INDENT }, { IDC_STATIC_1, S_DLG_LEFT }, { IDC_STATIC_2, S_DLG_RIGHT },
            { IDC_STATIC_3, S_DLG_FIRSTLINE }, { IDC_GROUP_2, S_DLG_SPACING }, { IDC_STATIC_4, S_DLG_LINESPACING },
            { IDC_PARA_SPACEAFTER, S_DLG_SPACEAFTER }, { IDC_STATIC_5, S_DLG_ALIGNMENT },
            { IDOK, S_DLG_OK }, { IDCANCEL, S_DLG_CANCEL } });
        PARAFORMAT2 pf;
        Editor_GetParaFormat(pf);
        if ((pf.dwMask & PFM_STARTINDENT) && (pf.dwMask & PFM_OFFSET)) {
            SetDlgItemTextW(dlg, IDC_PARA_LEFT, Measure_Format(pf.dxStartIndent + pf.dxOffset).c_str());
            SetDlgItemTextW(dlg, IDC_PARA_FIRST, Measure_Format(-pf.dxOffset).c_str());
        }
        if (pf.dwMask & PFM_RIGHTINDENT)
            SetDlgItemTextW(dlg, IDC_PARA_RIGHT, Measure_Format(pf.dxRightIndent).c_str());

        HWND spacing = GetDlgItem(dlg, IDC_PARA_SPACING);
        const wchar_t* names[] = { L"1.0", L"1.15", L"1.5", L"2.0" };
        int current = Editor_GetLineSpacingPercent(pf);
        g_paraInit.spacingIndex = -1;
        for (int i = 0; i < 4; ++i) {
            SendMessageW(spacing, CB_ADDSTRING, 0, (LPARAM)names[i]);
            if (kSpacing[i] == current) g_paraInit.spacingIndex = i;
        }
        SendMessageW(spacing, CB_SETCURSEL, (WPARAM)g_paraInit.spacingIndex, 0);

        g_paraInit.spaceAfter = (pf.dwMask & PFM_SPACEAFTER) && pf.dySpaceAfter > 0;
        CheckDlgButton(dlg, IDC_PARA_SPACEAFTER, g_paraInit.spaceAfter ? BST_CHECKED : BST_UNCHECKED);

        HWND align = GetDlgItem(dlg, IDC_PARA_ALIGN);
        const int alignNames[] = { S_DLG_ALIGN_LEFT, S_DLG_ALIGN_CENTER, S_DLG_ALIGN_RIGHT, S_DLG_ALIGN_JUSTIFY };
        g_paraInit.alignIndex = -1;
        for (int i = 0; i < 4; ++i) {
            SendMessageW(align, CB_ADDSTRING, 0, (LPARAM)S(alignNames[i]));
            if ((pf.dwMask & PFM_ALIGNMENT) && pf.wAlignment == kAlign[i]) g_paraInit.alignIndex = i;
        }
        SendMessageW(align, CB_SETCURSEL, (WPARAM)g_paraInit.alignIndex, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        if (LOWORD(wParam) == IDOK) {
            PARAFORMAT2 pf = {};
            pf.cbSize = sizeof(pf);
            std::wstring sl = GetText(dlg, IDC_PARA_LEFT), sr = GetText(dlg, IDC_PARA_RIGHT), sf = GetText(dlg, IDC_PARA_FIRST);
            int left = 0, right = 0, first = 0;
            if (!sl.empty() || !sf.empty()) {
                if (!Measure_Parse(sl.c_str(), &left) || left < 0) { InvalidField(dlg, IDC_PARA_LEFT); return TRUE; }
                if (sf.empty()) first = 0;
                else if (!Measure_Parse(sf.c_str(), &first)) { InvalidField(dlg, IDC_PARA_FIRST); return TRUE; }
                if (left + first < 0) { InvalidField(dlg, IDC_PARA_FIRST); return TRUE; }
                pf.dwMask |= PFM_STARTINDENT | PFM_OFFSET;
                pf.dxStartIndent = left + first;
                pf.dxOffset = -first;
            }
            if (!sr.empty()) {
                if (!Measure_Parse(sr.c_str(), &right) || right < 0) { InvalidField(dlg, IDC_PARA_RIGHT); return TRUE; }
                pf.dwMask |= PFM_RIGHTINDENT;
                pf.dxRightIndent = right;
            }
            int si = (int)SendDlgItemMessageW(dlg, IDC_PARA_SPACING, CB_GETCURSEL, 0, 0);
            if (si >= 0 && si != g_paraInit.spacingIndex) {
                pf.dwMask |= PFM_LINESPACING;
                pf.bLineSpacingRule = 5;
                pf.dyLineSpacing = MulDiv(kSpacing[si], 20, 100);
            }
            bool after = IsDlgButtonChecked(dlg, IDC_PARA_SPACEAFTER) == BST_CHECKED;
            if (after != g_paraInit.spaceAfter) {
                pf.dwMask |= PFM_SPACEAFTER;
                pf.dySpaceAfter = after ? 200 : 0;
            }
            int ai = (int)SendDlgItemMessageW(dlg, IDC_PARA_ALIGN, CB_GETCURSEL, 0, 0);
            if (ai >= 0 && ai != g_paraInit.alignIndex) {
                pf.dwMask |= PFM_ALIGNMENT;
                pf.wAlignment = kAlign[ai];
            }
            if (pf.dwMask) Editor_SetParaFormat(pf);
            EndDialog(dlg, 1);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------
std::vector<LONG> g_tabs;

void FillTabList(HWND dlg)
{
    HWND list = GetDlgItem(dlg, IDC_TABS_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    std::sort(g_tabs.begin(), g_tabs.end(), [](LONG a, LONG b) { return (a & 0x00FFFFFF) < (b & 0x00FFFFFF); });
    for (LONG t : g_tabs) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)Measure_Format(t & 0x00FFFFFF).c_str());
}

INT_PTR CALLBACK TabsProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG: {
        Localize(dlg, S_DLG_TABS, { { IDC_STATIC_1, S_DLG_TABSTOP }, { IDC_TABS_SET, S_DLG_TAB_SET },
            { IDC_TABS_CLEAR, S_DLG_TAB_CLEAR }, { IDC_TABS_CLEARALL, S_DLG_TAB_CLEARALL },
            { IDOK, S_DLG_OK }, { IDCANCEL, S_DLG_CANCEL } });
        PARAFORMAT2 pf;
        Editor_GetParaFormat(pf);
        g_tabs.clear();
        if (pf.dwMask & PFM_TABSTOPS)
            for (int i = 0; i < pf.cTabCount && i < MAX_TAB_STOPS; ++i) g_tabs.push_back(pf.rgxTabs[i]);
        FillTabList(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_TABS_SET: {
            int tw = 0;
            if (!Measure_Parse(GetText(dlg, IDC_TABS_EDIT).c_str(), &tw) || tw <= 0) { InvalidField(dlg, IDC_TABS_EDIT); return TRUE; }
            bool exists = false;
            for (LONG t : g_tabs) if ((t & 0x00FFFFFF) == tw) exists = true;
            if (!exists && g_tabs.size() < MAX_TAB_STOPS) g_tabs.push_back(tw);
            FillTabList(dlg);
            SetDlgItemTextW(dlg, IDC_TABS_EDIT, L"");
            SetFocus(GetDlgItem(dlg, IDC_TABS_EDIT));
            return TRUE;
        }
        case IDC_TABS_CLEAR: {
            int sel = (int)SendDlgItemMessageW(dlg, IDC_TABS_LIST, LB_GETCURSEL, 0, 0);
            if (sel < 0) {
                // No list selection: clear the position typed in the edit field.
                int tw = 0;
                if (Measure_Parse(GetText(dlg, IDC_TABS_EDIT).c_str(), &tw))
                    for (size_t i = 0; i < g_tabs.size(); ++i)
                        if (std::abs((int)(g_tabs[i] & 0x00FFFFFF) - tw) <= 8) { sel = (int)i; break; }
            }
            if (sel >= 0 && sel < (int)g_tabs.size()) g_tabs.erase(g_tabs.begin() + sel);
            FillTabList(dlg);
            SetDlgItemTextW(dlg, IDC_TABS_EDIT, L"");
            return TRUE;
        }
        case IDC_TABS_CLEARALL:
            g_tabs.clear();
            FillTabList(dlg);
            return TRUE;
        case IDC_TABS_LIST:
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                int sel = (int)SendDlgItemMessageW(dlg, IDC_TABS_LIST, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)g_tabs.size())
                    SetDlgItemTextW(dlg, IDC_TABS_EDIT, Measure_Format(g_tabs[sel] & 0x00FFFFFF).c_str());
            }
            return TRUE;
        case IDOK: {
            // A position that was typed but not yet "Set" is applied as well.
            int tw = 0;
            std::wstring pending = GetText(dlg, IDC_TABS_EDIT);
            if (!pending.empty() && Measure_Parse(pending.c_str(), &tw) && tw > 0 && g_tabs.size() < MAX_TAB_STOPS) {
                bool exists = false;
                for (LONG t : g_tabs) if (std::abs((int)(t & 0x00FFFFFF) - tw) <= 8) exists = true;
                if (!exists) g_tabs.push_back(tw);
            }
            std::sort(g_tabs.begin(), g_tabs.end(), [](LONG a, LONG b) { return (a & 0x00FFFFFF) < (b & 0x00FFFFFF); });
            PARAFORMAT2 pf = {};
            pf.cbSize = sizeof(pf);
            pf.dwMask = PFM_TABSTOPS;
            pf.cTabCount = (SHORT)g_tabs.size();
            for (size_t i = 0; i < g_tabs.size(); ++i) pf.rgxTabs[i] = g_tabs[i];
            Editor_SetParaFormat(pf);
            EndDialog(dlg, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Date and time
// ---------------------------------------------------------------------------
struct DateCollector {
    SYSTEMTIME now;
    std::vector<std::wstring> items;
    void Add(const std::wstring& s)
    {
        if (s.empty()) return;
        if (std::find(items.begin(), items.end(), s) == items.end()) items.push_back(s);
    }
    void AddDate(const wchar_t* pattern, DWORD flags = 0)
    {
        wchar_t buf[160];
        if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, flags, &now, pattern, buf, ARRAYSIZE(buf), nullptr)) Add(buf);
    }
    void AddTime(const wchar_t* pattern, DWORD flags = 0)
    {
        wchar_t buf[160];
        if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, flags, &now, pattern, buf, ARRAYSIZE(buf))) Add(buf);
    }
};

BOOL CALLBACK DateEnumProc(LPWSTR pattern, CALID, LPARAM lp)
{
    ((DateCollector*)lp)->AddDate(pattern);
    return TRUE;
}

BOOL CALLBACK TimeEnumProc(LPWSTR pattern, LPARAM lp)
{
    ((DateCollector*)lp)->AddTime(pattern);
    return TRUE;
}

std::vector<std::wstring> g_dateItems;

INT_PTR CALLBACK DateTimeProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG: {
        Localize(dlg, S_DLG_DATETIME, { { IDC_STATIC_1, S_DLG_FORMATS }, { IDOK, S_DLG_OK }, { IDCANCEL, S_DLG_CANCEL } });
        DateCollector dc;
        GetLocalTime(&dc.now);
        EnumDateFormatsExEx(DateEnumProc, LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, (LPARAM)&dc);
        dc.AddDate(L"yyyy-MM-dd");
        EnumDateFormatsExEx(DateEnumProc, LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, (LPARAM)&dc);
        // Long date followed by the time.
        {
            wchar_t d[160], t[80];
            if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &dc.now, nullptr, d, ARRAYSIZE(d), nullptr) &&
                GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &dc.now, nullptr, t, ARRAYSIZE(t)))
                dc.Add(std::wstring(d) + L" " + t);
        }
        dc.AddTime(nullptr, TIME_NOSECONDS);
        EnumTimeFormatsEx(TimeEnumProc, LOCALE_NAME_USER_DEFAULT, 0, (LPARAM)&dc);
        dc.AddTime(L"HH:mm");
        dc.AddTime(L"HH:mm:ss");
        g_dateItems = dc.items;
        HWND list = GetDlgItem(dlg, IDC_DATE_LIST);
        for (const std::wstring& s : g_dateItems) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)s.c_str());
        SendMessageW(list, LB_SETCURSEL, 0, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_DATE_LIST && HIWORD(wParam) == LBN_DBLCLK) {
            SendMessageW(dlg, WM_COMMAND, IDOK, 0);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK) {
            int sel = (int)SendDlgItemMessageW(dlg, IDC_DATE_LIST, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_dateItems.size()) Editor_ReplaceSel(g_dateItems[sel].c_str(), true);
            EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// About / text viewer
// ---------------------------------------------------------------------------
HFONT g_aboutTitleFont = nullptr;
HICON g_aboutIcon = nullptr;

INT_PTR CALLBACK AboutProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG: {
        Localize(dlg, S_DLG_ABOUT, { { IDC_ABOUT_DESC, S_ABOUT_DESC }, { IDC_ABOUT_NOTE, S_ABOUT_NOTE }, { IDOK, S_DLG_OK } });
        SetDlgItemTextW(dlg, IDC_ABOUT_TITLE, APP_NAME);
        SetDlgItemTextW(dlg, IDC_ABOUT_VERSION, Fmt(S(S_ABOUT_VERSION), APP_VERSION).c_str());
        int dpi = DpiOf(dlg);
        int size = Scale(48, dpi);
        g_aboutIcon = (HICON)LoadImageW(g.hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
        if (g_aboutIcon) SendDlgItemMessageW(dlg, IDC_ABOUT_ICON, STM_SETICON, (WPARAM)g_aboutIcon, 0);
        HFONT base = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
        LOGFONTW lf = {};
        if (base && GetObjectW(base, sizeof(lf), &lf)) {
            lf.lfWeight = FW_SEMIBOLD;
            lf.lfHeight = MulDiv(lf.lfHeight, 14, 9);
            g_aboutTitleFont = CreateFontIndirectW(&lf);
            SendDlgItemMessageW(dlg, IDC_ABOUT_TITLE, WM_SETFONT, (WPARAM)g_aboutTitleFont, TRUE);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;
    case WM_DESTROY:
        if (g_aboutTitleFont) { DeleteObject(g_aboutTitleFont); g_aboutTitleFont = nullptr; }
        if (g_aboutIcon) { DestroyIcon(g_aboutIcon); g_aboutIcon = nullptr; }
        break;
    }
    return FALSE;
}

INT_PTR CALLBACK TextViewProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG: {
        Localize(dlg, S_SHORTCUTS_TITLE, { { IDOK, S_DLG_CLOSE } });
        std::wstring text;
        for (const wchar_t* p = S(S_SHORTCUTS_TEXT); *p; ++p) {
            if (*p == L'\n') text += L"\r\n"; else text += *p;
        }
        HWND edit = GetDlgItem(dlg, IDC_TEXTVIEW_EDIT);
        UINT tab = 120;   // dialog units
        SendMessageW(edit, EM_SETTABSTOPS, 1, (LPARAM)&tab);
        SetWindowTextW(edit, text.c_str());
        SetFocus(GetDlgItem(dlg, IDOK));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Picture size
// ---------------------------------------------------------------------------
bool g_picUpdating = false;

INT_PTR CALLBACK PictureSizeProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG:
        Localize(dlg, S_DLG_PICTURESIZE, { { IDC_STATIC_1, S_DLG_PIC_WIDTH }, { IDC_STATIC_2, S_DLG_PIC_HEIGHT },
            { IDC_PIC_LOCK, S_DLG_PIC_LOCK }, { IDOK, S_DLG_OK }, { IDCANCEL, S_DLG_CANCEL } });
        g_picUpdating = true;
        SetDlgItemInt(dlg, IDC_PIC_WIDTH, 100, FALSE);
        SetDlgItemInt(dlg, IDC_PIC_HEIGHT, 100, FALSE);
        g_picUpdating = false;
        CheckDlgButton(dlg, IDC_PIC_LOCK, BST_CHECKED);
        return TRUE;
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && !g_picUpdating && IsDlgButtonChecked(dlg, IDC_PIC_LOCK) == BST_CHECKED &&
            (LOWORD(wParam) == IDC_PIC_WIDTH || LOWORD(wParam) == IDC_PIC_HEIGHT)) {
            BOOL ok = FALSE;
            UINT v = GetDlgItemInt(dlg, LOWORD(wParam), &ok, FALSE);
            if (ok) {
                g_picUpdating = true;
                SetDlgItemInt(dlg, LOWORD(wParam) == IDC_PIC_WIDTH ? IDC_PIC_HEIGHT : IDC_PIC_WIDTH, v, FALSE);
                g_picUpdating = false;
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK) {
            BOOL okW = FALSE, okH = FALSE;
            UINT w = GetDlgItemInt(dlg, IDC_PIC_WIDTH, &okW, FALSE), h = GetDlgItemInt(dlg, IDC_PIC_HEIGHT, &okH, FALSE);
            if (!okW || w < 1 || w > 1000) { InvalidField(dlg, IDC_PIC_WIDTH); return TRUE; }
            if (!okH || h < 1 || h > 1000) { InvalidField(dlg, IDC_PIC_HEIGHT); return TRUE; }
            Editor_ScaleSelectedPicture((int)w, (int)h);
            EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Find / replace (standard modeless common dialogs)
// ---------------------------------------------------------------------------
FINDREPLACEW g_fr;
wchar_t g_findBuf[256] = L"";
wchar_t g_replaceBuf[256] = L"";
UINT g_findMsg = 0;
bool g_isReplace = false;
std::wstring g_lastFind;
DWORD g_lastFlags = FR_DOWN;

void NotFound(const std::wstring& what)
{
    HWND owner = g.hFindDlg ? g.hFindDlg : g.hMain;
    if (g.selfTest) return;
    MessageBoxW(owner, Fmt(S(S_MSG_NOTFOUND), what.c_str()).c_str(), APP_NAME, MB_OK | MB_ICONINFORMATION);
}

} // namespace

void Dlg_Paragraph(HWND owner) { DialogBoxParamW(g.hInst, MAKEINTRESOURCEW(IDD_PARAGRAPH), owner, ParagraphProc, 0); }
void Dlg_Tabs(HWND owner) { DialogBoxParamW(g.hInst, MAKEINTRESOURCEW(IDD_TABS), owner, TabsProc, 0); }
void Dlg_DateTime(HWND owner) { DialogBoxParamW(g.hInst, MAKEINTRESOURCEW(IDD_DATETIME), owner, DateTimeProc, 0); }
void Dlg_About(HWND owner) { DialogBoxParamW(g.hInst, MAKEINTRESOURCEW(IDD_ABOUT), owner, AboutProc, 0); }
void Dlg_Shortcuts(HWND owner) { DialogBoxParamW(g.hInst, MAKEINTRESOURCEW(IDD_TEXTVIEW), owner, TextViewProc, 0); }
void Dlg_PictureSize(HWND owner) { DialogBoxParamW(g.hInst, MAKEINTRESOURCEW(IDD_PICTURESIZE), owner, PictureSizeProc, 0); }

void Dlg_Font(HWND owner)
{
    CHARFORMAT2W cf;
    Editor_GetCharFormat(cf);
    HDC screen = GetDC(nullptr);
    int logPix = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);

    LOGFONTW lf = {};
    CHOOSEFONTW ch = {};
    ch.lStructSize = sizeof(ch);
    ch.hwndOwner = owner;
    ch.lpLogFont = &lf;
    ch.Flags = CF_SCREENFONTS | CF_EFFECTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS | CF_NOSCRIPTSEL;
    lf.lfCharSet = DEFAULT_CHARSET;
    if (cf.dwMask & CFM_FACE) lstrcpynW(lf.lfFaceName, cf.szFaceName, LF_FACESIZE); else ch.Flags |= CF_NOFACESEL;
    if (cf.dwMask & CFM_SIZE) lf.lfHeight = -MulDiv(cf.yHeight, logPix, 1440); else ch.Flags |= CF_NOSIZESEL;
    if ((cf.dwMask & CFM_BOLD) && (cf.dwMask & CFM_ITALIC)) {
        lf.lfWeight = (cf.dwEffects & CFE_BOLD) ? FW_BOLD : FW_NORMAL;
        lf.lfItalic = (cf.dwEffects & CFE_ITALIC) ? TRUE : FALSE;
    } else {
        ch.Flags |= CF_NOSTYLESEL;
    }
    lf.lfUnderline = ((cf.dwMask & CFM_UNDERLINE) && (cf.dwEffects & CFE_UNDERLINE)) ? TRUE : FALSE;
    lf.lfStrikeOut = ((cf.dwMask & CFM_STRIKEOUT) && (cf.dwEffects & CFE_STRIKEOUT)) ? TRUE : FALSE;
    bool wasAuto = !(cf.dwMask & CFM_COLOR) || (cf.dwEffects & CFE_AUTOCOLOR);
    COLORREF initial = wasAuto ? RGB(0, 0, 0) : cf.crTextColor;
    ch.rgbColors = initial;

    if (!ChooseFontW(&ch)) return;

    CHARFORMAT2W out = {};
    out.cbSize = sizeof(out);
    if (!(ch.Flags & CF_NOFACESEL) && lf.lfFaceName[0]) {
        out.dwMask |= CFM_FACE | CFM_CHARSET;
        lstrcpynW(out.szFaceName, lf.lfFaceName, LF_FACESIZE);
        out.bCharSet = DEFAULT_CHARSET;
    }
    if (!(ch.Flags & CF_NOSIZESEL) && ch.iPointSize > 0) {
        out.dwMask |= CFM_SIZE;
        out.yHeight = ch.iPointSize * 2;   // tenths of a point -> twips
    }
    if (!(ch.Flags & CF_NOSTYLESEL)) {
        out.dwMask |= CFM_BOLD | CFM_ITALIC;
        if (lf.lfWeight >= FW_SEMIBOLD) out.dwEffects |= CFE_BOLD;
        if (lf.lfItalic) out.dwEffects |= CFE_ITALIC;
    }
    out.dwMask |= CFM_UNDERLINE | CFM_STRIKEOUT;
    if (lf.lfUnderline) out.dwEffects |= CFE_UNDERLINE;
    if (lf.lfStrikeOut) out.dwEffects |= CFE_STRIKEOUT;
    if (!(wasAuto && ch.rgbColors == initial)) {
        out.dwMask |= CFM_COLOR;
        out.crTextColor = ch.rgbColors;
    }
    Editor_SetCharFormat(out);
}

UINT Dlg_FindMessageId()
{
    if (!g_findMsg) g_findMsg = RegisterWindowMessageW(FINDMSGSTRINGW);
    return g_findMsg;
}

void Dlg_CloseFind()
{
    if (g.hFindDlg) {
        SendMessageW(g.hFindDlg, WM_CLOSE, 0, 0);
        if (g.hFindDlg && IsWindow(g.hFindDlg)) DestroyWindow(g.hFindDlg);
        g.hFindDlg = nullptr;
    }
}

void Dlg_ShowFind(bool replace)
{
    Dlg_FindMessageId();
    if (g.hFindDlg) {
        if (g_isReplace == replace) { SetActiveWindow(g.hFindDlg); return; }
        Dlg_CloseFind();
    }
    // Seed the search box with a short single-line selection.
    std::wstring sel = Editor_GetText(true);
    if (!sel.empty() && sel.size() < ARRAYSIZE(g_findBuf) && sel.find_first_of(L"\r\n\x0B\xFFFC") == std::wstring::npos)
        lstrcpynW(g_findBuf, sel.c_str(), ARRAYSIZE(g_findBuf));
    else if (!g_lastFind.empty())
        lstrcpynW(g_findBuf, g_lastFind.c_str(), ARRAYSIZE(g_findBuf));

    ZeroMemory(&g_fr, sizeof(g_fr));
    g_fr.lStructSize = sizeof(g_fr);
    g_fr.hwndOwner = g.hMain;
    g_fr.lpstrFindWhat = g_findBuf;
    g_fr.wFindWhatLen = ARRAYSIZE(g_findBuf);
    g_fr.lpstrReplaceWith = g_replaceBuf;
    g_fr.wReplaceWithLen = ARRAYSIZE(g_replaceBuf);
    g_fr.Flags = (g_lastFlags & (FR_MATCHCASE | FR_WHOLEWORD)) | FR_DOWN;
    g_isReplace = replace;
    g.hFindDlg = replace ? ReplaceTextW(&g_fr) : FindTextW(&g_fr);
}

void Dlg_FindNext(bool reverse)
{
    if (g_lastFind.empty()) {
        std::wstring sel = Editor_GetText(true);
        if (!sel.empty() && sel.size() < 200 && sel.find_first_of(L"\r\n") == std::wstring::npos) g_lastFind = sel;
    }
    if (g_lastFind.empty()) { Dlg_ShowFind(false); return; }
    bool down = (g_lastFlags & FR_DOWN) != 0;
    if (reverse) down = !down;
    if (!Editor_Find(g_lastFind, (g_lastFlags & FR_MATCHCASE) != 0, (g_lastFlags & FR_WHOLEWORD) != 0, down, true))
        NotFound(g_lastFind);
}

bool Dlg_HandleFindMessage(UINT msg, LPARAM lParam)
{
    if (!g_findMsg || msg != g_findMsg) return false;
    FINDREPLACEW* fr = (FINDREPLACEW*)lParam;
    if (!fr) return true;
    if (fr->Flags & FR_DIALOGTERM) {
        g.hFindDlg = nullptr;
        return true;
    }
    std::wstring what = fr->lpstrFindWhat ? fr->lpstrFindWhat : L"";
    std::wstring with = fr->lpstrReplaceWith ? fr->lpstrReplaceWith : L"";
    bool matchCase = (fr->Flags & FR_MATCHCASE) != 0, wholeWord = (fr->Flags & FR_WHOLEWORD) != 0;
    bool down = g_isReplace ? true : (fr->Flags & FR_DOWN) != 0;
    g_lastFind = what;
    g_lastFlags = (fr->Flags & (FR_MATCHCASE | FR_WHOLEWORD)) | (down ? FR_DOWN : 0);

    if (fr->Flags & FR_FINDNEXT) {
        if (!Editor_Find(what, matchCase, wholeWord, down, true)) NotFound(what);
    } else if (fr->Flags & FR_REPLACE) {
        Editor_ReplaceCurrent(what, with, matchCase, wholeWord);
        if (!Editor_Find(what, matchCase, wholeWord, true, true)) NotFound(what);
    } else if (fr->Flags & FR_REPLACEALL) {
        int n = Editor_ReplaceAll(what, with, matchCase, wholeWord);
        if (n == 0) NotFound(what);
        else if (!g.selfTest)
            MessageBoxW(g.hFindDlg ? g.hFindDlg : g.hMain, Fmt(S(S_MSG_REPLACED), n).c_str(), APP_NAME, MB_OK | MB_ICONINFORMATION);
    }
    PostUpdateUI();
    return true;
}

void Dlg_SetLastFind(const std::wstring& what, bool matchCase, bool wholeWord)
{
    g_lastFind = what;
    g_lastFlags = FR_DOWN | (matchCase ? FR_MATCHCASE : 0) | (wholeWord ? FR_WHOLEWORD : 0);
}

int Dlg_SelfTest(void (*visit)(HWND dlg, const wchar_t* name))
{
    // Creates every dialog once (modeless) so that its initialisation code runs.
    struct Entry { int id; DLGPROC proc; const wchar_t* name; };
    const Entry all[] = {
        { IDD_PARAGRAPH, ParagraphProc, L"paragraph" }, { IDD_TABS, TabsProc, L"tabs" },
        { IDD_DATETIME, DateTimeProc, L"datetime" }, { IDD_ABOUT, AboutProc, L"about" },
        { IDD_TEXTVIEW, TextViewProc, L"shortcuts" }, { IDD_PICTURESIZE, PictureSizeProc, L"picturesize" },
    };
    int created = 0;
    for (const Entry& e : all) {
        HWND dlg = CreateDialogParamW(g.hInst, MAKEINTRESOURCEW(e.id), g.hMain, e.proc, 0);
        if (!dlg) continue;
        ShowWindow(dlg, SW_SHOWNOACTIVATE);
        UpdateWindow(dlg);
        if (visit) visit(dlg, e.name);
        DestroyWindow(dlg);
        ++created;
    }
    return created;
}
