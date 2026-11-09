// The menu row and the ribbon-like command bar at the top of the window.
#pragma once
#include "app.h"

bool CmdBar_Register();
HWND CmdBar_Create(HWND parent);
int  CmdBar_GetHeight();

// State of individual commands. Call CmdBar_Commit() afterwards to repaint what changed.
void CmdBar_SetEnabled(int id, bool enabled);
void CmdBar_SetChecked(int id, bool checked);
void CmdBar_SetSwatch(int id, COLORREF color);
void CmdBar_SetFontInfo(const wchar_t* face, int sizeTwips);   // nullptr / 0 = mixed selection
void CmdBar_Commit();

bool CmdBar_GetItemScreenRect(int id, RECT* rc);
std::wstring CmdBar_GetFontName();        // what the user picked or typed in the font box
int  CmdBar_GetFontSizeTwips();           // -1 when the text is not a valid size
void CmdBar_RevertFontBoxes();

void CmdBar_Rebuild();                    // language, theme or DPI changed
void CmdBar_RefreshFonts();               // installed fonts changed

// Keyboard access to the menu row.
bool CmdBar_OpenMenuByMnemonic(wchar_t ch);
void CmdBar_EnterMenuMode();
bool CmdBar_IsChildFocused();             // focus is inside the font name / size boxes

// Forwarded from the main window while a menu is being tracked.
void CmdBar_OnMenuSelect(WPARAM wParam, LPARAM lParam);
void CmdBar_OnInitMenuPopup(HMENU menu);
void CmdBar_OnUninitMenuPopup(HMENU menu);

// Shows a popup menu aligned to the bottom-left of a command bar item (or at the cursor if unknown).
void CmdBar_TrackItemMenu(int itemId, HMENU menu);
