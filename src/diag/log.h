#pragma once

#include <string>

namespace moo::diag {

// Creates %LOCALAPPDATA%\MooClient\logs\client.log (or reuses it) and writes a
// session banner. Safe to call once, early in wWinMain.
void Init();

// Flushes and closes the log.
void Shutdown();

// printf-style, wide. Adds a timestamp and a trailing newline. Thread-safe.
void Write(const wchar_t* fmt, ...);

// Full path of the log file currently in use.
const std::wstring& Path();

} // namespace moo::diag
