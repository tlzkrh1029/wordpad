// Persistent settings (HKCU\Software\RichPad) and per-user file associations.
#include "app.h"

namespace {

const wchar_t* const kExtensions[] = { L".rtf", L".docx", L".odt", L".txt" };

DWORD GetDword(HKEY key, const wchar_t* name, DWORD def)
{
    DWORD value = def, size = sizeof(value);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return def;
    return value;
}

void SetDword(HKEY key, const wchar_t* name, DWORD value)
{
    RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
}

std::wstring GetString(HKEY key, const wchar_t* name)
{
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

void SetString(HKEY root, const wchar_t* subkey, const wchar_t* name, const std::wstring& value)
{
    HKEY key;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

std::wstring ExePath()
{
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return std::wstring(path, n);
}

void ApplyLocaleDefaults()
{
    DWORD measure = 0, paper = 0;
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE | LOCALE_RETURN_NUMBER, (LPWSTR)&measure, sizeof(measure) / sizeof(wchar_t)))
        g.cfg.units = measure == 1 ? UNIT_INCH : UNIT_CM;
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IPAPERSIZE | LOCALE_RETURN_NUMBER, (LPWSTR)&paper, sizeof(paper) / sizeof(wchar_t))) {
        switch (paper) {
        case 1: g.cfg.paper = { 12240, 15840 }; break;   // Letter
        case 5: g.cfg.paper = { 12240, 20160 }; break;   // Legal
        case 8: g.cfg.paper = { 16838, 23811 }; break;   // A3
        default: g.cfg.paper = { 11906, 16838 }; break;  // A4
        }
    }
}

} // namespace

void Settings_Load()
{
    ApplyLocaleDefaults();
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, APP_REGKEY, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    Settings& c = g.cfg;
    c.wrapMode = std::clamp((int)GetDword(key, L"WrapMode", c.wrapMode), 0, 2);
    c.units = std::clamp((int)GetDword(key, L"Units", c.units), 0, 3);
    c.showRuler = GetDword(key, L"ShowRuler", 1) != 0;
    c.showStatus = GetDword(key, L"ShowStatusBar", 1) != 0;
    c.theme = std::clamp((int)GetDword(key, L"Theme", 0), 0, 2);
    c.lang = std::clamp((int)GetDword(key, L"Language", 0), 0, 2);
    c.paper.cx = std::clamp((int)GetDword(key, L"PaperWidth", c.paper.cx), 2880, 31680);
    c.paper.cy = std::clamp((int)GetDword(key, L"PaperHeight", c.paper.cy), 2880, 31680);
    c.margins.left = std::clamp((int)GetDword(key, L"MarginLeft", c.margins.left), 0, 14400);
    c.margins.top = std::clamp((int)GetDword(key, L"MarginTop", c.margins.top), 0, 14400);
    c.margins.right = std::clamp((int)GetDword(key, L"MarginRight", c.margins.right), 0, 14400);
    c.margins.bottom = std::clamp((int)GetDword(key, L"MarginBottom", c.margins.bottom), 0, 14400);
    c.lastTextColor = GetDword(key, L"LastTextColor", c.lastTextColor) & 0x00FFFFFF;
    c.lastHighlight = GetDword(key, L"LastHighlight", c.lastHighlight) & 0x00FFFFFF;
    if (GetDword(key, L"HaveWindowPos", 0)) {
        c.haveWindowPos = true;
        c.windowRect.left = (LONG)(int)GetDword(key, L"WindowLeft", 0);
        c.windowRect.top = (LONG)(int)GetDword(key, L"WindowTop", 0);
        c.windowRect.right = (LONG)(int)GetDword(key, L"WindowRight", 0);
        c.windowRect.bottom = (LONG)(int)GetDword(key, L"WindowBottom", 0);
        c.maximized = GetDword(key, L"Maximized", 0) != 0;
        if (c.windowRect.right - c.windowRect.left < 200 || c.windowRect.bottom - c.windowRect.top < 150) c.haveWindowPos = false;
    }
    DWORD size = sizeof(c.customColors);
    RegGetValueW(key, nullptr, L"CustomColors", RRF_RT_REG_BINARY, nullptr, c.customColors, &size);
    c.mru.clear();
    for (int i = 0; i < 10; ++i) {
        wchar_t name[16];
        wsprintfW(name, L"Recent%d", i);
        std::wstring path = GetString(key, name);
        if (!path.empty()) c.mru.push_back(path);
    }
    RegCloseKey(key);
}

void Settings_Save()
{
    if (g.selfTest) return;
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_REGKEY, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const Settings& c = g.cfg;
    SetDword(key, L"WrapMode", c.wrapMode);
    SetDword(key, L"Units", c.units);
    SetDword(key, L"ShowRuler", c.showRuler);
    SetDword(key, L"ShowStatusBar", c.showStatus);
    SetDword(key, L"Theme", c.theme);
    SetDword(key, L"Language", c.lang);
    SetDword(key, L"PaperWidth", c.paper.cx);
    SetDword(key, L"PaperHeight", c.paper.cy);
    SetDword(key, L"MarginLeft", c.margins.left);
    SetDword(key, L"MarginTop", c.margins.top);
    SetDword(key, L"MarginRight", c.margins.right);
    SetDword(key, L"MarginBottom", c.margins.bottom);
    SetDword(key, L"LastTextColor", c.lastTextColor);
    SetDword(key, L"LastHighlight", c.lastHighlight);
    SetDword(key, L"HaveWindowPos", c.haveWindowPos);
    SetDword(key, L"WindowLeft", (DWORD)c.windowRect.left);
    SetDword(key, L"WindowTop", (DWORD)c.windowRect.top);
    SetDword(key, L"WindowRight", (DWORD)c.windowRect.right);
    SetDword(key, L"WindowBottom", (DWORD)c.windowRect.bottom);
    SetDword(key, L"Maximized", c.maximized);
    RegSetValueExW(key, L"CustomColors", 0, REG_BINARY, (const BYTE*)c.customColors, sizeof(c.customColors));
    for (int i = 0; i < 10; ++i) {
        wchar_t name[16];
        wsprintfW(name, L"Recent%d", i);
        if (i < (int)c.mru.size())
            RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)c.mru[i].c_str(), (DWORD)((c.mru[i].size() + 1) * sizeof(wchar_t)));
        else
            RegDeleteValueW(key, name);
    }
    RegCloseKey(key);
}

void Mru_Remove(const std::wstring& path)
{
    auto& m = g.cfg.mru;
    m.erase(std::remove_if(m.begin(), m.end(), [&](const std::wstring& p) { return lstrcmpiW(p.c_str(), path.c_str()) == 0; }), m.end());
}

void Mru_Add(const std::wstring& path)
{
    if (path.empty() || g.selfTest) return;
    Mru_Remove(path);
    g.cfg.mru.insert(g.cfg.mru.begin(), path);
    if (g.cfg.mru.size() > 10) g.cfg.mru.resize(10);
    SHAddToRecentDocs(SHARD_PATHW, path.c_str());
}

// ---------------------------------------------------------------------------
// File associations. Everything lives under HKCU\Software\Classes, so no
// administrator rights are needed and other users are not affected. The app is
// only added to the "Open with" lists; existing defaults are never replaced.
// ---------------------------------------------------------------------------
bool Assoc_IsRegistered()
{
    HKEY key;
    std::wstring sub = std::wstring(L"Software\\Classes\\") + APP_PROGID + L"\\shell\\open\\command";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    std::wstring cmd = GetString(key, nullptr);
    RegCloseKey(key);
    return StrStrIW(cmd.c_str(), ExePath().c_str()) != nullptr;
}

bool Assoc_Register()
{
    std::wstring exe = ExePath();
    std::wstring command = L"\"" + exe + L"\" \"%1\"";
    std::wstring classes = L"Software\\Classes\\";
    std::wstring progId = classes + APP_PROGID;

    SetString(HKEY_CURRENT_USER, progId.c_str(), nullptr, Strings_IsKorean() ? L"RichPad 문서" : L"RichPad Document");
    SetString(HKEY_CURRENT_USER, (progId + L"\\DefaultIcon").c_str(), nullptr, L"\"" + exe + L"\",0");
    SetString(HKEY_CURRENT_USER, (progId + L"\\shell\\open\\command").c_str(), nullptr, command);

    std::wstring exeName = PathFindFileNameW(exe.c_str());
    std::wstring appKey = classes + L"Applications\\" + exeName;
    SetString(HKEY_CURRENT_USER, appKey.c_str(), L"FriendlyAppName", APP_NAME);
    SetString(HKEY_CURRENT_USER, (appKey + L"\\shell\\open\\command").c_str(), nullptr, command);
    for (const wchar_t* ext : kExtensions) {
        SetString(HKEY_CURRENT_USER, (appKey + L"\\SupportedTypes").c_str(), ext, L"");
        SetString(HKEY_CURRENT_USER, (classes + ext + L"\\OpenWithProgids").c_str(), APP_PROGID, L"");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return Assoc_IsRegistered();
}

bool Assoc_Unregister()
{
    std::wstring classes = L"Software\\Classes\\";
    std::wstring exe = ExePath();
    std::wstring exeName = PathFindFileNameW(exe.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (classes + APP_PROGID).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (classes + L"Applications\\" + exeName).c_str());
    for (const wchar_t* ext : kExtensions) {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, (classes + ext + L"\\OpenWithProgids").c_str(), 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegDeleteValueW(key, APP_PROGID);
            RegCloseKey(key);
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}
