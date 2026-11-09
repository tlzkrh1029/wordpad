// The horizontal ruler: indent markers, tab stops and the printable width.
#pragma once
#include "app.h"

bool Ruler_Register();
HWND Ruler_Create(HWND parent);
int  Ruler_GetHeight();
void Ruler_Update();          // re-reads the paragraph format of the selection and repaints

// Measurement helpers shared with the dialogs (values in twips, text in the current unit).
std::wstring Measure_Format(int twips);
bool Measure_Parse(const wchar_t* text, int* twips);
double Measure_SnapStep();    // twips between two snap positions for the current unit
