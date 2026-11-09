// Status bar with document statistics and the zoom slider.
#pragma once
#include "app.h"

bool StatusBar_Register();
HWND StatusBar_Create(HWND parent);
int  StatusBar_GetHeight();
void StatusBar_SetParts(const std::vector<std::wstring>& parts);
std::wstring StatusBar_GetText();        // the parts joined with " | "
void StatusBar_SetZoom(int percent);
void StatusBar_Rebuild();     // theme / language / DPI changed
