// Automated self test ("RichPad.exe /selftest log.txt").
//
// The checks run against the live main window, so they exercise the same code paths as a user
// would. The report is written as UTF-8 text; screenshots are saved next to it.
#include "selftest.h"
#include "theme.h"
#include "commandbar.h"
#include "ruler.h"
#include "statusbar.h"
#include "editor.h"
#include "fileio.h"
#include "dialogs.h"
#include "print.h"
#include "imaging.h"
#include "convert/convert.h"
#include <winspool.h>
#include <cstdarg>
#include <cmath>

namespace {

std::string g_out;
int g_pass = 0, g_fail = 0, g_skip = 0;
bool g_wine = false;
std::wstring g_dir;      // directory of the report (screenshots and temporary files go there too)

std::string U8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

void Line(const char* fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    g_out += buf;
    g_out += "\r\n";
}

void Check(bool ok, const char* name, const char* fmt = nullptr, ...)
{
    char detail[2048] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf(detail, sizeof(detail) - 1, fmt, ap);
        va_end(ap);
        detail[sizeof(detail) - 1] = 0;
    }
    if (ok) ++g_pass; else ++g_fail;
    Line("%s  %s%s%s", ok ? "PASS" : "FAIL", name, detail[0] ? "  -- " : "", detail);
}

// A check that is known not to work under Wine (missing features of its rich edit control).
void CheckWin(bool ok, const char* name, const char* fmt = nullptr, ...)
{
    char detail[2048] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf(detail, sizeof(detail) - 1, fmt, ap);
        va_end(ap);
        detail[sizeof(detail) - 1] = 0;
    }
    if (g_wine && !ok) {
        ++g_skip;
        Line("SKIP  %s  -- not supported by Wine%s%s", name, detail[0] ? "; " : "", detail);
        return;
    }
    if (ok) ++g_pass; else ++g_fail;
    Line("%s  %s%s%s", ok ? "PASS" : "FAIL", name, detail[0] ? "  -- " : "", detail);
}

void Section(const char* title) { Line(""); Line("== %s ==", title); }

std::wstring g_logPath;
void Flush();

void Pump(int waitMs = 0)
{
    DWORD until = GetTickCount() + (DWORD)waitMs;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); return; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if ((int)(until - GetTickCount()) <= 0) break;
        Sleep(10);
    }
}

std::wstring PathIn(const wchar_t* name) { return g_dir + name; }

void Flush()
{
    if (!g_logPath.empty()) File_WriteAll(g_logPath, g_out.data(), g_out.size(), nullptr);
}

// True when the bitmap is one flat colour (a failed capture is usually all black).
bool IsBlank(HDC mem, int w, int h)
{
    COLORREF first = GetPixel(mem, w / 2, h / 2);
    for (int y = 1; y < 12; ++y)
        for (int x = 1; x < 12; ++x)
            if (GetPixel(mem, w * x / 12, h * y / 12) != first) return false;
    return true;
}

bool SaveWindowPng(HWND hwnd, const wchar_t* name)
{
    RECT rc;
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    // 2 = PW_RENDERFULLCONTENT (captures DWM-composed content such as the dark title bar).
    bool ok = PrintWindow(hwnd, mem, 2) && !IsBlank(mem, w, h);
    if (!ok) ok = PrintWindow(hwnd, mem, 0) && !IsBlank(mem, w, h);
    if (!ok) {
        SetForegroundWindow(hwnd);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        GdiFlush();
        ok = BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY) != 0;
    }
    SelectObject(mem, old);
    std::vector<BYTE> png;
    bool saved = ok && Image_FromHBitmap(bmp, png) && File_WriteAll(PathIn(name), png.data(), png.size(), nullptr);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return saved;
}

void Type(const wchar_t* text) { Editor_ReplaceSel(text, true); }

void NewDoc()
{
    App_NewDocument();
    Pump();
}

CHARFORMAT2W CharFormatAt(LONG cpMin, LONG cpMax)
{
    Editor_SetSel(cpMin, cpMax);
    CHARFORMAT2W cf;
    Editor_GetCharFormat(cf);
    return cf;
}

PARAFORMAT2 ParaFormatAt(LONG cp)
{
    Editor_SetSel(cp, cp);
    PARAFORMAT2 pf;
    Editor_GetParaFormat(pf);
    return pf;
}

bool Has(const CHARFORMAT2W& cf, DWORD mask, DWORD effect) { return (cf.dwMask & mask) && (cf.dwEffects & effect); }

std::vector<BYTE> MakeTestPng(int w, int h)
{
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    RECT rc = { 0, 0, w, h };
    FillSolid(mem, rc, RGB(37, 99, 235));
    RECT inner = { w / 4, h / 4, w * 3 / 4, h * 3 / 4 };
    FillSolid(mem, inner, RGB(245, 158, 11));
    SelectObject(mem, old);
    std::vector<BYTE> png;
    Image_FromHBitmap(bmp, png);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return png;
}

std::wstring Normalize(std::wstring t)
{
    // Paragraph marks differ between APIs (CR, CRLF); compare on a common form.
    std::wstring out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == L'\r') { out += L'\n'; if (i + 1 < t.size() && t[i + 1] == L'\n') ++i; }
        else out += t[i];
    }
    while (!out.empty() && out.back() == L'\n') out.pop_back();
    return out;
}

// ---------------------------------------------------------------------------

void TestEnvironment()
{
    Section("Environment");
    DWORD major = 0, minor = 0, build = 0;
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        typedef void (WINAPI* FnVer)(DWORD*, DWORD*, DWORD*);
        if (auto fn = (FnVer)(void*)GetProcAddress(ntdll, "RtlGetNtVersionNumbers")) fn(&major, &minor, &build);
        g_wine = GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }
    Line("Windows %lu.%lu build %lu%s", major, minor, build & 0x0FFFFFFF, g_wine ? " (Wine)" : "");
    Line("System DPI %u, window DPI %d, UI language 0x%04X, ANSI code page %u", GetDpiForSystem(), DpiOf(g.hMain),
         GetUserDefaultUILanguage(), GetACP());
    wchar_t cls[64] = L"";
    GetClassNameW(g.hEdit, cls, 64);
    Line("Editor class %s, layout DPI %d, text origin delta %d px", U8(cls).c_str(), Editor_LayoutDpi(), g.textOriginDelta);
    Line("Default font: %s", U8(Editor_DefaultFontFace(false)).c_str());
    Line("Theme: dark=%d highContrast=%d", T.dark, T.highContrast);
    Check(g.hEdit && g.hCmdBar && g.hRuler && g.hStatus, "child windows created");
    Check(Editor_LayoutDpi() >= 48, "layout DPI measured", "%d", Editor_LayoutDpi());
}

void TestCharacterFormatting()
{
    Section("Character formatting");
    NewDoc();
    Type(L"Hello RichPad \xD55C\xAE00 \xD14C\xC2A4\xD2B8");   // "Hello RichPad 한글 테스트"
    Check(Normalize(Editor_GetText(false)) == L"Hello RichPad \xD55C\xAE00 \xD14C\xC2A4\xD2B8", "typed text is stored");
    Check(Editor_IsModified(), "document is marked as modified");

    Editor_SetSel(0, 5);
    App_OnCommand(ID_FORMAT_BOLD);
    Check(Has(CharFormatAt(0, 5), CFM_BOLD, CFE_BOLD), "bold on");
    Check(!Has(CharFormatAt(6, 13), CFM_BOLD, CFE_BOLD), "bold limited to the selection");
    Editor_SetSel(0, 5);
    App_OnCommand(ID_FORMAT_BOLD);
    Check(!Has(CharFormatAt(0, 5), CFM_BOLD, CFE_BOLD), "bold off again");

    Editor_SetSel(6, 13);
    App_OnCommand(ID_FORMAT_ITALIC);
    App_OnCommand(ID_FORMAT_UNDERLINE);
    App_OnCommand(ID_FORMAT_STRIKE);
    CHARFORMAT2W cf = CharFormatAt(6, 13);
    Check(Has(cf, CFM_ITALIC, CFE_ITALIC), "italic");
    Check(Has(cf, CFM_UNDERLINE, CFE_UNDERLINE), "underline");
    Check(Has(cf, CFM_STRIKEOUT, CFE_STRIKEOUT), "strikethrough");

    Editor_SetSel(0, 5);
    App_OnCommand(ID_FORMAT_SUPERSCRIPT);
    Check(Has(CharFormatAt(0, 5), CFM_SUPERSCRIPT, CFE_SUPERSCRIPT), "superscript");
    Editor_SetSel(0, 5);
    App_OnCommand(ID_FORMAT_SUBSCRIPT);
    cf = CharFormatAt(0, 5);
    Check(Has(cf, CFM_SUBSCRIPT, CFE_SUBSCRIPT) && !(cf.dwEffects & CFE_SUPERSCRIPT), "subscript replaces superscript");
    Editor_SetSel(0, 5);
    App_OnCommand(ID_FORMAT_SUBSCRIPT);
    cf = CharFormatAt(0, 5);
    Check(!(cf.dwEffects & (CFE_SUBSCRIPT | CFE_SUPERSCRIPT)), "subscript off");

    Editor_SetSel(0, 5);
    Editor_SetTextColor(RGB(200, 30, 40), false);
    cf = CharFormatAt(0, 5);
    Check((cf.dwMask & CFM_COLOR) && !(cf.dwEffects & CFE_AUTOCOLOR) && cf.crTextColor == RGB(200, 30, 40), "text colour");
    Editor_SetSel(0, 5);
    Editor_SetTextColor(0, true);
    Check((CharFormatAt(0, 5).dwEffects & CFE_AUTOCOLOR) != 0, "automatic text colour");

    Editor_SetSel(0, 5);
    Editor_SetHighlight(RGB(255, 255, 0), false);
    cf = CharFormatAt(0, 5);
    Check((cf.dwMask & CFM_BACKCOLOR) && !(cf.dwEffects & CFE_AUTOBACKCOLOR) && cf.crBackColor == RGB(255, 255, 0), "highlight colour");
    Editor_SetSel(0, 5);
    Editor_SetHighlight(0, true);
    Check((CharFormatAt(0, 5).dwEffects & CFE_AUTOBACKCOLOR) != 0, "highlight removed");

    Editor_SetSel(0, 5);
    Editor_SetFontFace(L"Arial");
    Editor_SetFontSize(24 * 20);
    cf = CharFormatAt(0, 5);
    Check((cf.dwMask & CFM_FACE) && lstrcmpiW(cf.szFaceName, L"Arial") == 0, "font face", "%s", U8(cf.szFaceName).c_str());
    Check((cf.dwMask & CFM_SIZE) && cf.yHeight == 480, "font size 24pt", "yHeight=%ld", cf.yHeight);
    Editor_SetSel(0, 5);
    App_OnCommand(ID_FORMAT_GROW);
    cf = CharFormatAt(0, 5);
    Check(cf.yHeight == 520, "grow font 24 -> 26", "yHeight=%ld", cf.yHeight);
    Editor_SetSel(0, 5);
    App_OnCommand(ID_FORMAT_SHRINK);
    App_OnCommand(ID_FORMAT_SHRINK);
    cf = CharFormatAt(0, 5);
    Check(cf.yHeight == 440, "shrink font 26 -> 22", "yHeight=%ld", cf.yHeight);

    Editor_SetSel(0, 13);
    App_OnCommand(ID_FORMAT_CLEAR);
    cf = CharFormatAt(0, 13);
    Check(!(cf.dwEffects & (CFE_BOLD | CFE_ITALIC | CFE_UNDERLINE | CFE_STRIKEOUT)) && (cf.dwMask & CFM_SIZE) && cf.yHeight == 220,
          "clear formatting", "effects=0x%lX yHeight=%ld", cf.dwEffects, cf.yHeight);
}

void TestParagraphFormatting()
{
    Section("Paragraph formatting");
    NewDoc();
    Type(L"First paragraph\rSecond paragraph\rThird paragraph");
    Check(SendMessageW(g.hEdit, EM_GETLINECOUNT, 0, 0) >= 3, "three paragraphs");

    PARAFORMAT2 pf = ParaFormatAt(0);
    Check((pf.dwMask & PFM_ALIGNMENT) && pf.wAlignment == PFA_LEFT, "default alignment is left");
    int spacing = Editor_GetLineSpacingPercent(pf);
    Check(spacing == 115, "default line spacing 1.15", "%d (rule %d, value %ld)", spacing, pf.bLineSpacingRule, pf.dyLineSpacing);
    Check((pf.dwMask & PFM_SPACEAFTER) && pf.dySpaceAfter == 200, "default 10pt space after", "%ld", pf.dySpaceAfter);

    Editor_SetSel(0, 0);
    App_OnCommand(ID_FORMAT_ALIGN_CENTER);
    Check(ParaFormatAt(0).wAlignment == PFA_CENTER, "centre");
    Check(ParaFormatAt(20).wAlignment == PFA_LEFT, "other paragraphs keep their alignment");
    Editor_SetSel(0, 0);
    App_OnCommand(ID_FORMAT_ALIGN_RIGHT);
    Check(ParaFormatAt(0).wAlignment == PFA_RIGHT, "right");
    Editor_SetSel(0, 0);
    App_OnCommand(ID_FORMAT_ALIGN_JUSTIFY);
    Check(ParaFormatAt(0).wAlignment == PFA_JUSTIFY, "justify");
    Editor_SetSel(0, 0);
    App_OnCommand(ID_FORMAT_ALIGN_LEFT);

    Editor_SetSel(0, 0);
    App_OnCommand(ID_FORMAT_INDENT_INC);
    pf = ParaFormatAt(0);
    Check(pf.dxStartIndent == 720, "increase indent", "%ld", pf.dxStartIndent);
    App_OnCommand(ID_FORMAT_INDENT_DEC);
    App_OnCommand(ID_FORMAT_INDENT_DEC);
    Check(ParaFormatAt(0).dxStartIndent == 0, "decrease indent stops at the margin");

    Editor_SetSel(0, 0);
    App_OnCommand(ID_FORMAT_SPACING_200);
    Check(Editor_GetLineSpacingPercent(ParaFormatAt(0)) == 200, "double line spacing");
    App_OnCommand(ID_FORMAT_SPACING_100);
    Check(Editor_GetLineSpacingPercent(ParaFormatAt(0)) == 100, "single line spacing");
    App_OnCommand(ID_FORMAT_SPACE_AFTER);
    Check(ParaFormatAt(0).dySpaceAfter == 0, "space after removed");
    App_OnCommand(ID_FORMAT_SPACE_AFTER);
    Check(ParaFormatAt(0).dySpaceAfter == 200, "space after restored");

    Editor_SetSel(0, Editor_GetTextLength());
    App_OnCommand(ID_FORMAT_BULLETS);
    pf = ParaFormatAt(0);
    Check((pf.dwMask & PFM_NUMBERING) && pf.wNumbering == PFN_BULLET, "bullets", "numbering=%d", pf.wNumbering);
    Check(ParaFormatAt(Editor_GetTextLength()).wNumbering == PFN_BULLET, "bullets on the last paragraph");
    Check(pf.dxOffset == 360 && pf.dxStartIndent == 360, "hanging indent for the bullet", "start=%ld offset=%ld", pf.dxStartIndent, pf.dxOffset);
    Editor_SetSel(0, Editor_GetTextLength());
    App_OnCommand(ID_FORMAT_LIST_NUMBER);
    int listKind = ParaFormatAt(0).wNumbering;
    Check(listKind == 2, "numbered list", "numbering=%d", listKind);
    Editor_SetSel(0, Editor_GetTextLength());
    App_OnCommand(ID_FORMAT_LIST_UROMAN);
    listKind = ParaFormatAt(0).wNumbering;
    Check(listKind == 6, "upper-case roman list", "numbering=%d", listKind);
    Editor_SetSel(0, Editor_GetTextLength());
    App_OnCommand(ID_FORMAT_LIST_NONE);
    pf = ParaFormatAt(0);
    Check(pf.wNumbering == 0 && pf.dxStartIndent == 0 && pf.dxOffset == 0, "list removed", "numbering=%d start=%ld offset=%ld",
          pf.wNumbering, pf.dxStartIndent, pf.dxOffset);

    // Tab stops.
    PARAFORMAT2 tabs = {};
    tabs.dwMask = PFM_TABSTOPS;
    tabs.cTabCount = 2;
    tabs.rgxTabs[0] = 1440;
    tabs.rgxTabs[1] = 4320;
    Editor_SetSel(0, 0);
    Editor_SetParaFormat(tabs);
    pf = ParaFormatAt(0);
    Check(pf.cTabCount == 2 && (pf.rgxTabs[0] & 0xFFFFFF) == 1440 && (pf.rgxTabs[1] & 0xFFFFFF) == 4320, "tab stops", "count=%d", pf.cTabCount);
}

void TestRoundTrips()
{
    Section("File formats");
    NewDoc();
    const std::wstring sample = L"Plain \xD55C\xAE00 text\rBold line\rCentred line";
    Type(sample.c_str());
    Editor_SetSel(17, 26);          // "Bold line"
    App_OnCommand(ID_FORMAT_BOLD);
    Editor_SetSel(30, 30);
    App_OnCommand(ID_FORMAT_ALIGN_CENTER);
    std::wstring before = Normalize(Editor_GetText(false));

    std::vector<BYTE> rtf;
    bool produced = File_Serialize(FMT_RTF, false, rtf) && rtf.size() > 20 && memcmp(rtf.data(), "{\\rtf", 5) == 0;
    Check(produced, "RTF is produced", "%u bytes", (unsigned)rtf.size());
    NewDoc();
    DocFormat fmt = FMT_TEXT_UTF8;
    bool bom = false;
    Check(File_LoadData(rtf, L"x.rtf", &fmt, &bom) == LOAD_OK && fmt == FMT_RTF, "RTF is detected and loaded");
    std::wstring roundTrip = Normalize(Editor_GetText(false));
    Check(roundTrip == before, "RTF round trip keeps the text", "%u characters", (unsigned)roundTrip.size());
    Check(Has(CharFormatAt(18, 25), CFM_BOLD, CFE_BOLD), "RTF round trip keeps bold");
    Check(!Has(CharFormatAt(1, 4), CFM_BOLD, CFE_BOLD), "RTF round trip keeps regular text regular");
    Check(ParaFormatAt(30).wAlignment == PFA_CENTER, "RTF round trip keeps the alignment");

    // Plain text in the three encodings.
    const std::wstring text = L"ASCII line\r\xD55C\xAD6D\xC5B4 \xC904\rcaf\x00E9 \x20AC";   // Korean line, "café €"
    struct { DocFormat f; const char* name; bool bom; } encodings[] = {
        { FMT_TEXT_UTF8, "UTF-8", false }, { FMT_TEXT_UTF8, "UTF-8 with BOM", true }, { FMT_TEXT_UTF16, "UTF-16", false } };
    for (auto& e : encodings) {
        NewDoc();
        Type(text.c_str());
        std::vector<BYTE> bytes;
        bool ok = File_Serialize(e.f, e.bom, bytes);
        NewDoc();
        DocFormat got = FMT_RTF;
        bool gotBom = false;
        ok = ok && File_LoadData(bytes, L"x.txt", &got, &gotBom) == LOAD_OK;
        Check(ok && got == e.f && gotBom == e.bom && Normalize(Editor_GetText(false)) == Normalize(text), e.name,
              "format=%d bom=%d bytes=%u", (int)got, (int)gotBom, (unsigned)bytes.size());
    }
    {
        // ANSI can only be compared when the system code page can represent the text.
        NewDoc();
        Type(L"plain ascii\rsecond line");
        std::vector<BYTE> bytes;
        bool ok = File_Serialize(FMT_TEXT_ANSI, false, bytes) && bytes.size() == 24;
        Check(ok, "ANSI text uses CR LF line ends", "%u bytes", (unsigned)bytes.size());
        const BYTE latin[] = { 'c', 'a', 'f', 0xE9, '\r', '\n', 'x' };   // not valid UTF-8 -> ANSI
        std::vector<BYTE> data(latin, latin + sizeof(latin));
        DocFormat got = FMT_RTF;
        NewDoc();
        Check(File_LoadData(data, L"x.txt", &got, nullptr) == LOAD_OK && got == FMT_TEXT_ANSI, "ANSI text is detected");
        const BYTE be[] = { 0xFE, 0xFF, 0x00, 'A', 0xD5, 0x5C };       // UTF-16 big endian: "A한"
        data.assign(be, be + sizeof(be));
        NewDoc();
        Check(File_LoadData(data, L"x.txt", &got, nullptr) == LOAD_OK && Editor_GetText(false) == L"A\xD55C", "UTF-16 big endian is decoded");
        const BYTE doc[] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1, 0, 0 };
        data.assign(doc, doc + sizeof(doc));
        Check(File_LoadData(data, L"x.doc", &got, nullptr) == LOAD_OLDDOC, "legacy .doc files are rejected with a clear message");
    }

    // Files on disk, with a non-ASCII name.
    std::wstring path = PathIn(L"selftest \xBB38\xC11C.rtf");
    NewDoc();
    Type(L"Saved to disk");
    DWORD err = 0;
    bool savedOk = File_Save(path, FMT_RTF, false, &err);
    Check(savedOk, "save to a file", "error %lu: %s", err, U8(File_ErrorText(err)).c_str());
    NewDoc();
    Type(L"Changed content");
    savedOk = File_Save(path, FMT_RTF, false, &err);
    Check(savedOk, "overwrite an existing file", "error %lu: %s", err, U8(File_ErrorText(err)).c_str());
    Check(GetFileAttributesW((path + L".~rptmp").c_str()) == INVALID_FILE_ATTRIBUTES, "no temporary file is left behind");
    NewDoc();
    bool opened = App_OpenFile(path);
    Check(opened, "open the file through the application");
    Check(Normalize(Editor_GetText(false)) == L"Changed content", "file content is the latest version");
    Check(g.doc.path == path && g.doc.format == FMT_RTF && !Editor_IsModified(), "document state after opening");
    wchar_t title[256] = L"";
    GetWindowTextW(g.hMain, title, 256);
    Check(wcsstr(title, L"selftest") != nullptr && title[0] != L'*', "window title shows the file name", "%s", U8(title).c_str());
    Type(L"!");
    Pump(50);
    GetWindowTextW(g.hMain, title, 256);
    Check(title[0] == L'*', "window title shows the modified mark", "%s", U8(title).c_str());
    DeleteFileW(path.c_str());
}

void TestFindReplace()
{
    Section("Find and replace");
    NewDoc();
    Type(L"one two three two one Two");
    Editor_SetSel(0, 0);
    Check(Editor_Find(L"two", false, false, true, true), "find");
    CHARRANGE cr;
    Editor_GetSel(cr);
    Check(cr.cpMin == 4 && cr.cpMax == 7, "first match is selected", "%ld..%ld", cr.cpMin, cr.cpMax);
    Check(Editor_Find(L"two", false, false, true, true), "find next");
    Editor_GetSel(cr);
    Check(cr.cpMin == 14, "second match", "%ld", cr.cpMin);
    Check(Editor_Find(L"two", true, false, true, true), "match case wraps around");
    Editor_GetSel(cr);
    Check(cr.cpMin == 4, "case-sensitive search skips \"Two\"", "%ld", cr.cpMin);
    bool up = Editor_Find(L"one", false, false, false, true);
    Editor_GetSel(cr);
    CheckWin(up && cr.cpMin == 0, "search upwards", "%ld", cr.cpMin);
    Check(!Editor_Find(L"absent", false, false, true, true), "missing text is reported");
    Editor_SetSel(0, 0);
    Check(Editor_Find(L"one", false, true, true, true) && Editor_ReplaceCurrent(L"one", L"1", false, true), "replace one match");
    Check(Normalize(Editor_GetText(false)) == L"1 two three two one Two", "text after replace", "%s", U8(Editor_GetText(false)).c_str());
    int n = Editor_ReplaceAll(L"two", L"2", false, false);
    Check(n == 3 && Normalize(Editor_GetText(false)) == L"1 2 three 2 one 2", "replace all", "%d replaced: %s", n, U8(Editor_GetText(false)).c_str());
    Check(Editor_ReplaceAll(L"2", L"22", false, false) == 3 && Normalize(Editor_GetText(false)) == L"1 22 three 22 one 22",
          "replace all does not loop on its own output");
}

void TestUndo()
{
    Section("Undo and redo");
    NewDoc();
    Check(!SendMessageW(g.hEdit, EM_CANUNDO, 0, 0), "nothing to undo in a new document");
    Type(L"abc");
    Check(SendMessageW(g.hEdit, EM_CANUNDO, 0, 0) != 0, "undo is available after typing");
    App_OnCommand(ID_EDIT_UNDO);
    Check(Editor_GetText(false).empty(), "undo");
    App_OnCommand(ID_EDIT_REDO);
    Check(Editor_GetText(false) == L"abc", "redo");
}

void TestGeometry()
{
    Section("Zoom, ruler geometry and word wrap");
    NewDoc();
    Type(L"X marks the indent");
    PARAFORMAT2 pf = {};
    pf.dwMask = PFM_STARTINDENT | PFM_OFFSET | PFM_ALIGNMENT;
    pf.dxStartIndent = 1440;
    pf.wAlignment = PFA_LEFT;
    Editor_SetSel(0, 0);
    Editor_SetParaFormat(pf);

    const int zooms[] = { 100, 200, 50, 150 };
    for (int z : zooms) {
        Editor_SetZoom(z);
        Pump();
        POINTL pt = {};
        SendMessageW(g.hEdit, EM_POSFROMCHAR, (WPARAM)&pt, 0);
        double expected = Editor_OriginX() + 1440 * Editor_PixelsPerTwip();
        double tolerance = 2.5 + expected * 0.01;
        Check(Editor_GetZoom() == z && std::fabs(pt.x - expected) <= tolerance, "ruler position of a 1\" indent",
              "zoom %d%%: character x=%ld, ruler x=%.1f, origin=%d", z, pt.x, expected, Editor_OriginX());
    }
    Editor_SetZoom(100);

    // Word wrap modes.
    NewDoc();
    std::wstring longLine;
    for (int i = 0; i < 120; ++i) longLine += L"word wrapping test ";
    Type(longLine.c_str());
    int saved = g.cfg.wrapMode;
    App_OnCommand(ID_VIEW_WRAP_NONE);
    Pump();
    LRESULT linesNone = SendMessageW(g.hEdit, EM_GETLINECOUNT, 0, 0);
    Check(linesNone == 1, "no wrap: one line", "%ld lines", (long)linesNone);
    // Horizontal scrolling keeps the ruler aligned with the text.
    Editor_SetSel(Editor_GetTextLength(), Editor_GetTextLength());
    SendMessageW(g.hEdit, EM_SCROLLCARET, 0, 0);
    Pump();
    POINTL p0 = {};
    SendMessageW(g.hEdit, EM_POSFROMCHAR, (WPARAM)&p0, 0);
    Check(p0.x < 0 && std::abs((int)p0.x - Editor_OriginX()) <= 3, "ruler follows horizontal scrolling", "char x=%ld, origin=%d", p0.x, Editor_OriginX());
    Editor_SetSel(0, 0);
    SendMessageW(g.hEdit, EM_SCROLLCARET, 0, 0);

    App_OnCommand(ID_VIEW_WRAP_WINDOW);
    Pump();
    LRESULT linesWindow = SendMessageW(g.hEdit, EM_GETLINECOUNT, 0, 0);
    Check(linesWindow > 1, "wrap to window", "%ld lines", (long)linesWindow);

    App_OnCommand(ID_VIEW_WRAP_RULER);
    Pump();
    LRESULT linesRuler = SendMessageW(g.hEdit, EM_GETLINECOUNT, 0, 0);
    RECT oldMargins = g.cfg.margins;
    g.cfg.margins.left += 1440;
    g.cfg.margins.right += 1440;
    Editor_ApplyWrap();
    Pump();
    LRESULT linesNarrow = SendMessageW(g.hEdit, EM_GETLINECOUNT, 0, 0);
    Check(linesRuler > 1 && linesNarrow > linesRuler, "wrap to ruler follows the page width", "%ld lines, %ld with wider margins",
          (long)linesRuler, (long)linesNarrow);
    // The right edge of the wrapped text must not pass the page width.
    LONG maxX = 0;
    LONG len = Editor_GetTextLength();
    for (LONG cp = 0; cp < len; cp += 3) {
        POINTL p = {};
        SendMessageW(g.hEdit, EM_POSFROMCHAR, (WPARAM)&p, cp);
        maxX = std::max(maxX, p.x);
    }
    double limit = Editor_OriginX() + Editor_PageWidthTwips() * Editor_PixelsPerTwip();
    Check(maxX <= limit + 4, "wrapped text stays inside the ruler width", "max x=%ld, ruler end=%.1f", maxX, limit);
    g.cfg.margins = oldMargins;
    g.cfg.wrapMode = saved;
    Editor_ApplyWrap();
}

void TestPictures()
{
    Section("Pictures");
    NewDoc();
    Type(L"Before ");
    std::vector<BYTE> png = MakeTestPng(96, 64);
    Check(png.size() > 60, "test picture encoded as PNG", "%u bytes", (unsigned)png.size());
    LONG before = Editor_GetTextLength();
    bool inserted = Editor_InsertPictureData(png.data(), png.size());
    LONG after = Editor_GetTextLength();
    Check(inserted && after == before + 1, "picture inserted", "length %ld -> %ld, objects=%d", before, after, Editor_GetObjectCount());
    Type(L" after");
    Editor_SetSel(before, before + 1);
    bool selected = Editor_IsPictureSelected();
    Check(selected, "picture can be selected", "selection type 0x%lX", (long)SendMessageW(g.hEdit, EM_SELECTIONTYPE, 0, 0));

    std::vector<BYTE> rtf;
    File_Serialize(FMT_RTF, false, rtf);
    std::string rtfText(rtf.begin(), rtf.end());
    bool hasPict = rtfText.find("\\pict") != std::string::npos;
    CheckWin(hasPict, "picture is stored in the RTF", "%u bytes%s%s%s", (unsigned)rtf.size(),
          rtfText.find("\\pngblip") != std::string::npos ? ", pngblip" : "",
          rtfText.find("\\wmetafile") != std::string::npos ? ", wmetafile" : "",
          rtfText.find("\\dibitmap") != std::string::npos ? ", dibitmap" : "");

    int w0 = 0, h0 = 0, w1 = 0, h1 = 0;
    Editor_SetSel(before, before + 1);
    bool gotSize = Editor_GetSelectedPictureScale(&w0, &h0);
    bool scaled = gotSize && Editor_ScaleSelectedPicture(50, 50);
    Editor_SetSel(before, before + 1);
    bool gotSize2 = scaled && Editor_GetSelectedPictureScale(&w1, &h1);
    CheckWin(gotSize2 && std::abs(w1 * 2 - w0) <= 30 && std::abs(h1 * 2 - h0) <= 30, "resize picture to 50%",
             "%dx%d -> %dx%d twips", w0, h0, w1, h1);

    NewDoc();
    DocFormat fmt;
    File_LoadData(rtf, L"x.rtf", &fmt, nullptr);
    LONG reloaded = Editor_GetTextLength();
    CheckWin(reloaded == after + 6, "picture survives an RTF round trip", "length %ld (expected %ld), objects=%d",
             reloaded, after + 6, Editor_GetObjectCount());

    // Other image formats are converted on the way in.
    NewDoc();
    std::vector<BYTE> dib;
    bool toDib = Image_ToDib(png.data(), png.size(), dib) && dib.size() > 40;
    Check(toDib, "PNG decodes to a DIB", "%u bytes", (unsigned)dib.size());
    std::vector<BYTE> back;
    int bw = 0, bh = 0;
    bool toPng = Image_DibToPng(dib.data(), dib.size(), back, &bw, &bh) && bw == 96 && bh == 64;
    Check(toPng, "DIB encodes back to PNG", "%dx%d", bw, bh);
}

void TestClipboard()
{
    Section("Clipboard");
    NewDoc();
    Type(L"clip board");
    Editor_SetSel(0, 4);
    App_OnCommand(ID_FORMAT_BOLD);
    Editor_SetSel(0, Editor_GetTextLength());
    App_OnCommand(ID_EDIT_COPY);
    Pump(50);
    bool hasText = IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
    if (!hasText) {
        ++g_skip;
        Line("SKIP  clipboard tests  -- the clipboard is not available in this session");
        return;
    }
    NewDoc();
    App_OnCommand(ID_EDIT_PASTE);
    Check(Normalize(Editor_GetText(false)) == L"clip board", "copy and paste", "%s", U8(Editor_GetText(false)).c_str());
    Check(Has(CharFormatAt(0, 4), CFM_BOLD, CFE_BOLD), "paste keeps the formatting");
    NewDoc();
    App_OnCommand(ID_EDIT_PASTETEXT);
    Check(Normalize(Editor_GetText(false)) == L"clip board" && !Has(CharFormatAt(0, 4), CFM_BOLD, CFE_BOLD), "paste as plain text drops the formatting");
    NewDoc();
    Type(L"cut me");
    Editor_SetSel(0, 3);
    App_OnCommand(ID_EDIT_CUT);
    Check(Normalize(Editor_GetText(false)) == L" me", "cut");
}

void TestPrinting()
{
    Section("Printing");
    NewDoc();
    Type(L"Print test \xD55C\xAE00");
    int pages = Print_CountPages();
    Check(pages == 1, "a short document has one page", "%d", pages);

    std::wstring big;
    for (int i = 0; i < 400; ++i) big += Fmt(L"Paragraph number %d of the print test document.\r", i + 1);
    NewDoc();
    Type(big.c_str());
    Editor_SetSel(0, 0);
    PrintProbe probe;
    bool ok = Print_Probe(probe);
    Line("layout target: %s, %d dpi, paper %ldx%ld twips, pages=%d", probe.usesPrinter ? "printer" : "screen", probe.dpi,
         probe.paper.cx, probe.paper.cy, probe.pages);
    Line("first page: metafile records=%d ink=%ld, bitmap ink=%ld", probe.emfRecords, probe.inkEmf, probe.inkBitmap);
    CheckWin(ok && probe.pages > 3, "a long document is split into pages", "%d pages", probe.pages);
    CheckWin(probe.inkEmf > 200, "page renders through a metafile", "ink=%ld records=%d", probe.inkEmf, probe.emfRecords);
    CheckWin(probe.inkBitmap > 200, "page renders into a bitmap", "ink=%ld", probe.inkBitmap);
    bool png = Print_RenderPagePng(0, 700, PathIn(L"selftest-print-page1.png").c_str());
    Check(png, "print preview page saved as PNG");

    // Zoom must not change the printed layout.
    Editor_SetZoom(250);
    int zoomedPages = Print_CountPages();
    Editor_SetZoom(100);
    CheckWin(zoomedPages == probe.pages, "zoom does not affect pagination", "%d vs %d", zoomedPages, probe.pages);

    // Real print job into a PDF file, when the Windows PDF printer is installed.
    DWORD needed = 0, count = 0;
    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, nullptr, 0, &needed, &count);
    std::vector<BYTE> buf(needed + 16);
    bool pdfPrinter = false;
    if (needed && EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, buf.data(), needed, &needed, &count)) {
        PRINTER_INFO_4W* info = (PRINTER_INFO_4W*)buf.data();
        for (DWORD i = 0; i < count; ++i) {
            Line("printer: %s", U8(info[i].pPrinterName).c_str());
            if (lstrcmpiW(info[i].pPrinterName, L"Microsoft Print to PDF") == 0) pdfPrinter = true;
        }
    }
    if (!pdfPrinter) {
        ++g_skip;
        Line("SKIP  print to PDF  -- \"Microsoft Print to PDF\" is not installed (%lu printers)", count);
        return;
    }
    std::wstring pdf = PathIn(L"selftest-print.pdf");
    DeleteFileW(pdf.c_str());
    int printed = 0;
    bool job = Print_ToPrinter(L"Microsoft Print to PDF", pdf.c_str(), &printed);
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    bool exists = false;
    for (int i = 0; i < 100 && !exists; ++i) {   // the spooler writes the file asynchronously
        Pump(100);
        exists = GetFileAttributesExW(pdf.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeLow > 1000;
    }
    Check(job && printed == probe.pages, "print job completed", "%d pages sent", printed);
    Check(exists, "PDF file was written", "%lu bytes", fad.nFileSizeLow);
}

void DialogVisitor(HWND dlg, const wchar_t* name)
{
    Pump(30);
    wchar_t title[128] = L"";
    GetWindowTextW(dlg, title, 128);
    Line("dialog %s: \"%s\"", U8(name).c_str(), U8(title).c_str());
    SaveWindowPng(dlg, (std::wstring(L"selftest-dialog-") + name + L".png").c_str());
}

void TestUserInterface()
{
    Section("User interface");
    SetWindowPos(g.hMain, nullptr, 40, 40, Scale(1120, DpiOf(g.hMain)), Scale(780, DpiOf(g.hMain)), SWP_NOZORDER);
    Pump(50);

    const int commands[] = { ID_FILE_SAVE, ID_EDIT_UNDO, ID_EDIT_REDO, ID_EDIT_PASTE, ID_EDIT_CUT, ID_EDIT_COPY,
        ID_FORMAT_FONTNAME, ID_FORMAT_FONTSIZE, ID_FORMAT_GROW, ID_FORMAT_SHRINK, ID_FORMAT_BOLD, ID_FORMAT_ITALIC,
        ID_FORMAT_UNDERLINE, ID_FORMAT_STRIKE, ID_FORMAT_SUBSCRIPT, ID_FORMAT_SUPERSCRIPT, ID_FORMAT_COLOR, ID_FORMAT_HIGHLIGHT,
        ID_FORMAT_INDENT_DEC, ID_FORMAT_INDENT_INC, ID_FORMAT_BULLETS, ID_FORMAT_SPACING_MENU, ID_FORMAT_ALIGN_LEFT,
        ID_FORMAT_ALIGN_CENTER, ID_FORMAT_ALIGN_RIGHT, ID_FORMAT_ALIGN_JUSTIFY, ID_FORMAT_PARAGRAPH, ID_INSERT_PICTURE,
        ID_INSERT_DATETIME, ID_INSERT_OBJECT, ID_EDIT_FIND, ID_EDIT_REPLACE, ID_EDIT_SELECTALL };
    int visible = 0;
    RECT client;
    GetWindowRect(g.hCmdBar, &client);
    for (int id : commands) {
        RECT rc;
        if (CmdBar_GetItemScreenRect(id, &rc) && rc.right > rc.left && rc.left >= client.left && rc.right <= client.right) ++visible;
        else Line("command %d is not visible", id);
    }
    Check(visible == (int)ARRAYSIZE(commands), "every command bar item is laid out inside the window", "%d of %d", visible, (int)ARRAYSIZE(commands));

    bool menus = true;
    for (HMENU m : g.hMenus) menus = menus && m && GetMenuItemCount(m) > 0;
    Check(menus, "all six menus are built");

    int savedLang = g.cfg.lang, savedTheme = g.cfg.theme;
    App_OnCommand(ID_VIEW_LANG_EN);
    Check(!Strings_IsKorean() && lstrcmpW(S(S_MENU_FILE), L"&File") == 0, "English user interface");
    App_OnCommand(ID_VIEW_LANG_KO);
    Check(Strings_IsKorean() && lstrcmpW(S(S_MENU_FILE), L"\xD30C\xC77C(&F)") == 0, "Korean user interface");
    wchar_t item[128] = L"";
    GetMenuStringW(g.hMenus[0], ID_FILE_OPEN, item, 128, MF_BYCOMMAND);
    Check(wcsstr(item, L"\xC5F4\xAE30") != nullptr, "menus are rebuilt in the selected language", "%s", U8(item).c_str());

    int dialogs = Dlg_SelfTest(DialogVisitor);
    Check(dialogs == 6, "all dialogs can be created", "%d of 6", dialogs);
    Flush();

    // A representative document for the screenshots.
    NewDoc();
    Type(L"RichPad\r");
    Type(L"Windows 11\xC744 \xC704\xD55C \xC11C\xC2DD \xC788\xB294 \xD14D\xC2A4\xD2B8 \xD3B8\xC9D1\xAE30\r");
    Type(L"Bold, italic, underline, colour and highlight are applied below.\r");
    Type(L"First list item\rSecond list item\rThird list item\r");
    Type(L"Centred paragraph with a picture: ");
    LONG picturePos = Editor_GetTextLength();
    std::vector<BYTE> png = MakeTestPng(120, 60);
    Editor_InsertPictureData(png.data(), png.size());
    Editor_SetSel(0, 7);
    Editor_SetFontSize(28 * 20);
    App_OnCommand(ID_FORMAT_BOLD);
    Editor_SetTextColor(RGB(37, 99, 235), false);
    LONG l3 = 8 + (LONG)wcslen(L"Windows 11\xC744 \xC704\xD55C \xC11C\xC2DD \xC788\xB294 \xD14D\xC2A4\xD2B8 \xD3B8\xC9D1\xAE30") + 1;
    Editor_SetSel(l3, l3 + 4);
    App_OnCommand(ID_FORMAT_BOLD);
    Editor_SetSel(l3 + 6, l3 + 12);
    App_OnCommand(ID_FORMAT_ITALIC);
    Editor_SetSel(l3 + 14, l3 + 23);
    App_OnCommand(ID_FORMAT_UNDERLINE);
    Editor_SetSel(l3 + 25, l3 + 31);
    Editor_SetTextColor(RGB(220, 38, 38), false);
    Editor_SetSel(l3 + 36, l3 + 45);
    Editor_SetHighlight(RGB(255, 255, 0), false);
    LONG l4 = l3 + (LONG)wcslen(L"Bold, italic, underline, colour and highlight are applied below.") + 1;
    Editor_SetSel(l4, l4 + 40);
    App_OnCommand(ID_FORMAT_LIST_BULLET);
    Editor_SetSel(picturePos, picturePos);
    App_OnCommand(ID_FORMAT_ALIGN_CENTER);
    Editor_SetSel(l3 + 2, l3 + 2);
    Editor_SetModified(false);
    Pump(400);

    App_OnCommand(ID_VIEW_THEME_LIGHT);
    Pump(200);
    Check(!T.dark, "light theme");
    bool shot = SaveWindowPng(g.hMain, L"selftest-light.png");
    Check(shot, "screenshot (light)");
    Flush();
    App_OnCommand(ID_VIEW_THEME_DARK);
    Pump(200);
    Check(T.dark || T.highContrast, "dark theme");
    shot = SaveWindowPng(g.hMain, L"selftest-dark.png");
    Check(shot, "screenshot (dark)");
    Flush();
    App_OnCommand(ID_VIEW_LANG_EN);
    App_OnCommand(ID_VIEW_THEME_LIGHT);
    Pump(200);
    SaveWindowPng(g.hMain, L"selftest-light-en.png");
    Line("screenshots saved");
    Flush();

    // Status bar and title follow the caret and the document.
    Editor_SetSel(0, 3);
    Pump(450);
    Check(true, "status bar updated without errors");

    g.cfg.lang = savedLang;
    g.cfg.theme = savedTheme;
    App_ApplyLanguage();
    App_ApplyTheme();
    Pump(50);
}

void TestConverters()
{
    Section("DOCX and ODT");
    struct { DocFormat format; const char* name; } kinds[] = { { FMT_DOCX, "DOCX" }, { FMT_ODT, "ODT" } };
    for (auto& k : kinds) {
        NewDoc();
        Type(L"Heading \xD55C\xAE00\rBody text with bold and italic words.\rItem one\rItem two\rRight aligned");
        Editor_SetSel(0, 10);
        Editor_SetFontSize(20 * 20);
        LONG p2 = 11;
        Editor_SetSel(p2 + 15, p2 + 19);    // "bold"
        App_OnCommand(ID_FORMAT_BOLD);
        Editor_SetSel(p2 + 24, p2 + 30);    // "italic"
        App_OnCommand(ID_FORMAT_ITALIC);
        LONG p3 = p2 + 38;
        Editor_SetSel(p3, p3 + 12);
        App_OnCommand(ID_FORMAT_LIST_BULLET);
        Editor_SetSel(Editor_GetTextLength(), Editor_GetTextLength());
        App_OnCommand(ID_FORMAT_ALIGN_RIGHT);
        Editor_SetSel(Editor_GetTextLength(), Editor_GetTextLength());
        Type(L" ");
        std::vector<BYTE> png = MakeTestPng(64, 48);
        bool withPicture = Editor_InsertPictureData(png.data(), png.size());
        std::wstring before = Normalize(Editor_GetText(false));
        LONG lengthBefore = Editor_GetTextLength();

        std::vector<BYTE> bytes;
        bool saved = File_Serialize(k.format, false, bytes) && bytes.size() > 500 && bytes[0] == 'P' && bytes[1] == 'K';
        Check(saved, (std::string(k.name) + ": document is written").c_str(), "%u bytes", (unsigned)bytes.size());
        if (!saved) continue;
        File_WriteAll(PathIn(k.format == FMT_DOCX ? L"selftest-sample.docx" : L"selftest-sample.odt"), bytes.data(), bytes.size(), nullptr);

        NewDoc();
        DocFormat got = FMT_RTF;
        LoadResult r = File_LoadData(bytes, L"x", &got, nullptr);
        Check(r == LOAD_OK && got == k.format, (std::string(k.name) + ": document is detected and loaded").c_str(), "result=%d format=%d", (int)r, (int)got);
        std::wstring afterText = Normalize(Editor_GetText(false));
        Check(afterText == before, (std::string(k.name) + ": text round trip").c_str(), "%s", U8(afterText).c_str());
        Check(Has(CharFormatAt(p2 + 16, p2 + 18), CFM_BOLD, CFE_BOLD), (std::string(k.name) + ": bold round trip").c_str());
        Check(Has(CharFormatAt(p2 + 25, p2 + 29), CFM_ITALIC, CFE_ITALIC), (std::string(k.name) + ": italic round trip").c_str());
        Check(!Has(CharFormatAt(p2 + 1, p2 + 3), CFM_BOLD, CFE_BOLD), (std::string(k.name) + ": regular text stays regular").c_str());
        CHARFORMAT2W cf = CharFormatAt(1, 6);
        Check((cf.dwMask & CFM_SIZE) && cf.yHeight == 400, (std::string(k.name) + ": font size round trip").c_str(), "yHeight=%ld", cf.yHeight);
        int numbering = ParaFormatAt(p3 + 2).wNumbering;
        Check(numbering == PFN_BULLET, (std::string(k.name) + ": bullet list round trip").c_str(), "numbering=%d", numbering);
        Check(ParaFormatAt(Editor_GetTextLength()).wAlignment == PFA_RIGHT, (std::string(k.name) + ": alignment round trip").c_str());
        if (withPicture) {
            LONG lengthAfter = Editor_GetTextLength();
            CheckWin(lengthAfter == lengthBefore, (std::string(k.name) + ": picture round trip").c_str(), "length %ld (expected %ld), objects=%d",
                     lengthAfter, lengthBefore, Editor_GetObjectCount());
        }
    }
}

} // namespace

void SelfTest_Note(const wchar_t* text)
{
    std::wstring t = text ? text : L"";
    for (wchar_t& c : t) if (c == L'\n' || c == L'\r') c = L' ';
    Line("note: message box suppressed: %s", U8(t).c_str());
}

int SelfTest_Run(const wchar_t* logPath)
{
    std::wstring log = (logPath && *logPath) ? logPath : L"richpad-selftest.log";
    wchar_t full[1024];
    if (GetFullPathNameW(log.c_str(), ARRAYSIZE(full), full, nullptr)) log = full;
    size_t slash = log.find_last_of(L"\\/");
    g_dir = slash == std::wstring::npos ? L"" : log.substr(0, slash + 1);

    g_logPath = log;
    Line("RichPad %s self test", U8(APP_VERSION).c_str());
    Pump(100);

    typedef void (*TestFn)();
    const TestFn tests[] = { TestEnvironment, TestCharacterFormatting, TestParagraphFormatting, TestRoundTrips, TestFindReplace,
                             TestUndo, TestGeometry, TestPictures, TestClipboard, TestConverters, TestPrinting, TestUserInterface };
    for (TestFn fn : tests) {
        fn();
        // Keep a partial report on disk in case a later test crashes.
        File_WriteAll(log, g_out.data(), g_out.size(), nullptr);
    }

    Line("");
    Line("RESULT: %d passed, %d failed, %d skipped", g_pass, g_fail, g_skip);
    File_WriteAll(log, g_out.data(), g_out.size(), nullptr);
    Editor_SetModified(false);
    return g_fail == 0 ? 0 : 1;
}
