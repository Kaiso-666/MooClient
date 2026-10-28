#include "shell/single_instance.h"

#include <windows.h>

#include "diag/log.h"

namespace moo::shell {
namespace {
HANDLE g_mutex = nullptr;
constexpr wchar_t kClass[] = L"MooClientMainWindow";
} // namespace

bool AcquireSingleInstance() {
    g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\MooClient.SingleInstance");
    if (!g_mutex) {
        moo::diag::Write(L"single-instance: CreateMutex failed (%lu)", GetLastError());
        return true; // fail open; a duplicate window is better than no window
    }

    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;

    HWND existing = FindWindowW(kClass, nullptr);
    if (existing) {
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        moo::diag::Write(L"single-instance: focused existing window %p", existing);
    }
    return false;
}

} // namespace moo::shell
