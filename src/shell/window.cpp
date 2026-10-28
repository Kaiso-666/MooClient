#include "shell/window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <memory>
#include <string>
#include <thread>

#include "diag/log.h"
#include "resource.h"
#include "update/updater.h"
#include "version.h"
#include "webview/nav_policy.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")

namespace moo::shell {
namespace {

constexpr wchar_t kClassName[] = L"MooClientMainWindow";
constexpr wchar_t kTitle[] = L"MooMoo.IO";
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;
constexpr int kProbeHotkey = 1;
constexpr int kRefreshHotkey = 3;
constexpr UINT_PTR kReadyTimer = 2;
constexpr UINT kReadyTimeoutMs = 20000;

void ApplyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE on current Windows 10/11 builds.
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

// ------------------------------------------------------------ update install

struct DownloadJob {
    moo::update::Info info;
    std::atomic<bool> cancelled{false};
    std::atomic<int> state{0}; // 0 = running, 1 = ready, 2 = failed
    std::wstring path;
    std::wstring error;
};

HRESULT CALLBACK DownloadCallback(HWND dialog, UINT message, WPARAM, LPARAM, LONG_PTR ref) {
    auto* job = reinterpret_cast<DownloadJob*>(ref);
    if (!job) return S_OK;

    switch (message) {
    case TDN_CREATED: {
        SendMessageW(dialog, TDM_ENABLE_BUTTON, IDOK, FALSE);
        SendMessageW(dialog, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
        std::thread([job] {
            std::wstring path;
            std::wstring error;
            const bool ok =
                moo::update::DownloadInstaller(job->info, &job->cancelled, &path, &error);
            job->path = std::move(path);
            job->error = std::move(error);
            job->state.store(ok ? 1 : 2);
        }).detach();
        break;
    }
    case TDN_TIMER: {
        const int state = job->state.load();
        if (state == 1) {
            SendMessageW(dialog, TDM_SET_PROGRESS_BAR_MARQUEE, FALSE, 0);
            SendMessageW(dialog, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION,
                         reinterpret_cast<LPARAM>(L"Update downloaded"));
            SendMessageW(dialog, TDM_SET_ELEMENT_TEXT, TDE_CONTENT,
                         reinterpret_cast<LPARAM>(
                             L"MooClient will close so the installer can run."));
            SendMessageW(dialog, TDM_ENABLE_BUTTON, IDOK, TRUE);
            SendMessageW(dialog, TDM_CLICK_BUTTON, IDOK, 0);
        } else if (state == 2) {
            SendMessageW(dialog, TDM_SET_PROGRESS_BAR_MARQUEE, FALSE, 0);
            SendMessageW(dialog, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION,
                         reinterpret_cast<LPARAM>(L"The update could not be downloaded"));
            const wchar_t* detail =
                job->error.empty() ? L"Check your connection and try again." : job->error.c_str();
            SendMessageW(dialog, TDM_SET_ELEMENT_TEXT, TDE_CONTENT,
                         reinterpret_cast<LPARAM>(detail));
            SendMessageW(dialog, TDM_CLICK_BUTTON, IDCANCEL, 0);
        }
        break;
    }
    default:
        break;
    }
    return S_OK;
}

} // namespace

bool Window::Create(HINSTANCE instance, int show_command, const Options& options) {
    instance_ = instance;
    options_ = options;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = &Window::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    HICON icon_big = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON), LR_SHARED));
    HICON icon_small = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!icon_big) icon_big = LoadIconW(nullptr, IDI_APPLICATION);
    if (!icon_small) icon_small = icon_big;

    wc.hIcon = icon_big;
    wc.hIconSm = icon_small;
    wc.lpszClassName = kClassName;
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        moo::diag::Write(L"window: RegisterClassEx failed (%lu)", GetLastError());
        return false;
    }

    RECT rect{0, 0, kDefaultWidth, kDefaultHeight};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);
    const int x = (screen_w - width) / 2;
    const int y = (screen_h - height) / 2;

    hwnd_ = CreateWindowExW(0, kClassName, kTitle,
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width, height,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) {
        moo::diag::Write(L"window: CreateWindowEx failed (%lu)", GetLastError());
        return false;
    }

    ApplyDarkTitleBar(hwnd_);
    // The window text is normally taken from CreateWindowExW's lpWindowName, but
    // the custom WM_NCCREATE path bypasses DefWindowProc, so set it explicitly.
    SetWindowTextW(hwnd_, kTitle);
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_big));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small));
    if (!RegisterHotKey(hwnd_, kProbeHotkey, MOD_CONTROL | MOD_ALT, 'E')) {
        moo::diag::Write(L"window: RegisterHotKey failed (%lu)", GetLastError());
    }
    if (!RegisterHotKey(hwnd_, kRefreshHotkey, MOD_CONTROL, VK_F5)) {
        moo::diag::Write(L"window: refresh hotkey unavailable (%lu)", GetLastError());
    }

    show_command_ = show_command;
    moo::diag::Write(L"window: created %p (hidden until ready)", hwnd_);

    // Floating logo while WebView2 boots and the game document loads.
    const bool splash_shown = splash_.Show(instance);
    SetTimer(hwnd_, kReadyTimer, kReadyTimeoutMs, nullptr);

    web_.SetFullscreenCallback([this] { ToggleFullscreen(); });
    const bool started = web_.Initialize(hwnd_, options_.user_data_dir, options_.cache_dir,
                                         options_.start_url, options_.dev, options_.runtime_dir);
    if (!started || !splash_shown) ShowMainWindow();
    return true;
}

void Window::ShowMainWindow() {
    if (shown_ || !hwnd_) return;
    shown_ = true;

    KillTimer(hwnd_, kReadyTimer);
    splash_.Close();

    ShowWindow(hwnd_, show_command_);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    web_.Focus();
    moo::diag::Write(L"window: main window shown");

    // The check is silent and off the UI thread; a result arrives later as
    // kMsgUpdateAvailable. Starting it here keeps the splash path untouched.
    if (options_.check_updates && !update_prompted_) {
        update_prompted_ = true;
        moo::update::CheckAsync(hwnd_, MOO_VERSION_WSTRING);
    }
}

void Window::PromptUpdate(moo::update::Info* info) {
    std::unique_ptr<moo::update::Info> owned(info);
    if (!owned) return;

    std::wstring content = L"You are running ";
    content += MOO_VERSION_WSTRING;
    content += L". Version ";
    content += owned->version;
    content += L" is available.";
    if (!owned->notes.empty()) {
        std::wstring notes = owned->notes;
        if (notes.size() > 1200) {
            notes.resize(1200);
            notes += L"\u2026";
        }
        content += L"\n\n";
        content += notes;
    }

    TASKDIALOG_BUTTON buttons[] = {
        {IDOK, L"Update now"},
        {IDCANCEL, L"Ignore"},
    };

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = hwnd_;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = L"MooClient";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"A new version of MooClient is available";
    config.pszContent = content.c_str();
    config.pButtons = buttons;
    config.cButtons = ARRAYSIZE(buttons);
    config.nDefaultButton = IDOK;

    int pressed = IDCANCEL;
    const HRESULT result = TaskDialogIndirect(&config, &pressed, nullptr, nullptr);
    if (FAILED(result)) {
        moo::diag::Write(L"update: TaskDialog failed (0x%08lX)",
                         static_cast<unsigned long>(result));
        if (MessageBoxW(hwnd_, content.c_str(), L"A new version of MooClient is available",
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
        pressed = IDOK;
    }

    if (pressed != IDOK) {
        moo::diag::Write(L"update: ignored %s", owned->version.c_str());
        return;
    }
    StartUpdateDownload(*owned);
}

void Window::StartUpdateDownload(const moo::update::Info& info) {
    auto job = std::make_shared<DownloadJob>();
    job->info = info;

    std::wstring content = L"Downloading MooClient ";
    content += info.version;
    content += L"\u2026";

    TASKDIALOG_BUTTON buttons[] = {
        {IDCANCEL, L"Cancel"},
        {IDOK, L"Install"},
    };

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = hwnd_;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW |
                     TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER;
    config.pszWindowTitle = L"MooClient";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"Downloading update";
    config.pszContent = content.c_str();
    config.pButtons = buttons;
    config.cButtons = ARRAYSIZE(buttons);
    config.nDefaultButton = IDCANCEL;
    config.pfCallback = &DownloadCallback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(job.get());

    int pressed = IDCANCEL;
    const HRESULT result = TaskDialogIndirect(&config, &pressed, nullptr, nullptr);
    if (FAILED(result) || pressed != IDOK || job->path.empty()) {
        job->cancelled.store(true);
        moo::diag::Write(L"update: download cancelled");
        return;
    }

    // The installer replaces MooClient.exe, so close first. /SILENT keeps a
    // progress window without asking questions; Inno also closes any stragglers.
    const HINSTANCE launched = ShellExecuteW(
        hwnd_, L"open", job->path.c_str(),
        L"/SILENT /NORESTART /CLOSEAPPLICATIONS /SUPPRESSMSGBOXES", nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(launched) <= 32) {
        moo::diag::Write(L"update: could not launch %s (%lld)", job->path.c_str(),
                         static_cast<long long>(reinterpret_cast<INT_PTR>(launched)));
        MessageBoxW(hwnd_,
                    L"The installer was downloaded but could not be started. It is in your "
                    L"temporary folder.",
                    L"MooClient", MB_ICONERROR | MB_OK);
        return;
    }

    moo::diag::Write(L"update: installer launched, closing");
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

int Window::Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<Window*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }

    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->Handle(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Window::Handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) {
            // Chromium keeps full caches while hidden; LOW trims them and is
            // restored when the window comes back. Do not resize the controller
            // to 0x0: that forces a pointless re-layout and repaint.
            web_.SetMemoryUsageTargetLevel(true);
            return 0;
        }
        if (wparam == SIZE_RESTORED || wparam == SIZE_MAXIMIZED) {
            web_.SetMemoryUsageTargetLevel(false);
        }
        if (web_.ready()) {
            web_.Resize(LOWORD(lparam), HIWORD(lparam));
        } else {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        if (web_.ready()) {
            // The WebView2 child covers the client area; acknowledge the paint
            // without running GDI when it does.
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            EndPaint(hwnd_, &paint);
            return 0;
        }
        Paint();
        return 0;

    case WM_SETFOCUS:
        web_.Focus();
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) {
            SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
            web_.Focus();
        } else {
            SetThreadExecutionState(ES_CONTINUOUS);
        }
        return 0;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = POINT{900, 600};
        return 0;
    }

    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case moo::web::kMsgExternal: {
        std::unique_ptr<std::wstring> uri(reinterpret_cast<std::wstring*>(lparam));
        if (uri) moo::nav::HandleExternal(hwnd_, *uri);
        return 0;
    }

    case moo::web::kMsgReady:
        ShowMainWindow();
        return 0;

    case moo::update::kMsgUpdateAvailable: {
        std::unique_ptr<moo::update::Info> info(
            reinterpret_cast<moo::update::Info*>(lparam));
        if (info) PromptUpdate(info.release());
        return 0;
    }

    case WM_TIMER:
        if (wparam == kReadyTimer) {
            moo::diag::Write(L"window: ready timeout, showing main window anyway");
            ShowMainWindow();
            return 0;
        }
        break;

    case WM_HOTKEY:
        if (wparam == kProbeHotkey) {
            moo::diag::Write(L"nav: policy test requested via Ctrl+Alt+E");
            moo::nav::HandleExternal(hwnd_, L"https://example.com/");
            return 0;
        }
        if (wparam == kRefreshHotkey) {
            moo::diag::Write(L"window: refresh assets requested via Ctrl+F5");
            web_.RefreshAssets();
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(hwnd_, kProbeHotkey);
        UnregisterHotKey(hwnd_, kRefreshHotkey);
        KillTimer(hwnd_, kReadyTimer);
        SetThreadExecutionState(ES_CONTINUOUS);
        splash_.Close();
        web_.Close();
        hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

void Window::ToggleFullscreen() {
    if (!hwnd_) return;

    if (!fullscreen_) {
        saved_placement_.length = sizeof(saved_placement_);
        if (!GetWindowPlacement(hwnd_, &saved_placement_)) {
            saved_placement_ = {};
        }
        saved_style_ = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));

        HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) return;

        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd_, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left,
                     info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        fullscreen_ = true;
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, static_cast<LONG_PTR>(saved_style_));
        if (saved_placement_.length == sizeof(saved_placement_)) {
            SetWindowPlacement(hwnd_, &saved_placement_);
        }
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER);
        fullscreen_ = false;
    }

    moo::diag::Write(L"window: fullscreen=%d", fullscreen_ ? 1 : 0);
}

void Window::Paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);

    RECT client{};
    GetClientRect(hwnd_, &client);

    HBRUSH background = CreateSolidBrush(RGB(17, 17, 19));
    FillRect(dc, &client, background);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(238, 238, 242));

    HFONT title_font = CreateFontW(-40, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT body_font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");

    RECT title_rect = client;
    title_rect.bottom = client.top + (client.bottom - client.top) / 2;
    HGDIOBJ old = SelectObject(dc, title_font);
    DrawTextW(dc, L"MooMoo.IO", -1, &title_rect, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
    SelectObject(dc, old);

    RECT body_rect = client;
    body_rect.top = title_rect.bottom + 10;
    old = SelectObject(dc, body_font);
    SetTextColor(dc, RGB(150, 150, 160));
    DrawTextW(dc, L"Starting moomoo.io\x2026", -1, &body_rect,
              DT_CENTER | DT_TOP | DT_SINGLELINE);
    SelectObject(dc, old);

    DeleteObject(title_font);
    DeleteObject(body_font);
    EndPaint(hwnd_, &paint);
}

} // namespace moo::shell
