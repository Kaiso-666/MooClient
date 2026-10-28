#pragma once

#include <windows.h>

namespace moo::shell {

// Borderless, click-through, per-pixel-alpha splash shown while WebView2 starts
// and the game document loads. Destroyed as soon as the main window is ready.
class Splash {
public:
    ~Splash();

    bool Show(HINSTANCE instance);
    void Close();

    bool visible() const { return hwnd_ != nullptr; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    HWND hwnd_ = nullptr;
};

} // namespace moo::shell
