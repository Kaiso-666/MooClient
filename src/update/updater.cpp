#include "update/updater.h"

#include <winhttp.h>

#include <cstdio>
#include <thread>
#include <vector>

#include "diag/log.h"
#include "update/json.h"

#pragma comment(lib, "winhttp.lib")

namespace moo::update {
namespace {

constexpr size_t kMaxApiBytes = 1024 * 1024;
constexpr size_t kMaxInstallerBytes = 256ull * 1024 * 1024;

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

struct Response {
    int status = 0;
    std::string body;
};

// Synchronous GET. `path_and_query` is UTF-8, e.g. "/repos/a/b/releases/latest".
bool HttpGet(HINTERNET session, const wchar_t* host, const std::wstring& path_and_query,
             size_t max_bytes, Response* out, std::wstring* error) {
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        *error = L"connect failed";
        return false;
    }

    HINTERNET request =
        WinHttpOpenRequest(connection, L"GET", path_and_query.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connection);
        *error = L"request creation failed";
        return false;
    }

    const wchar_t* headers =
        L"User-Agent: MooClient\r\n"
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    const DWORD headers_length = static_cast<DWORD>(wcslen(headers));

    bool ok = false;
    if (!WinHttpSendRequest(request, headers, headers_length, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        *error = L"no response";
    } else {
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            *error = L"no status";
        } else {
            out->status = static_cast<int>(status);
            out->body.clear();
            ok = true;
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                if (out->body.size() + available > max_bytes) {
                    ok = false;
                    *error = L"response too large";
                    break;
                }
                const size_t offset = out->body.size();
                out->body.resize(offset + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, out->body.data() + offset, available, &read)) {
                    ok = false;
                    *error = L"read failed";
                    break;
                }
                out->body.resize(offset + read);
                if (read == 0) break;
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    return ok;
}

std::wstring TempDir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) return L".";
    return buffer;
}

std::wstring FileNameOf(const std::wstring& url) {
    const size_t slash = url.find_last_of(L'/');
    std::wstring name = slash == std::wstring::npos ? url : url.substr(slash + 1);
    const size_t query = name.find(L'?');
    if (query != std::wstring::npos) name.erase(query);
    return name;
}

void CheckThread(HWND notify, std::wstring current) {
    HINTERNET session = WinHttpOpen(L"MooClient/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        moo::diag::Write(L"update: WinHttpOpen failed");
        return;
    }
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 8000);

    std::wstring path = L"/repos/" + Widen(MOO_UPDATE_REPO) + L"/releases/latest";
    Response response;
    std::wstring error;
    if (!HttpGet(session, L"api.github.com", path, kMaxApiBytes, &response, &error)) {
        moo::diag::Write(L"update: check failed (%s)", error.c_str());
        WinHttpCloseHandle(session);
        return;
    }
    WinHttpCloseHandle(session);

    if (response.status == 404) {
        moo::diag::Write(L"update: no releases published yet");
        return;
    }
    if (response.status != 200) {
        moo::diag::Write(L"update: api status %d", response.status);
        return;
    }

    Json root;
    if (!ParseJson(response.body, &root)) {
        moo::diag::Write(L"update: release json did not parse");
        return;
    }

    std::wstring tag;
    if (const Json* tag_value = root.Find("tag_name")) {
        if (const std::string* text = tag_value->AsString()) tag = Widen(*text);
    }
    if (tag.empty()) {
        moo::diag::Write(L"update: release has no tag");
        return;
    }

    std::wstring version = tag;
    if (!version.empty() && (version[0] == L'v' || version[0] == L'V')) version.erase(0, 1);

    if (!IsNewer(version, current)) {
        moo::diag::Write(L"update: %s is current (latest %s)", current.c_str(), version.c_str());
        return;
    }

    auto* info = new Info();
    info->version = version;
    if (const Json* page = root.Find("html_url")) {
        if (const std::string* value = page->AsString()) info->page = Widen(*value);
    }
    if (const Json* body = root.Find("body")) {
        if (const std::string* value = body->AsString()) info->notes = Widen(*value);
    }

    if (const Json* assets = root.Find("assets"); assets && assets->IsArray()) {
        for (const Json& asset : assets->items) {
            const Json* url = asset.Find("browser_download_url");
            if (!url) continue;
            const std::string* value = url->AsString();
            if (!value) continue;
            const std::wstring wide = Widen(*value);
            if (FileNameOf(wide).ends_with(L".exe")) {
                info->installer = wide;
                break;
            }
        }
    }

    if (info->installer.empty()) {
        moo::diag::Write(L"update: %s has no installer asset", version.c_str());
        delete info;
        return;
    }

    moo::diag::Write(L"update: %s available (running %s)", version.c_str(), current.c_str());
    if (!PostMessageW(notify, kMsgUpdateAvailable, 0, reinterpret_cast<LPARAM>(info))) {
        delete info;
    }
}

} // namespace

bool IsNewer(const std::wstring& candidate, const std::wstring& current) {
    auto parts = [](const std::wstring& value) {
        std::vector<int> out;
        int number = 0;
        bool any = false;
        for (const wchar_t c : value) {
            if (c >= L'0' && c <= L'9') {
                number = number * 10 + (c - L'0');
                if (number > 100000) return std::vector<int>{};
                any = true;
            } else if (c == L'.') {
                out.push_back(any ? number : 0);
                number = 0;
                any = false;
            } else {
                break;
            }
        }
        if (any) out.push_back(number);
        return out;
    };

    const std::vector<int> a = parts(candidate);
    const std::vector<int> b = parts(current);
    const size_t count = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < count; ++i) {
        const int left = i < a.size() ? a[i] : 0;
        const int right = i < b.size() ? b[i] : 0;
        if (left != right) return left > right;
    }
    return false;
}

void CheckAsync(HWND notify, std::wstring current_version) {
    if (!notify) return;
    std::thread([notify, current_version = std::move(current_version)] {
        CheckThread(notify, current_version);
    }).detach();
}

bool DownloadInstaller(const Info& info, std::atomic<bool>* cancelled, std::wstring* path,
                       std::wstring* error) {
    if (!path || !error) return false;

    HINTERNET session = WinHttpOpen(L"MooClient/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        *error = L"could not open a network session";
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 15000);

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(info.installer.c_str(), static_cast<DWORD>(info.installer.size()), 0,
                         &components)) {
        WinHttpCloseHandle(session);
        *error = L"the download URL is not valid";
        return false;
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring target(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        target.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    HINTERNET request =
        connection ? WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        components.nScheme == INTERNET_SCHEME_HTTPS
                                            ? WINHTTP_FLAG_SECURE
                                            : 0)
                   : nullptr;
    if (!request) {
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        *error = L"could not start the download";
        return false;
    }

    const wchar_t* headers = L"User-Agent: MooClient\r\nAccept: */*\r\n";
    bool ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(wcslen(headers)),
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (!ok) *error = L"the server did not respond";

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (ok) {
        ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                 WINHTTP_NO_HEADER_INDEX) &&
             status == 200;
        if (!ok) *error = L"the download returned HTTP " + std::to_wstring(status);
    }

    std::wstring name = FileNameOf(info.installer);
    if (name.empty() || !name.ends_with(L".exe")) name = L"MooClientSetup.exe";
    const std::wstring destination = TempDir() + L"MooClient-" + name;

    HANDLE file = INVALID_HANDLE_VALUE;
    if (ok) {
        file = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            ok = false;
            *error = L"could not create the download file";
        }
    }

    size_t total = 0;
    std::vector<char> buffer(64 * 1024);
    while (ok) {
        if (cancelled && cancelled->load()) {
            ok = false;
            *error = L"cancelled";
            break;
        }

        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        if (total + available > kMaxInstallerBytes) {
            ok = false;
            *error = L"the installer is unexpectedly large";
            break;
        }

        DWORD read = 0;
        const DWORD wanted = available < buffer.size() ? available
                                                       : static_cast<DWORD>(buffer.size());
        if (!WinHttpReadData(request, buffer.data(), wanted, &read) || read == 0) break;

        DWORD written = 0;
        if (!WriteFile(file, buffer.data(), read, &written, nullptr) || written != read) {
            ok = false;
            *error = L"could not write the installer";
            break;
        }
        total += read;
    }

    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!ok) {
        DeleteFileW(destination.c_str());
        return false;
    }
    if (total == 0) {
        DeleteFileW(destination.c_str());
        *error = L"the download was empty";
        return false;
    }

    moo::diag::Write(L"update: downloaded %llu bytes to %s",
                     static_cast<unsigned long long>(total), destination.c_str());
    *path = destination;
    return true;
}

} // namespace moo::update
