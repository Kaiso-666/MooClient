#include "webview/webview_host.h"

#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <wrl/implements.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <WebView2EnvironmentOptions.h>

#include "cache/asset_policy.h"
#include "diag/log.h"
#include "resource.h"
#include "webview/nav_policy.h"

using Microsoft::WRL::ComPtr;

namespace moo::web {
namespace {

std::wstring TakeUri(LPWSTR raw) {
    if (!raw) return {};
    std::wstring value(raw);
    CoTaskMemFree(raw);
    return value;
}

// Reads a UTF-8 RCDATA resource (scripts) into a wide string for WebView2.
std::wstring LoadTextResource(int id) {    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const HRSRC res = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return {};

    const HGLOBAL handle = LoadResource(instance, res);
    if (!handle) return {};

    const auto* bytes = static_cast<const char*>(LockResource(handle));
    const DWORD size = SizeofResource(instance, res);
    if (!bytes || !size) return {};

    const int length = MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(size), nullptr, 0);
    if (length <= 0) return {};

    std::wstring text(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(size), text.data(), length);
    return text;
}

// Writes an embedded RCDATA asset to %LOCALAPPDATA%\MooClient\assets and returns
// the folder, so WebView2 can serve it through a virtual host.
std::wstring EnsureAsset(int id, const wchar_t* filename) {
    PWSTR raw = nullptr;
    std::wstring local;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        local = raw;
    }
    if (raw) CoTaskMemFree(raw);
    if (local.empty()) return {};

    const std::wstring app_dir = local + L"\\MooClient";
    const std::wstring assets = app_dir + L"\\assets";
    CreateDirectoryW(app_dir.c_str(), nullptr);
    CreateDirectoryW(assets.c_str(), nullptr);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const HRSRC res = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return {};
    const HGLOBAL handle = LoadResource(instance, res);
    if (!handle) return {};
    const void* bytes = LockResource(handle);
    const DWORD size = SizeofResource(instance, res);
    if (!bytes || !size) return {};

    const std::wstring path = assets + L"\\" + filename;
    WIN32_FILE_ATTRIBUTE_DATA existing{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &existing) &&
        existing.nFileSizeHigh == 0 && existing.nFileSizeLow == size) {
        return assets;
    }

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes, size, &written, nullptr);
    CloseHandle(file);
    if (!ok || written != size) return {};
    return assets;
}

// Completion handler for AddScriptToExecuteOnDocumentCreated; the script id is
// only useful for RemoveScriptToExecuteOnDocumentCreated, which we never call.
class ScriptInstalledHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, LPCWSTR script_id) override {
        moo::diag::Write(L"webview: document view script id=%s (0x%08lX)",
                         script_id ? script_id : L"(null)",
                         static_cast<unsigned long>(result));
        return S_OK;
    }
};

// ---------------------------------------------------------------- asset cache

constexpr size_t kMaxAssetBytes = 32ull * 1024 * 1024;

bool ShouldDropHeader(const std::wstring& name) {
    static const wchar_t* const kDropped[] = {
        L"content-encoding", L"content-length", L"transfer-encoding", L"connection",
        L"keep-alive",       L"set-cookie",     L"set-cookie2",       L"alt-svc",
        L"age",              L"date",           L"server",            L"vary",
        L"nel",              L"report-to",      L"strict-transport-security",
    };
    for (const wchar_t* dropped : kDropped) {
        if (_wcsicmp(name.c_str(), dropped) == 0) return true;
    }
    return false;
}

// Copies the origin's headers into a block we can replay verbatim, minus the
// ones that describe the wire encoding rather than the resource.
std::wstring BuildReplayHeaders(ICoreWebView2HttpResponseHeaders* headers) {
    std::wstring block;
    if (!headers) return block;

    ComPtr<ICoreWebView2HttpHeadersCollectionIterator> iterator;
    if (FAILED(headers->GetIterator(&iterator)) || !iterator) return block;

    BOOL has_current = FALSE;
    while (SUCCEEDED(iterator->get_HasCurrentHeader(&has_current)) && has_current) {
        LPWSTR raw_name = nullptr;
        LPWSTR raw_value = nullptr;
        if (SUCCEEDED(iterator->GetCurrentHeader(&raw_name, &raw_value))) {
            const std::wstring name = raw_name ? raw_name : L"";
            const std::wstring value = raw_value ? raw_value : L"";
            if (!name.empty() && !ShouldDropHeader(name)) {
                block += name;
                block += L": ";
                block += value;
                block += L"\r\n";
            }
        }
        CoTaskMemFree(raw_name);
        CoTaskMemFree(raw_value);

        BOOL more = FALSE;
        if (FAILED(iterator->MoveNext(&more)) || !more) break;
    }
    return block;
}

std::string Narrow(const std::wstring& value) {
    std::string out;
    out.reserve(value.size());
    for (const wchar_t wide : value) out.push_back(wide < 128 ? static_cast<char>(wide) : '?');
    return out;
}

// `ICoreWebView2HttpResponseHeaders::GetHeader` matches names case-sensitively
// in practice, and the origin sends lowercase names, so scan instead.
std::wstring GetHeader(ICoreWebView2HttpResponseHeaders* headers, const wchar_t* name) {
    if (!headers) return {};
    ComPtr<ICoreWebView2HttpHeadersCollectionIterator> iterator;
    if (FAILED(headers->GetIterator(&iterator)) || !iterator) return {};

    BOOL has_current = FALSE;
    while (SUCCEEDED(iterator->get_HasCurrentHeader(&has_current)) && has_current) {
        LPWSTR raw_name = nullptr;
        LPWSTR raw_value = nullptr;
        if (SUCCEEDED(iterator->GetCurrentHeader(&raw_name, &raw_value))) {
            const bool match = raw_name && _wcsicmp(raw_name, name) == 0;
            std::wstring value;
            if (match && raw_value) value = raw_value;
            CoTaskMemFree(raw_name);
            CoTaskMemFree(raw_value);
            if (match) return value;
        } else {
            CoTaskMemFree(raw_name);
            CoTaskMemFree(raw_value);
        }

        BOOL more = FALSE;
        if (FAILED(iterator->MoveNext(&more)) || !more) break;
    }
    return {};
}

bool ReadStream(IStream* stream, std::string* out) {
    if (!stream || !out) return false;

    LARGE_INTEGER start{};
    stream->Seek(start, STREAM_SEEK_SET, nullptr);

    out->clear();
    std::vector<char> buffer(64 * 1024);
    for (;;) {
        ULONG read = 0;
        const HRESULT hr = stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &read);
        if (read == 0) break;
        if (out->size() + read > kMaxAssetBytes) return false;
        out->append(buffer.data(), read);
        if (hr == S_FALSE) break;
        if (FAILED(hr)) return false;
    }
    return true;
}

// Reads the (already decoded) response body, then writes it to the L2 store.
class CaptureHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ICoreWebView2WebResourceResponseViewGetContentCompletedHandler> {
public:
    CaptureHandler(std::shared_ptr<moo::cache::AssetCache> cache, std::wstring url,
                   std::wstring headers, moo::cache::Policy policy, std::string cache_control,
                   uint64_t started_tick)
        : cache_(std::move(cache)),
          url_(std::move(url)),
          headers_(std::move(headers)),
          policy_(policy),
          cache_control_(std::move(cache_control)),
          started_tick_(started_tick) {}

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, IStream* stream) override {
        if (FAILED(result) || !stream || !cache_) return S_OK;

        std::string body;
        if (!ReadStream(stream, &body) || body.empty()) return S_OK;

        std::wstring headers = headers_;
        headers += L"Content-Length: ";
        headers += std::to_wstring(body.size());
        headers += L"\r\n";

        cache_->Store(url_, headers, body, policy_, cache_control_, started_tick_);
        return S_OK;
    }

private:
    std::shared_ptr<moo::cache::AssetCache> cache_;
    std::wstring url_;
    std::wstring headers_;
    moo::cache::Policy policy_;
    std::string cache_control_;
    uint64_t started_tick_ = 0;
};

} // namespace

class Host::EnvHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
    EnvHandler(Host* host, std::shared_ptr<bool> alive)
        : host_(host), alive_(std::move(alive)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
        if (*alive_) host_->OnEnvironment(result, env);
        return S_OK;
    }

private:
    Host* host_;
    std::shared_ptr<bool> alive_;
};

class Host::ControllerHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    ControllerHandler(Host* host, std::shared_ptr<bool> alive)
        : host_(host), alive_(std::move(alive)) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
        if (*alive_) host_->OnController(result, controller);
        return S_OK;
    }

private:
    Host* host_;
    std::shared_ptr<bool> alive_;
};

class Host::NavStartingHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ICoreWebView2NavigationStartingEventHandler> {
public:
    NavStartingHandler(Host* host, std::shared_ptr<bool> alive)
        : host_(host), alive_(std::move(alive)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2NavigationStartingEventArgs* args) override {
        if (!*alive_) return S_OK;
        return host_->OnNavigationStarting(sender, args);
    }

private:
    Host* host_;
    std::shared_ptr<bool> alive_;
};

class Host::NewWindowHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ICoreWebView2NewWindowRequestedEventHandler> {
public:
    NewWindowHandler(Host* host, std::shared_ptr<bool> alive)
        : host_(host), alive_(std::move(alive)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2NewWindowRequestedEventArgs* args) override {
        if (!*alive_) return S_OK;
        return host_->OnNewWindow(sender, args);
    }

private:
    Host* host_;
    std::shared_ptr<bool> alive_;
};

class Host::ProcessFailedHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ICoreWebView2ProcessFailedEventHandler> {
public:
    ProcessFailedHandler(Host* host, std::shared_ptr<bool> alive)
        : host_(host), alive_(std::move(alive)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2ProcessFailedEventArgs* args) override {
        if (!*alive_) return S_OK;
        return host_->OnProcessFailed(sender, args);
    }

private:
    Host* host_;
    std::shared_ptr<bool> alive_;
};

class Host::DomLoadedHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ICoreWebView2DOMContentLoadedEventHandler> {
public:
    DomLoadedHandler(Host* host, std::shared_ptr<bool> alive)
        : host_(host), alive_(std::move(alive)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                     ICoreWebView2DOMContentLoadedEventArgs*) override {
        if (*alive_) host_->OnDomLoaded();
        return S_OK;
    }

private:
    Host* host_;
    std::shared_ptr<bool> alive_;
};

class Host::NavCompletedHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ICoreWebView2NavigationCompletedEventHandler> {
public:
    NavCompletedHandler(Host* host, std::shared_ptr<bool> alive)
        : host_(host), alive_(std::move(alive)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                     ICoreWebView2NavigationCompletedEventArgs* args) override {
        if (!*alive_) return S_OK;
        BOOL success = FALSE;
        COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
        args->get_IsSuccess(&success);
        args->get_WebErrorStatus(&status);
        host_->OnNavigationCompleted(success, status);
        return S_OK;
    }

private:
    Host* host_;
    std::shared_ptr<bool> alive_;
};

class Host::AcceleratorKeyHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ICoreWebView2AcceleratorKeyPressedEventHandler> {
public:
    AcceleratorKeyHandler(Host* host, std::shared_ptr<bool> alive)
        : host_(host), alive_(std::move(alive)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2Controller*,
                                     ICoreWebView2AcceleratorKeyPressedEventArgs* args) override {
        if (!*alive_) return S_OK;

        COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
        if (FAILED(args->get_KeyEventKind(&kind))) return S_OK;
        if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
            kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
            return S_OK;
        }

        UINT key = 0;
        if (FAILED(args->get_VirtualKey(&key))) return S_OK;

        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (key == VK_F11 || (key == VK_RETURN && alt)) {
            args->put_Handled(TRUE);
            host_->ToggleFullscreen();
        }
        return S_OK;
    }

private:
    Host* host_;
    std::shared_ptr<bool> alive_;
};

class Host::ResourceRequestedHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ICoreWebView2WebResourceRequestedEventHandler> {
public:
    ResourceRequestedHandler(Host* host, std::shared_ptr<bool> alive)
        : host_(host), alive_(std::move(alive)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2WebResourceRequestedEventArgs* args) override {
        if (!*alive_) return S_OK;
        return host_->OnResourceRequested(sender, args);
    }

private:
    Host* host_;
    std::shared_ptr<bool> alive_;
};

class Host::ResponseReceivedHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ICoreWebView2WebResourceResponseReceivedEventHandler> {
public:
    ResponseReceivedHandler(Host* host, std::shared_ptr<bool> alive)
        : host_(host), alive_(std::move(alive)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender,
                                     ICoreWebView2WebResourceResponseReceivedEventArgs* args) override {
        if (!*alive_) return S_OK;
        return host_->OnResponseReceived(sender, args);
    }

private:
    Host* host_;
    std::shared_ptr<bool> alive_;
};

Host::Host() : alive_(std::make_shared<bool>(true)) {}

Host::~Host() {
    *alive_ = false;
    Close();
}

bool Host::Initialize(HWND parent, std::wstring user_data_dir, std::wstring cache_dir,
                      std::wstring start_url, bool dev,
                      std::wstring browser_executable_folder) {
    parent_ = parent;
    start_url_ = std::move(start_url);
    browser_folder_ = std::move(browser_executable_folder);
    dev_ = dev;
    start_tick_ = GetTickCount64();

    cache_ = std::make_shared<moo::cache::AssetCache>();
    if (!cache_dir.empty() && !cache_->Open(cache_dir, 512ull * 1024 * 1024)) {
        moo::diag::Write(L"webview: asset cache disabled (%s)", cache_dir.c_str());
        cache_.reset();
    }
    if (cache_) {
        // Warm the index in memory before the first navigation; nothing else
        // touches the network here. Freshness is enforced per request.
        moo::diag::Write(L"webview: asset cache ready");
    }

    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (options) {
        // Performance flags: keep the renderer foregrounded while the game is
        // focused, keep GPU rasterization on, and stop Chromium's background
        // services (component updates, sync, breakpad) from spawning extra
        // processes. The game is a single page, so a 32 MB disk cache is plenty.
        options->put_AdditionalBrowserArguments(
            L"--disable-features=msWebOOUI,msPdfOOUI,UseEcoQoSForBackgroundProcess,"
            L"CalculateNativeWinOcclusion,SpareRendererForSitePerProcess,"
            L"OptimizationHints,OptimizationHintsFetching,Translate,MediaRouter,"
            L"GlobalMediaControls,HardwareMediaKeyHandling,AutofillServerCommunication,"
            L"InterestFeedContentSuggestions,PrivacySandboxSettings4,SegmentationPlatform "
            L"--enable-features=CanvasOopRasterization "
            L"--disable-background-timer-throttling "
            L"--disable-renderer-backgrounding "
            L"--disable-backgrounding-occluded-windows "
            L"--disable-background-networking "
            L"--disable-component-update "
            L"--disable-domain-reliability "
            L"--disable-client-side-phishing-detection "
            L"--disable-sync "
            L"--disable-default-apps "
            L"--disable-extensions "
            L"--disable-breakpad "
            L"--no-first-run "
            L"--no-default-browser-check "
            L"--no-proxy-server "
            L"--disk-cache-size=33554432 "
            L"--js-flags=--max-old-space-size=512 "
            L"--enable-gpu-rasterization "
            L"--enable-zero-copy "
            L"--force-high-performance-gpu "
            L"--use-angle=d3d11");
        // No OS-account SSO: one less identity network path on startup.
        options->put_AllowSingleSignOnUsingOSPrimaryAccount(FALSE);
    }

    const wchar_t* folder = browser_folder_.empty() ? nullptr : browser_folder_.c_str();
    ComPtr<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> handler =
        Microsoft::WRL::Make<EnvHandler>(this, alive_);
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        folder, user_data_dir.c_str(), options.Get(), handler.Get());
    if (FAILED(hr)) {
        Fail(L"CreateCoreWebView2EnvironmentWithOptions", hr);
        return false;
    }
    moo::diag::Write(L"webview: environment requested (udf=%s, runtime=%s)",
                     user_data_dir.c_str(),
                     browser_folder_.empty() ? L"evergreen" : browser_folder_.c_str());
    return true;
}

void Host::OnEnvironment(HRESULT result, ICoreWebView2Environment* env) {
    if (FAILED(result) || !env) {
        Fail(L"CoreWebView2 environment creation", result);
        return;
    }
    env_ = env;

    LPWSTR version = nullptr;
    if (SUCCEEDED(env_->get_BrowserVersionString(&version)) && version) {
        moo::diag::Write(L"webview: runtime %s", version);
        CoTaskMemFree(version);
    }

    ComPtr<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> handler =
        Microsoft::WRL::Make<ControllerHandler>(this, alive_);
    const HRESULT hr = env_->CreateCoreWebView2Controller(parent_, handler.Get());
    if (FAILED(hr)) Fail(L"CreateCoreWebView2Controller", hr);
}

void Host::OnController(HRESULT result, ICoreWebView2Controller* controller) {
    if (FAILED(result) || !controller) {
        Fail(L"CoreWebView2 controller creation", result);
        return;
    }
    controller_ = controller;

    if (FAILED(controller_->get_CoreWebView2(&webview_)) || !webview_) {
        Fail(L"get_CoreWebView2", E_FAIL);
        return;
    }
    webview_.As(&webview2_);
    webview_.As(&webview19_);

    // Serve the changelog background from a local folder instead of embedding a
    // multi-megabyte data URL in the injected script.
    ComPtr<ICoreWebView2_3> webview3;
    if (SUCCEEDED(webview_.As(&webview3))) {
        const std::wstring assets = EnsureAsset(IDR_DOCVIEW_BG, L"changelog-bg.png");
        if (!assets.empty()) {
            const HRESULT mapping = webview3->SetVirtualHostNameToFolderMapping(
                L"mooclient.local", assets.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
            moo::diag::Write(L"webview: document background %s (0x%08lX)",
                             SUCCEEDED(mapping) ? L"mapped" : L"FAILED",
                             static_cast<unsigned long>(mapping));
        } else {
            moo::diag::Write(L"webview: document background asset missing");
        }
    }

    RECT bounds{};
    GetClientRect(parent_, &bounds);
    controller_->put_Bounds(bounds);
    controller_->put_IsVisible(TRUE);

    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(controller_.As(&controller2))) {
        const COREWEBVIEW2_COLOR background{255, 17, 17, 19};
        controller2->put_DefaultBackgroundColor(background);
    }

    ApplySettings();

    // Styled document view for plain-text pages (changelog), injected before
    // any game document runs. The script itself no-ops outside text/plain.
    const std::wstring doc_view = LoadTextResource(IDR_DOCVIEW);
    if (!doc_view.empty()) {
        auto handler = Microsoft::WRL::Make<ScriptInstalledHandler>();
        const HRESULT hr = webview_->AddScriptToExecuteOnDocumentCreated(
            doc_view.c_str(), handler.Get());
        moo::diag::Write(L"webview: document view script %s (%zu chars)",
                         SUCCEEDED(hr) ? L"installed" : L"FAILED", doc_view.size());
    } else {
        moo::diag::Write(L"webview: document view script resource missing");
    }

    nav_handler_ = Microsoft::WRL::Make<NavStartingHandler>(this, alive_);
    new_window_handler_ = Microsoft::WRL::Make<NewWindowHandler>(this, alive_);
    process_failed_handler_ = Microsoft::WRL::Make<ProcessFailedHandler>(this, alive_);
    dom_loaded_handler_ = Microsoft::WRL::Make<DomLoadedHandler>(this, alive_);
    nav_completed_handler_ = Microsoft::WRL::Make<NavCompletedHandler>(this, alive_);
    accelerator_handler_ = Microsoft::WRL::Make<AcceleratorKeyHandler>(this, alive_);

    webview_->add_NavigationStarting(nav_handler_.Get(), &nav_token_);
    webview_->add_NewWindowRequested(new_window_handler_.Get(), &new_window_token_);
    webview_->add_ProcessFailed(process_failed_handler_.Get(), &process_failed_token_);
    if (webview2_) webview2_->add_DOMContentLoaded(dom_loaded_handler_.Get(), &dom_loaded_token_);
    webview_->add_NavigationCompleted(nav_completed_handler_.Get(), &nav_completed_token_);
    controller_->add_AcceleratorKeyPressed(accelerator_handler_.Get(), &accelerator_token_);

    if (cache_ && cache_->opened()) {
        // Only the game origin is filtered. Everything else (api.moomoo.io,
        // Turnstile, the ad/consent iframes) keeps its normal network path.
        resource_handler_ = Microsoft::WRL::Make<ResourceRequestedHandler>(this, alive_);
        response_handler_ = Microsoft::WRL::Make<ResponseReceivedHandler>(this, alive_);

        webview_->AddWebResourceRequestedFilter(L"https://moomoo.io/*",
                                                COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        webview_->AddWebResourceRequestedFilter(L"https://www.moomoo.io/*",
                                                COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        webview_->add_WebResourceRequested(resource_handler_.Get(), &resource_token_);
        if (webview2_) {
            webview2_->add_WebResourceResponseReceived(response_handler_.Get(),
                                                       &response_token_);
        }
        moo::diag::Write(L"webview: asset cache attached");
    }

    moo::diag::Write(L"webview: controller ready, navigating to %s", start_url_.c_str());
    webview_->Navigate(start_url_.c_str());
}

void Host::ApplySettings() {
    ComPtr<ICoreWebView2Settings> settings;
    if (FAILED(webview_->get_Settings(&settings)) || !settings) return;

    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsBuiltInErrorPageEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    if (!dev_) settings->put_AreDevToolsEnabled(FALSE);

    ComPtr<ICoreWebView2Settings3> settings3;
    if (SUCCEEDED(settings.As(&settings3)) && !dev_) {
        settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    }

    ComPtr<ICoreWebView2Settings4> settings4;
    if (SUCCEEDED(settings.As(&settings4))) {
        settings4->put_IsPasswordAutosaveEnabled(FALSE);
        settings4->put_IsGeneralAutofillEnabled(FALSE);
    }

    ComPtr<ICoreWebView2Settings5> settings5;
    if (SUCCEEDED(settings.As(&settings5))) {
        settings5->put_IsPinchZoomEnabled(FALSE);
    }

    ComPtr<ICoreWebView2Settings6> settings6;
    if (SUCCEEDED(settings.As(&settings6))) {
        settings6->put_IsSwipeNavigationEnabled(FALSE);
    }

    ComPtr<ICoreWebView2Settings8> settings8;
    if (SUCCEEDED(settings.As(&settings8))) {
        // The game is a known origin; skip SmartScreen reputation lookups.
        settings8->put_IsReputationCheckingRequired(FALSE);
    }
}

HRESULT Host::OnNavigationStarting(ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
    LPWSTR raw = nullptr;
    if (FAILED(args->get_Uri(&raw)) || !raw) return S_OK;
    const std::wstring uri = TakeUri(raw);

    if (moo::nav::IsGameUri(uri)) {
        moo::diag::Write(L"nav: allow %s", uri.c_str());
        return S_OK;
    }

    args->put_Cancel(TRUE);

    if (moo::nav::IsSilentScheme(uri)) {
        moo::diag::Write(L"nav: silently cancelled %s", uri.c_str());
        return S_OK;
    }

    moo::diag::Write(L"nav: blocked in-app navigation, asking user: %s", uri.c_str());
    RequestExternal(uri);
    return S_OK;
}

HRESULT Host::OnNewWindow(ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
    LPWSTR raw = nullptr;
    if (FAILED(args->get_Uri(&raw)) || !raw) return S_OK;
    const std::wstring uri = TakeUri(raw);

    args->put_Handled(TRUE);

    if (moo::nav::IsGameUri(uri)) {
        // Same-origin popups stay inside the single game surface.
        moo::diag::Write(L"nav: redirecting same-origin popup into main view: %s", uri.c_str());
        if (webview_) webview_->Navigate(uri.c_str());
        return S_OK;
    }

    if (moo::nav::IsSilentScheme(uri)) {
        moo::diag::Write(L"nav: silently dropped popup %s", uri.c_str());
        return S_OK;
    }

    moo::diag::Write(L"nav: blocked popup, asking user: %s", uri.c_str());
    RequestExternal(uri);
    return S_OK;
}

HRESULT Host::OnProcessFailed(ICoreWebView2*,
                              ICoreWebView2ProcessFailedEventArgs* args) {
    COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
    args->get_ProcessFailedKind(&kind);
    moo::diag::Write(L"webview: process failed, kind=%d", static_cast<int>(kind));

    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
        MessageBoxW(parent_,
                    L"The WebView2 browser process exited.\n\nMooClient will close. Please restart.",
                    L"MooClient", MB_ICONERROR | MB_OK);
        PostMessageW(parent_, WM_CLOSE, 0, 0);
    }
    return S_OK;
}

void Host::OnDomLoaded() {
    moo::diag::Write(L"webview: DOMContentLoaded +%llu ms", GetTickCount64() - start_tick_);
    PostMessageW(parent_, kMsgReady, 0, 0);
}

void Host::OnNavigationCompleted(BOOL success, COREWEBVIEW2_WEB_ERROR_STATUS status) {
    moo::diag::Write(L"webview: navigation completed success=%d status=%d +%llu ms",
                     static_cast<int>(success), static_cast<int>(status),
                     GetTickCount64() - start_tick_);
    if (!success) PostMessageW(parent_, kMsgReady, 0, 0);
}

HRESULT Host::OnResourceRequested(ICoreWebView2*,
                                 ICoreWebView2WebResourceRequestedEventArgs* args) {
    if (!cache_ || !cache_->opened() || !env_) return S_OK;

    ComPtr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(&request)) || !request) return S_OK;

    LPWSTR raw_uri = nullptr;
    if (FAILED(request->get_Uri(&raw_uri)) || !raw_uri) return S_OK;
    const std::wstring uri = TakeUri(raw_uri);

    const moo::cache::Policy policy = moo::cache::Decide(uri);
    if (policy.tier == moo::cache::Tier::Never) return S_OK;

    // Partial content is not modelled; let Chromium handle range requests.
    ComPtr<ICoreWebView2HttpRequestHeaders> request_headers;
    if (SUCCEEDED(request->get_Headers(&request_headers)) && request_headers) {
        BOOL has_range = FALSE;
        if (SUCCEEDED(request_headers->Contains(L"Range", &has_range)) && has_range) return S_OK;
    }

    if (policy.tier == moo::cache::Tier::RevalidateOnce &&
        !session_validated_.contains(uri)) {
        // First touch this run goes to the network so an in-place update is
        // picked up; the capture path then refreshes the stored copy.
        session_validated_.insert(uri);
        request_started_[uri] = GetTickCount64();
        cache_->RecordMiss();
        return S_OK;
    }

    moo::cache::Hit hit;
    if (!cache_->Lookup(uri, policy, &hit)) {
        request_started_[uri] = GetTickCount64();
        cache_->RecordMiss();
        return S_OK;
    }

    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(hit.path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
                                      FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream)) ||
        !stream) {
        cache_->Drop(uri);
        return S_OK;
    }

    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (FAILED(env_->CreateWebResourceResponse(stream.Get(), 200, L"OK", hit.headers.c_str(),
                                               &response)) ||
        !response) {
        return S_OK;
    }
    if (FAILED(args->put_Response(response.Get()))) return S_OK;

    cache_->Touch(uri);
    cache_->RecordHit(hit.size);
    served_[uri] += 1;
    return S_OK;
}

HRESULT Host::OnResponseReceived(ICoreWebView2*,
                                 ICoreWebView2WebResourceResponseReceivedEventArgs* args) {
    if (!cache_ || !cache_->opened()) return S_OK;

    ComPtr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(&request)) || !request) return S_OK;

    LPWSTR raw_uri = nullptr;
    if (FAILED(request->get_Uri(&raw_uri)) || !raw_uri) return S_OK;
    const std::wstring uri = TakeUri(raw_uri);

    // A response we produced ourselves: do not round-trip it back into the store.
    const auto served = served_.find(uri);
    if (served != served_.end()) {
        if (--served->second <= 0) served_.erase(served);
        return S_OK;
    }

    const moo::cache::Policy policy = moo::cache::Decide(uri);
    if (policy.tier == moo::cache::Tier::Never) return S_OK;

    ComPtr<ICoreWebView2WebResourceResponseView> response;
    if (FAILED(args->get_Response(&response)) || !response) return S_OK;

    int status = 0;
    if (FAILED(response->get_StatusCode(&status))) return S_OK;

    // "Your copy is still good": restart the TTL without touching the blob.
    if (status == 304) {
        cache_->MarkFresh(uri);
        return S_OK;
    }

    if (status == 404 || status == 410) {
        cache_->Drop(uri);
        return S_OK;
    }

    if (status != 200) return S_OK;

    ComPtr<ICoreWebView2HttpResponseHeaders> headers;
    if (FAILED(response->get_Headers(&headers)) || !headers) return S_OK;

    const std::string cache_control = Narrow(GetHeader(headers.Get(), L"Cache-Control"));
    if (moo::cache::IsUncacheable(cache_control)) return S_OK;

    uint64_t started = GetTickCount64();
    const auto recorded = request_started_.find(uri);
    if (recorded != request_started_.end()) {
        started = recorded->second;
        request_started_.erase(recorded);
    }

    auto handler = Microsoft::WRL::Make<CaptureHandler>(cache_, uri,
                                                        BuildReplayHeaders(headers.Get()), policy,
                                                        cache_control, started);
    response->GetContent(handler.Get());
    return S_OK;
}

void Host::RefreshAssets() {
    if (cache_) cache_->PurgeMutable();
    if (webview_) webview_->Reload();
}

void Host::RequestExternal(const std::wstring& uri) {
    auto* copy = new std::wstring(uri);
    if (!PostMessageW(parent_, kMsgExternal, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
        moo::diag::Write(L"nav: PostMessage failed for %s", uri.c_str());
    }
}

void Host::SetFullscreenCallback(std::function<void()> callback) {
    on_toggle_fullscreen_ = std::move(callback);
}

void Host::ToggleFullscreen() {
    if (on_toggle_fullscreen_) on_toggle_fullscreen_();
}

void Host::SetMemoryUsageTargetLevel(bool low) {
    if (!webview19_) return;
    webview19_->put_MemoryUsageTargetLevel(
        low ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW
            : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
    moo::diag::Write(L"webview: memory target %s", low ? L"low" : L"normal");
}

void Host::Resize(int width, int height) {
    if (!controller_) return;
    RECT bounds{0, 0, width, height};
    controller_->put_Bounds(bounds);
}

void Host::Focus() {
    if (controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void Host::Close() {
    if (closed_) return;
    closed_ = true;
    *alive_ = false;

    if (webview_) {
        webview_->remove_NavigationStarting(nav_token_);
        webview_->remove_NewWindowRequested(new_window_token_);
        webview_->remove_ProcessFailed(process_failed_token_);
        webview_->remove_NavigationCompleted(nav_completed_token_);
        if (resource_handler_) webview_->remove_WebResourceRequested(resource_token_);
    }
    if (webview2_) {
        webview2_->remove_DOMContentLoaded(dom_loaded_token_);
        if (response_handler_) webview2_->remove_WebResourceResponseReceived(response_token_);
    }
    if (controller_) controller_->remove_AcceleratorKeyPressed(accelerator_token_);

    nav_handler_.Reset();
    new_window_handler_.Reset();
    process_failed_handler_.Reset();
    dom_loaded_handler_.Reset();
    nav_completed_handler_.Reset();
    accelerator_handler_.Reset();
    resource_handler_.Reset();
    response_handler_.Reset();

    if (cache_) {
        const auto stats = cache_->stats();
        moo::diag::Write(L"cache: hits=%llu misses=%llu stores=%llu evictions=%llu "
                         L"revalidated=%llu served=%.1f MB stored=%.1f MB entries=%llu",
                         static_cast<unsigned long long>(stats.hits),
                         static_cast<unsigned long long>(stats.misses),
                         static_cast<unsigned long long>(stats.stores),
                         static_cast<unsigned long long>(stats.evictions),
                         static_cast<unsigned long long>(stats.revalidated),
                         static_cast<double>(stats.bytes_served) / (1024.0 * 1024.0),
                         static_cast<double>(stats.bytes_stored) / (1024.0 * 1024.0),
                         static_cast<unsigned long long>(stats.entries));
        cache_->Flush();
        cache_.reset();
    }

    if (controller_) {
        controller_->Close();
        controller_.Reset();
    }
    webview_.Reset();
    webview2_.Reset();
    env_.Reset();
}

void Host::Fail(const wchar_t* what, HRESULT hr) {
    wchar_t detail[512]{};
    LPWSTR version = nullptr;
    GetAvailableCoreWebView2BrowserVersionString(
        browser_folder_.empty() ? nullptr : browser_folder_.c_str(), &version);

    swprintf_s(detail,
               L"%s failed.\n\nHRESULT: 0x%08lX\nWebView2 runtime: %s\n\n"
               L"Install or repair the Microsoft Edge WebView2 Runtime and try again.",
               what, static_cast<unsigned long>(hr), version ? version : L"(not found)");
    if (version) CoTaskMemFree(version);

    moo::diag::Write(L"error: %s (0x%08lX)", what, static_cast<unsigned long>(hr));
    // Make sure the shell window exists before a modal dialog is parented to it.
    PostMessageW(parent_, kMsgReady, 0, 0);
    MessageBoxW(parent_, detail, L"MooClient", MB_ICONERROR | MB_OK);
}

} // namespace moo::web
