#include "cache/asset_policy.h"

#include <windows.h>

namespace moo::cache {
namespace {

std::wstring Lower(std::wstring value) {
    if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

bool StartsWith(const std::wstring& value, const wchar_t* prefix) {
    const size_t length = wcslen(prefix);
    return value.size() >= length && _wcsnicmp(value.c_str(), prefix, length) == 0;
}

} // namespace

Policy Decide(const std::wstring& url) {
    Policy policy;

    // Hand-rolled parsing: this runs for every request on the origin, and IUri
    // would add a COM round trip per call.
    if (!StartsWith(url, L"https://")) return policy;

    constexpr size_t kAuthorityStart = 8; // sizeof("https://") - 1
    const size_t path_start = url.find(L'/', kAuthorityStart);
    if (path_start == std::wstring::npos) return policy;

    std::wstring host = Lower(url.substr(kAuthorityStart, path_start - kAuthorityStart));
    const size_t at = host.rfind(L'@');
    if (at != std::wstring::npos) host.erase(0, at + 1);
    const size_t colon = host.find(L':');
    if (colon != std::wstring::npos) host.erase(colon);

    if (host != L"moomoo.io" && host != L"www.moomoo.io") return policy;

    size_t path_end = url.size();
    const size_t query = url.find(L'?', path_start);
    if (query != std::wstring::npos) path_end = query;
    const size_t fragment = url.find(L'#', path_start);
    if (fragment != std::wstring::npos && fragment < path_end) path_end = fragment;

    const std::wstring path = Lower(url.substr(path_start, path_end - path_start));

    // Vite emits content hashes in the bundle names; a new build cannot reuse
    // one, so these are safe to serve from disk forever.
    if (path.starts_with(L"/assets/")) {
        policy.tier = Tier::Immutable;
        return policy;
    }

    // The sealed protocol modules are not hashed, and a stale one breaks the
    // game. Two small files: check them on the wire once per launch.
    if (path.starts_with(L"/p/")) {
        policy.tier = Tier::RevalidateOnce;
        return policy;
    }

    return policy;
}

bool IsUncacheable(const std::string& cache_control) {
    std::string lowered;
    lowered.reserve(cache_control.size());
    for (const char c : cache_control) {
        lowered.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    }
    return lowered.find("no-store") != std::string::npos ||
           lowered.find("private") != std::string::npos;
}

} // namespace moo::cache
