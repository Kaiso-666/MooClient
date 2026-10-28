#pragma once

namespace moo::shell {

// Puts the process (and the WebView2 children that inherit it) in a job with
// ABOVE_NORMAL priority and opts out of EcoQoS so the renderer stays on P-cores.
void ApplyProcessTuning();

} // namespace moo::shell
