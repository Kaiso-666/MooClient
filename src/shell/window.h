#pragma once

#include <windows.h>

#include <string>

#include "shell/splash.h"
#include "update/updater.h"
#include "webview/webview_host.h"

namespace moo::shell {

struct Options {
    std::wstring start_url = L"https://moomoo.io/";
    std::wstring user_data_dir;
    // Empty disables the L2 asset store (--no-cache).
    std::wstring cache_dir;
    // Empty = use the installed Evergreen WebView2 runtime. Otherwise points at
    // a fixed-version runtime folder (contains msedgewebview2.exe).
    std::wstring runtime_dir;
    bool dev = false;
    bool check_updates = true;
};

class Window {
public:
    bool Create(HINSTANCE instance, int show_command, const Options& options);
    int Run();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam);
    void Paint();
    void ToggleFullscreen();
    void ShowMainWindow();
    void PromptUpdate(moo::update::Info* info);
    void StartUpdateDownload(const moo::update::Info& info);

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    moo::web::Host web_;
    Splash splash_;
    Options options_;
    int show_command_ = SW_SHOWNORMAL;
    bool shown_ = false;
    bool fullscreen_ = false;
    bool update_prompted_ = false;
    WINDOWPLACEMENT saved_placement_{};
    DWORD saved_style_ = 0;
};

} // namespace moo::shell
