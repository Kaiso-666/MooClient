#pragma once

namespace moo::shell {

// Creates the named single-instance mutex. If another instance already owns it,
// brings that window to the foreground and returns false.
bool AcquireSingleInstance();

} // namespace moo::shell
