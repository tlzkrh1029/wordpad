// Localised UI strings (Korean / English).
#pragma once
#include <string>

enum StrId {
#define X(id, ko, en) id,
#include "strings.def"
#undef X
    S_COUNT
};

// Returns the string in the current UI language.
const wchar_t* S(int id);

// mode: 0 = follow the Windows display language, 1 = Korean, 2 = English.
void Strings_SetLanguage(int mode);
bool Strings_IsKorean();

// "파일(&F)" -> "파일",  "&File" -> "File",  "열기(&O)..." -> "열기..."
std::wstring StripMnemonic(const wchar_t* s);
// Returns the upper-case mnemonic character, or 0 when there is none.
wchar_t GetMnemonic(const wchar_t* s);
