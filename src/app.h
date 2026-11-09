// RichPad - shared declarations.
#pragma once

#include <windows.h>
#include <windowsx.h>
#include <ole2.h>          // before commdlg.h: PRINTDLGEX needs the COM macros
#include <commctrl.h>
#include <commdlg.h>
#include <cderr.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <richedit.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#include "resource.h"
#include "strings.h"

#define APP_NAME        L"RichPad"
#define APP_VERSION     L"1.0.0"
#define APP_REGKEY      L"Software\\RichPad"
#define APP_PROGID      L"RichPad.Document"
#define APP_MAINCLASS   L"RichPadMainWindow"

// ---- private window messages ---------------------------------------------
#define WM_APP_UPDATEUI     (WM_APP + 1)   // refresh command states (posted, coalesced)
#define WM_APP_OPENFILE     (WM_APP + 2)   // lParam: wchar_t* allocated with _wcsdup
#define WM_APP_ZOOMCHANGED  (WM_APP + 3)   // zoom changed inside the editor (Ctrl+wheel)
#define WM_APP_FONTAPPLY    (WM_APP + 4)   // wParam: 0 = face name, 1 = size (from command bar)
#define WM_APP_RULERCHANGED (WM_APP + 5)   // ruler modified paragraph formatting
#define WM_APP_SETZOOM      (WM_APP + 6)   // wParam: percent (from status bar)
#define WM_APP_OPENLINK     (WM_APP + 7)   // open the link that was Ctrl+clicked (posted)

enum WrapMode  { WRAP_NONE = 0, WRAP_WINDOW = 1, WRAP_RULER = 2 };
enum UnitMode  { UNIT_INCH = 0, UNIT_CM = 1, UNIT_PT = 2, UNIT_PICA = 3 };
enum ThemeMode { THEME_SYSTEM = 0, THEME_LIGHT = 1, THEME_DARK = 2 };
enum LangMode  { LANG_AUTO = 0, LANG_KO = 1, LANG_EN = 2 };

enum DocFormat {
    FMT_RTF = 0,
    FMT_DOCX,
    FMT_ODT,
    FMT_TEXT_UTF8,
    FMT_TEXT_UTF16,
    FMT_TEXT_ANSI,
};

inline bool IsTextFormat(DocFormat f) { return f == FMT_TEXT_UTF8 || f == FMT_TEXT_UTF16 || f == FMT_TEXT_ANSI; }

struct Settings {
    int  wrapMode   = WRAP_WINDOW;
    int  units      = UNIT_CM;
    bool showRuler  = true;
    bool showStatus = true;
    int  theme      = THEME_SYSTEM;
    int  lang       = LANG_AUTO;
    // Page setup, in twips (1/1440 inch).
    SIZE paper      = { 11906, 16838 };            // A4 portrait
    RECT margins    = { 1800, 1440, 1800, 1440 };  // 1.25" left/right, 1" top/bottom
    bool haveWindowPos = false;
    RECT windowRect = { 0, 0, 0, 0 };
    bool maximized  = false;
    std::vector<std::wstring> mru;
    COLORREF customColors[16] = {};
    COLORREF lastTextColor  = RGB(255, 0, 0);
    COLORREF lastHighlight  = RGB(255, 255, 0);
};

struct Document {
    std::wstring path;          // empty when untitled
    DocFormat    format = FMT_RTF;
    bool         utf8Bom = false;   // text files: keep the byte order mark when saving
    bool         readOnly = false;
    bool         imported = false;      // opened through the DOCX / ODT converter and not saved since
    bool         objectsWarned = false; // the user accepted that objects are saved as pictures
};

struct App {
    HINSTANCE hInst = nullptr;
    HWND hMain = nullptr;
    HWND hCmdBar = nullptr;
    HWND hRuler = nullptr;
    HWND hEdit = nullptr;
    HWND hStatus = nullptr;
    HWND hFindDlg = nullptr;     // modeless find/replace dialog
    HWND hPreview = nullptr;     // print preview window
    HACCEL hAccel = nullptr;
    HMENU hMenus[6] = {};        // File, Edit, View, Insert, Format, Help
    Settings cfg;
    Document doc;
    HGLOBAL hDevMode = nullptr;  // printer settings shared by Page Setup and Print
    HGLOBAL hDevNames = nullptr;
    bool selfTest = false;
    bool uiUpdatePending = false;
    int  textOriginDelta = 0;    // px between formatting rect left and the first character
};

extern App g;

// ---- helpers ---------------------------------------------------------------
inline int Scale(int v, int dpi) { return MulDiv(v, dpi, 96); }
int  DpiOf(HWND hwnd);
std::wstring Fmt(const wchar_t* fmt, ...);
std::wstring FormatNumber(long long n);            // with locale digit grouping
std::wstring FileTitle(const std::wstring& path);  // file name without directory
std::wstring DocDisplayName();
int  MsgBox(HWND owner, const wchar_t* text, UINT flags);
void PostUpdateUI();

// ---- main.cpp --------------------------------------------------------------
void App_UpdateTitle();
void App_UpdateUI();
void App_Layout();
void App_RebuildMenus();
void App_ApplyTheme();
void App_ApplyLanguage();
bool App_OpenFile(const std::wstring& path);
bool App_QuerySave();
bool App_Save(bool saveAs);
void App_NewDocument();
void App_OnCommand(int id);
void App_ShowContextMenu(POINT ptScreen);
void App_SetLinkHint(const std::wstring& url);   // address of the link under the mouse pointer ("" = none)
void App_ForgetContextLink();                    // the next context menu does not belong to a link
// Self test: observe what the edit control reports about links instead of acting on it.
typedef void (*LinkTestHook)(UINT msg, const CHARRANGE& range, const std::wstring& url);
void App_SetLinkTestHook(LinkTestHook hook);

// ---- settings.cpp ----------------------------------------------------------
void Settings_Load();
void Settings_Save();
void Mru_Add(const std::wstring& path);
void Mru_Remove(const std::wstring& path);
bool Assoc_IsRegistered();
bool Assoc_Register();
bool Assoc_Unregister();
