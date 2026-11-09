// RichPad - a rich text editor for Windows 11.
// Main window, command dispatch and application entry point.
#include "app.h"
#include "theme.h"
#include "commandbar.h"
#include "ruler.h"
#include "statusbar.h"
#include "editor.h"
#include "fileio.h"
#include "dialogs.h"
#include "print.h"
#include "popups.h"
#include "selftest.h"
#include <shobjidl.h>
#include <oledlg.h>
#include <cstdarg>

App g;

namespace {

const UINT_PTR kTimerCount = 1;

// Own copies of the shell GUIDs so the build does not depend on what a toolchain's uuid library exports.
const CLSID kCLSID_FileOpenDialog = { 0xDC1C5A9C, 0xE88A, 0x4DDE, { 0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7 } };
const CLSID kCLSID_FileSaveDialog = { 0xC0B4E2F3, 0xBA21, 0x4773, { 0x8D, 0xBA, 0x33, 0x5E, 0xC9, 0x46, 0xEB, 0x8B } };
const IID   kIID_IFileDialog      = { 0x42F85136, 0xDB7E, 0x439C, { 0x85, 0xF1, 0xE4, 0x07, 0x5D, 0x13, 0x5F, 0xC8 } };

std::wstring g_shownTitle;
long long g_charCount = 0, g_wordCount = -1;
bool g_altAlone = false;

struct UiState {
    bool canUndo = false, canRedo = false, hasSel = false, canPaste = false, picture = false;
    bool bold = false, italic = false, underline = false, strike = false, sub = false, super = false;
    WORD align = 0;
    WORD numbering = 0;
    int spacing = 0;
    bool spaceAfter = false;
    std::wstring face;
    int size = 0;
};

UiState GetUiState()
{
    UiState u;
    u.canUndo = SendMessageW(g.hEdit, EM_CANUNDO, 0, 0) != 0;
    u.canRedo = SendMessageW(g.hEdit, EM_CANREDO, 0, 0) != 0;
    u.hasSel = Editor_HasSelection();
    u.canPaste = SendMessageW(g.hEdit, EM_CANPASTE, 0, 0) != 0 || IsClipboardFormatAvailable(CF_BITMAP) ||
                 IsClipboardFormatAvailable(CF_DIB);
    u.picture = Editor_IsPictureSelected();

    CHARFORMAT2W cf;
    Editor_GetCharFormat(cf);
    u.bold = (cf.dwMask & CFM_BOLD) && (cf.dwEffects & CFE_BOLD);
    u.italic = (cf.dwMask & CFM_ITALIC) && (cf.dwEffects & CFE_ITALIC);
    u.underline = (cf.dwMask & CFM_UNDERLINE) && (cf.dwEffects & CFE_UNDERLINE);
    u.strike = (cf.dwMask & CFM_STRIKEOUT) && (cf.dwEffects & CFE_STRIKEOUT);
    u.sub = (cf.dwMask & CFM_SUBSCRIPT) && (cf.dwEffects & CFE_SUBSCRIPT);
    u.super = (cf.dwMask & CFM_SUPERSCRIPT) && (cf.dwEffects & CFE_SUPERSCRIPT);
    if (cf.dwMask & CFM_FACE) u.face = cf.szFaceName;
    if (cf.dwMask & CFM_SIZE) u.size = cf.yHeight;

    PARAFORMAT2 pf;
    Editor_GetParaFormat(pf);
    if (pf.dwMask & PFM_ALIGNMENT) u.align = pf.wAlignment;
    if (pf.dwMask & PFM_NUMBERING) u.numbering = pf.wNumbering;
    u.spacing = Editor_GetLineSpacingPercent(pf);
    u.spaceAfter = (pf.dwMask & PFM_SPACEAFTER) && pf.dySpaceAfter > 0;
    return u;
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------
void AddItem(HMENU menu, UINT id, int str, const wchar_t* accel = nullptr)
{
    std::wstring text = S(str);
    if (accel) { text += L'\t'; text += accel; }
    AppendMenuW(menu, MF_STRING, id, text.c_str());
}

void AddSep(HMENU menu) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); }

HMENU AddSub(HMENU menu, int str)
{
    HMENU sub = CreatePopupMenu();
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, S(str));
    return sub;
}

void AddListItems(HMENU menu)
{
    AddItem(menu, ID_FORMAT_LIST_NONE, S_FORMAT_LIST_NONE);
    AddItem(menu, ID_FORMAT_LIST_BULLET, S_FORMAT_LIST_BULLET);
    AddItem(menu, ID_FORMAT_LIST_NUMBER, S_FORMAT_LIST_NUMBER);
    AddItem(menu, ID_FORMAT_LIST_LALPHA, S_FORMAT_LIST_LALPHA);
    AddItem(menu, ID_FORMAT_LIST_UALPHA, S_FORMAT_LIST_UALPHA);
    AddItem(menu, ID_FORMAT_LIST_LROMAN, S_FORMAT_LIST_LROMAN);
    AddItem(menu, ID_FORMAT_LIST_UROMAN, S_FORMAT_LIST_UROMAN);
}

void AddSpacingItems(HMENU menu)
{
    AppendMenuW(menu, MF_STRING, ID_FORMAT_SPACING_100, L"1.0\tCtrl+1");
    AppendMenuW(menu, MF_STRING, ID_FORMAT_SPACING_115, L"1.15");
    AppendMenuW(menu, MF_STRING, ID_FORMAT_SPACING_150, L"1.5\tCtrl+5");
    AppendMenuW(menu, MF_STRING, ID_FORMAT_SPACING_200, L"2.0\tCtrl+2");
    AddSep(menu);
    AddItem(menu, ID_FORMAT_SPACE_AFTER, S_FORMAT_SPACE_AFTER);
}

std::wstring MruLabel(int index, const std::wstring& path)
{
    wchar_t compact[80];
    if (!PathCompactPathExW(compact, path.c_str(), 64, 0)) lstrcpynW(compact, path.c_str(), 80);
    std::wstring label = (index < 9) ? Fmt(L"&%d  ", index + 1) : L"1&0  ";
    for (const wchar_t* p = compact; *p; ++p) {
        if (*p == L'&') label += L'&';
        label += *p;
    }
    return label;
}

void UpdateMenuStates(HMENU menu)
{
    UiState u = GetUiState();
    auto enable = [&](UINT id, bool on) { EnableMenuItem(menu, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    auto check = [&](UINT id, bool on) { CheckMenuItem(menu, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    auto radio = [&](UINT first, UINT last, UINT sel) {
        for (UINT id = first; id <= last; ++id) {
            MENUITEMINFOW mii = { sizeof(mii) };
            mii.fMask = MIIM_STATE | MIIM_FTYPE;
            if (!GetMenuItemInfoW(menu, id, FALSE, &mii)) continue;
            mii.fType |= MFT_RADIOCHECK;
            mii.fState = (mii.fState & ~MFS_CHECKED) | (id == sel ? MFS_CHECKED : 0);
            SetMenuItemInfoW(menu, id, FALSE, &mii);
        }
    };
    enable(ID_EDIT_UNDO, u.canUndo);
    enable(ID_EDIT_REDO, u.canRedo);
    enable(ID_EDIT_CUT, u.hasSel);
    enable(ID_EDIT_COPY, u.hasSel);
    enable(ID_EDIT_DELETE, u.hasSel);
    enable(ID_EDIT_PASTE, u.canPaste);
    enable(ID_EDIT_PASTESPECIAL, u.canPaste);
    enable(ID_EDIT_PASTETEXT, IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT));
    enable(ID_EDIT_PICTURESIZE, u.picture);

    check(ID_VIEW_RULER, g.cfg.showRuler);
    check(ID_VIEW_STATUSBAR, g.cfg.showStatus);
    radio(ID_VIEW_WRAP_NONE, ID_VIEW_WRAP_RULER, ID_VIEW_WRAP_NONE + g.cfg.wrapMode);
    radio(ID_VIEW_UNIT_INCH, ID_VIEW_UNIT_PICA, ID_VIEW_UNIT_INCH + g.cfg.units);
    radio(ID_VIEW_THEME_SYSTEM, ID_VIEW_THEME_DARK, ID_VIEW_THEME_SYSTEM + g.cfg.theme);
    radio(ID_VIEW_LANG_AUTO, ID_VIEW_LANG_EN, ID_VIEW_LANG_AUTO + g.cfg.lang);
    int zoom = Editor_GetZoom();
    enable(ID_VIEW_ZOOMIN, zoom < 500);
    enable(ID_VIEW_ZOOMOUT, zoom > 10);

    check(ID_FORMAT_BOLD, u.bold);
    check(ID_FORMAT_ITALIC, u.italic);
    check(ID_FORMAT_UNDERLINE, u.underline);
    check(ID_FORMAT_STRIKE, u.strike);
    check(ID_FORMAT_SUBSCRIPT, u.sub);
    check(ID_FORMAT_SUPERSCRIPT, u.super);
    check(ID_FORMAT_BULLETS, u.numbering != 0);
    UINT alignId = u.align == PFA_LEFT ? ID_FORMAT_ALIGN_LEFT : u.align == PFA_CENTER ? ID_FORMAT_ALIGN_CENTER :
                   u.align == PFA_RIGHT ? ID_FORMAT_ALIGN_RIGHT : u.align == PFA_JUSTIFY ? ID_FORMAT_ALIGN_JUSTIFY : 0;
    radio(ID_FORMAT_ALIGN_LEFT, ID_FORMAT_ALIGN_JUSTIFY, alignId);
    UINT listId = ID_FORMAT_LIST_NONE + std::min<int>(u.numbering, 6);
    radio(ID_FORMAT_LIST_NONE, ID_FORMAT_LIST_UROMAN, listId);
    UINT spacingId = u.spacing == 100 ? ID_FORMAT_SPACING_100 : u.spacing == 115 ? ID_FORMAT_SPACING_115 :
                     u.spacing == 150 ? ID_FORMAT_SPACING_150 : u.spacing == 200 ? ID_FORMAT_SPACING_200 : 0;
    radio(ID_FORMAT_SPACING_100, ID_FORMAT_SPACING_200, spacingId);
    check(ID_FORMAT_SPACE_AFTER, u.spaceAfter);
}

// ---------------------------------------------------------------------------
// File dialogs
// ---------------------------------------------------------------------------
struct FileType { int name; const wchar_t* pattern; };

bool ShowFileDialog(HWND owner, bool save, const FileType* types, int typeCount, int* typeIndex,
                    const wchar_t* defExt, const wchar_t* title, std::wstring& path)
{
    std::vector<COMDLG_FILTERSPEC> specs;
    for (int i = 0; i < typeCount; ++i) specs.push_back({ S(types[i].name), types[i].pattern });

    IFileDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(save ? kCLSID_FileSaveDialog : kCLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  kIID_IFileDialog, (void**)&dlg);
    if (SUCCEEDED(hr) && dlg) {
        bool ok = false;
        dlg->SetFileTypes((UINT)specs.size(), specs.data());
        dlg->SetFileTypeIndex((UINT)*typeIndex);
        if (defExt) dlg->SetDefaultExtension(defExt);
        if (title) dlg->SetTitle(title);
        if (save && !path.empty()) dlg->SetFileName(path.c_str());
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        opts |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
        opts |= save ? (FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST) : FOS_FILEMUSTEXIST;
        dlg->SetOptions(opts);
        if (dlg->Show(owner) == S_OK) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    path = p;
                    CoTaskMemFree(p);
                    ok = true;
                }
                item->Release();
            }
            UINT idx = 1;
            if (SUCCEEDED(dlg->GetFileTypeIndex(&idx))) *typeIndex = (int)idx;
        }
        dlg->Release();
        return ok;
    }

    // Classic dialog as a fallback.
    std::wstring filter;
    for (int i = 0; i < typeCount; ++i) {
        filter += S(types[i].name); filter += L'\0';
        filter += types[i].pattern; filter += L'\0';
    }
    filter += L'\0';
    wchar_t file[1024] = L"";
    if (save) lstrcpynW(file, path.c_str(), ARRAYSIZE(file));
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.nFilterIndex = (DWORD)*typeIndex;
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAYSIZE(file);
    ofn.lpstrDefExt = defExt;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return false;
    path = file;
    *typeIndex = (int)ofn.nFilterIndex;
    return true;
}

const FileType kOpenTypes[] = {
    { S_FILTER_ALLDOCS, L"*.rtf;*.docx;*.odt;*.txt" }, { S_FILTER_RTF, L"*.rtf" }, { S_FILTER_DOCX, L"*.docx" },
    { S_FILTER_ODT, L"*.odt" }, { S_FILTER_TXT, L"*.txt" }, { S_FILTER_ALL, L"*.*" } };

const FileType kSaveTypes[] = {
    { S_FILTER_RTF, L"*.rtf" }, { S_FILTER_DOCX, L"*.docx" }, { S_FILTER_ODT, L"*.odt" },
    { S_FILTER_TXT, L"*.txt" }, { S_FILTER_TXT_UTF16, L"*.txt" }, { S_FILTER_TXT_ANSI, L"*.txt" } };
const DocFormat kSaveFormats[] = { FMT_RTF, FMT_DOCX, FMT_ODT, FMT_TEXT_UTF8, FMT_TEXT_UTF16, FMT_TEXT_ANSI };
const wchar_t* const kSaveExt[] = { L"rtf", L"docx", L"odt", L"txt", L"txt", L"txt" };

const FileType kPictureTypes[] = {
    { S_FILTER_IMAGES, L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff;*.ico" }, { S_FILTER_ALL, L"*.*" } };

// ---------------------------------------------------------------------------
// Status bar text
// ---------------------------------------------------------------------------
void Recount()
{
    // Characters without paragraph marks, and white-space separated words.
    std::wstring text = Editor_GetText(false);
    long long chars = 0, words = 0;
    bool inWord = false;
    for (wchar_t c : text) {
        bool space = (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x0B || c == 0x0C || c == 0xA0 || c == 0x3000);
        if (c != L'\r' && c != L'\n') ++chars;
        if (space) inWord = false;
        else if (!inWord) { inWord = true; ++words; }
    }
    g_charCount = chars;
    g_wordCount = words;
}

void UpdateStatus()
{
    if (!g.hStatus) return;
    CHARRANGE cr;
    Editor_GetSel(cr);
    LONG line = (LONG)SendMessageW(g.hEdit, EM_EXLINEFROMCHAR, 0, cr.cpMin);
    LONG lineStart = (LONG)SendMessageW(g.hEdit, EM_LINEINDEX, (WPARAM)line, 0);
    std::vector<std::wstring> parts;
    parts.push_back(Fmt(S(S_STATUS_LINECOL), line + 1, std::max<LONG>(0, cr.cpMin - lineStart) + 1));
    if (g_wordCount >= 0) parts.push_back(Fmt(S(S_STATUS_WORDS), FormatNumber(g_wordCount).c_str()));
    parts.push_back(Fmt(S(S_STATUS_CHARS), FormatNumber(g_charCount).c_str()));
    if (cr.cpMax > cr.cpMin) parts.push_back(Fmt(S(S_STATUS_SELECTED), FormatNumber(cr.cpMax - cr.cpMin).c_str()));
    StatusBar_SetParts(parts);
    StatusBar_SetZoom(Editor_GetZoom());
}

void ScheduleRecount()
{
    SetTimer(g.hMain, kTimerCount, Editor_GetTextLength() > 500000 ? 1500 : 300, nullptr);
}

// ---------------------------------------------------------------------------
// Individual commands
// ---------------------------------------------------------------------------
void ShowLoadError(const std::wstring& path, LoadResult result, DWORD err)
{
    int str = result == LOAD_UNSUPPORTED ? S_MSG_UNSUPPORTED : result == LOAD_OLDDOC ? S_MSG_OLDDOC : S_MSG_OPENFAILED;
    std::wstring detail = path;
    if (result == LOAD_FAILED && err) detail += L"\n\n" + File_ErrorText(err);
    MsgBox(g.hMain, Fmt(S(str), detail.c_str()).c_str(), MB_OK | MB_ICONWARNING);
}

void DoPaste()
{
    HWND focus = GetFocus();
    if (focus && focus != g.hEdit && CmdBar_IsChildFocused()) { SendMessageW(focus, WM_PASTE, 0, 0); return; }
    if (!Editor_PasteClipboardImage()) SendMessageW(g.hEdit, WM_PASTE, 0, 0);
}

void DoPasteSpecial()
{
    static const UINT cfRtf = RegisterClipboardFormatW(L"Rich Text Format");
    IDataObject* data = nullptr;
    if (FAILED(OleGetClipboard(&data)) || !data) return;

    struct Entry { CLIPFORMAT cf; DWORD tymed; const wchar_t* ko; const wchar_t* en; const wchar_t* resultKo; const wchar_t* resultEn; };
    const Entry all[] = {
        { (CLIPFORMAT)cfRtf, TYMED_HGLOBAL, L"서식 있는 텍스트(RTF)", L"Formatted Text (RTF)", L"글꼴과 서식이 있는 텍스트", L"text with font and paragraph formatting" },
        { CF_UNICODETEXT, TYMED_HGLOBAL, L"서식 없는 텍스트", L"Unformatted Text", L"서식이 없는 텍스트", L"text without any formatting" },
        { CF_METAFILEPICT, TYMED_MFPICT, L"그림(메타파일)", L"Picture (Metafile)", L"그림", L"a picture" },
        { CF_DIB, TYMED_HGLOBAL, L"장치 독립 비트맵", L"Device Independent Bitmap", L"비트맵 그림", L"a bitmap picture" },
    };
    OLEUIPASTEENTRYW entries[ARRAYSIZE(all)] = {};
    bool korean = Strings_IsKorean();
    for (size_t i = 0; i < ARRAYSIZE(all); ++i) {
        entries[i].fmtetc.cfFormat = all[i].cf;
        entries[i].fmtetc.dwAspect = DVASPECT_CONTENT;
        entries[i].fmtetc.lindex = -1;
        entries[i].fmtetc.tymed = all[i].tymed;
        entries[i].lpstrFormatName = korean ? all[i].ko : all[i].en;
        entries[i].lpstrResultText = korean ? all[i].resultKo : all[i].resultEn;
        entries[i].dwFlags = OLEUIPASTE_PASTEONLY;
    }
    OLEUIPASTESPECIALW ps = {};
    ps.cbStruct = sizeof(ps);
    ps.dwFlags = PSF_SELECTPASTE | PSF_DISABLEDISPLAYASICON;
    ps.hWndOwner = g.hMain;
    ps.lpSrcDataObj = data;
    ps.arrPasteEntries = entries;
    ps.cPasteEntries = (int)ARRAYSIZE(entries);
    UINT rc = OleUIPasteSpecialW(&ps);
    if (rc == OLEUI_OK && ps.nSelectedIndex >= 0 && ps.nSelectedIndex < (int)ARRAYSIZE(entries))
        SendMessageW(g.hEdit, EM_PASTESPECIAL, entries[ps.nSelectedIndex].fmtetc.cfFormat, 0);
    if (ps.lpSrcDataObj) ps.lpSrcDataObj->Release();
}

void DoInsertPicture()
{
    std::wstring path;
    int type = 1;
    if (!ShowFileDialog(g.hMain, false, kPictureTypes, (int)ARRAYSIZE(kPictureTypes), &type, nullptr, S(S_TITLE_INSERTPICTURE), path))
        return;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = Editor_InsertPictureFile(path);
    SetCursor(old);
    if (!ok) MsgBox(g.hMain, Fmt(S(S_MSG_PICTUREFAILED), path.c_str()).c_str(), MB_OK | MB_ICONWARNING);
}

void DoColorPick(bool highlight)
{
    RECT anchor;
    int itemId = highlight ? ID_FORMAT_HIGHLIGHT : ID_FORMAT_COLOR;
    if (!CmdBar_GetItemScreenRect(itemId, &anchor)) {
        POINT pt;
        GetCursorPos(&pt);
        anchor = { pt.x, pt.y, pt.x, pt.y };
    }
    COLORREF color = highlight ? g.cfg.lastHighlight : g.cfg.lastTextColor;
    ColorPopupResult r = ColorPopup_Show(g.hMain, anchor, highlight, &color);
    if (r == COLORPOPUP_COLOR) {
        if (highlight) { g.cfg.lastHighlight = color; Editor_SetHighlight(color, false); }
        else { g.cfg.lastTextColor = color; Editor_SetTextColor(color, false); }
        CmdBar_SetSwatch(itemId, color);
    } else if (r == COLORPOPUP_DEFAULT) {
        if (highlight) Editor_SetHighlight(0, true);
        else Editor_SetTextColor(0, true);
    }
}

void DoAssociate()
{
    bool registered = Assoc_IsRegistered();
    TASKDIALOGCONFIG cfg = {};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = g.hMain;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.pszWindowTitle = APP_NAME;
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszContent = S(S_MSG_ASSOC_ASK);
    TASKDIALOG_BUTTON buttons[3];
    int count = 0;
    buttons[count++] = { IDYES, S(S_MSG_ASSOC_REGISTER) };
    if (registered) buttons[count++] = { IDNO, S(S_MSG_ASSOC_REMOVE) };
    buttons[count++] = { IDCANCEL, S(S_MSG_CANCEL) };
    cfg.pButtons = buttons;
    cfg.cButtons = (UINT)count;
    cfg.nDefaultButton = IDYES;
    int choice = 0;
    if (FAILED(TaskDialogIndirect(&cfg, &choice, nullptr, nullptr)))
        choice = MessageBoxW(g.hMain, S(S_MSG_ASSOC_ASK), APP_NAME, MB_YESNO | MB_ICONQUESTION);
    if (choice == IDYES) {
        Assoc_Register();
        MsgBox(g.hMain, S(S_MSG_ASSOC_DONE), MB_OK | MB_ICONINFORMATION);
    } else if (choice == IDNO && registered) {
        Assoc_Unregister();
        MsgBox(g.hMain, S(S_MSG_ASSOC_REMOVED), MB_OK | MB_ICONINFORMATION);
    }
}

void OpenLink(const std::wstring& url)
{
    // Only well-known, non-executable schemes are handed to the shell.
    static const wchar_t* const kSchemes[] = { L"http://", L"https://", L"mailto:", L"ftp://" };
    for (const wchar_t* s : kSchemes) {
        if (_wcsnicmp(url.c_str(), s, wcslen(s)) == 0) {
            ShellExecuteW(g.hMain, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
    }
    if (_wcsnicmp(url.c_str(), L"www.", 4) == 0)
        ShellExecuteW(g.hMain, L"open", (L"https://" + url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void SetZoomFromUi(int percent)
{
    Editor_SetZoom(percent);
    StatusBar_SetZoom(Editor_GetZoom());
    Ruler_Update();
}

void NewWindow()
{
    wchar_t exe[MAX_PATH * 2];
    if (!GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe))) return;
    ShellExecuteW(nullptr, L"open", exe, nullptr, nullptr, SW_SHOWNORMAL);
}

void SaveWindowPlacement()
{
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (!GetWindowPlacement(g.hMain, &wp)) return;
    g.cfg.haveWindowPos = true;
    g.cfg.windowRect = wp.rcNormalPosition;
    g.cfg.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (Dlg_HandleFindMessage(msg, lParam)) return 0;

    switch (msg) {
    case WM_CREATE: {
        g.hMain = hwnd;
        Theme_ApplyTitleBar(hwnd);
        g.hCmdBar = CmdBar_Create(hwnd);
        g.hRuler = Ruler_Create(hwnd);
        g.hStatus = StatusBar_Create(hwnd);
        if (!Editor_Create(hwnd)) return -1;
        App_RebuildMenus();
        DragAcceptFiles(hwnd, TRUE);
        AddClipboardFormatListener(hwnd);
        CmdBar_SetSwatch(ID_FORMAT_COLOR, g.cfg.lastTextColor);
        CmdBar_SetSwatch(ID_FORMAT_HIGHLIGHT, g.cfg.lastHighlight);
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) App_Layout();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SETFOCUS:
        if (g.hEdit) SetFocus(g.hEdit);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        int dpi = DpiOf(hwnd);
        mmi->ptMinTrackSize.x = Scale(520, dpi);
        mmi->ptMinTrackSize.y = Scale(340, dpi);
        return 0;
    }

    case WM_COMMAND:
        if ((HWND)lParam == g.hEdit && g.hEdit) {
            switch (HIWORD(wParam)) {
            case EN_CHANGE:
                ScheduleRecount();
                PostUpdateUI();
                break;
            case EN_HSCROLL:
                if (g.hRuler) InvalidateRect(g.hRuler, nullptr, FALSE);
                break;
            }
            return 0;
        }
        if (lParam == 0 || HIWORD(wParam) <= 1) App_OnCommand(LOWORD(wParam));
        return 0;

    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->hwndFrom == g.hEdit) {
            if (hdr->code == EN_SELCHANGE) {
                PostUpdateUI();
            } else if (hdr->code == EN_LINK) {
                ENLINK* link = (ENLINK*)lParam;
                if (link->msg == WM_LBUTTONUP && GetKeyState(VK_CONTROL) < 0) {
                    LONG len = link->chrg.cpMax - link->chrg.cpMin;
                    if (len > 0 && len < 2048) {
                        std::wstring url((size_t)len + 1, L'\0');
                        TEXTRANGEW tr = { link->chrg, &url[0] };
                        LRESULT n = SendMessageW(g.hEdit, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
                        url.resize((size_t)std::max<LRESULT>(0, n));
                        OpenLink(url);
                        return 1;
                    }
                }
            }
        }
        return 0;
    }

    case WM_INITMENUPOPUP:
        if (!HIWORD(lParam)) UpdateMenuStates((HMENU)wParam);
        CmdBar_OnInitMenuPopup((HMENU)wParam);
        return 0;

    case WM_UNINITMENUPOPUP:
        CmdBar_OnUninitMenuPopup((HMENU)wParam);
        return 0;

    case WM_MENUSELECT:
        CmdBar_OnMenuSelect(wParam, lParam);
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) {
            if (lParam == 0) { CmdBar_EnterMenuMode(); return 0; }
            if (lParam != VK_SPACE && lParam != L'-' && CmdBar_OpenMenuByMnemonic((wchar_t)lParam)) return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == kTimerCount) {
            KillTimer(hwnd, kTimerCount);
            Recount();
            UpdateStatus();
        }
        return 0;

    case WM_APP_UPDATEUI:
        App_UpdateUI();
        return 0;

    case WM_APP_ZOOMCHANGED:
        StatusBar_SetZoom(Editor_GetZoom());
        Ruler_Update();
        return 0;

    case WM_APP_SETZOOM:
        SetZoomFromUi((int)wParam);
        return 0;

    case WM_APP_RULERCHANGED:
        App_UpdateUI();
        return 0;

    case WM_APP_FONTAPPLY:
        if (wParam == 0) {
            std::wstring face = CmdBar_GetFontName();
            if (!face.empty()) Editor_SetFontFace(face.c_str());
        } else {
            int twips = CmdBar_GetFontSizeTwips();
            if (twips > 0) Editor_SetFontSize(twips);
            else MsgBox(hwnd, S(S_MSG_INVALIDSIZE), MB_OK | MB_ICONWARNING);
        }
        SetFocus(g.hEdit);
        CmdBar_RevertFontBoxes();
        App_UpdateUI();
        return 0;

    case WM_APP_OPENFILE: {
        wchar_t* p = (wchar_t*)lParam;
        if (p) {
            std::wstring path = p;
            free(p);
            if (wParam == 1 && File_IsImagePath(path)) {
                if (!Editor_InsertPictureFile(path))
                    MsgBox(hwnd, Fmt(S(S_MSG_PICTUREFAILED), path.c_str()).c_str(), MB_OK | MB_ICONWARNING);
            } else if (App_QuerySave()) {
                App_OpenFile(path);
            }
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wParam;
        wchar_t path[1024];
        if (DragQueryFileW(drop, 0, path, ARRAYSIZE(path)))
            PostMessageW(hwnd, WM_APP_OPENFILE, 0, (LPARAM)_wcsdup(path));
        DragFinish(drop);
        return 0;
    }

    case WM_CLIPBOARDUPDATE:
        PostUpdateUI();
        return 0;

    case WM_DPICHANGED: {
        RECT* r = (RECT*)lParam;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Editor_OnDpiChanged();
        CmdBar_Rebuild();
        StatusBar_Rebuild();
        App_Layout();
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (lParam && lstrcmpiW((const wchar_t*)lParam, L"ImmersiveColorSet") == 0) App_ApplyTheme();
        else if (wParam == SPI_SETNONCLIENTMETRICS || wParam == SPI_SETHIGHCONTRAST) {
            Theme_FlushFonts();
            App_ApplyTheme();
            App_Layout();
        }
        return 0;

    case WM_SYSCOLORCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        App_ApplyTheme();
        break;

    case WM_FONTCHANGE:
        CmdBar_RefreshFonts();
        return 0;

    case WM_QUERYENDSESSION:
        return App_QuerySave() ? TRUE : FALSE;

    case WM_CLOSE:
        if (g.hPreview) return 0;
        if (!App_QuerySave()) return 0;
        SaveWindowPlacement();
        Settings_Save();
        Dlg_CloseFind();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        RemoveClipboardFormatListener(hwnd);
        KillTimer(hwnd, kTimerCount);
        Editor_Destroy();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HACCEL CreateAccelerators()
{
    const BYTE C = FCONTROL | FVIRTKEY, CS = FCONTROL | FSHIFT | FVIRTKEY, V = FVIRTKEY, SV = FSHIFT | FVIRTKEY,
               CA = FCONTROL | FALT | FVIRTKEY;
    ACCEL table[] = {
        { C, 'N', ID_FILE_NEW }, { CS, 'N', ID_FILE_NEWWINDOW }, { C, 'O', ID_FILE_OPEN }, { C, 'S', ID_FILE_SAVE },
        { CS, 'S', ID_FILE_SAVEAS }, { V, VK_F12, ID_FILE_SAVEAS }, { C, 'P', ID_FILE_PRINT }, { C, VK_F2, ID_FILE_PRINTPREVIEW },
        { C, 'Z', ID_EDIT_UNDO }, { C, 'Y', ID_EDIT_REDO }, { C, 'X', ID_EDIT_CUT }, { C, 'C', ID_EDIT_COPY },
        { C, 'V', ID_EDIT_PASTE }, { CA, 'V', ID_EDIT_PASTESPECIAL }, { CS, 'V', ID_EDIT_PASTETEXT },
        { C, 'A', ID_EDIT_SELECTALL }, { C, 'F', ID_EDIT_FIND }, { V, VK_F3, ID_EDIT_FINDNEXT },
        { SV, VK_F3, ID_EDIT_FINDPREV }, { C, 'H', ID_EDIT_REPLACE },
        { C, 'B', ID_FORMAT_BOLD }, { C, 'I', ID_FORMAT_ITALIC }, { C, 'U', ID_FORMAT_UNDERLINE },
        { C, 'L', ID_FORMAT_ALIGN_LEFT }, { C, 'E', ID_FORMAT_ALIGN_CENTER }, { C, 'R', ID_FORMAT_ALIGN_RIGHT },
        { C, 'J', ID_FORMAT_ALIGN_JUSTIFY }, { CS, 'L', ID_FORMAT_BULLETS }, { C, 'D', ID_FORMAT_FONT },
        { C, '1', ID_FORMAT_SPACING_100 }, { C, '2', ID_FORMAT_SPACING_200 }, { C, '5', ID_FORMAT_SPACING_150 },
        { CS, VK_OEM_PERIOD, ID_FORMAT_GROW }, { CS, VK_OEM_COMMA, ID_FORMAT_SHRINK },
        { C, VK_OEM_PLUS, ID_FORMAT_SUBSCRIPT }, { CS, VK_OEM_PLUS, ID_FORMAT_SUPERSCRIPT },
        { C, VK_SPACE, ID_FORMAT_CLEAR },
        { C, VK_ADD, ID_VIEW_ZOOMIN }, { C, VK_SUBTRACT, ID_VIEW_ZOOMOUT }, { C, '0', ID_VIEW_ZOOM100 },
        { C, VK_NUMPAD0, ID_VIEW_ZOOM100 },
    };
    return CreateAcceleratorTableW(table, (int)ARRAYSIZE(table));
}

// Keyboard access to the custom menu row: Alt, F10 and Alt+letter.
bool HandleMenuKeys(const MSG& msg)
{
    if (GetActiveWindow() != g.hMain) { g_altAlone = false; return false; }
    switch (msg.message) {
    case WM_SYSKEYDOWN:
        if (msg.wParam == VK_MENU) {
            // First press (not auto-repeat). AltGr arrives as Ctrl+Alt and must not open the menus.
            if (!(msg.lParam & (1 << 30))) g_altAlone = GetKeyState(VK_CONTROL) >= 0;
            return false;
        }
        g_altAlone = false;
        if (msg.wParam == VK_F10 && GetKeyState(VK_SHIFT) >= 0) { CmdBar_EnterMenuMode(); return true; }
        if ((msg.lParam & (1 << 29)) && GetKeyState(VK_CONTROL) >= 0 &&
            ((msg.wParam >= 'A' && msg.wParam <= 'Z') || (msg.wParam >= '0' && msg.wParam <= '9'))) {
            if (CmdBar_OpenMenuByMnemonic((wchar_t)msg.wParam)) return true;
        }
        return false;
    case WM_SYSKEYUP:
    case WM_KEYUP:
        if (msg.wParam == VK_MENU && g_altAlone) {
            g_altAlone = false;
            CmdBar_EnterMenuMode();
            return true;
        }
        return false;
    case WM_KEYDOWN: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_MOUSEWHEEL:
        g_altAlone = false;
        return false;
    }
    return false;
}

struct Options {
    std::wstring file;
    std::wstring selfTestLog;
    std::wstring printer;
    bool print = false;
    int theme = -1, lang = -1;
};

Options ParseCommandLine()
{
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return o;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.size() > 1 && (a[0] == L'/' || a[0] == L'-')) {
            std::wstring opt = a.substr(a[1] == L'-' ? 2 : 1);
            CharLowerBuffW(&opt[0], (DWORD)opt.size());
            if (opt == L"selftest") { g.selfTest = true; if (i + 1 < argc) o.selfTestLog = argv[++i]; continue; }
            if (opt == L"p") { o.print = true; continue; }
            if (opt == L"pt") { o.print = true; if (i + 2 < argc) { o.file = argv[i + 1]; o.printer = argv[i + 2]; i += 2; } continue; }
            if (opt == L"theme:dark") { o.theme = THEME_DARK; continue; }
            if (opt == L"theme:light") { o.theme = THEME_LIGHT; continue; }
            if (opt == L"lang:ko") { o.lang = LANG_KO; continue; }
            if (opt == L"lang:en") { o.lang = LANG_EN; continue; }
            // Anything else that starts with '/' may still be a path (e.g. a UNC-style argument).
            if (a[0] == L'-') continue;
        }
        if (o.file.empty()) o.file = a;
    }
    LocalFree(argv);
    return o;
}

} // namespace

// ===========================================================================
// Shared helpers
// ===========================================================================
int DpiOf(HWND hwnd)
{
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    return dpi ? (int)dpi : 96;
}

std::wstring Fmt(const wchar_t* fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, ARRAYSIZE(buf) - 1, fmt, ap);
    va_end(ap);
    buf[ARRAYSIZE(buf) - 1] = 0;
    return buf;
}

std::wstring FormatNumber(long long n)
{
    wchar_t sep[8] = L",";
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, sep, ARRAYSIZE(sep));
    std::wstring digits = std::to_wstring(n < 0 ? -n : n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += sep;
        out += digits[i];
    }
    return n < 0 ? L"-" + out : out;
}

std::wstring FileTitle(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring DocDisplayName()
{
    return g.doc.path.empty() ? std::wstring(S(S_UNTITLED)) : FileTitle(g.doc.path);
}

int MsgBox(HWND owner, const wchar_t* text, UINT flags)
{
    if (g.selfTest) {
        SelfTest_Note(text);
        UINT type = flags & MB_TYPEMASK;
        return (type == MB_YESNO || type == MB_YESNOCANCEL) ? IDYES : IDOK;
    }
    return MessageBoxW(owner ? owner : g.hMain, text, APP_NAME, flags);
}

void PostUpdateUI()
{
    if (g.uiUpdatePending || !g.hMain) return;
    g.uiUpdatePending = true;
    PostMessageW(g.hMain, WM_APP_UPDATEUI, 0, 0);
}

// ===========================================================================
// Application-level operations
// ===========================================================================
void App_UpdateTitle()
{
    std::wstring title = (Editor_IsModified() ? L"*" : L"") + DocDisplayName() + L" - " + APP_NAME;
    if (title != g_shownTitle) {
        g_shownTitle = title;
        SetWindowTextW(g.hMain, title.c_str());
    }
}

void App_UpdateUI()
{
    g.uiUpdatePending = false;
    if (!g.hEdit || !g.hCmdBar) return;
    UiState u = GetUiState();
    CmdBar_SetEnabled(ID_EDIT_UNDO, u.canUndo);
    CmdBar_SetEnabled(ID_EDIT_REDO, u.canRedo);
    CmdBar_SetEnabled(ID_EDIT_CUT, u.hasSel);
    CmdBar_SetEnabled(ID_EDIT_COPY, u.hasSel);
    CmdBar_SetEnabled(ID_EDIT_PASTE, u.canPaste);
    CmdBar_SetChecked(ID_FORMAT_BOLD, u.bold);
    CmdBar_SetChecked(ID_FORMAT_ITALIC, u.italic);
    CmdBar_SetChecked(ID_FORMAT_UNDERLINE, u.underline);
    CmdBar_SetChecked(ID_FORMAT_STRIKE, u.strike);
    CmdBar_SetChecked(ID_FORMAT_SUBSCRIPT, u.sub);
    CmdBar_SetChecked(ID_FORMAT_SUPERSCRIPT, u.super);
    CmdBar_SetChecked(ID_FORMAT_ALIGN_LEFT, u.align == PFA_LEFT);
    CmdBar_SetChecked(ID_FORMAT_ALIGN_CENTER, u.align == PFA_CENTER);
    CmdBar_SetChecked(ID_FORMAT_ALIGN_RIGHT, u.align == PFA_RIGHT);
    CmdBar_SetChecked(ID_FORMAT_ALIGN_JUSTIFY, u.align == PFA_JUSTIFY);
    CmdBar_SetChecked(ID_FORMAT_BULLETS, u.numbering != 0);
    CmdBar_SetFontInfo(u.face.empty() ? nullptr : u.face.c_str(), u.size);
    CmdBar_Commit();
    Ruler_Update();
    UpdateStatus();
    App_UpdateTitle();
}

void App_Layout()
{
    if (!g.hMain || !g.hEdit) return;
    RECT rc;
    GetClientRect(g.hMain, &rc);
    int y = 0;
    int barH = CmdBar_GetHeight();
    SetWindowPos(g.hCmdBar, nullptr, 0, 0, rc.right, barH, SWP_NOZORDER | SWP_NOACTIVATE);
    y += barH;
    if (g.cfg.showRuler) {
        int h = Ruler_GetHeight();
        SetWindowPos(g.hRuler, nullptr, 0, y, rc.right, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        y += h;
    } else {
        ShowWindow(g.hRuler, SW_HIDE);
    }
    int bottom = rc.bottom;
    if (g.cfg.showStatus) {
        int h = StatusBar_GetHeight();
        bottom -= h;
        SetWindowPos(g.hStatus, nullptr, 0, bottom, rc.right, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(g.hStatus, SW_HIDE);
    }
    SetWindowPos(g.hEdit, nullptr, 0, y, rc.right, std::max(0, bottom - y), SWP_NOZORDER | SWP_NOACTIVATE);
    Editor_UpdateInsets();
    Ruler_Update();
}

void App_RebuildMenus()
{
    for (HMENU& m : g.hMenus) { if (m) DestroyMenu(m); m = nullptr; }

    HMENU file = CreatePopupMenu();
    AddItem(file, ID_FILE_NEW, S_FILE_NEW, L"Ctrl+N");
    AddItem(file, ID_FILE_NEWWINDOW, S_FILE_NEWWINDOW, L"Ctrl+Shift+N");
    AddItem(file, ID_FILE_OPEN, S_FILE_OPEN, L"Ctrl+O");
    AddItem(file, ID_FILE_SAVE, S_FILE_SAVE, L"Ctrl+S");
    AddItem(file, ID_FILE_SAVEAS, S_FILE_SAVEAS, L"F12");
    AddSep(file);
    AddItem(file, ID_FILE_PRINT, S_FILE_PRINT, L"Ctrl+P");
    AddItem(file, ID_FILE_QUICKPRINT, S_FILE_QUICKPRINT);
    AddItem(file, ID_FILE_PRINTPREVIEW, S_FILE_PRINTPREVIEW, L"Ctrl+F2");
    AddItem(file, ID_FILE_PAGESETUP, S_FILE_PAGESETUP);
    AddSep(file);
    HMENU recent = AddSub(file, S_FILE_RECENT);
    if (g.cfg.mru.empty()) {
        AppendMenuW(recent, MF_STRING | MF_GRAYED, 0, S(S_FILE_RECENT_NONE));
    } else {
        for (size_t i = 0; i < g.cfg.mru.size() && i < 10; ++i)
            AppendMenuW(recent, MF_STRING, ID_FILE_MRU_FIRST + (UINT)i, MruLabel((int)i, g.cfg.mru[i]).c_str());
    }
    AddSep(file);
    AddItem(file, ID_FILE_EXIT, S_FILE_EXIT, L"Alt+F4");
    g.hMenus[0] = file;

    HMENU edit = CreatePopupMenu();
    AddItem(edit, ID_EDIT_UNDO, S_EDIT_UNDO, L"Ctrl+Z");
    AddItem(edit, ID_EDIT_REDO, S_EDIT_REDO, L"Ctrl+Y");
    AddSep(edit);
    AddItem(edit, ID_EDIT_CUT, S_EDIT_CUT, L"Ctrl+X");
    AddItem(edit, ID_EDIT_COPY, S_EDIT_COPY, L"Ctrl+C");
    AddItem(edit, ID_EDIT_PASTE, S_EDIT_PASTE, L"Ctrl+V");
    AddItem(edit, ID_EDIT_PASTESPECIAL, S_EDIT_PASTESPECIAL, L"Ctrl+Alt+V");
    AddItem(edit, ID_EDIT_PASTETEXT, S_EDIT_PASTETEXT, L"Ctrl+Shift+V");
    AddItem(edit, ID_EDIT_DELETE, S_EDIT_DELETE, L"Del");
    AddSep(edit);
    AddItem(edit, ID_EDIT_FIND, S_EDIT_FIND, L"Ctrl+F");
    AddItem(edit, ID_EDIT_FINDNEXT, S_EDIT_FINDNEXT, L"F3");
    AddItem(edit, ID_EDIT_FINDPREV, S_EDIT_FINDPREV, L"Shift+F3");
    AddItem(edit, ID_EDIT_REPLACE, S_EDIT_REPLACE, L"Ctrl+H");
    AddSep(edit);
    AddItem(edit, ID_EDIT_SELECTALL, S_EDIT_SELECTALL, L"Ctrl+A");
    AddSep(edit);
    AddItem(edit, ID_EDIT_PICTURESIZE, S_EDIT_PICTURESIZE);
    g.hMenus[1] = edit;

    HMENU view = CreatePopupMenu();
    AddItem(view, ID_VIEW_ZOOMIN, S_VIEW_ZOOMIN, L"Ctrl++");
    AddItem(view, ID_VIEW_ZOOMOUT, S_VIEW_ZOOMOUT, L"Ctrl+-");
    AddItem(view, ID_VIEW_ZOOM100, S_VIEW_ZOOM100, L"Ctrl+0");
    AddSep(view);
    AddItem(view, ID_VIEW_RULER, S_VIEW_RULER);
    AddItem(view, ID_VIEW_STATUSBAR, S_VIEW_STATUSBAR);
    AddSep(view);
    HMENU wrap = AddSub(view, S_VIEW_WRAP);
    AddItem(wrap, ID_VIEW_WRAP_NONE, S_VIEW_WRAP_NONE);
    AddItem(wrap, ID_VIEW_WRAP_WINDOW, S_VIEW_WRAP_WINDOW);
    AddItem(wrap, ID_VIEW_WRAP_RULER, S_VIEW_WRAP_RULER);
    HMENU units = AddSub(view, S_VIEW_UNITS);
    AddItem(units, ID_VIEW_UNIT_INCH, S_VIEW_UNIT_INCH);
    AddItem(units, ID_VIEW_UNIT_CM, S_VIEW_UNIT_CM);
    AddItem(units, ID_VIEW_UNIT_PT, S_VIEW_UNIT_PT);
    AddItem(units, ID_VIEW_UNIT_PICA, S_VIEW_UNIT_PICA);
    AddSep(view);
    HMENU theme = AddSub(view, S_VIEW_THEME);
    AddItem(theme, ID_VIEW_THEME_SYSTEM, S_VIEW_THEME_SYSTEM);
    AddItem(theme, ID_VIEW_THEME_LIGHT, S_VIEW_THEME_LIGHT);
    AddItem(theme, ID_VIEW_THEME_DARK, S_VIEW_THEME_DARK);
    HMENU lang = AddSub(view, S_VIEW_LANG);
    AddItem(lang, ID_VIEW_LANG_AUTO, S_VIEW_LANG_AUTO);
    AddItem(lang, ID_VIEW_LANG_KO, S_VIEW_LANG_KO);
    AddItem(lang, ID_VIEW_LANG_EN, S_VIEW_LANG_EN);
    g.hMenus[2] = view;

    HMENU insert = CreatePopupMenu();
    AddItem(insert, ID_INSERT_PICTURE, S_INSERT_PICTURE);
    AddItem(insert, ID_INSERT_DATETIME, S_INSERT_DATETIME);
    AddItem(insert, ID_INSERT_OBJECT, S_INSERT_OBJECT);
    g.hMenus[3] = insert;

    HMENU format = CreatePopupMenu();
    AddItem(format, ID_FORMAT_FONT, S_FORMAT_FONT, L"Ctrl+D");
    AddSep(format);
    AddItem(format, ID_FORMAT_BOLD, S_FORMAT_BOLD, L"Ctrl+B");
    AddItem(format, ID_FORMAT_ITALIC, S_FORMAT_ITALIC, L"Ctrl+I");
    AddItem(format, ID_FORMAT_UNDERLINE, S_FORMAT_UNDERLINE, L"Ctrl+U");
    AddItem(format, ID_FORMAT_STRIKE, S_FORMAT_STRIKE);
    AddItem(format, ID_FORMAT_SUBSCRIPT, S_FORMAT_SUBSCRIPT, L"Ctrl+=");
    AddItem(format, ID_FORMAT_SUPERSCRIPT, S_FORMAT_SUPERSCRIPT, L"Ctrl+Shift+=");
    AddSep(format);
    HMENU align = AddSub(format, S_FORMAT_ALIGN);
    AddItem(align, ID_FORMAT_ALIGN_LEFT, S_FORMAT_ALIGN_LEFT, L"Ctrl+L");
    AddItem(align, ID_FORMAT_ALIGN_CENTER, S_FORMAT_ALIGN_CENTER, L"Ctrl+E");
    AddItem(align, ID_FORMAT_ALIGN_RIGHT, S_FORMAT_ALIGN_RIGHT, L"Ctrl+R");
    AddItem(align, ID_FORMAT_ALIGN_JUSTIFY, S_FORMAT_ALIGN_JUSTIFY, L"Ctrl+J");
    HMENU list = AddSub(format, S_FORMAT_LIST);
    AddListItems(list);
    HMENU spacing = AddSub(format, S_FORMAT_SPACING);
    AddSpacingItems(spacing);
    AddItem(format, ID_FORMAT_INDENT_INC, S_FORMAT_INDENT_INC);
    AddItem(format, ID_FORMAT_INDENT_DEC, S_FORMAT_INDENT_DEC);
    AddSep(format);
    AddItem(format, ID_FORMAT_PARAGRAPH, S_FORMAT_PARAGRAPH);
    AddItem(format, ID_FORMAT_TABS, S_FORMAT_TABS);
    AddSep(format);
    AddItem(format, ID_FORMAT_CLEAR, S_FORMAT_CLEAR, L"Ctrl+Space");
    g.hMenus[4] = format;

    HMENU help = CreatePopupMenu();
    AddItem(help, ID_HELP_SHORTCUTS, S_HELP_SHORTCUTS);
    AddItem(help, ID_HELP_ASSOCIATE, S_HELP_ASSOCIATE);
    AddSep(help);
    AddItem(help, ID_HELP_ABOUT, S_HELP_ABOUT);
    g.hMenus[5] = help;
}

void App_ApplyTheme()
{
    Theme_Refresh();
    Theme_ApplyTitleBar(g.hMain);
    CmdBar_Rebuild();
    StatusBar_Rebuild();
    Editor_ApplyTheme();
    RedrawWindow(g.hMain, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void App_ApplyLanguage()
{
    Strings_SetLanguage(g.cfg.lang);
    Dlg_CloseFind();
    App_RebuildMenus();
    CmdBar_Rebuild();
    StatusBar_Rebuild();
    g_shownTitle.clear();
    App_UpdateUI();
}

void App_NewDocument()
{
    Editor_Clear();
    Editor_SetDefaultFormat(false);
    Editor_SetModified(false);
    SendMessageW(g.hEdit, EM_EMPTYUNDOBUFFER, 0, 0);
    g.doc = Document();
    g_charCount = 0;
    g_wordCount = 0;
    App_UpdateUI();
}

bool App_OpenFile(const std::wstring& pathIn)
{
    wchar_t full[1024];
    std::wstring path = pathIn;
    if (GetFullPathNameW(pathIn.c_str(), ARRAYSIZE(full), full, nullptr)) path = full;

    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    DocFormat format = FMT_RTF;
    bool bom = false;
    DWORD err = 0;
    LoadResult result = File_Load(path, &format, &bom, &err);
    SetCursor(old);
    if (result != LOAD_OK) {
        if (result == LOAD_FAILED) {
            // A failed load may have cleared the control; start from a clean document.
            App_NewDocument();
            Mru_Remove(path);
            App_RebuildMenus();
        }
        ShowLoadError(path, result, err);
        return false;
    }
    g.doc.path = path;
    g.doc.format = format;
    g.doc.utf8Bom = bom;
    DWORD attrs = GetFileAttributesW(path.c_str());
    g.doc.readOnly = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY);
    Editor_SetSel(0, 0);
    SendMessageW(g.hEdit, EM_SCROLLCARET, 0, 0);
    SendMessageW(g.hEdit, EM_EMPTYUNDOBUFFER, 0, 0);
    Editor_SetModified(false);
    Mru_Add(path);
    App_RebuildMenus();
    Recount();
    App_UpdateUI();
    return true;
}

bool App_Save(bool saveAs)
{
    std::wstring path = g.doc.path;
    DocFormat format = g.doc.format;
    bool bom = g.doc.utf8Bom;

    if (saveAs || path.empty() || g.doc.readOnly) {
        int type = 1;
        for (int i = 0; i < (int)ARRAYSIZE(kSaveFormats); ++i) if (kSaveFormats[i] == format) type = i + 1;
        std::wstring name = path.empty() ? std::wstring(S(S_UNTITLED)) : path;
        if (!ShowFileDialog(g.hMain, true, kSaveTypes, (int)ARRAYSIZE(kSaveTypes), &type, kSaveExt[type - 1], nullptr, name))
            return false;
        path = name;
        DocFormat chosen = kSaveFormats[std::clamp(type, 1, (int)ARRAYSIZE(kSaveFormats)) - 1];
        // An explicit, known extension wins over the selected type (except for the text encodings).
        format = File_FormatFromPath(path, chosen);
        if (IsTextFormat(format) && IsTextFormat(chosen)) format = chosen;
        if (format != g.doc.format) bom = false;
    }

    if (IsTextFormat(format)) {
        bool wasRich = !IsTextFormat(g.doc.format) || Editor_GetObjectCount() > 0;
        if (wasRich && (saveAs || g.doc.path.empty() || !IsTextFormat(g.doc.format))) {
            if (MsgBox(g.hMain, S(S_MSG_TEXTONLY), MB_YESNO | MB_ICONWARNING) != IDYES) return false;
        }
        if (format == FMT_TEXT_ANSI && !File_CanSaveAsAnsi()) {
            if (MsgBox(g.hMain, S(S_MSG_ANSILOSS), MB_YESNO | MB_ICONWARNING) != IDYES) return false;
        }
    }

    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    DWORD err = 0;
    bool ok = File_Save(path, format, bom, &err);
    SetCursor(old);
    if (!ok) {
        std::wstring detail = path;
        if (err) detail += L"\n\n" + File_ErrorText(err);
        MsgBox(g.hMain, Fmt(S(S_MSG_SAVEFAILED), detail.c_str()).c_str(), MB_OK | MB_ICONERROR);
        return false;
    }
    g.doc.path = path;
    g.doc.format = format;
    g.doc.utf8Bom = bom;
    g.doc.readOnly = false;
    Editor_SetModified(false);
    Mru_Add(path);
    App_RebuildMenus();
    App_UpdateUI();
    return true;
}

bool App_QuerySave()
{
    if (!g.hEdit || !Editor_IsModified() || g.selfTest) return true;
    std::wstring text = Fmt(S(S_MSG_SAVECHANGES), DocDisplayName().c_str());
    TASKDIALOGCONFIG cfg = {};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = g.hMain;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.pszWindowTitle = APP_NAME;
    cfg.pszMainInstruction = text.c_str();
    TASKDIALOG_BUTTON buttons[] = { { IDYES, S(S_MSG_SAVE) }, { IDNO, S(S_MSG_DONTSAVE) }, { IDCANCEL, S(S_MSG_CANCEL) } };
    cfg.pButtons = buttons;
    cfg.cButtons = ARRAYSIZE(buttons);
    cfg.nDefaultButton = IDYES;
    int choice = 0;
    if (FAILED(TaskDialogIndirect(&cfg, &choice, nullptr, nullptr)))
        choice = MessageBoxW(g.hMain, text.c_str(), APP_NAME, MB_YESNOCANCEL | MB_ICONWARNING);
    if (choice == IDYES) return App_Save(false);
    return choice == IDNO;
}

void App_ShowContextMenu(POINT pt)
{
    HMENU menu = CreatePopupMenu();
    AddItem(menu, ID_EDIT_CUT, S_EDIT_CUT, L"Ctrl+X");
    AddItem(menu, ID_EDIT_COPY, S_EDIT_COPY, L"Ctrl+C");
    AddItem(menu, ID_EDIT_PASTE, S_EDIT_PASTE, L"Ctrl+V");
    AddItem(menu, ID_EDIT_PASTETEXT, S_EDIT_PASTETEXT, L"Ctrl+Shift+V");
    AddSep(menu);
    AddItem(menu, ID_EDIT_SELECTALL, S_EDIT_SELECTALL, L"Ctrl+A");
    AddSep(menu);
    AddItem(menu, ID_FORMAT_FONT, S_FORMAT_FONT);
    AddItem(menu, ID_FORMAT_PARAGRAPH, S_FORMAT_PARAGRAPH);
    AddItem(menu, ID_FORMAT_BULLETS, S_FORMAT_LIST_BULLET);
    if (Editor_IsPictureSelected()) {
        AddSep(menu);
        AddItem(menu, ID_EDIT_PICTURESIZE, S_EDIT_PICTURESIZE);
    }
    TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, g.hMain, nullptr);
    DestroyMenu(menu);
}

void App_OnCommand(int id)
{
    HWND edit = g.hEdit;
    bool refocus = true;

    if (id >= ID_FILE_MRU_FIRST && id <= ID_FILE_MRU_LAST) {
        size_t index = (size_t)(id - ID_FILE_MRU_FIRST);
        if (index < g.cfg.mru.size() && App_QuerySave()) {
            std::wstring path = g.cfg.mru[index];
            App_OpenFile(path);
        }
        return;
    }

    switch (id) {
    // ---- File ----
    case ID_FILE_NEW:
        if (App_QuerySave()) App_NewDocument();
        break;
    case ID_FILE_NEWWINDOW: NewWindow(); break;
    case ID_FILE_OPEN:
        if (App_QuerySave()) {
            std::wstring path;
            int type = 1;
            if (ShowFileDialog(g.hMain, false, kOpenTypes, (int)ARRAYSIZE(kOpenTypes), &type, nullptr, nullptr, path))
                App_OpenFile(path);
        }
        break;
    case ID_FILE_SAVE: App_Save(false); break;
    case ID_FILE_SAVEAS: App_Save(true); break;
    case ID_FILE_PRINT: Print_Print(g.hMain, true); break;
    case ID_FILE_QUICKPRINT: Print_Print(g.hMain, false); break;
    case ID_FILE_PRINTPREVIEW: Print_Preview(g.hMain); break;
    case ID_FILE_PAGESETUP: Print_PageSetup(g.hMain); break;
    case ID_FILE_EXIT: PostMessageW(g.hMain, WM_CLOSE, 0, 0); return;

    // ---- Edit ----
    case ID_EDIT_UNDO: SendMessageW(edit, EM_UNDO, 0, 0); break;
    case ID_EDIT_REDO: SendMessageW(edit, EM_REDO, 0, 0); break;
    case ID_EDIT_CUT: SendMessageW(edit, WM_CUT, 0, 0); break;
    case ID_EDIT_COPY: SendMessageW(edit, WM_COPY, 0, 0); break;
    case ID_EDIT_PASTE: DoPaste(); break;
    case ID_EDIT_PASTESPECIAL: DoPasteSpecial(); break;
    case ID_EDIT_PASTETEXT: SendMessageW(edit, EM_PASTESPECIAL, CF_UNICODETEXT, 0); break;
    case ID_EDIT_PASTE_MENU: {
        HMENU menu = CreatePopupMenu();
        AddItem(menu, ID_EDIT_PASTE, S_EDIT_PASTE, L"Ctrl+V");
        AddItem(menu, ID_EDIT_PASTESPECIAL, S_EDIT_PASTESPECIAL, L"Ctrl+Alt+V");
        AddItem(menu, ID_EDIT_PASTETEXT, S_EDIT_PASTETEXT, L"Ctrl+Shift+V");
        CmdBar_TrackItemMenu(ID_EDIT_PASTE, menu);
        DestroyMenu(menu);
        return;
    }
    case ID_EDIT_DELETE: SendMessageW(edit, WM_CLEAR, 0, 0); break;
    case ID_EDIT_SELECTALL: SendMessageW(edit, EM_SETSEL, 0, -1); break;
    case ID_EDIT_FIND: Dlg_ShowFind(false); refocus = false; break;
    case ID_EDIT_REPLACE: Dlg_ShowFind(true); refocus = false; break;
    case ID_EDIT_FINDNEXT: Dlg_FindNext(false); refocus = g.hFindDlg == nullptr; break;
    case ID_EDIT_FINDPREV: Dlg_FindNext(true); refocus = g.hFindDlg == nullptr; break;
    case ID_EDIT_PICTURESIZE: if (Editor_IsPictureSelected()) Dlg_PictureSize(g.hMain); break;

    // ---- View ----
    case ID_VIEW_ZOOMIN: SetZoomFromUi((Editor_GetZoom() / 10 + 1) * 10); break;
    case ID_VIEW_ZOOMOUT: SetZoomFromUi(((Editor_GetZoom() + 9) / 10 - 1) * 10); break;
    case ID_VIEW_ZOOM100: SetZoomFromUi(100); break;
    case ID_VIEW_RULER: g.cfg.showRuler = !g.cfg.showRuler; App_Layout(); break;
    case ID_VIEW_STATUSBAR: g.cfg.showStatus = !g.cfg.showStatus; App_Layout(); break;
    case ID_VIEW_WRAP_NONE: case ID_VIEW_WRAP_WINDOW: case ID_VIEW_WRAP_RULER:
        g.cfg.wrapMode = id - ID_VIEW_WRAP_NONE;
        Editor_ApplyWrap();
        break;
    case ID_VIEW_UNIT_INCH: case ID_VIEW_UNIT_CM: case ID_VIEW_UNIT_PT: case ID_VIEW_UNIT_PICA:
        g.cfg.units = id - ID_VIEW_UNIT_INCH;
        break;
    case ID_VIEW_THEME_SYSTEM: case ID_VIEW_THEME_LIGHT: case ID_VIEW_THEME_DARK:
        g.cfg.theme = id - ID_VIEW_THEME_SYSTEM;
        App_ApplyTheme();
        break;
    case ID_VIEW_LANG_AUTO: case ID_VIEW_LANG_KO: case ID_VIEW_LANG_EN:
        g.cfg.lang = id - ID_VIEW_LANG_AUTO;
        App_ApplyLanguage();
        break;

    // ---- Insert ----
    case ID_INSERT_PICTURE: DoInsertPicture(); break;
    case ID_INSERT_DATETIME: Dlg_DateTime(g.hMain); break;
    case ID_INSERT_OBJECT:
        if (!Editor_InsertObjectDialog(g.hMain)) MsgBox(g.hMain, S(S_MSG_OBJECTFAILED), MB_OK | MB_ICONWARNING);
        break;

    // ---- Format ----
    case ID_FORMAT_FONT: Dlg_Font(g.hMain); break;
    case ID_FORMAT_BOLD: Editor_ToggleEffect(CFM_BOLD, CFE_BOLD); break;
    case ID_FORMAT_ITALIC: Editor_ToggleEffect(CFM_ITALIC, CFE_ITALIC); break;
    case ID_FORMAT_UNDERLINE: Editor_ToggleEffect(CFM_UNDERLINE, CFE_UNDERLINE); break;
    case ID_FORMAT_STRIKE: Editor_ToggleEffect(CFM_STRIKEOUT, CFE_STRIKEOUT); break;
    case ID_FORMAT_SUBSCRIPT: Editor_ToggleScript(false); break;
    case ID_FORMAT_SUPERSCRIPT: Editor_ToggleScript(true); break;
    case ID_FORMAT_GROW: Editor_ChangeFontSize(1); break;
    case ID_FORMAT_SHRINK: Editor_ChangeFontSize(-1); break;
    case ID_FORMAT_COLOR: Editor_SetTextColor(g.cfg.lastTextColor, false); break;
    case ID_FORMAT_HIGHLIGHT: Editor_SetHighlight(g.cfg.lastHighlight, false); break;
    case ID_FORMAT_COLOR_PICK: DoColorPick(false); break;
    case ID_FORMAT_HIGHLIGHT_PICK: DoColorPick(true); break;
    case ID_FORMAT_ALIGN_LEFT: Editor_SetAlignment(PFA_LEFT); break;
    case ID_FORMAT_ALIGN_CENTER: Editor_SetAlignment(PFA_CENTER); break;
    case ID_FORMAT_ALIGN_RIGHT: Editor_SetAlignment(PFA_RIGHT); break;
    case ID_FORMAT_ALIGN_JUSTIFY: Editor_SetAlignment(PFA_JUSTIFY); break;
    case ID_FORMAT_INDENT_INC: Editor_ChangeIndent(1); break;
    case ID_FORMAT_INDENT_DEC: Editor_ChangeIndent(-1); break;
    case ID_FORMAT_BULLETS: {
        PARAFORMAT2 pf;
        Editor_GetParaFormat(pf);
        bool on = (pf.dwMask & PFM_NUMBERING) && pf.wNumbering != 0;
        Editor_SetNumbering(on ? 0 : PFN_BULLET);
        break;
    }
    case ID_FORMAT_LIST_NONE: case ID_FORMAT_LIST_BULLET: case ID_FORMAT_LIST_NUMBER: case ID_FORMAT_LIST_LALPHA:
    case ID_FORMAT_LIST_UALPHA: case ID_FORMAT_LIST_LROMAN: case ID_FORMAT_LIST_UROMAN:
        Editor_SetNumbering((WORD)(id - ID_FORMAT_LIST_NONE));   // 0 none, 1 bullet, 2 arabic, 3..6 letters / roman
        break;
    case ID_FORMAT_LIST_MENU: {
        HMENU menu = CreatePopupMenu();
        AddListItems(menu);
        CmdBar_TrackItemMenu(ID_FORMAT_BULLETS, menu);
        DestroyMenu(menu);
        return;
    }
    case ID_FORMAT_SPACING_MENU: {
        HMENU menu = CreatePopupMenu();
        AddSpacingItems(menu);
        CmdBar_TrackItemMenu(ID_FORMAT_SPACING_MENU, menu);
        DestroyMenu(menu);
        return;
    }
    case ID_FORMAT_SPACING_100: Editor_SetLineSpacing(100); break;
    case ID_FORMAT_SPACING_115: Editor_SetLineSpacing(115); break;
    case ID_FORMAT_SPACING_150: Editor_SetLineSpacing(150); break;
    case ID_FORMAT_SPACING_200: Editor_SetLineSpacing(200); break;
    case ID_FORMAT_SPACE_AFTER: Editor_ToggleSpaceAfter(); break;
    case ID_FORMAT_PARAGRAPH: Dlg_Paragraph(g.hMain); break;
    case ID_FORMAT_TABS: Dlg_Tabs(g.hMain); break;
    case ID_FORMAT_CLEAR: Editor_ClearFormatting(); break;

    // ---- Help ----
    case ID_HELP_ABOUT: Dlg_About(g.hMain); break;
    case ID_HELP_SHORTCUTS: Dlg_Shortcuts(g.hMain); break;
    case ID_HELP_ASSOCIATE: DoAssociate(); break;

    default:
        return;
    }

    if (refocus && g.hMain && IsWindow(g.hMain) && GetActiveWindow() == g.hMain && GetFocus() != g.hEdit)
        SetFocus(g.hEdit);
    App_UpdateUI();
}

// ===========================================================================
// Entry point
// ===========================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nShow)
{
    g.hInst = hInstance;
    Options opt = ParseCommandLine();

    if (FAILED(OleInitialize(nullptr))) return 1;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_USEREX_CLASSES };
    InitCommonControlsEx(&icc);

    Settings_Load();
    if (opt.theme >= 0) g.cfg.theme = opt.theme;
    if (opt.lang >= 0) g.cfg.lang = opt.lang;
    Strings_SetLanguage(g.cfg.lang);
    Theme_Init();
    File_Init();

    if (!LoadLibraryW(L"msftedit.dll")) {
        MessageBoxW(nullptr, L"msftedit.dll could not be loaded.", APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = APP_MAINCLASS;
    if (!RegisterClassExW(&wc) || !CmdBar_Register() || !Ruler_Register() || !StatusBar_Register() ||
        !ColorPopup_Register() || !Print_Register())
        return 1;

    // Initial placement: last session's rectangle if it is still on a monitor.
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1040, h = 760;
    if (g.cfg.haveWindowPos && MonitorFromRect(&g.cfg.windowRect, MONITOR_DEFAULTTONULL)) {
        x = g.cfg.windowRect.left;
        y = g.cfg.windowRect.top;
        w = g.cfg.windowRect.right - g.cfg.windowRect.left;
        h = g.cfg.windowRect.bottom - g.cfg.windowRect.top;
    } else {
        int dpi = (int)GetDpiForSystem();
        w = Scale(w, dpi);
        h = Scale(h, dpi);
    }
    HWND hwnd = CreateWindowExW(0, APP_MAINCLASS, APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, w, h, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;
    g.hAccel = CreateAccelerators();
    App_Layout();
    Editor_OnDpiChanged();
    App_NewDocument();

    int exitCode = 0;
    if (g.selfTest) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
        exitCode = SelfTest_Run(opt.selfTestLog.c_str());
        DestroyWindow(hwnd);
    } else if (opt.print && !opt.file.empty()) {
        // "/p file" and "/pt file printer": print without showing the window.
        if (App_OpenFile(opt.file)) {
            if (opt.printer.empty()) Print_Print(nullptr, false);
            else Print_ToPrinter(opt.printer.c_str(), nullptr, nullptr);
        }
        DestroyWindow(hwnd);
    } else {
        ShowWindow(hwnd, (g.cfg.maximized && nShow == SW_SHOWNORMAL) ? SW_SHOWMAXIMIZED : nShow);
        UpdateWindow(hwnd);
        if (!opt.file.empty()) App_OpenFile(opt.file);
        SetFocus(g.hEdit);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g.hFindDlg && IsDialogMessageW(g.hFindDlg, &msg)) continue;
        if (HandleMenuKeys(msg)) continue;
        if (g.hAccel && GetActiveWindow() == g.hMain && !CmdBar_IsChildFocused() &&
            TranslateAcceleratorW(g.hMain, g.hAccel, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!g.selfTest && msg.message == WM_QUIT) exitCode = (int)msg.wParam;

    if (g.hAccel) DestroyAcceleratorTable(g.hAccel);
    for (HMENU m : g.hMenus) if (m) DestroyMenu(m);
    Theme_Shutdown();
    OleUninitialize();
    return exitCode;
}
