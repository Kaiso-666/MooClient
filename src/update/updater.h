#pragma once

#include <windows.h>

#include <atomic>
#include <string>

namespace moo::update {

// Posted to the shell window with a heap-allocated Info* when a newer release
// exists. The receiver owns it.
inline constexpr UINT kMsgUpdateAvailable = WM_APP + 3;

struct Info {
    std::wstring version;   // "0.2.0" (tag without the leading v)
    std::wstring installer; // direct download URL of the setup .exe
    std::wstring page;      // release page, for a "what's new" link
    std::wstring notes;     // release body, trimmed
};

// Owner/repo the update check talks to. Overridable at build time.
#ifndef MOO_UPDATE_REPO
#define MOO_UPDATE_REPO "Kaiso-666/MooClient"
#endif

// Compares dotted versions; "v1.2.3" and "1.2.3" are equivalent. Missing
// components count as zero, so 1.2 == 1.2.0.
bool IsNewer(const std::wstring& candidate, const std::wstring& current);

// Starts a background check. On success with a newer release it posts
// kMsgUpdateAvailable to `notify`. Never blocks the caller.
void CheckAsync(HWND notify, std::wstring current_version);

// Downloads the installer to %TEMP% and launches it. Blocking; call from a
// worker thread. `cancelled` is polled between chunks.
bool DownloadInstaller(const Info& info, std::atomic<bool>* cancelled, std::wstring* path,
                       std::wstring* error);

} // namespace moo::update
