#include "stdafx.h"
#include "config.hpp"
#include "feature.hpp"

static Config::Value nDisplayMode("Graphics", "DisplayMode", 0);

namespace
{
    enum { Stock, Borderless, Windowed };

    constexpr ptrdiff_t Fullscreen   = 0x00d0;  // RSDisplayMgr, taken from the options.xml FullScreen flag
    constexpr ptrdiff_t GameWindow   = 0x00b8;
    constexpr ptrdiff_t DeviceWindow = 0x92ac;  // RSDirect3DRenderer

    SafetyHookInline shCreateGameWindow{};
    SafetyHookInline shSetResolution{};

    bool MonitorRect(HWND hWnd, RECT& rect)
    {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);

        if (hWnd == nullptr || !GetMonitorInfoW(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &monitor))
            return false;

        rect = monitor.rcMonitor;
        return true;
    }

    // Clearing the fullscreen flag settles both the window shape and, through IsFullscreen,
    // the device mode. The stock fullscreen path opens a cover window per monitor, so it is
    // not reused for borderless.
    char __fastcall CreateGameWindow(uint8_t* self, void* edx, HINSTANCE instance, int show)
    {
        *reinterpret_cast<uint8_t*>(self + Fullscreen) = 0;

        const char result = shCreateGameWindow.thiscall<char>(self, instance, show);

        HWND hWnd = *reinterpret_cast<HWND*>(self + GameWindow);
        RECT monitor{};

        if (result && nDisplayMode == Borderless && MonitorRect(hWnd, monitor))
        {
            SetWindowLongW(hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(hWnd, nullptr, monitor.left, monitor.top, monitor.right - monitor.left,
                         monitor.bottom - monitor.top, SWP_NOZORDER | SWP_FRAMECHANGED);
        }

        return result;
    }

    // A windowed mode change resizes the window to the requested client size, which would pull
    // a borderless window off the monitor; answer with the monitor so the backbuffer follows.
    char __fastcall SetResolution(uint8_t* self, void* edx, int width, int height, int bits,
                                  int depthBits, int vsync)
    {
        RECT monitor{};

        if (MonitorRect(*reinterpret_cast<HWND*>(self + DeviceWindow), monitor))
        {
            width = monitor.right - monitor.left;
            height = monitor.bottom - monitor.top;
        }

        return shSetResolution.thiscall<char>(self, width, height, bits, depthBits, vsync);
    }
}

FEATURE(Game, DisplayMode)
{
    if (nDisplayMode == Stock)
        return;

    auto window = hook::pattern("81 EC 00 02 00 00 53 55 56 8B F1 57 89 74 24 18 8B 86 B8 00 00 00");
    if (window.empty())
    {
        spdlog::error("DisplayMode: window creation not found");
        return;
    }

    if (nDisplayMode == Borderless)
    {
        auto resolution = hook::pattern("83 EC 10 53 55 56 8B F1 57 8A 86 A8 92 00 00 84 C0");
        if (resolution.empty())
        {
            spdlog::error("DisplayMode: mode change not found");
            return;
        }

        shSetResolution = safetyhook::create_inline(resolution.get_first(), SetResolution);
    }

    shCreateGameWindow = safetyhook::create_inline(window.get_first(), CreateGameWindow);
    spdlog::info("DisplayMode: {}", nDisplayMode == Borderless ? "borderless at the desktop resolution"
                                                              : "windowed at the game resolution");
}
