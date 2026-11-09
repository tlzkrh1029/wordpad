// Localised UI strings (Korean / English).
#include "app.h"

namespace {

struct Entry { const wchar_t* ko; const wchar_t* en; };

const Entry kStrings[S_COUNT] = {
#define X(id, ko, en) { ko, en },
#include "strings.def"
#undef X
};

bool g_korean = true;

} // namespace

const wchar_t* S(int id)
{
    if (id < 0 || id >= S_COUNT) return L"";
    return g_korean ? kStrings[id].ko : kStrings[id].en;
}

void Strings_SetLanguage(int mode)
{
    if (mode == LANG_KO) g_korean = true;
    else if (mode == LANG_EN) g_korean = false;
    else g_korean = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
}

bool Strings_IsKorean() { return g_korean; }

std::wstring StripMnemonic(const wchar_t* s)
{
    std::wstring out;
    for (const wchar_t* p = s; *p; ++p) {
        if (p[0] == L'(' && p[1] == L'&' && p[2] && p[3] == L')') { p += 3; continue; }  // "(&F)"
        if (p[0] == L'&') {
            if (p[1] == L'&') { out += L'&'; ++p; }
            continue;
        }
        out += *p;
    }
    return out;
}

wchar_t GetMnemonic(const wchar_t* s)
{
    for (const wchar_t* p = s; *p; ++p) {
        if (p[0] == L'&') {
            if (p[1] == L'&') { ++p; continue; }
            if (p[1]) return (wchar_t)(UINT_PTR)CharUpperW((LPWSTR)(UINT_PTR)p[1]);
        }
    }
    return 0;
}
