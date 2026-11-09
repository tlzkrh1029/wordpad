// Loading and saving documents.
#pragma once
#include "app.h"

enum LoadResult { LOAD_OK, LOAD_FAILED, LOAD_UNSUPPORTED, LOAD_OLDDOC };

void File_Init();
bool File_ReadAll(const std::wstring& path, std::vector<BYTE>& data, DWORD* err);
bool File_WriteAll(const std::wstring& path, const void* data, size_t size, DWORD* err);

// Loads a file into the editor. On success *format and *utf8Bom describe what was found.
LoadResult File_Load(const std::wstring& path, DocFormat* format, bool* utf8Bom, DWORD* err);
// Loads a document from memory (used by the self test).
LoadResult File_LoadData(const std::vector<BYTE>& data, const std::wstring& nameHint, DocFormat* format, bool* utf8Bom);
bool File_Save(const std::wstring& path, DocFormat format, bool utf8Bom, DWORD* err);
// Serialises the document into memory in the given format.
bool File_Serialize(DocFormat format, bool utf8Bom, std::vector<BYTE>& out);

DocFormat File_FormatFromPath(const std::wstring& path, DocFormat fallback);
bool File_IsImagePath(const std::wstring& path);
bool File_CanSaveAsAnsi();
std::wstring File_ErrorText(DWORD err);
