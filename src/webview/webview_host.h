#pragma once

#include <windows.h>
#include <eventtoken.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <WebView2.h>

#include "cache/asset_cache.h"

namespace moo::web {

// Posted to the main window with a heap-allocated std::wstring* when a
// navigation leaves the game and needs the user's decision.
inline constexpr UINT kMsgExternal = WM_APP + 1;

// Posted when the game document is ready (or failed) so the shell can swap the
// logo splash for the main window.
inline constexpr UINT kMsgReady = WM_APP + 2;

class Host {
public:
    Host();
    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    bool Initialize(HWND parent, std::wstring user_data_dir, std::wstring cache_dir,
                    std::wstring start_url, bool dev,
                    std::wstring browser_executable_folder);
    void SetFullscreenCallback(std::function<void()> callback);
    void SetMemoryUsageTargetLevel(bool low);
    void Resize(int width, int height);
    void Focus();
    void Close();

    // Drops every cache entry that is not content-addressed and reloads, so the
    // next load pulls fresh copies of anything that can change in place.
    void RefreshAssets();

    bool ready() const { return webview_ != nullptr; }

private:
    class EnvHandler;
    class ControllerHandler;
    class NavStartingHandler;
    class NewWindowHandler;
    class ProcessFailedHandler;
    class DomLoadedHandler;
    class NavCompletedHandler;
    class AcceleratorKeyHandler;
    class ResourceRequestedHandler;
    class ResponseReceivedHandler;

    void OnEnvironment(HRESULT result, ICoreWebView2Environment* env);
    void OnController(HRESULT result, ICoreWebView2Controller* controller);
    HRESULT OnNavigationStarting(ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args);
    HRESULT OnNewWindow(ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args);
    HRESULT OnProcessFailed(ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args);
    void OnDomLoaded();
    void OnNavigationCompleted(BOOL success, COREWEBVIEW2_WEB_ERROR_STATUS status);
    HRESULT OnResourceRequested(ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args);
    HRESULT OnResponseReceived(ICoreWebView2* sender,
                               ICoreWebView2WebResourceResponseReceivedEventArgs* args);

    void ApplySettings();
    void RequestExternal(const std::wstring& uri);
    void ToggleFullscreen();
    void Fail(const wchar_t* what, HRESULT hr);

    HWND parent_ = nullptr;
    std::wstring start_url_;
    std::wstring browser_folder_;
    bool dev_ = false;

    std::shared_ptr<moo::cache::AssetCache> cache_;
    // When each pass-through request started, so a slow older response cannot
    // overwrite a newer body that already landed.
    std::unordered_map<std::wstring, uint64_t> request_started_;
    // `/p/` modules and versioned URLs are checked once per process; after that
    // the on-disk copy is trusted for the rest of the run.
    std::unordered_set<std::wstring> session_validated_;
    // Counts responses we served ourselves, so the capture path does not store
    // them again if WebView2 reports them.
    std::unordered_map<std::wstring, int> served_;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    Microsoft::WRL::ComPtr<ICoreWebView2_2> webview2_;
    Microsoft::WRL::ComPtr<ICoreWebView2_19> webview19_;

    Microsoft::WRL::ComPtr<ICoreWebView2NavigationStartingEventHandler> nav_handler_;
    Microsoft::WRL::ComPtr<ICoreWebView2NewWindowRequestedEventHandler> new_window_handler_;
    Microsoft::WRL::ComPtr<ICoreWebView2ProcessFailedEventHandler> process_failed_handler_;
    Microsoft::WRL::ComPtr<ICoreWebView2DOMContentLoadedEventHandler> dom_loaded_handler_;
    Microsoft::WRL::ComPtr<ICoreWebView2NavigationCompletedEventHandler> nav_completed_handler_;
    Microsoft::WRL::ComPtr<ICoreWebView2AcceleratorKeyPressedEventHandler> accelerator_handler_;
    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequestedEventHandler> resource_handler_;
    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponseReceivedEventHandler> response_handler_;

    EventRegistrationToken nav_token_{};
    EventRegistrationToken new_window_token_{};
    EventRegistrationToken process_failed_token_{};
    EventRegistrationToken dom_loaded_token_{};
    EventRegistrationToken nav_completed_token_{};
    EventRegistrationToken accelerator_token_{};
    EventRegistrationToken resource_token_{};
    EventRegistrationToken response_token_{};

    std::function<void()> on_toggle_fullscreen_;

    ULONGLONG start_tick_ = 0;

    // Shared with the async handlers. They check it before touching `this`, so a
    // close during startup cannot turn into a use-after-free.
    std::shared_ptr<bool> alive_;

    bool closed_ = false;
};

} // namespace moo::web
