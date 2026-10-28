#include "webview/nav_policy.h"

#include <objbase.h>
#include <oleauto.h>

#include <commctrl.h>
#include <shellapi.h>
#include <urlmon.h>
#include <wrl/client.h>

#include <cwchar>
#include <mutex>
#include <set>

#include "diag/log.h"

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace moo::nav {
namespace {

std::mutex g_rules_mutex;
std::set<std::wstring> g_remembered;

std::wstring Lower(std::wstring value) {
    if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

bool StartsWithNoCase(const std::wstring& value, const wchar_t* prefix) {
    const size_t n = wcslen(prefix);
    return value.size() >= n && _wcsnicmp(value.c_str(), prefix, n) == 0;
}

bool Parse(const std::wstring& uri, std::wstring* scheme, std::wstring* host) {
    Microsoft::WRL::ComPtr<IUri> parsed;
    if (FAILED(CreateUri(uri.c_str(), Uri_CREATE_CANONICALIZE | Uri_CREATE_NO_DECODE_EXTRA_INFO,
                         0, &parsed)) ||
        !parsed) {
        return false;
    }

    BSTR scheme_raw = nullptr;
    BSTR host_raw = nullptr;
    if (SUCCEEDED(parsed->GetSchemeName(&scheme_raw)) && scheme_raw) {
        *scheme = Lower(scheme_raw);
    }
    if (SUCCEEDED(parsed->GetHost(&host_raw)) && host_raw) {
        *host = Lower(host_raw);
    }
    SysFreeString(scheme_raw);
    SysFreeString(host_raw);
    return true;
}

bool OpenInBrowser(const std::wstring& uri) {
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const auto code = reinterpret_cast<INT_PTR>(result);
    if (code <= 32) {
        moo::diag::Write(L"nav: ShellExecuteW failed (%lld) for %s", static_cast<long long>(code),
                         uri.c_str());
        return false;
    }
    moo::diag::Write(L"nav: opened externally: %s", uri.c_str());
    return true;
}

} // namespace

bool IsGameUri(const std::wstring& uri) {
    std::wstring scheme;
    std::wstring host;
    if (!Parse(uri, &scheme, &host)) return false;
    if (scheme != L"https") return false;
    return host == L"moomoo.io" || host == L"www.moomoo.io";
}

bool IsSilentScheme(const std::wstring& uri) {
    std::wstring scheme;
    std::wstring host;
    if (!Parse(uri, &scheme, &host)) {
        return StartsWithNoCase(uri, L"about:") || StartsWithNoCase(uri, L"blob:") ||
               StartsWithNoCase(uri, L"data:") || StartsWithNoCase(uri, L"javascript:") ||
               StartsWithNoCase(uri, L"file:");
    }
    return scheme == L"about" || scheme == L"blob" || scheme == L"data" ||
           scheme == L"javascript" || scheme == L"file";
}

std::wstring HostOf(const std::wstring& uri) {
    std::wstring scheme;
    std::wstring host;
    if (!Parse(uri, &scheme, &host)) return {};
    return host;
}

bool IsRemembered(const std::wstring& host) {
    if (host.empty()) return false;
    std::lock_guard lock(g_rules_mutex);
    return g_remembered.contains(host);
}

void Remember(const std::wstring& host) {
    if (host.empty()) return;
    std::lock_guard lock(g_rules_mutex);
    g_remembered.insert(host);
    moo::diag::Write(L"nav: remembered browser rule for %s", host.c_str());
}

bool HandleExternal(HWND owner, const std::wstring& uri) {
    const std::wstring host = HostOf(uri);
    if (IsRemembered(host)) return OpenInBrowser(uri);

    std::wstring content =
        L"MooClient only hosts moomoo.io. This link will open in your default browser.\n\n";
    content += uri;

    TASKDIALOG_BUTTON buttons[] = {
        {IDOK, L"Open in browser"},
        {IDCANCEL, L"Cancel"},
    };

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = L"MooClient";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"Open this link in your browser?";
    config.pszContent = content.c_str();
    config.pButtons = buttons;
    config.cButtons = ARRAYSIZE(buttons);
    config.nDefaultButton = IDOK;
    if (!host.empty()) config.pszVerificationText = L"Remember for this site";

    BOOL remembered = FALSE;
    int pressed = IDCANCEL;
    const HRESULT hr = TaskDialogIndirect(&config, &pressed, nullptr, &remembered);

    if (FAILED(hr)) {
        moo::diag::Write(L"nav: TaskDialog failed (0x%08lX), falling back to MessageBox",
                         static_cast<unsigned long>(hr));
        if (MessageBoxW(owner, content.c_str(), L"Open this link in your browser?",
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return false;
        }
        pressed = IDOK;
        remembered = FALSE;
    }

    if (pressed != IDOK) {
        moo::diag::Write(L"nav: external blocked by user: %s", uri.c_str());
        return false;
    }
    if (remembered) Remember(host);
    return OpenInBrowser(uri);
}

} // namespace moo::nav
