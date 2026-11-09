// Page setup, printing and print preview.
#pragma once
#include "app.h"

bool Print_Register();
void Print_PageSetup(HWND owner);
void Print_Print(HWND owner, bool showDialog);
void Print_Preview(HWND owner);

// Silent printing to a named printer (nullptr = default); outputFile redirects the job to a file.
bool Print_ToPrinter(const wchar_t* printerName, const wchar_t* outputFile, int* pages);
// Number of pages for the current page setup (uses the default printer when there is one).
int  Print_CountPages();
// Renders one page of the print layout into a PNG file (used by the self test).
bool Print_RenderPagePng(int pageIndex, int widthPx, const wchar_t* pngPath);

// Diagnostics for the self test: how the first page renders through both code paths.
struct PrintProbe {
    int pages = 0;
    bool usesPrinter = false;
    int dpi = 0;
    SIZE paper = {};
    int emfRecords = 0;
    long inkEmf = 0;        // non-white pixels when the page is rendered via a metafile
    long inkBitmap = 0;     // ... and when rendered directly into a bitmap
};
bool Print_Probe(PrintProbe& out);
