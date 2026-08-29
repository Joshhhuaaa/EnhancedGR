#include "stdafx.h"
#include "common.hpp"
#include "config.hpp"
#include "feature.hpp"

#include <hidusage.h>

static constexpr bool bRawInput = true;

static Config::Float fLookSensitivity("Input", "LookSensitivity", 1.0f);
static Config::Float fCursorSensitivity("Input", "CursorSensitivity", 1.0f);

namespace
{
    // RSInputImpl. Only the DirectInput sampling is replaced; the event queue downstream is untouched.
    constexpr ptrdiff_t Window    = 0x004;  // the hwnd handed to SetCooperativeLevel
    constexpr ptrdiff_t Mouse     = 0x198;  // IDirectInputDevice8A*
    constexpr ptrdiff_t Sampled   = 0x1a8;  // previous state is valid
    constexpr ptrdiff_t PrevState = 0x1ac;  // the last DIMOUSESTATE2: lX, lY, lZ, then 8 buttons
    constexpr ptrdiff_t Bindings  = 0xdf7c; // one entry per input code

    constexpr int32_t MouseX    = 0x32;
    constexpr int32_t MouseY    = 0x33;
    constexpr int32_t WheelUp   = 0x34;
    constexpr int32_t WheelDown = 0x35;
    constexpr int32_t Button1   = 0x36;

    // Set on the mouse axis bindings while in play; the menus rebind the axes to the UI cursor.
    constexpr uint32_t LookBinding = 0x600;

    // Wheel code currently reading as held; released when the next frame differs.
    constexpr ptrdiff_t WheelHeld = 0xf060;

    struct Binding
    {
        int32_t  action;
        int32_t  altAction;
        uint32_t flags;
        uint32_t reserved;
    };

    SafetyHookInline shUpdateMouse{};

    uint8_t (__fastcall* QueueEvent)(void* self, void* edx, int32_t action, int32_t value,
                                     int32_t code, uint32_t flags, int32_t axis, int32_t repeat) = nullptr;

    HHOOK hGetMessageHook = nullptr;
    HHOOK hCallWndProcHook = nullptr;
    HWND  hGameWindow = nullptr;

    int32_t rawX = 0;
    int32_t rawY = 0;
    int32_t rawZ = 0;
    uint8_t rawButtons[8]{};

    // Fraction dropped by the integer conversion, carried into the next frame.
    float carryX = 0.0f;
    float carryY = 0.0f;

    int  standDowns = 0;
    bool bRegistered = false;


    // RIDEV_NOLEGACY holds for as long as the registration exists, so the background state is
    // no registration at all.
    void Register(bool bOn)
    {
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
        rid.usUsage = HID_USAGE_GENERIC_MOUSE;
        rid.dwFlags = bOn ? RIDEV_INPUTSINK | RIDEV_NOLEGACY : RIDEV_REMOVE;
        rid.hwndTarget = bOn ? hGameWindow : nullptr;

        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
            spdlog::error("RawInput: RegisterRawInputDevices({}) failed, GetLastError {}", bOn, GetLastError());
        else
            spdlog::info("RawInput: registration {}", bOn ? "on" : "off");
    }

    void SetRegistered(bool bOn)
    {
        if (bOn == bRegistered || hGameWindow == nullptr)
            return;

        bRegistered = bOn;
        Register(bOn);
    }

    // The wndproc chain is never touched: d3d9 subclasses the device window of an exclusive
    // fullscreen device and breaks if anything is layered on top. WM_INPUT is posted (message
    // hook), the focus losses are sent (call hook); both hooks are thread-local.
    LRESULT CALLBACK OnGetMessage(int code, WPARAM wParam, LPARAM lParam)
    {
        const MSG& msg = *reinterpret_cast<const MSG*>(lParam);

        if (code == HC_ACTION && wParam == PM_REMOVE && msg.message == WM_INPUT)
        {
            RAWINPUT raw{};
            UINT size = sizeof(raw);

            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg.lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                raw.header.dwType == RIM_TYPEMOUSE)
            {
                const RAWMOUSE& mouse = raw.data.mouse;

                if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                {
                    rawX += mouse.lLastX;
                    rawY += mouse.lLastY;
                }

                for (int i = 0; i < 5; ++i)
                {
                    if (mouse.usButtonFlags & (RI_MOUSE_BUTTON_1_DOWN << (i * 2)))
                        rawButtons[i] = 0x80;
                    else if (mouse.usButtonFlags & (RI_MOUSE_BUTTON_1_UP << (i * 2)))
                        rawButtons[i] = 0;
                }

                if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
                    rawZ += static_cast<SHORT>(mouse.usButtonData);
            }
        }

        return CallNextHookEx(hGetMessageHook, code, wParam, lParam);
    }

    LRESULT CALLBACK OnCallWndProc(int code, WPARAM wParam, LPARAM lParam)
    {
        const CWPSTRUCT& cwp = *reinterpret_cast<const CWPSTRUCT*>(lParam);

        if (code == HC_ACTION &&
            (cwp.message == WM_KILLFOCUS || (cwp.message == WM_ACTIVATEAPP && cwp.wParam == 0)))
        {
            // Raw input stops at the focus boundary, so a button held across the switch would
            // otherwise never see its release.
            memset(rawButtons, 0, sizeof(rawButtons));
            rawX = rawY = rawZ = 0;
            SetRegistered(false);
        }

        return CallNextHookEx(hCallWndProcHook, code, wParam, lParam);
    }

    void Queue(uint8_t* self, int32_t code, int32_t value)
    {
        const Binding& binding = reinterpret_cast<const Binding*>(self + Bindings)[code];

        if (binding.action != -1)
            QueueEvent(self, nullptr, binding.action, value, code, binding.flags, 1, 0);
    }

    // The engine re-acquires on its own, so this runs every frame. DI_NOEFFECT means it was
    // already down; a second DI_OK is the engine having picked it back up.
    void StandDown(uint8_t* self)
    {
        void* mouse = *reinterpret_cast<void**>(self + Mouse);

        if (mouse == nullptr)
            return;

        void** vtable = *reinterpret_cast<void***>(mouse);

        if (reinterpret_cast<HRESULT(__stdcall*)(void*)>(vtable[8])(mouse) == S_OK && ++standDowns <= 2)
            spdlog::info("RawInput: DirectInput mouse unacquired ({})", standDowns);
    }

    void Attach(uint8_t* self, HWND hWnd)
    {
        hGameWindow = hWnd;

        if (hGetMessageHook == nullptr)
        {
            hGetMessageHook = SetWindowsHookExA(WH_GETMESSAGE, OnGetMessage, nullptr, GetCurrentThreadId());
            hCallWndProcHook = SetWindowsHookExA(WH_CALLWNDPROC, OnCallWndProc, nullptr, GetCurrentThreadId());
        }

        // Must precede the registration: raw input registrations are per process and per usage,
        // and dinput8 tearing its own down takes ours with it.
        StandDown(self);

        bRegistered = GetForegroundWindow() == hGameWindow;

        if (bRegistered)
            Register(true);

        spdlog::info("RawInput: attached to hwnd {:#x}", reinterpret_cast<uintptr_t>(hGameWindow));
    }

    void __fastcall UpdateMouse(uint8_t* self, void* edx)
    {
        // The hwnd at +0x4 need not be the window on screen; the display manager creates one
        // per display mode.
        HWND hWnd = GetActiveWindow();

        if (hWnd == nullptr && hGameWindow == nullptr)
            hWnd = *reinterpret_cast<HWND*>(self + Window);

        if (hWnd != nullptr && hWnd != hGameWindow)
            Attach(self, hWnd);

        // Regaining focus is handled here, not in the hook: WM_ACTIVATEAPP arrives while the
        // window is still iconic, and registering then loses the picture after an alt-tab.
        SetRegistered(GetForegroundWindow() == hGameWindow && !IsIconic(hGameWindow));

        // No legacy messages means no WM_SETCURSOR, so a busy cursor Windows puts up during a
        // stalled load is never replaced by the class cursor. Done here in its place.
        if (bRegistered)
        {
            HCURSOR hClass = reinterpret_cast<HCURSOR>(GetClassLongPtrA(hGameWindow, GCLP_HCURSOR));

            if (GetCursor() != hClass)
                SetCursor(hClass);
        }

        StandDown(self);

        const Binding& axis = reinterpret_cast<const Binding*>(self + Bindings)[MouseX];
        const bool bLooking = (axis.flags & LookBinding) == LookBinding;
        const float sensitivity = bLooking ? fLookSensitivity : fCursorSensitivity;
        const float scaledX = rawX * sensitivity + carryX;
        const float scaledY = rawY * sensitivity + carryY;

        const int32_t x = static_cast<int32_t>(scaledX);
        const int32_t y = static_cast<int32_t>(scaledY);
        const int32_t z = rawZ;

        carryX = scaledX - x;
        carryY = scaledY - y;
        rawX = rawY = rawZ = 0;

        auto* prev = reinterpret_cast<int32_t*>(self + PrevState);
        auto* prevButtons = reinterpret_cast<uint8_t*>(self + PrevState + 12);
        auto& wheelHeld = *reinterpret_cast<int32_t*>(self + WheelHeld);

        if (self[Sampled])
        {
            if (x != 0)
                Queue(self, MouseX, x);

            if (y != 0)
                Queue(self, MouseY, y);

            if (z != prev[2])
            {
                if (wheelHeld != 0)
                    Queue(self, wheelHeld, 0);

                wheelHeld = z == 0 ? 0 : (z > 0 ? WheelUp : WheelDown);

                if (wheelHeld != 0)
                    Queue(self, wheelHeld, 1);
            }

            for (int32_t i = 0; i < 8; ++i)
            {
                const Binding& binding = reinterpret_cast<const Binding*>(self + Bindings)[Button1 + i];

                if (binding.action == -1 || rawButtons[i] == prevButtons[i])
                    continue;

                int32_t action = binding.action;
                const bool bPressed = (rawButtons[i] & 0x80) != 0;

                if (binding.flags & 0x20)
                {
                    if (binding.altAction != -1 && !bPressed)
                        action = binding.altAction;
                }
                else if ((binding.flags & 0x40) && (bPressed || (binding.flags & 0x80)))
                {
                    continue;
                }

                QueueEvent(self, nullptr, action, rawButtons[i], Button1 + i, binding.flags, 0, 0);
            }
        }

        prev[0] = x;
        prev[1] = y;
        prev[2] = z;
        memcpy(prevButtons, rawButtons, sizeof(rawButtons));
        self[Sampled] = 1;
    }
}

FEATURE(Game, RawInput)
{
    if (!bRawInput)
        return;

    auto update = hook::pattern("83 EC 1C 53 55 8B E9 56 33 DB 8B B5 98 01 00 00 3B F3");
    if (update.empty())
    {
        spdlog::error("RawInput: RSInputImpl mouse update not found");
        return;
    }

    auto queue = hook::pattern("8B 81 5C EB 00 00 83 F8 40 7C 05 32 C0 C2 18 00");
    if (queue.empty())
    {
        spdlog::error("RawInput: RSInputImpl event queue not found");
        return;
    }

    QueueEvent = reinterpret_cast<decltype(QueueEvent)>(queue.get_first());

    // The original is never called; it only read the DirectInput mouse.
    shUpdateMouse = safetyhook::create_inline(update.get_first(), UpdateMouse);

    if (!shUpdateMouse)
        spdlog::error("RawInput: hook installation failed");
    else
        spdlog::info("RawInput: mouse taken from raw input, look x{} / cursor x{}",
                     static_cast<float>(fLookSensitivity), static_cast<float>(fCursorSensitivity));
}
