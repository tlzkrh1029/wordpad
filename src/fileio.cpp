// Loading and saving documents.
#include "fileio.h"
#include "editor.h"
#include "imaging.h"
#include "convert/convert.h"
#include "convert/model.h"

namespace {

bool PictureHook(const conv::PictureRequest& req, conv::PictureResult& res)
{
    std::vector<BYTE> png;
    int w = req.widthPx, h = req.heightPx;
    bool ok = false;
    switch (req.kind) {
    case conv::PictureKind::Wmf: ok = Image_MetafileToPng(req.data, req.size, false, w, h, png); break;
    case conv::PictureKind::Emf: ok = Image_MetafileToPng(req.data, req.size, true, w, h, png); break;
    case conv::PictureKind::Dib: ok = Image_DibToPng(req.data, req.size, png, &w, &h); break;
    case conv::PictureKind::AnyImageFile: {
        ImageInfo info;
        std::vector<BYTE> converted;
        ok = Image_Normalize(req.data, req.size, converted, info) && !info.isJpeg;
        if (ok) {
            if (converted.empty()) png.assign(req.data, req.data + req.size);
            else png.swap(converted);
            w = info.width;
            h = info.height;
        }
        break;
    }
    }
    if (!ok) return false;
    res.png.assign(png.begin(), png.end());
    res.widthPx = w;
    res.heightPx = h;
    return true;
}

// Code page conversion for the RTF reader / writer (font names and text in legacy code pages).
bool DecodeHook(unsigned codePage, const std::string& bytes, std::string& utf8)
{
    utf8.clear();
    if (bytes.empty()) return true;
    UINT cp = codePage ? codePage : CP_ACP;
    int wlen = MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wide((size_t)wlen, L'\0');
    MultiByteToWideChar(cp, 0, bytes.data(), (int)bytes.size(), &wide[0], wlen);
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, nullptr, 0, nullptr, nullptr);
    utf8.resize((size_t)len);
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, &utf8[0], len, nullptr, nullptr);
    return true;
}

bool EncodeHook(unsigned codePage, const std::string& utf8, std::string& bytes)
{
    bytes.clear();
    if (utf8.empty()) return true;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wide((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &wide[0], wlen);
    BOOL usedDefault = FALSE;
    int len = WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS, wide.data(), wlen, nullptr, 0, nullptr, &usedDefault);
    if (len <= 0 || usedDefault) return false;
    bytes.resize((size_t)len);
    WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS, wide.data(), wlen, &bytes[0], len, nullptr, nullptr);
    return true;
}

bool IsValidUtf8(const BYTE* p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        BYTE c = p[i];
        size_t need;
        if (c < 0x80) { ++i; continue; }
        else if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c >= 0xE0 && c <= 0xEF) need = 2;
        else if (c >= 0xF0 && c <= 0xF4) need = 3;
        else return false;
        if (i + need >= n) return false;
        for (size_t k = 1; k <= need; ++k)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        if (c == 0xE0 && p[i + 1] < 0xA0) return false;            // overlong
        if (c == 0xED && p[i + 1] > 0x9F) return false;            // surrogates
        if (c == 0xF0 && p[i + 1] < 0x90) return false;            // overlong
        if (c == 0xF4 && p[i + 1] > 0x8F) return false;            // > U+10FFFF
        i += need + 1;
    }
    return true;
}

// Heuristic for UTF-16 text without a byte order mark.
bool LooksLikeUtf16(const BYTE* p, size_t n, bool* bigEndian)
{
    if (n < 4 || (n & 1)) return false;
    size_t sample = std::min<size_t>(n, 4096) & ~(size_t)1;
    size_t zeroEven = 0, zeroOdd = 0, pairs = sample / 2;
    for (size_t i = 0; i < sample; i += 2) {
        if (p[i] == 0) ++zeroEven;
        if (p[i + 1] == 0) ++zeroOdd;
    }
    if (zeroOdd * 10 >= pairs * 4 && zeroEven * 20 < pairs) { *bigEndian = false; return true; }
    if (zeroEven * 10 >= pairs * 4 && zeroOdd * 20 < pairs) { *bigEndian = true; return true; }
    return false;
}

std::wstring DecodeBytes(const BYTE* p, size_t n, UINT codepage)
{
    std::wstring out;
    if (n == 0) return out;
    int len = MultiByteToWideChar(codepage, 0, (const char*)p, (int)n, nullptr, 0);
    if (len <= 0) return out;
    out.resize((size_t)len);
    MultiByteToWideChar(codepage, 0, (const char*)p, (int)n, &out[0], len);
    return out;
}

bool LoadPlainText(const std::wstring& text)
{
    Editor_Clear();
    Editor_SetDefaultFormat(true);
    if (text.empty()) return true;
    return Editor_StreamIn(text.data(), text.size() * sizeof(wchar_t), SF_TEXT | SF_UNICODE);
}

bool LoadRtf(const void* data, size_t size)
{
    Editor_Clear();
    Editor_SetDefaultFormat(false);
    return Editor_StreamIn(data, size, SF_RTF);
}

bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix)
{
    size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool WriteDirect(const std::wstring& path, const void* data, size_t size, DWORD* err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = GetLastError(); return false; }
    const BYTE* p = (const BYTE*)data;
    size_t left = size;
    bool ok = true;
    while (left > 0) {
        DWORD chunk = (DWORD)std::min<size_t>(left, 16 * 1024 * 1024), written = 0;
        if (!WriteFile(h, p, chunk, &written, nullptr) || written != chunk) { ok = false; if (err) *err = GetLastError(); break; }
        p += chunk;
        left -= chunk;
    }
    if (ok && !FlushFileBuffers(h)) { /* not fatal on some file systems */ }
    CloseHandle(h);
    return ok;
}

} // namespace

void File_Init()
{
    conv::SetPictureConverter(PictureHook);
    conv::SetCodePageHooks(DecodeHook, EncodeHook);
}

bool File_ReadAll(const std::wstring& path, std::vector<BYTE>& data, DWORD* err)
{
    data.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = GetLastError(); return false; }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(h, &size)) { if (err) *err = GetLastError(); CloseHandle(h); return false; }
    if (size.QuadPart > 1024LL * 1024 * 1024) { if (err) *err = ERROR_FILE_TOO_LARGE; CloseHandle(h); return false; }
    data.resize((size_t)size.QuadPart);
    size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - done, 16 * 1024 * 1024), read = 0;
        if (!ReadFile(h, data.data() + done, chunk, &read, nullptr)) { ok = false; if (err) *err = GetLastError(); break; }
        if (read == 0) break;
        done += read;
    }
    data.resize(done);
    CloseHandle(h);
    return ok;
}

bool File_WriteAll(const std::wstring& path, const void* data, size_t size, DWORD* err)
{
    // Write next to the target first so a failure never destroys the existing file.
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring tmp = path + L".~rptmp";
        DWORD tmpErr = 0;
        if (WriteDirect(tmp, data, size, &tmpErr)) {
            if (ReplaceFileW(path.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) return true;
            if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        }
        DeleteFileW(tmp.c_str());
    }
    return WriteDirect(path, data, size, err);
}

LoadResult File_LoadData(const std::vector<BYTE>& data, const std::wstring& nameHint, DocFormat* format, bool* utf8Bom)
{
    const BYTE* p = data.data();
    size_t n = data.size();
    if (utf8Bom) *utf8Bom = false;

    // Skip a UTF-8 BOM / leading white space when sniffing for RTF.
    size_t skip = (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) ? 3 : 0;
    size_t probe = skip;
    while (probe < n && (p[probe] == ' ' || p[probe] == '\r' || p[probe] == '\n' || p[probe] == '\t')) ++probe;
    if (n - probe >= 5 && memcmp(p + probe, "{\\rtf", 5) == 0) {
        if (!LoadRtf(p + probe, n - probe)) return LOAD_FAILED;
        *format = FMT_RTF;
        return LOAD_OK;
    }
    if (n >= 4 && memcmp(p, "PK\x03\x04", 4) == 0) {
        conv::Bytes bytes(data.begin(), data.end());
        conv::Container kind = conv::DetectContainer(bytes);
        std::string rtf, error;
        bool ok = false;
        if (kind == conv::Container::Docx) ok = conv::DocxToRtf(bytes, rtf, &error);
        else if (kind == conv::Container::Odt) ok = conv::OdtToRtf(bytes, rtf, &error);
        else return LOAD_UNSUPPORTED;
        if (!ok || !LoadRtf(rtf.data(), rtf.size())) return LOAD_FAILED;
        *format = kind == conv::Container::Docx ? FMT_DOCX : FMT_ODT;
        return LOAD_OK;
    }
    if (n >= 8 && memcmp(p, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0) return LOAD_OLDDOC;
    if (n >= 5 && memcmp(p, "%PDF-", 5) == 0) return LOAD_UNSUPPORTED;

    // Plain text in one of the supported encodings.
    std::wstring text;
    DocFormat fmt = FMT_TEXT_UTF8;
    bool bigEndian = false;
    if (skip == 3) {
        text = DecodeBytes(p + 3, n - 3, CP_UTF8);
        if (utf8Bom) *utf8Bom = true;
    } else if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) {
        bigEndian = p[0] == 0xFE;
        size_t count = (n - 2) / 2;
        text.resize(count);
        for (size_t i = 0; i < count; ++i) {
            BYTE a = p[2 + i * 2], b = p[3 + i * 2];
            text[i] = bigEndian ? (wchar_t)((a << 8) | b) : (wchar_t)((b << 8) | a);
        }
        fmt = FMT_TEXT_UTF16;
    } else if (IsValidUtf8(p, n)) {
        text = DecodeBytes(p, n, CP_UTF8);
    } else if (LooksLikeUtf16(p, n, &bigEndian)) {
        size_t count = n / 2;
        text.resize(count);
        for (size_t i = 0; i < count; ++i) {
            BYTE a = p[i * 2], b = p[i * 2 + 1];
            text[i] = bigEndian ? (wchar_t)((a << 8) | b) : (wchar_t)((b << 8) | a);
        }
        fmt = FMT_TEXT_UTF16;
    } else {
        text = DecodeBytes(p, n, CP_ACP);
        fmt = FMT_TEXT_ANSI;
    }
    // Embedded NULs would truncate the text inside the control.
    for (wchar_t& c : text) if (c == 0) c = L' ';
    if (!LoadPlainText(text)) return LOAD_FAILED;
    *format = fmt;
    (void)nameHint;
    return LOAD_OK;
}

LoadResult File_Load(const std::wstring& path, DocFormat* format, bool* utf8Bom, DWORD* err)
{
    std::vector<BYTE> data;
    if (err) *err = 0;
    if (!File_ReadAll(path, data, err)) return LOAD_FAILED;
    if (data.empty()) {
        // An empty file takes its type from the extension.
        DocFormat fmt = File_FormatFromPath(path, FMT_TEXT_UTF8);
        if (IsTextFormat(fmt)) LoadPlainText(L"");
        else { Editor_Clear(); Editor_SetDefaultFormat(false); }
        *format = fmt;
        if (utf8Bom) *utf8Bom = false;
        return LOAD_OK;
    }
    return File_LoadData(data, path, format, utf8Bom);
}

bool File_Serialize(DocFormat format, bool utf8Bom, std::vector<BYTE>& out)
{
    out.clear();
    if (format == FMT_RTF || format == FMT_DOCX || format == FMT_ODT) {
        std::string rtf;
        if (!Editor_StreamOut(rtf, SF_RTF)) return false;
        if (format == FMT_RTF) {
            out.assign(rtf.begin(), rtf.end());
            return true;
        }
        // Exported documents carry the current page setup.
        conv::PageSetup page;
        page.width = g.cfg.paper.cx;
        page.height = g.cfg.paper.cy;
        page.left = g.cfg.margins.left;
        page.top = g.cfg.margins.top;
        page.right = g.cfg.margins.right;
        page.bottom = g.cfg.margins.bottom;
        conv::SetExportPage(page);
        conv::Bytes bytes;
        std::string error;
        bool ok = format == FMT_DOCX ? conv::RtfToDocx(rtf, bytes, &error) : conv::RtfToOdt(rtf, bytes, &error);
        if (!ok) return false;
        out.assign(bytes.begin(), bytes.end());
        return true;
    }

    // The text as it is displayed: no hidden text (link addresses) and no object placeholders.
    std::wstring text = Editor_GetPlainText();

    if (format == FMT_TEXT_UTF16) {
        out.resize(2 + text.size() * 2);
        out[0] = 0xFF; out[1] = 0xFE;
        memcpy(out.data() + 2, text.data(), text.size() * 2);
        return true;
    }
    UINT cp = format == FMT_TEXT_ANSI ? CP_ACP : CP_UTF8;
    int len = text.empty() ? 0 : WideCharToMultiByte(cp, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    size_t head = (format == FMT_TEXT_UTF8 && utf8Bom) ? 3 : 0;
    out.resize(head + (size_t)len);
    if (head) { out[0] = 0xEF; out[1] = 0xBB; out[2] = 0xBF; }
    if (len > 0) WideCharToMultiByte(cp, 0, text.c_str(), (int)text.size(), (char*)out.data() + head, len, nullptr, nullptr);
    return true;
}

bool File_Save(const std::wstring& path, DocFormat format, bool utf8Bom, DWORD* err)
{
    if (err) *err = 0;
    std::vector<BYTE> out;
    if (!File_Serialize(format, utf8Bom, out)) { if (err) *err = ERROR_INVALID_DATA; return false; }
    return File_WriteAll(path, out.data(), out.size(), err);
}

bool File_CanSaveAsAnsi()
{
    std::wstring text = Editor_GetPlainText();
    if (text.empty()) return true;
    BOOL usedDefault = FALSE;
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.c_str(), (int)text.size(), nullptr, 0, nullptr, &usedDefault);
    return !usedDefault;
}

DocFormat File_FormatFromPath(const std::wstring& path, DocFormat fallback)
{
    if (EndsWithNoCase(path, L".rtf")) return FMT_RTF;
    if (EndsWithNoCase(path, L".docx")) return FMT_DOCX;
    if (EndsWithNoCase(path, L".odt")) return FMT_ODT;
    if (EndsWithNoCase(path, L".txt")) return IsTextFormat(fallback) ? fallback : FMT_TEXT_UTF8;
    return fallback;
}

bool File_IsImagePath(const std::wstring& path)
{
    static const wchar_t* const kExt[] = { L".png", L".jpg", L".jpeg", L".jpe", L".gif", L".bmp", L".dib", L".tif", L".tiff", L".ico" };
    for (const wchar_t* e : kExt)
        if (EndsWithNoCase(path, e)) return true;
    return false;
}

std::wstring File_ErrorText(DWORD err)
{
    if (err == 0) return L"";
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring text = n ? std::wstring(buf, n) : Fmt(L"Error %u", err);
    if (buf) LocalFree(buf);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}
