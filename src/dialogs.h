// Dialog boxes: paragraph, tabs, date and time, font, find / replace, about.
#pragma once
#include "app.h"

void Dlg_Paragraph(HWND owner);
void Dlg_Tabs(HWND owner);
void Dlg_DateTime(HWND owner);
void Dlg_About(HWND owner);
void Dlg_Shortcuts(HWND owner);
void Dlg_PictureSize(HWND owner);
void Dlg_Font(HWND owner);

void Dlg_ShowFind(bool replace);
void Dlg_FindNext(bool reverse);                       // F3 / Shift+F3
void Dlg_CloseFind();
UINT Dlg_FindMessageId();
bool Dlg_HandleFindMessage(UINT msg, LPARAM lParam);   // true when msg was the find/replace message
void Dlg_SetLastFind(const std::wstring& what, bool matchCase, bool wholeWord);

// Self test: creates each dialog once, calls visit(dialog, name) and destroys it. Returns how many were created.
int Dlg_SelfTest(void (*visit)(HWND dlg, const wchar_t* name));
