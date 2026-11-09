// The rich edit control and all formatting operations on it.
#include "gfx.h"
#include "editor.h"
#include "theme.h"
#include "imaging.h"
#include <richole.h>
#include <tom.h>
#include <oledlg.h>

#ifndef EM_SETFONTSIZE
#define EM_SETFONTSIZE (WM_USER + 223)
#endif
#ifndef EM_SETTYPOGRAPHYOPTIONS
#define EM_SETTYPOGRAPHYOPTIONS (WM_USER + 202)
#endif
#ifndef TO_ADVANCEDTYPOGRAPHY
#define TO_ADVANCEDTYPOGRAPHY 1
#endif
#ifndef AURL_ENABLEURL
#define AURL_ENABLEURL 1
#endif
#ifndef PFNS_PERIOD
#define PFNS_PERIOD 0x200
#endif
#ifndef PFN_ARABIC
#define PFN_ARABIC 2
#endif
#ifndef IMF_AUTOKEYBOARD
#define IMF_AUTOKEYBOARD 0x0001
#endif
#ifndef IMF_AUTOFONT
#define IMF_AUTOFONT 0x0002
#endif
#ifndef IMF_AUTOFONTSIZEADJUST
#define IMF_AUTOFONTSIZEADJUST 0x0010
#endif

namespace {

const IID kIID_IRichEditOleCallback = { 0x00020D03, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
const IID kIID_ITextDocument = { 0x8CC497C0, 0xA1DF, 0x11CE, { 0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D } };

HDC  g_targetDC = nullptr;      // display DC used as the "wrap to ruler" target device
int  g_lastScrollX = 0;
bool g_inInsets = false;
int  g_wheelAccum = 0;

// A hidden twin of the editor. It is used to measure how the rich edit control really maps twips
// to pixels on the current monitor, so the ruler stays aligned whatever the control does when the
// window moves between monitors with different DPI.
HWND g_probe = nullptr;
int  g_userZoom = 100;          // zoom chosen by the user, in percent
int  g_editDpi = 96;            // DPI the control uses for its layout
int  g_monitorDpi = 96;         // DPI of the monitor the window is on

const DWORD kEditStyle = WS_CHILD | WS_VSCROLL | WS_HSCROLL | WS_CLIPSIBLINGS |
                         ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL | ES_WANTRETURN |
                         ES_SAVESEL | ES_SELECTIONBAR;

int Gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a ? a : 1; }

void SendZoom(HWND hwnd)
{
    // The extra monitorDpi / editDpi factor keeps the text in step with the rest of the window
    // when the control itself does not rescale for the monitor.
    if (g_userZoom == 100 && g_monitorDpi == g_editDpi) {
        SendMessageW(hwnd, EM_SETZOOM, 0, 0);
        return;
    }
    int num = g_userZoom * g_monitorDpi, den = 100 * g_editDpi;
    int d = Gcd(num, den);
    SendMessageW(hwnd, EM_SETZOOM, (WPARAM)(num / d), (LPARAM)(den / d));
}

void ApplyInsets(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    int dpi = DpiOf(hwnd);
    int padX = Scale(12, dpi), padY = Scale(8, dpi);
    RECT fr = { padX, padY, std::max(padX + 16, (int)rc.right - padX), std::max(padY + 16, (int)rc.bottom) };
    SendMessageW(hwnd, EM_SETRECT, 0, (LPARAM)&fr);
}

// ---------------------------------------------------------------------------
// IRichEditOleCallback: storage for embedded objects and drag-and-drop policy.
// ---------------------------------------------------------------------------
class OleCallback : public IRichEditOleCallback {
public:
    OleCallback()
    {
        StgCreateDocfile(nullptr, STGM_TRANSACTED | STGM_READWRITE | STGM_SHARE_EXCLUSIVE | STGM_CREATE | STGM_DELETEONRELEASE,
                         0, &m_root);
    }
    virtual ~OleCallback() { if (m_root) m_root->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIID_IRichEditOleCallback)) {
            *ppv = static_cast<IRichEditOleCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    STDMETHODIMP GetNewStorage(LPSTORAGE* ppstg) override
    {
        if (!ppstg) return E_POINTER;
        *ppstg = nullptr;
        if (!m_root) return E_FAIL;
        wchar_t name[32];
        wsprintfW(name, L"RichPadObj%d", ++m_count);
        return m_root->CreateStorage(name, STGM_TRANSACTED | STGM_READWRITE | STGM_SHARE_EXCLUSIVE | STGM_CREATE, 0, 0, ppstg);
    }
    STDMETHODIMP GetInPlaceContext(LPOLEINPLACEFRAME*, LPOLEINPLACEUIWINDOW*, LPOLEINPLACEFRAMEINFO) override { return E_NOTIMPL; }
    STDMETHODIMP ShowContainerUI(BOOL) override { return S_OK; }
    STDMETHODIMP QueryInsertObject(LPCLSID, LPSTORAGE, LONG) override { return S_OK; }
    STDMETHODIMP DeleteObject(LPOLEOBJECT) override { return S_OK; }
    STDMETHODIMP QueryAcceptData(LPDATAOBJECT pdo, CLIPFORMAT*, DWORD reco, BOOL fReally, HGLOBAL) override
    {
        // Files dropped from Explorer are opened (or inserted as pictures) instead of being embedded.
        if (reco == RECO_DROP && pdo) {
            FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            if (pdo->QueryGetData(&fe) == S_OK) {
                if (fReally) {
                    STGMEDIUM stg = {};
                    if (SUCCEEDED(pdo->GetData(&fe, &stg))) {
                        wchar_t path[1024];
                        if (DragQueryFileW((HDROP)stg.hGlobal, 0, path, ARRAYSIZE(path)))
                            PostMessageW(g.hMain, WM_APP_OPENFILE, 1, (LPARAM)_wcsdup(path));
                        ReleaseStgMedium(&stg);
                    }
                }
                return S_FALSE;   // "handled by the callback"
            }
        }
        return S_OK;
    }
    STDMETHODIMP ContextSensitiveHelp(BOOL) override { return S_OK; }
    STDMETHODIMP GetClipboardData(CHARRANGE*, DWORD, LPDATAOBJECT*) override { return E_NOTIMPL; }
    STDMETHODIMP GetDragDropEffect(BOOL fDrag, DWORD grfKeyState, LPDWORD pdwEffect) override
    {
        if (!fDrag && pdwEffect) {
            DWORD effect;
            if ((grfKeyState & (MK_CONTROL | MK_SHIFT)) == (MK_CONTROL | MK_SHIFT)) effect = DROPEFFECT_LINK;
            else if (grfKeyState & MK_CONTROL) effect = DROPEFFECT_COPY;
            else effect = DROPEFFECT_MOVE;
            if (effect & *pdwEffect) *pdwEffect = effect;
        }
        return S_OK;
    }
    STDMETHODIMP GetContextMenu(WORD, LPOLEOBJECT, CHARRANGE*, HMENU* phmenu) override
    {
        // The context menu is shown from the WM_CONTEXTMENU handler of the subclass instead.
        if (phmenu) *phmenu = nullptr;
        return E_NOTIMPL;
    }

private:
    LONG m_ref = 1;
    int m_count = 0;
    IStorage* m_root = nullptr;
};

OleCallback* g_callback = nullptr;

IRichEditOle* GetOle()
{
    IRichEditOle* ole = nullptr;
    SendMessageW(g.hEdit, EM_GETOLEINTERFACE, 0, (LPARAM)&ole);
    return ole;
}

ITextDocument* GetTextDocument()
{
    ITextDocument* doc = nullptr;
    if (IRichEditOle* ole = GetOle()) {
        ole->QueryInterface(kIID_ITextDocument, (void**)&doc);
        ole->Release();
    }
    return doc;
}

void CheckHorizontalScroll()
{
    POINT pt = {};
    SendMessageW(g.hEdit, EM_GETSCROLLPOS, 0, (LPARAM)&pt);
    if (pt.x != g_lastScrollX) {
        g_lastScrollX = pt.x;
        if (g.hRuler) InvalidateRect(g.hRuler, nullptr, FALSE);
    }
}

// Link under the mouse pointer.  The control reports it (EN_LINK) while it handles WM_SETCURSOR and
// WM_MOUSEMOVE; a mouse move without such a report means that the pointer has left the link.
bool g_linkInCursor = false, g_linkInMove = false;
std::wstring g_linkReported, g_linkShown;

void UpdateLinkHint(HWND hwnd, bool onLink)
{
    std::wstring now = onLink ? g_linkReported : std::wstring();
    if (now == g_linkShown) return;
    g_linkShown = now;
    if (!now.empty()) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
    }
    App_SetLinkHint(now);
}

LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
{
    switch (msg) {
    case WM_SETCURSOR: g_linkInCursor = false; break;
    case WM_MOUSEMOVE: g_linkInMove = false; break;
    case WM_MOUSELEAVE:
        g_linkInCursor = g_linkInMove = false;
        UpdateLinkHint(hwnd, false);
        break;
    case WM_RBUTTONDOWN:
        App_ForgetContextLink();
        break;
    case WM_CONTEXTMENU: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (pt.x == -1 && pt.y == -1) {
            App_ForgetContextLink();
            // Invoked from the keyboard: show the menu below the caret.
            CHARRANGE cr; Editor_GetSel(cr);
            POINTL p = {};
            SendMessageW(hwnd, EM_POSFROMCHAR, (WPARAM)&p, cr.cpMax);
            pt.x = p.x; pt.y = p.y + Scale(18, DpiOf(hwnd));
            RECT rc; GetClientRect(hwnd, &rc);
            pt.x = std::clamp((int)pt.x, 0, (int)rc.right);
            pt.y = std::clamp((int)pt.y, 0, (int)rc.bottom);
            ClientToScreen(hwnd, &pt);
        }
        App_ShowContextMenu(pt);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) {
            g_wheelAccum += GET_WHEEL_DELTA_WPARAM(wParam);
            int steps = g_wheelAccum / WHEEL_DELTA;
            if (steps != 0) {
                g_wheelAccum -= steps * WHEEL_DELTA;
                int zoom = Editor_GetZoom();
                int next = steps > 0 ? ((zoom / 10) + steps) * 10 : (((zoom + 9) / 10) + steps) * 10;
                Editor_SetZoom(next);
                PostMessageW(g.hMain, WM_APP_ZOOMCHANGED, 0, 0);
            }
            return 0;
        }
        break;
    case WM_SIZE: {
        LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
        Editor_UpdateInsets();
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, EditSubclassProc, id);
        break;
    }

    LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_HSCROLL: case WM_MOUSEHWHEEL: case WM_MOUSEWHEEL: case WM_KEYDOWN: case WM_KEYUP:
    case WM_CHAR: case WM_LBUTTONUP: case WM_IME_ENDCOMPOSITION: case EM_SETSCROLLPOS:
        CheckHorizontalScroll();
        break;
    case WM_MOUSEMOVE:
        if (wParam & MK_LBUTTON) CheckHorizontalScroll();
        UpdateLinkHint(hwnd, g_linkInCursor || g_linkInMove);
        g_linkInCursor = false;     // belongs to this mouse move only
        break;
    }
    return r;
}

bool FontExists(const wchar_t* face)
{
    HDC hdc = GetDC(nullptr);
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    bool found = false;
    EnumFontFamiliesExW(hdc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int {
        *(bool*)lp = true;
        return 0;
    }, (LPARAM)&found, 0);
    ReleaseDC(nullptr, hdc);
    return found;
}

struct StreamInCtx { const BYTE* p; size_t left; };
struct StreamOutCtx { std::string* out; };

DWORD CALLBACK StreamInCb(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb)
{
    StreamInCtx* ctx = (StreamInCtx*)cookie;
    size_t n = std::min((size_t)cb, ctx->left);
    memcpy(buf, ctx->p, n);
    ctx->p += n;
    ctx->left -= n;
    *pcb = (LONG)n;
    return 0;
}

DWORD CALLBACK StreamOutCb(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb)
{
    StreamOutCtx* ctx = (StreamOutCtx*)cookie;
    ctx->out->append((const char*)buf, (size_t)cb);
    *pcb = cb;
    return 0;
}

void AppendHex(std::string& out, const BYTE* data, size_t size)
{
    static const char kHex[] = "0123456789abcdef";
    out.reserve(out.size() + size * 2 + size / 32 + 16);
    for (size_t i = 0; i < size; ++i) {
        out += kHex[data[i] >> 4];
        out += kHex[data[i] & 15];
        if ((i & 63) == 63) out += "\r\n";
    }
}

// Inserts an RTF fragment that contains exactly one picture; reports whether the control accepted it.
bool InsertPictureRtf(const std::string& rtf)
{
    CHARRANGE cr; Editor_GetSel(cr);
    LONG before = Editor_GetTextLength() - (cr.cpMax - cr.cpMin);
    Editor_StreamIn(rtf.data(), rtf.size(), SF_RTF | SFF_SELECTION);
    return Editor_GetTextLength() > before;
}

// Finds "\keywordN" in an RTF fragment and multiplies N by percent/100.
int ScaleRtfValue(std::string& rtf, const char* keyword, int percent)
{
    int changed = 0;
    size_t klen = strlen(keyword), pos = 0;
    while ((pos = rtf.find(keyword, pos)) != std::string::npos) {
        size_t numStart = pos + klen, numEnd = numStart;
        if (numEnd < rtf.size() && rtf[numEnd] == '-') ++numEnd;
        while (numEnd < rtf.size() && rtf[numEnd] >= '0' && rtf[numEnd] <= '9') ++numEnd;
        if (numEnd == numStart) { pos = numStart; continue; }
        long value = atol(rtf.substr(numStart, numEnd - numStart).c_str());
        long scaled = std::max(1L, (long)MulDiv((int)value, percent, 100));
        std::string repl = std::to_string(scaled);
        rtf.replace(numStart, numEnd - numStart, repl);
        pos = numStart + repl.size();
        ++changed;
    }
    return changed;
}

long ReadRtfValue(const std::string& rtf, const char* keyword, long def)
{
    size_t pos = rtf.find(keyword);
    if (pos == std::string::npos) return def;
    const char* p = rtf.c_str() + pos + strlen(keyword);
    if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return def;
    return atol(p);
}

} // namespace

// ===========================================================================

bool Editor_Create(HWND parent)
{
    g.hEdit = CreateWindowExW(0, MSFTEDIT_CLASS, L"", kEditStyle | WS_VISIBLE | WS_TABSTOP, 0, 0, 100, 100,
                              parent, (HMENU)(INT_PTR)1, g.hInst, nullptr);
    if (!g.hEdit) return false;
    // Never shown; see the comment at g_probe.
    g_probe = CreateWindowExW(0, MSFTEDIT_CLASS, L"", kEditStyle, 0, 0, 2400, 120,
                              parent, (HMENU)(INT_PTR)2, g.hInst, nullptr);
    if (g_probe) SendMessageW(g_probe, EM_SETTARGETDEVICE, 0, 1);

    SendMessageW(g.hEdit, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
    SendMessageW(g.hEdit, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE | ENM_SCROLL | ENM_LINK);
    SendMessageW(g.hEdit, EM_SETUNDOLIMIT, 200, 0);
    SendMessageW(g.hEdit, EM_AUTOURLDETECT, AURL_ENABLEURL, 0);
    // Advanced typography is required for justified paragraphs.
    SendMessageW(g.hEdit, EM_SETTYPOGRAPHYOPTIONS, TO_ADVANCEDTYPOGRAPHY, TO_ADVANCEDTYPOGRAPHY);
    // Keep automatic font binding (Korean text typed in a Latin font gets a Korean font), but do not
    // let the control switch keyboard layouts or silently change font sizes per script.
    LRESULT lang = SendMessageW(g.hEdit, EM_GETLANGOPTIONS, 0, 0);
    lang &= ~(LRESULT)(IMF_AUTOKEYBOARD | IMF_AUTOFONTSIZEADJUST);
    lang |= IMF_AUTOFONT;
    SendMessageW(g.hEdit, EM_SETLANGOPTIONS, 0, lang);

    g_callback = new OleCallback();
    SendMessageW(g.hEdit, EM_SETOLECALLBACK, 0, (LPARAM)static_cast<IRichEditOleCallback*>(g_callback));

    SetWindowSubclass(g.hEdit, EditSubclassProc, 1, 0);
    Editor_ApplyTheme();
    Editor_UpdateInsets();
    Editor_ApplyWrap();
    return true;
}

void Editor_Destroy()
{
    if (g_probe) { DestroyWindow(g_probe); g_probe = nullptr; }
    if (g_callback) { g_callback->Release(); g_callback = nullptr; }
    if (g_targetDC) { DeleteDC(g_targetDC); g_targetDC = nullptr; }
}

void Editor_ApplyTheme()
{
    // The page itself stays white (like paper); only the scroll bars follow the app theme.
    Theme_ApplyControl(g.hEdit, L"Explorer");
    SendMessageW(g.hEdit, EM_SETBKGNDCOLOR, 0, RGB(255, 255, 255));
}

void Editor_UpdateInsets()
{
    if (!g.hEdit || g_inInsets) return;
    g_inInsets = true;
    ApplyInsets(g.hEdit);
    g_inInsets = false;
}

void Editor_ApplyWrap()
{
    switch (g.cfg.wrapMode) {
    case WRAP_NONE:
        SendMessageW(g.hEdit, EM_SETTARGETDEVICE, 0, 1);
        break;
    case WRAP_RULER:
        if (!g_targetDC) g_targetDC = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
        SendMessageW(g.hEdit, EM_SETTARGETDEVICE, (WPARAM)g_targetDC, Editor_PageWidthTwips());
        break;
    default:
        SendMessageW(g.hEdit, EM_SETTARGETDEVICE, 0, 0);
        break;
    }
    CheckHorizontalScroll();
    if (g.hRuler) InvalidateRect(g.hRuler, nullptr, FALSE);
}

void Editor_CalibrateOrigin()
{
    // Distance between the left edge of the formatting rectangle and the first character
    // (the selection bar), measured on the probe with the editor's current zoom.
    HWND h = g_probe;
    if (!h) { g.textOriginDelta = 0; return; }
    ApplyInsets(h);
    SendZoom(h);
    RECT rc = {};
    SendMessageW(h, EM_GETRECT, 0, (LPARAM)&rc);
    POINTL pt = {};
    SendMessageW(h, EM_POSFROMCHAR, (WPARAM)&pt, 0);
    int delta = (int)pt.x - (int)rc.left;
    g.textOriginDelta = (delta >= -16 && delta <= 256) ? delta : 0;
}

void Editor_OnDpiChanged()
{
    if (!g.hEdit) return;
    g_monitorDpi = DpiOf(g.hEdit);
    g_editDpi = g_monitorDpi;
    if (g_probe) {
        // A tab stop two inches from the margin tells how many pixels the control uses per inch.
        SendMessageW(g_probe, EM_SETZOOM, 0, 0);
        SETTEXTEX st = { ST_DEFAULT, 1200 };
        SendMessageW(g_probe, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)L"\tX");
        PARAFORMAT2 pf = {};
        pf.cbSize = sizeof(pf);
        pf.dwMask = PFM_TABSTOPS | PFM_ALIGNMENT | PFM_STARTINDENT | PFM_OFFSET | PFM_RIGHTINDENT;
        pf.wAlignment = PFA_LEFT;
        pf.cTabCount = 1;
        pf.rgxTabs[0] = 2880;
        SendMessageW(g_probe, EM_SETSEL, 0, -1);
        SendMessageW(g_probe, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
        SendMessageW(g_probe, EM_SETSEL, 0, 0);
        POINTL p0 = {}, p1 = {};
        SendMessageW(g_probe, EM_POSFROMCHAR, (WPARAM)&p0, 0);
        SendMessageW(g_probe, EM_POSFROMCHAR, (WPARAM)&p1, 1);
        int dpi = (int)(p1.x - p0.x) / 2;
        if (dpi >= 48 && dpi <= 960) {
            // Ignore rounding noise.
            if (std::abs(dpi - g_monitorDpi) * 50 > g_monitorDpi) g_editDpi = dpi;
        }
    }
    SendZoom(g.hEdit);
    Editor_UpdateInsets();
    Editor_CalibrateOrigin();
    CheckHorizontalScroll();
    if (g.hRuler) InvalidateRect(g.hRuler, nullptr, FALSE);
}

int Editor_GetZoom() { return g_userZoom; }

void Editor_SetZoom(int percent)
{
    g_userZoom = std::clamp(percent, 10, 500);
    SendZoom(g.hEdit);
    Editor_CalibrateOrigin();
    CheckHorizontalScroll();
    if (g.hRuler) InvalidateRect(g.hRuler, nullptr, FALSE);
}

bool Editor_SuspendZoom()
{
    if (g_userZoom == 100 && g_monitorDpi == g_editDpi) return false;
    SendMessageW(g.hEdit, EM_SETZOOM, 0, 0);
    return true;
}

void Editor_ResumeZoom() { SendZoom(g.hEdit); }

int Editor_LayoutDpi() { return g_editDpi; }

LONG Editor_GetTextLength()
{
    GETTEXTLENGTHEX gtl = { GTL_DEFAULT, 1200 };
    return (LONG)SendMessageW(g.hEdit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
}

void Editor_GetSel(CHARRANGE& cr)
{
    cr.cpMin = cr.cpMax = 0;
    SendMessageW(g.hEdit, EM_EXGETSEL, 0, (LPARAM)&cr);
}

void Editor_SetSel(LONG cpMin, LONG cpMax)
{
    CHARRANGE cr = { cpMin, cpMax };
    SendMessageW(g.hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);
}

bool Editor_HasSelection()
{
    CHARRANGE cr; Editor_GetSel(cr);
    return cr.cpMin != cr.cpMax;
}

bool Editor_IsModified() { return SendMessageW(g.hEdit, EM_GETMODIFY, 0, 0) != 0; }
void Editor_SetModified(bool modified) { SendMessageW(g.hEdit, EM_SETMODIFY, modified ? TRUE : FALSE, 0); }

std::wstring Editor_GetText(bool selectionOnly, bool visibleOnly)
{
    std::wstring text;
    LONG len;
    if (selectionOnly) {
        CHARRANGE cr; Editor_GetSel(cr);
        len = cr.cpMax - cr.cpMin;
    } else {
        len = Editor_GetTextLength();
    }
    if (len <= 0) return text;
    text.resize((size_t)len + 1);
    GETTEXTEX gt = {};
    gt.cb = (DWORD)((len + 1) * sizeof(wchar_t));
    gt.flags = (selectionOnly ? GT_SELECTION : GT_DEFAULT) | (visibleOnly ? GT_NOHIDDENTEXT : 0);
    gt.codepage = 1200;
    LRESULT n = SendMessageW(g.hEdit, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)&text[0]);
    text.resize((size_t)std::min<LRESULT>(std::max<LRESULT>(0, n), len));
    return text;
}

std::wstring Editor_GetPlainText()
{
    std::wstring text;
    GETTEXTLENGTHEX gtl = { GTL_USECRLF | GTL_PRECISE | GTL_NUMCHARS, 1200 };
    LRESULT len = SendMessageW(g.hEdit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    if (len <= 0) len = (LRESULT)Editor_GetTextLength() * 2;     // E_INVALIDARG: assume the worst case
    if (len <= 0) return text;
    text.resize((size_t)len + 1);
    GETTEXTEX gt = {};
    gt.cb = (DWORD)((len + 1) * sizeof(wchar_t));
    gt.flags = GT_USECRLF | GT_NOHIDDENTEXT;
    gt.codepage = 1200;
    LRESULT n = SendMessageW(g.hEdit, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)&text[0]);
    text.resize((size_t)std::min<LRESULT>(std::max<LRESULT>(0, n), len));
    // Embedded objects have no textual representation.
    text.erase(std::remove(text.begin(), text.end(), (wchar_t)0xFFFC), text.end());
    return text;
}

// tomTrue, tomFalse, or tomUndefined when only a part of the range is hidden.
static long HiddenState(LONG cpMin, LONG cpMax)
{
    long value = tomFalse;
    if (cpMax <= cpMin) return value;
    if (ITextDocument* doc = GetTextDocument()) {
        ITextRange* range = nullptr;
        if (SUCCEEDED(doc->Range(cpMin, cpMax, &range)) && range) {
            ITextFont* font = nullptr;
            if (SUCCEEDED(range->GetFont(&font)) && font) {
                long v = tomFalse;
                if (SUCCEEDED(font->GetHidden(&v)) && (v == tomTrue || v == tomUndefined)) value = v;
                font->Release();
            }
            range->Release();
        }
        doc->Release();
    }
    return value;
}

bool Editor_IsRangeHidden(LONG cpMin, LONG cpMax)
{
    return HiddenState(cpMin, cpMax) != tomFalse;
}

namespace {

std::wstring TextRange(LONG cpMin, LONG cpMax)
{
    std::wstring text;
    if (cpMax <= cpMin) return text;
    text.resize((size_t)(cpMax - cpMin) + 1);
    TEXTRANGEW tr = { { cpMin, cpMax }, &text[0] };
    LRESULT n = SendMessageW(g.hEdit, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    text.resize((size_t)std::min<LRESULT>(std::max<LRESULT>(0, n), cpMax - cpMin));
    return text;
}

// Extracts the address from a field instruction such as: HYPERLINK "https://example.com" \o "tip"
bool ParseLinkInstruction(const std::wstring& text, size_t keyword, std::wstring& url)
{
    size_t p = keyword + 9;     // strlen("HYPERLINK")
    while (p < text.size() && text[p] == L' ') ++p;
    // Switches such as \l "bookmark" come with their own argument.
    if (p < text.size() && text[p] == L'\\') return false;
    if (p < text.size() && text[p] == L'"') {
        size_t end = text.find(L'"', p + 1);
        if (end == std::wstring::npos) return false;
        url = text.substr(p + 1, end - p - 1);
    } else {
        size_t end = text.find(L' ', p);
        url = text.substr(p, end == std::wstring::npos ? std::wstring::npos : end - p);
    }
    return !url.empty();
}

}

void Editor_NoteLinkUnderMouse(UINT msg, const std::wstring& url)
{
    if (msg == WM_SETCURSOR) g_linkInCursor = true;
    else if (msg == WM_MOUSEMOVE) g_linkInMove = true;
    else return;
    g_linkReported = url;
}

std::wstring Editor_LinkTarget(const CHARRANGE& range)
{
    // Depending on the kind of link, the control reports the address itself (automatically detected
    // URLs and the address inside a field instruction), the whole field, or the visible name of the link.
    std::wstring url;
    LONG len = range.cpMax - range.cpMin;
    if (len <= 0 || len > 4096) return url;
    std::wstring text = TextRange(range.cpMin, range.cpMax);
    static const wchar_t kKeyword[] = L"HYPERLINK";
    size_t start = text.find_first_not_of(L' ');
    if (start != std::wstring::npos && text.compare(start, 9, kKeyword) == 0)
        return ParseLinkInstruction(text, start, url) ? url : std::wstring();

    // The visible name of a link: the instruction is the hidden text right in front of it.
    if (HiddenState(range.cpMin, range.cpMin + 1) == tomFalse) {
        LONG from = std::max<LONG>(0, range.cpMin - 2200);
        std::wstring before = TextRange(from, range.cpMin);
        for (size_t k = before.find(kKeyword); k != std::wstring::npos; k = before.find(kKeyword, k + 1)) {
            if (HiddenState(from + (LONG)k, range.cpMin) != tomTrue) continue;
            return ParseLinkInstruction(before, k, url) ? url : std::wstring();
        }
    }
    return text;
}

void Editor_ReplaceSel(const wchar_t* text, bool canUndo)
{
    SendMessageW(g.hEdit, EM_REPLACESEL, canUndo ? TRUE : FALSE, (LPARAM)text);
}

void Editor_Clear()
{
    SETTEXTEX st = { ST_DEFAULT, 1200 };
    SendMessageW(g.hEdit, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)L"");
    SendMessageW(g.hEdit, EM_EMPTYUNDOBUFFER, 0, 0);
}

// ---- character formatting --------------------------------------------------

void Editor_GetCharFormat(CHARFORMAT2W& cf)
{
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    SendMessageW(g.hEdit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

void Editor_SetCharFormat(CHARFORMAT2W& cf)
{
    cf.cbSize = sizeof(cf);
    SendMessageW(g.hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

void Editor_ToggleEffect(DWORD mask, DWORD effect)
{
    CHARFORMAT2W cur; Editor_GetCharFormat(cur);
    bool on = (cur.dwMask & mask) && (cur.dwEffects & effect);
    CHARFORMAT2W cf = {};
    cf.dwMask = mask;
    cf.dwEffects = on ? 0 : effect;
    if (mask == CFM_UNDERLINE) {
        cf.dwMask |= CFM_UNDERLINETYPE;
        cf.bUnderlineType = on ? CFU_UNDERLINENONE : CFU_UNDERLINE;
    }
    Editor_SetCharFormat(cf);
}

void Editor_ToggleScript(bool superscript)
{
    CHARFORMAT2W cur; Editor_GetCharFormat(cur);
    DWORD want = superscript ? CFE_SUPERSCRIPT : CFE_SUBSCRIPT;
    bool on = (cur.dwMask & CFM_SUBSCRIPT) && (cur.dwEffects & want);
    CHARFORMAT2W cf = {};
    cf.dwMask = CFM_SUBSCRIPT;       // same bits as CFM_SUPERSCRIPT
    cf.dwEffects = on ? 0 : want;
    Editor_SetCharFormat(cf);
}

void Editor_SetFontFace(const wchar_t* face)
{
    CHARFORMAT2W cf = {};
    cf.dwMask = CFM_FACE | CFM_CHARSET;
    cf.bCharSet = DEFAULT_CHARSET;
    lstrcpynW(cf.szFaceName, face, LF_FACESIZE);
    Editor_SetCharFormat(cf);
}

void Editor_SetFontSize(int twips)
{
    CHARFORMAT2W cf = {};
    cf.dwMask = CFM_SIZE;
    cf.yHeight = std::clamp(twips, 20, 1638 * 20);
    Editor_SetCharFormat(cf);
}

void Editor_ChangeFontSize(int direction)
{
    // The control steps every run of a mixed selection through the usual size list.
    if (SendMessageW(g.hEdit, EM_SETFONTSIZE, (WPARAM)(direction > 0 ? 1 : -1), 0)) return;

    static const int kSizes[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 };
    CHARFORMAT2W cur; Editor_GetCharFormat(cur);
    if (!(cur.dwMask & CFM_SIZE)) return;
    int pt = (cur.yHeight + 10) / 20, next = pt;
    if (direction > 0) {
        next = pt >= 72 ? std::min(1638, (pt / 10 + 1) * 10) : pt + 1;
        for (int s : kSizes) if (s > pt) { next = s; break; }
        if (pt >= 72) next = std::min(1638, (pt / 10 + 1) * 10);
    } else {
        next = pt > 80 ? ((pt - 1) / 10) * 10 : std::max(1, pt - 1);
        for (int i = (int)ARRAYSIZE(kSizes) - 1; i >= 0; --i) if (kSizes[i] < pt) { next = kSizes[i]; break; }
        if (pt > 80) next = ((pt - 1) / 10) * 10;
    }
    Editor_SetFontSize(next * 20);
}

void Editor_SetTextColor(COLORREF c, bool automatic)
{
    CHARFORMAT2W cf = {};
    cf.dwMask = CFM_COLOR;
    if (automatic) cf.dwEffects = CFE_AUTOCOLOR; else cf.crTextColor = c;
    Editor_SetCharFormat(cf);
}

void Editor_SetHighlight(COLORREF c, bool none)
{
    CHARFORMAT2W cf = {};
    cf.dwMask = CFM_BACKCOLOR;
    if (none) cf.dwEffects = CFE_AUTOBACKCOLOR; else cf.crBackColor = c;
    Editor_SetCharFormat(cf);
}

std::wstring Editor_DefaultFontFace(bool plainText)
{
    if (plainText) {
        if (FontExists(L"Consolas")) return L"Consolas";
        return L"Courier New";
    }
    if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN && FontExists(L"Malgun Gothic")) return L"맑은 고딕";
    if (FontExists(L"Calibri")) return L"Calibri";
    if (FontExists(L"Segoe UI")) return L"Segoe UI";
    return L"Arial";
}

static void FillDefaultCharFormat(CHARFORMAT2W& cf, bool plainText)
{
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT |
                CFM_SUBSCRIPT | CFM_COLOR | CFM_BACKCOLOR | CFM_UNDERLINETYPE;
    cf.dwEffects = CFE_AUTOCOLOR | CFE_AUTOBACKCOLOR;
    cf.yHeight = 220;   // 11 pt
    cf.bCharSet = DEFAULT_CHARSET;
    cf.bUnderlineType = CFU_UNDERLINENONE;
    lstrcpynW(cf.szFaceName, Editor_DefaultFontFace(plainText).c_str(), LF_FACESIZE);
}

void Editor_SetDefaultFormat(bool plainText)
{
    CHARFORMAT2W cf;
    FillDefaultCharFormat(cf, plainText);
    SendMessageW(g.hEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(g.hEdit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);

    PARAFORMAT2 pf = {};
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_ALIGNMENT | PFM_STARTINDENT | PFM_OFFSET | PFM_RIGHTINDENT | PFM_NUMBERING |
                PFM_LINESPACING | PFM_SPACEAFTER | PFM_SPACEBEFORE | PFM_TABSTOPS;
    pf.wAlignment = PFA_LEFT;
    if (plainText) {
        pf.bLineSpacingRule = 0;
    } else {
        pf.bLineSpacingRule = 5;        // multiple of single spacing, in 1/20 line
        pf.dyLineSpacing = 23;          // 1.15
        pf.dySpaceAfter = 200;          // 10 pt
    }
    SendMessageW(g.hEdit, EM_SETSEL, 0, -1);
    SendMessageW(g.hEdit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    SendMessageW(g.hEdit, EM_SETSEL, 0, 0);
}

void Editor_ClearFormatting()
{
    CHARFORMAT2W cf;
    FillDefaultCharFormat(cf, IsTextFormat(g.doc.format));
    Editor_SetCharFormat(cf);
}

// ---- paragraph formatting --------------------------------------------------

void Editor_GetParaFormat(PARAFORMAT2& pf)
{
    ZeroMemory(&pf, sizeof(pf));
    pf.cbSize = sizeof(pf);
    SendMessageW(g.hEdit, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
}

void Editor_SetParaFormat(PARAFORMAT2& pf)
{
    pf.cbSize = sizeof(pf);
    SendMessageW(g.hEdit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

void Editor_SetAlignment(WORD alignment)
{
    PARAFORMAT2 pf = {};
    pf.dwMask = PFM_ALIGNMENT;
    pf.wAlignment = alignment;
    Editor_SetParaFormat(pf);
}

void Editor_SetNumbering(WORD numbering)
{
    PARAFORMAT2 cur; Editor_GetParaFormat(cur);
    bool wasList = (cur.dwMask & PFM_NUMBERING) && cur.wNumbering != 0;

    PARAFORMAT2 pf = {};
    pf.dwMask = PFM_NUMBERING | PFM_NUMBERINGSTYLE | PFM_NUMBERINGSTART | PFM_NUMBERINGTAB;
    pf.wNumbering = numbering;
    pf.wNumberingStart = 1;
    pf.wNumberingStyle = (numbering > PFN_BULLET) ? PFNS_PERIOD : 0;
    pf.wNumberingTab = numbering ? 360 : 0;
    if (numbering && !wasList) {
        // Hanging indent: the marker sits 0.25" to the left of the text.
        LONG body = (cur.dwMask & PFM_STARTINDENT) && (cur.dwMask & PFM_OFFSET) ? cur.dxStartIndent + cur.dxOffset : 0;
        pf.dwMask |= PFM_STARTINDENT | PFM_OFFSET;
        pf.dxStartIndent = std::max<LONG>(0, body) + 360;
        pf.dxOffset = 360;
    } else if (!numbering && wasList) {
        LONG body = (cur.dwMask & PFM_STARTINDENT) && (cur.dwMask & PFM_OFFSET) ? cur.dxStartIndent + cur.dxOffset : 720;
        pf.dwMask |= PFM_STARTINDENT | PFM_OFFSET;
        pf.dxStartIndent = std::max<LONG>(0, body - 720);
        pf.dxOffset = 0;
    }
    Editor_SetParaFormat(pf);
}

void Editor_ChangeIndent(int direction)
{
    const LONG step = 720;   // half an inch
    PARAFORMAT2 cur; Editor_GetParaFormat(cur);
    PARAFORMAT2 pf = {};
    if (cur.dwMask & PFM_STARTINDENT) {
        // Never let the hanging part of a list run off the left edge.
        LONG minStart = (cur.dwMask & PFM_OFFSET) && cur.dxOffset < 0 ? -cur.dxOffset : 0;
        pf.dwMask = PFM_STARTINDENT;
        pf.dxStartIndent = std::max(minStart, cur.dxStartIndent + direction * step);
        if (pf.dxStartIndent == cur.dxStartIndent) return;
    } else {
        pf.dwMask = PFM_OFFSETINDENT;      // relative change for a mixed selection
        pf.dxStartIndent = direction * step;
    }
    Editor_SetParaFormat(pf);
}

void Editor_SetLineSpacing(int percent)
{
    PARAFORMAT2 pf = {};
    pf.dwMask = PFM_LINESPACING;
    pf.bLineSpacingRule = 5;
    pf.dyLineSpacing = MulDiv(percent, 20, 100);
    Editor_SetParaFormat(pf);
}

int Editor_GetLineSpacingPercent(const PARAFORMAT2& pf)
{
    if (!(pf.dwMask & PFM_LINESPACING)) return 0;
    switch (pf.bLineSpacingRule) {
    case 0: return 100;
    case 1: return 150;
    case 2: return 200;
    case 5: return pf.dyLineSpacing * 5;
    }
    return 0;
}

void Editor_ToggleSpaceAfter()
{
    PARAFORMAT2 cur; Editor_GetParaFormat(cur);
    bool on = (cur.dwMask & PFM_SPACEAFTER) && cur.dySpaceAfter > 0;
    PARAFORMAT2 pf = {};
    pf.dwMask = PFM_SPACEAFTER;
    pf.dySpaceAfter = on ? 0 : 200;
    Editor_SetParaFormat(pf);
}

// ---- geometry ----------------------------------------------------------------

int Editor_PageWidthTwips()
{
    int w = g.cfg.paper.cx - g.cfg.margins.left - g.cfg.margins.right;
    return std::max(1440, w);
}

double Editor_PixelsPerTwip()
{
    // layout DPI x effective zoom (user zoom x monitorDpi / layoutDpi)
    return g_monitorDpi / 1440.0 * (g_userZoom / 100.0);
}

int Editor_OriginX()
{
    RECT rc = {};
    SendMessageW(g.hEdit, EM_GETRECT, 0, (LPARAM)&rc);
    POINT sp = {};
    SendMessageW(g.hEdit, EM_GETSCROLLPOS, 0, (LPARAM)&sp);
    return rc.left + g.textOriginDelta - sp.x;
}

// ---- searching ---------------------------------------------------------------

// EM_FINDTEXTEX also looks into hidden text (the address part of a link, for example).  A match the
// user cannot see would select "nothing", so such matches are skipped.
static bool FindVisible(const std::wstring& what, WPARAM flags, LONG from, LONG to, CHARRANGE& found)
{
    bool down = (flags & FR_DOWN) != 0;
    FINDTEXTEXW ft = {};
    ft.lpstrText = what.c_str();
    ft.chrg.cpMin = from;
    ft.chrg.cpMax = to;
    for (;;) {
        if (SendMessageW(g.hEdit, EM_FINDTEXTEXW, flags, (LPARAM)&ft) < 0) return false;
        if (ft.chrgText.cpMax <= ft.chrgText.cpMin) return false;
        if (!Editor_IsRangeHidden(ft.chrgText.cpMin, ft.chrgText.cpMax)) { found = ft.chrgText; return true; }
        LONG next = down ? ft.chrgText.cpMin + 1 : ft.chrgText.cpMax - 1;
        if (down ? next <= ft.chrg.cpMin : next >= ft.chrg.cpMin) return false;    // no progress
        ft.chrg.cpMin = next;
    }
}

bool Editor_Find(const std::wstring& what, bool matchCase, bool wholeWord, bool down, bool wrapAround)
{
    if (what.empty()) return false;
    CHARRANGE sel; Editor_GetSel(sel);
    WPARAM flags = (matchCase ? FR_MATCHCASE : 0) | (wholeWord ? FR_WHOLEWORD : 0) | (down ? FR_DOWN : 0);
    CHARRANGE found = {};
    bool ok = down ? FindVisible(what, flags, sel.cpMax, -1, found) : FindVisible(what, flags, sel.cpMin, 0, found);
    if (!ok && wrapAround)
        ok = down ? FindVisible(what, flags, 0, -1, found) : FindVisible(what, flags, Editor_GetTextLength(), 0, found);
    if (!ok) return false;
    SendMessageW(g.hEdit, EM_EXSETSEL, 0, (LPARAM)&found);
    SendMessageW(g.hEdit, EM_SCROLLCARET, 0, 0);
    return true;
}

static bool SelectionMatches(const std::wstring& what, bool matchCase)
{
    std::wstring sel = Editor_GetText(true);
    if (sel.size() != what.size()) return false;
    return CompareStringOrdinal(sel.c_str(), (int)sel.size(), what.c_str(), (int)what.size(), matchCase ? FALSE : TRUE) == CSTR_EQUAL;
}

bool Editor_ReplaceCurrent(const std::wstring& what, const std::wstring& with, bool matchCase, bool)
{
    if (what.empty() || !SelectionMatches(what, matchCase)) return false;
    Editor_ReplaceSel(with.c_str(), true);
    return true;
}

int Editor_ReplaceAll(const std::wstring& what, const std::wstring& with, bool matchCase, bool wholeWord)
{
    if (what.empty()) return 0;
    WPARAM flags = FR_DOWN | (matchCase ? FR_MATCHCASE : 0) | (wholeWord ? FR_WHOLEWORD : 0);
    CHARRANGE saved; Editor_GetSel(saved);
    int count = 0;
    LONG pos = 0;
    SendMessageW(g.hEdit, WM_SETREDRAW, FALSE, 0);
    Editor_BeginUndoGroup();
    for (;;) {
        CHARRANGE found = {};
        if (!FindVisible(what, flags, pos, -1, found)) break;
        SendMessageW(g.hEdit, EM_EXSETSEL, 0, (LPARAM)&found);
        LONG before = Editor_GetTextLength();
        Editor_ReplaceSel(with.c_str(), true);
        LONG after = Editor_GetTextLength();
        pos = found.cpMax + (after - before);
        if (pos < found.cpMin) pos = found.cpMin;
        // Protected text cannot be replaced: the match then simply stays selected, and is not counted.
        CHARRANGE now; Editor_GetSel(now);
        if (now.cpMin == now.cpMax) ++count;
    }
    Editor_EndUndoGroup();
    LONG len = Editor_GetTextLength();
    Editor_SetSel(std::min(saved.cpMin, len), std::min(saved.cpMin, len));
    SendMessageW(g.hEdit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.hEdit, nullptr, TRUE);
    return count;
}

// ---- streaming ---------------------------------------------------------------

bool Editor_StreamIn(const void* data, size_t size, UINT format)
{
    StreamInCtx ctx = { (const BYTE*)data, size };
    EDITSTREAM es = {};
    es.dwCookie = (DWORD_PTR)&ctx;
    es.pfnCallback = StreamInCb;
    SendMessageW(g.hEdit, EM_STREAMIN, format, (LPARAM)&es);
    return es.dwError == 0;
}

bool Editor_StreamOut(std::string& out, UINT format)
{
    out.clear();
    StreamOutCtx ctx = { &out };
    EDITSTREAM es = {};
    es.dwCookie = (DWORD_PTR)&ctx;
    es.pfnCallback = StreamOutCb;
    SendMessageW(g.hEdit, EM_STREAMOUT, format, (LPARAM)&es);
    return es.dwError == 0;
}

// ---- pictures and objects -----------------------------------------------------

bool Editor_InsertPictureData(const BYTE* data, size_t size)
{
    ImageInfo info;
    std::vector<BYTE> converted;
    if (!Image_Normalize(data, size, converted, info)) return false;
    const BYTE* bytes = converted.empty() ? data : converted.data();
    size_t count = converted.empty() ? size : converted.size();

    // Natural size in twips, limited to the printable width of the page.
    int wTw = MulDiv(info.width, 1440, info.dpiX);
    int hTw = MulDiv(info.height, 1440, info.dpiY);
    int maxW = Editor_PageWidthTwips();
    if (wTw > maxW) { hTw = MulDiv(hTw, maxW, wTw); wTw = maxW; }
    wTw = std::max(15, wTw);
    hTw = std::max(15, hTw);

    char head[200];
    wsprintfA(head, "{\\rtf1\\ansi{\\pict\\%s\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d\r\n",
              info.isJpeg ? "jpegblip" : "pngblip", info.width, info.height, wTw, hTw);
    std::string rtf = head;
    AppendHex(rtf, bytes, count);
    rtf += "}}";
    if (InsertPictureRtf(rtf)) return true;

    // Older rich edit versions do not understand PNG/JPEG blips: fall back to a device independent bitmap.
    std::vector<BYTE> dib;
    if (!Image_ToDib(data, size, dib)) return false;
    wsprintfA(head, "{\\rtf1\\ansi{\\pict\\dibitmap0\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d\r\n",
              info.width, info.height, wTw, hTw);
    rtf = head;
    AppendHex(rtf, dib.data(), dib.size());
    rtf += "}}";
    return InsertPictureRtf(rtf);
}

bool Editor_InsertPictureFile(const std::wstring& path)
{
    std::vector<BYTE> data;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < 256LL * 1024 * 1024;
    if (ok) {
        data.resize((size_t)sz.QuadPart);
        DWORD read = 0;
        ok = ReadFile(h, data.data(), (DWORD)data.size(), &read, nullptr) && read == data.size();
    }
    CloseHandle(h);
    return ok && Editor_InsertPictureData(data.data(), data.size());
}

bool Editor_PasteClipboardImage()
{
    static const UINT cfRtf = RegisterClipboardFormatW(L"Rich Text Format");
    static const UINT cfPng = RegisterClipboardFormatW(L"PNG");
    static const UINT cfEmbed = RegisterClipboardFormatW(L"Embed Source");
    // Only take over for a plain image; text, RTF and OLE objects use the control's own paste.
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT) ||
        IsClipboardFormatAvailable(cfRtf) || IsClipboardFormatAvailable(cfEmbed) || IsClipboardFormatAvailable(CF_HDROP))
        return false;
    if (!IsClipboardFormatAvailable(cfPng) && !IsClipboardFormatAvailable(CF_BITMAP) && !IsClipboardFormatAvailable(CF_DIB))
        return false;
    if (!OpenClipboard(g.hMain)) return false;

    std::vector<BYTE> bytes;
    if (HANDLE h = GetClipboardData(cfPng)) {
        SIZE_T size = GlobalSize(h);
        if (void* p = GlobalLock(h)) {
            bytes.assign((const BYTE*)p, (const BYTE*)p + size);
            GlobalUnlock(h);
        }
    }
    if (bytes.empty()) {
        if (HBITMAP hb = (HBITMAP)GetClipboardData(CF_BITMAP))
            Image_FromHBitmap(hb, bytes);
    }
    CloseClipboard();
    return !bytes.empty() && Editor_InsertPictureData(bytes.data(), bytes.size());
}

int Editor_GetObjectCount()
{
    int count = 0;
    if (IRichEditOle* ole = GetOle()) {
        count = (int)ole->GetObjectCount();
        ole->Release();
    }
    return count;
}

bool Editor_IsPictureSelected()
{
    CHARRANGE cr; Editor_GetSel(cr);
    if (cr.cpMax - cr.cpMin != 1) return false;
    return (SendMessageW(g.hEdit, EM_SELECTIONTYPE, 0, 0) & SEL_OBJECT) != 0;
}

bool Editor_GetSelectedPictureScale(int* widthTwips, int* heightTwips)
{
    if (!Editor_IsPictureSelected()) return false;
    std::string rtf;
    if (!Editor_StreamOut(rtf, SF_RTF | SFF_SELECTION)) return false;
    long w = ReadRtfValue(rtf, "\\picwgoal", 0), h = ReadRtfValue(rtf, "\\pichgoal", 0);
    if (w <= 0 || h <= 0) { w = ReadRtfValue(rtf, "\\objw", 0); h = ReadRtfValue(rtf, "\\objh", 0); }
    if (w <= 0 || h <= 0) return false;
    w = MulDiv((int)w, (int)ReadRtfValue(rtf, "\\picscalex", 100), 100);
    h = MulDiv((int)h, (int)ReadRtfValue(rtf, "\\picscaley", 100), 100);
    if (widthTwips) *widthTwips = (int)w;
    if (heightTwips) *heightTwips = (int)h;
    return true;
}

bool Editor_ScaleSelectedPicture(int percentX, int percentY)
{
    if (!Editor_IsPictureSelected()) return false;
    percentX = std::clamp(percentX, 1, 1000);
    percentY = std::clamp(percentY, 1, 1000);
    std::string rtf;
    if (!Editor_StreamOut(rtf, SF_RTF | SFF_SELECTION)) return false;
    int changed = ScaleRtfValue(rtf, "\\picwgoal", percentX) + ScaleRtfValue(rtf, "\\pichgoal", percentY) +
                  ScaleRtfValue(rtf, "\\objw", percentX) + ScaleRtfValue(rtf, "\\objh", percentY);
    if (!changed) return false;
    CHARRANGE cr; Editor_GetSel(cr);
    if (!Editor_StreamIn(rtf.data(), rtf.size(), SF_RTF | SFF_SELECTION)) return false;
    Editor_SetSel(cr.cpMin, cr.cpMin + 1);
    return true;
}

bool Editor_InsertObjectDialog(HWND owner)
{
    wchar_t file[MAX_PATH] = L"";
    OLEUIINSERTOBJECTW io = {};
    io.cbStruct = sizeof(io);
    io.dwFlags = IOF_SELECTCREATENEW | IOF_DISABLELINK | IOF_DISABLEDISPLAYASICON;
    io.hWndOwner = owner;
    io.lpszFile = file;
    io.cchFile = ARRAYSIZE(file);
    UINT rc = OleUIInsertObjectW(&io);
    if (rc == OLEUI_CANCEL) return true;
    if (rc != OLEUI_OK) return false;

    bool ok = false;
    IRichEditOle* ole = GetOle();
    IOleClientSite* site = nullptr;
    IStorage* stg = nullptr;
    IOleObject* obj = nullptr;
    if (ole && SUCCEEDED(ole->GetClientSite(&site)) && g_callback && SUCCEEDED(g_callback->GetNewStorage(&stg))) {
        HRESULT hr;
        bool createNew = (io.dwFlags & IOF_SELECTCREATENEW) != 0;
        if (createNew)
            hr = OleCreate(io.clsid, IID_IOleObject, OLERENDER_DRAW, nullptr, site, stg, (void**)&obj);
        else
            hr = OleCreateFromFile(CLSID_NULL, file, IID_IOleObject, OLERENDER_DRAW, nullptr, site, stg, (void**)&obj);
        if (SUCCEEDED(hr) && obj) {
            OleSetContainedObject(obj, TRUE);
            REOBJECT reo = {};
            reo.cbStruct = sizeof(reo);
            reo.cp = REO_CP_SELECTION;
            obj->GetUserClassID(&reo.clsid);
            reo.poleobj = obj;
            reo.pstg = stg;
            reo.polesite = site;
            reo.dvaspect = DVASPECT_CONTENT;
            reo.dwFlags = REO_RESIZABLE | (createNew ? REO_BLANK : 0);
            if (SUCCEEDED(ole->InsertObject(&reo))) {
                ok = true;
                if (createNew) obj->DoVerb(OLEIVERB_SHOW, nullptr, site, 0, g.hEdit, nullptr);
            }
        }
    }
    if (obj) obj->Release();
    if (stg) stg->Release();
    if (site) site->Release();
    if (ole) ole->Release();
    return ok;
}

void Editor_BeginUndoGroup()
{
    if (ITextDocument* doc = GetTextDocument()) {
        doc->BeginEditCollection();
        doc->Release();
    }
}

void Editor_EndUndoGroup()
{
    if (ITextDocument* doc = GetTextDocument()) {
        doc->EndEditCollection();
        doc->Release();
    }
}
