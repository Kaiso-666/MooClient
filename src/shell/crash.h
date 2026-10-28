#pragma once

namespace moo::shell {

// Installs an unhandled-exception filter that writes a minidump to
// %LOCALAPPDATA%\MooClient\crashes\ and logs the path.
void InstallCrashHandler();

} // namespace moo::shell
