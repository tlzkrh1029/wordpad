// The rich edit control and all formatting operations on it.
#pragma once
#include "app.h"

bool Editor_Create(HWND parent);
void Editor_Destroy();
void Editor_UpdateInsets();              // call after the control was resized
void Editor_ApplyWrap();
void Editor_ApplyTheme();
void Editor_CalibrateOrigin();
void Editor_OnDpiChanged();              // also call once after the window was created

int  Editor_GetZoom();                   // percent, as chosen by the user
void Editor_SetZoom(int percent);
bool Editor_SuspendZoom();               // temporarily lays the text out at 100%; true if it changed anything
void Editor_ResumeZoom();
int  Editor_LayoutDpi();                 // DPI the control uses to convert twips to pixels
LONG Editor_GetTextLength();
void Editor_GetSel(CHARRANGE& cr);
void Editor_SetSel(LONG cpMin, LONG cpMax);
bool Editor_HasSelection();
bool Editor_IsModified();
void Editor_SetModified(bool modified);
std::wstring Editor_GetText(bool selectionOnly);
void Editor_ReplaceSel(const wchar_t* text, bool canUndo = true);
void Editor_Clear();                     // empties the document and the undo stack

// ---- character formatting --------------------------------------------------
void Editor_GetCharFormat(CHARFORMAT2W& cf);
void Editor_SetCharFormat(CHARFORMAT2W& cf);
void Editor_ToggleEffect(DWORD mask, DWORD effect);
void Editor_ToggleScript(bool superscript);
void Editor_SetFontFace(const wchar_t* face);
void Editor_SetFontSize(int twips);
void Editor_ChangeFontSize(int direction);     // +1 grow, -1 shrink
void Editor_SetTextColor(COLORREF c, bool automatic);
void Editor_SetHighlight(COLORREF c, bool none);
void Editor_ClearFormatting();
void Editor_SetDefaultFormat(bool plainText);
std::wstring Editor_DefaultFontFace(bool plainText);

// ---- paragraph formatting --------------------------------------------------
void Editor_GetParaFormat(PARAFORMAT2& pf);
void Editor_SetParaFormat(PARAFORMAT2& pf);
void Editor_SetAlignment(WORD alignment);
void Editor_SetNumbering(WORD numbering);       // 0 = none, PFN_BULLET, PFN_ARABIC, ...
void Editor_ChangeIndent(int direction);        // +1 / -1 (half an inch)
void Editor_SetLineSpacing(int percent);        // 100, 115, 150, 200
int  Editor_GetLineSpacingPercent(const PARAFORMAT2& pf);   // 0 when unknown / mixed
void Editor_ToggleSpaceAfter();

// ---- geometry (used by the ruler) -----------------------------------------
int    Editor_OriginX();           // client x of horizontal position 0 at the current scroll offset
double Editor_PixelsPerTwip();     // includes the zoom factor
int    Editor_PageWidthTwips();    // paper width minus the left and right margins

// ---- searching -------------------------------------------------------------
bool Editor_Find(const std::wstring& what, bool matchCase, bool wholeWord, bool down, bool wrapAround);
bool Editor_ReplaceCurrent(const std::wstring& what, const std::wstring& with, bool matchCase, bool wholeWord);
int  Editor_ReplaceAll(const std::wstring& what, const std::wstring& with, bool matchCase, bool wholeWord);

// ---- streaming -------------------------------------------------------------
bool Editor_StreamIn(const void* data, size_t size, UINT format);
bool Editor_StreamOut(std::string& out, UINT format);

// ---- pictures and objects ---------------------------------------------------
bool Editor_InsertPictureFile(const std::wstring& path);
bool Editor_InsertPictureData(const BYTE* data, size_t size);   // PNG / JPEG / BMP / GIF / TIFF bytes
bool Editor_PasteClipboardImage();    // returns false when the clipboard holds no (plain) image
bool Editor_InsertObjectDialog(HWND owner);
bool Editor_IsPictureSelected();
bool Editor_GetSelectedPictureScale(int* widthTwips, int* heightTwips);
bool Editor_ScaleSelectedPicture(int percentX, int percentY);
int  Editor_GetObjectCount();

// Groups several edits into a single undo step (no-op where unsupported).
void Editor_BeginUndoGroup();
void Editor_EndUndoGroup();
