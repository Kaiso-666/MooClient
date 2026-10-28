#include "shell/crash.h"

#include <windows.h>
#include <dbghelp.h>
#include <objbase.h>
#include <shlobj.h>

#include <cwchar>
#include <cstdio>
#include <string>

#include "diag/log.h"

#pragma comment(lib, "dbghelp.lib")

namespace moo::shell {
namespace {

std::wstring LocalAppData() {
    PWSTR raw = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        dir = raw;
    }
    if (raw) CoTaskMemFree(raw);
    return dir;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    std::wstring dir = LocalAppData();
    if (dir.empty()) dir = L".";
    dir += L"\\MooClient\\crashes";
    CreateDirectoryW(dir.c_str(), nullptr);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"%s\\crash-%04u%02u%02u-%02u%02u%02u.dmp", dir.c_str(), st.wYear,
               st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION dump{};
        dump.ThreadId = GetCurrentThreadId();
        dump.ExceptionPointers = info;
        dump.ClientPointers = FALSE;

        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo);
        const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                          &dump, nullptr, nullptr);
        CloseHandle(file);
        moo::diag::Write(L"crash: dump %s (%s)", path, ok ? L"ok" : L"failed");
    } else {
        moo::diag::Write(L"crash: CreateFile failed (%lu) for %s", GetLastError(), path);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void InstallCrashHandler() {
    SetUnhandledExceptionFilter(CrashFilter);
}

} // namespace moo::shell
