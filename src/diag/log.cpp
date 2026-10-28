#include "diag/log.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <share.h>

namespace moo::diag {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::wstring g_path;

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        out = raw;
    }
    if (raw) CoTaskMemFree(raw);
    return out;
}

void RawWrite(const wchar_t* text) {
    if (g_file) {
        fputws(text, g_file);
        fflush(g_file);
    }
    OutputDebugStringW(text);
}

} // namespace

void Init() {
    std::lock_guard lock(g_mutex);
    if (g_file) return;

    std::wstring dir = KnownFolder(FOLDERID_LocalAppData);
    if (dir.empty()) dir = L".";
    dir += L"\\MooClient\\logs";

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    if (ec) {
        g_path = L"(unavailable)";
        return;
    }

    g_path = dir + L"\\client.log";
    g_file = _wfsopen(g_path.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    if (!g_file) {
        g_path = L"(unavailable)";
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    fwprintf(g_file, L"\n===== MooClient session %04u-%02u-%02u %02u:%02u:%02u =====\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fflush(g_file);
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    RawWrite(L"===== session end =====\n");
    fclose(g_file);
    g_file = nullptr;
}

void Write(const wchar_t* fmt, ...) {
    std::lock_guard lock(g_mutex);
    if (!g_file) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    fwprintf(g_file, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfwprintf(g_file, fmt, args);
    va_end(args);

    fputws(L"\n", g_file);
    fflush(g_file);
}

const std::wstring& Path() {
    return g_path;
}

} // namespace moo::diag
