#include "shell/splash.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

#include "diag/log.h"
#include "resource.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

namespace moo::shell {
namespace {

constexpr wchar_t kClass[] = L"MooClientSplash";
constexpr int kLogicalWidth = 560;

bool LoadResourceBytes(HINSTANCE instance, int id, const void** data, DWORD* size) {
    const HRSRC res = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    const HGLOBAL handle = LoadResource(instance, res);
    if (!handle) return false;
    *data = LockResource(handle);
    *size = SizeofResource(instance, res);
    return *data && *size;
}

bool CreatePremultipliedBitmap(IWICImagingFactory* factory, const void* data, DWORD size,
                               int target_w, int* out_w, int* out_h, HBITMAP* bitmap,
                               void** bits) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return false;

    void* dst = GlobalLock(mem);
    if (!dst) {
        GlobalFree(mem);
        return false;
    }
    std::memcpy(dst, data, size);
    GlobalUnlock(mem);

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
        GlobalFree(mem);
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnLoad, &decoder))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;

    UINT source_w = 0;
    UINT source_h = 0;
    if (FAILED(frame->GetSize(&source_w, &source_h)) || !source_w || !source_h) return false;
    const int target_h =
        std::max(1, MulDiv(target_w, static_cast<int>(source_h), static_cast<int>(source_w)));
    *out_w = target_w;
    *out_h = target_h;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler))) return false;
    if (FAILED(scaler->Initialize(converter.Get(), target_w, target_h,
                                  WICBitmapInterpolationModeFant))) {
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = target_w;
    bmi.bmiHeader.biHeight = -target_h; // top-down, as UpdateLayeredWindow expects
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    *bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!*bitmap || !*bits) return false;

    const UINT stride = static_cast<UINT>(target_w) * 4;
    return SUCCEEDED(scaler->CopyPixels(nullptr, stride, stride * target_h,
                                        static_cast<BYTE*>(*bits)));
}

} // namespace

Splash::~Splash() {
    Close();
}

bool Splash::Show(HINSTANCE instance) {
    if (hwnd_) return true;

    const void* data = nullptr;
    DWORD size = 0;
    if (!LoadResourceBytes(instance, IDR_SPLASH, &data, &size)) {
        moo::diag::Write(L"splash: RCDATA %d missing", IDR_SPLASH);
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        moo::diag::Write(L"splash: WIC unavailable");
        return false;
    }

    const int requested_width = MulDiv(kLogicalWidth, static_cast<int>(GetDpiForSystem()), 96);
    int width = 0;
    int height = 0;

    HBITMAP bitmap = nullptr;
    void* bits = nullptr;
    if (!CreatePremultipliedBitmap(factory.Get(), data, size, requested_width, &width, &height,
                                   &bitmap, &bits)) {
        moo::diag::Write(L"splash: decode failed");
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Splash::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DeleteObject(bitmap);
        return false;
    }

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - width) / 2;
    const int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - height) / 2;

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
            WS_EX_NOACTIVATE,
        kClass, L"", WS_POPUP, x, y, width, height, nullptr, nullptr, instance, this);
    if (!hwnd_) {
        DeleteObject(bitmap);
        return false;
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    const HGDIOBJ old = SelectObject(mem, bitmap);

    POINT src{0, 0};
    SIZE window_size{width, height};
    POINT dst{x, y};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL ok =
        UpdateLayeredWindow(hwnd_, screen, &dst, &window_size, mem, &src, 0, &blend, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    DeleteObject(bitmap);

    if (!ok) {
        moo::diag::Write(L"splash: UpdateLayeredWindow failed (%lu)", GetLastError());
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    moo::diag::Write(L"splash: shown %dx%d at %d,%d", width, height, x, y);
    return true;
}

void Splash::Close() {
    if (!hwnd_) return;
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    moo::diag::Write(L"splash: closed");
}

LRESULT CALLBACK Splash::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace moo::shell
