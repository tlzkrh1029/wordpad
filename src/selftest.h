// Automated self test ("RichPad.exe /selftest log.txt").
#pragma once
#include "app.h"

// Runs the checks against the live main window and writes a UTF-8 report. Returns the process exit code.
int SelfTest_Run(const wchar_t* logPath);
// Message boxes are suppressed while the self test runs; their text is recorded instead.
void SelfTest_Note(const wchar_t* text);
