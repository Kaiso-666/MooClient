#include <windows.h>

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <string>

#include "diag/log.h"
#include "shell/crash.h"
#include "shell/job.h"
#include "shell/single_instance.h"
#include "shell/window.h"

namespace {

// One-time move from the old working name so an existing WebView2 profile and
// session survive the rename. Must run before diag::Init() creates the new dir.
void MigrateLegacyData() {
    PWSTR raw = nullptr;
    std::wstring local;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        local = raw;
    }
    if (raw) CoTaskMemFree(raw);
    if (local.empty()) return;

    const auto legacy = std::filesystem::path(local) / L"MooDesk";
    const auto current = std::filesystem::path(local) / L"MooClient";

    std::error_code ec;
    if (!std::filesystem::exists(current) && std::filesystem::exists(legacy)) {
        std::filesystem::rename(legacy, current, ec);
        if (ec) OutputDebugStringW(L"MooClient: legacy data migration failed\n");
    }
}

std::wstring DefaultUserDataDir() {
    PWSTR raw = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        dir = raw;
    }
    if (raw) CoTaskMemFree(raw);
    if (dir.empty()) dir = L".";
    dir += L"\\MooClient\\profiles\\default";

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    return dir;
}

// The asset store is shared by every profile: it is keyed by URL and holds no
// account data, so switching accounts does not throw the cache away.
std::wstring DefaultCacheDir() {
    PWSTR raw = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        dir = raw;
    }
    if (raw) CoTaskMemFree(raw);
    if (dir.empty()) dir = L".";
    dir += L"\\MooClient\\cache";

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    return dir;
}

std::wstring ExecutableDir() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH) return {};
    return std::filesystem::path(path).parent_path().wstring();
}

// A fixed-version runtime next to the exe makes the app independent of the
// WebView2 runtime installed on the machine (and of Edge updates).
std::wstring DetectFixedRuntime() {
    const std::wstring dir = ExecutableDir();
    if (dir.empty()) return {};
    const auto candidate = std::filesystem::path(dir) / L"runtime";
    if (std::filesystem::exists(candidate / L"msedgewebview2.exe")) {
        return candidate.wstring();
    }
    return {};
}

void ParseCommandLine(moo::shell::Options& options) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--url" && i + 1 < argc) {
            options.start_url = argv[++i];
        } else if (arg == L"--user-data" && i + 1 < argc) {
            options.user_data_dir = argv[++i];
        } else if (arg == L"--runtime" && i + 1 < argc) {
            options.runtime_dir = argv[++i];
        } else if (arg == L"--cache-dir" && i + 1 < argc) {
            options.cache_dir = argv[++i];
        } else if (arg == L"--no-cache") {
            options.cache_dir.clear();
        } else if (arg == L"--no-update") {
            options.check_updates = false;
        } else if (arg == L"--dev") {
            options.dev = true;
        }
    }
    LocalFree(argv);

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(options.user_data_dir), ec);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) {
        MessageBoxW(nullptr, L"COM initialization failed.", L"MooClient", MB_ICONERROR | MB_OK);
        return 1;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    MigrateLegacyData();
    moo::diag::Init();
    SetCurrentProcessExplicitAppUserModelID(L"MooClient");
    moo::shell::InstallCrashHandler();
    moo::shell::ApplyProcessTuning();

    if (!moo::shell::AcquireSingleInstance()) {
        moo::diag::Write(L"main: another instance is already running");
        moo::diag::Shutdown();
        CoUninitialize();
        return 0;
    }

    moo::shell::Options options;
    options.user_data_dir = DefaultUserDataDir();
    options.cache_dir = DefaultCacheDir();
    options.runtime_dir = DetectFixedRuntime();
    ParseCommandLine(options);

    moo::diag::Write(L"main: url=%s", options.start_url.c_str());
    moo::diag::Write(L"main: user-data=%s dev=%d", options.user_data_dir.c_str(), options.dev);
    moo::diag::Write(L"main: cache=%s", options.cache_dir.empty() ? L"(disabled)"
                                                                  : options.cache_dir.c_str());
    moo::diag::Write(L"main: runtime=%s",
                     options.runtime_dir.empty() ? L"evergreen (installed Edge WebView2)"
                                                 : options.runtime_dir.c_str());

    moo::shell::Window window;
    if (!window.Create(instance, show_command, options)) {
        MessageBoxW(nullptr, L"MooClient could not create its main window.", L"MooClient",
                    MB_ICONERROR | MB_OK);
        moo::diag::Shutdown();
        CoUninitialize();
        return 1;
    }

    const int result = window.Run();

    moo::diag::Write(L"main: exit %d", result);
    moo::diag::Shutdown();
    CoUninitialize();
    return result;
}
