#pragma once

#include <windows.h>

#include <string>

namespace moo::nav {

// True only for https://moomoo.io/ and https://www.moomoo.io/ (any path).
bool IsGameUri(const std::wstring& uri);

// Schemes that must never reach the shell (about:, blob:, data:, javascript:,
// file:) and must not produce a prompt either.
bool IsSilentScheme(const std::wstring& uri);

// Lower-cased host, or empty when the URI cannot be parsed.
std::wstring HostOf(const std::wstring& uri);

// Shows the "open in browser?" confirmation and, if accepted, hands the URI to
// the default browser. Returns true when the browser was launched.
bool HandleExternal(HWND owner, const std::wstring& uri);

// Per-host "remember" rules (in-memory for M0; SQLite in M2).
bool IsRemembered(const std::wstring& host);
void Remember(const std::wstring& host);

} // namespace moo::nav
