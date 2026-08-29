#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"

namespace
{
    // The engine holds a horizontal 90 degrees and derives the vertical from it, so the view
    // loses height as the aspect widens. Scaling the horizontal against 4:3 keeps the vertical
    // at 73.74 degrees; correcting the engine's own value keeps culling and HUD consistent.
    constexpr double BaseAspect = 4.0 / 3.0;
    constexpr float RadToDeg = 57.2957795f;

    constexpr ptrdiff_t BaseFOV = 0xe8;      // settings struct
    constexpr ptrdiff_t ClientRect = 0x92c0; // renderer, measured in Initialize3DEnvironment

    uint8_t** ppSettings = nullptr;
    float* pStockFOV = nullptr;
    float stockFOV = 0.0f;
    float appliedFOV = 0.0f;

    // Bases a still-living camera may have been created under: the SimCamera only re-derives
    // on init, reset or zoom change, and can sit inactive across several changes.
    float staleFOV[8]{};
    size_t staleCount = 0;

    SafetyHookInline shInitialize3DEnvironment{};
    SafetyHookInline shResetDevice{};
    SafetyHookInline shSetWorldFOV{};

    void Apply(uint8_t* self)
    {
        // The only field describing the real render area in both modes.
        const RECT& client = *reinterpret_cast<const RECT*>(self + ClientRect);
        const long width = client.right - client.left;
        const long height = client.bottom - client.top;

        if (height <= 0)
            return;

        const double aspect = static_cast<double>(width) / static_cast<double>(height);
        const float fov = static_cast<float>(2.0 * std::atan(std::tan(stockFOV / 2.0) * aspect / BaseAspect));

        if (appliedFOV != 0.0f && appliedFOV != fov && staleCount < sizeof(staleFOV) / sizeof(staleFOV[0]))
            staleFOV[staleCount++] = appliedFOV;
        appliedFOV = fov;

        // The current value must not read as stale, or the substitution would loop.
        for (size_t s = 0; s < staleCount;)
        {
            if (staleFOV[s] == fov)
                staleFOV[s] = staleFOV[--staleCount];
            else
                ++s;
        }

        // Every settings instance is born from the constructor immediate, so patch that too.
        injector::WriteMemory<float>(pStockFOV, fov, true);

        if (*ppSettings != nullptr)
            *reinterpret_cast<float*>(*ppSettings + BaseFOV) = fov;

        spdlog::info("FOV: {}x{}, horizontal {:.2f} -> {:.2f} deg",
                     width, height, stockFOV * RadToDeg, fov * RadToDeg);
    }

    int __fastcall Initialize3DEnvironment(uint8_t* self, void* edx)
    {
        const int result = shInitialize3DEnvironment.thiscall<int>(self);

        if (result != 0)
            Apply(self);

        return result;
    }

    int __fastcall ResetDevice(uint8_t* self, void* edx)
    {
        const int result = shResetDevice.thiscall<int>(self);

        // An in-game resolution change goes through Reset, which has just refreshed the rect.
        Apply(self);

        return result;
    }

    // A per-frame FOV that bitwise-matches a base this feature wrote is a stale copy of it.
    // A zoomed camera never matches and re-derives from the live base on its own.
    void __stdcall SetWorldFOV(float fov)
    {
        for (size_t s = 0; s < staleCount; ++s)
        {
            if (*reinterpret_cast<const uint32_t*>(&fov) !=
                *reinterpret_cast<const uint32_t*>(&staleFOV[s]))
                continue;

            fov = appliedFOV;
            break;
        }

        shSetWorldFOV.stdcall<void>(fov);
    }
}

FEATURE(Game, FOV)
{
    // Settings constructor: MOV [ESI+0xe8], pi/2.
    auto stock = hook::pattern("C7 86 E8 00 00 00 DB 0F C9 3F");
    if (stock.empty())
    {
        spdlog::error("FOV: base field of view initialiser not found");
        return;
    }

    // Camera FOV reset; loads the settings pointer first.
    auto settings = hook::pattern("A1 ? ? ? ? D9 80 E8 00 00 00 D9 9E B8 03 00 00");
    if (settings.empty())
    {
        spdlog::error("FOV: settings pointer not found");
        return;
    }

    auto init3D = hook::pattern("81 EC 0C 02 00 00 53 55 56 8B F1 57 8B 8E AC 92 00 00");
    if (init3D.empty())
    {
        spdlog::error("FOV: RSDirect3DRenderer::Initialize3DEnvironment not found");
        return;
    }

    auto reset = hook::pattern("51 53 56 8B F1 57 8B 86 EC 93 00 00 8D 9E D8 94 00 00");
    if (reset.empty())
    {
        spdlog::error("FOV: RSDirect3DRenderer device reset handler not found");
        return;
    }

    auto worldFOV = hook::pattern("A1 ? ? ? ? 8B 88 F8 00 00 00 D9 81 C8 01 00 00 D8 5C 24 04");
    if (worldFOV.empty())
    {
        spdlog::error("FOV: world camera FOV setter not found");
        return;
    }

    pStockFOV = static_cast<float*>(stock.get_first(6));
    stockFOV = *pStockFOV;
    ppSettings = *static_cast<uint8_t***>(settings.get_first(1));

    shInitialize3DEnvironment = safetyhook::create_inline(init3D.get_first(), Initialize3DEnvironment);
    shResetDevice = safetyhook::create_inline(reset.get_first(), ResetDevice);
    shSetWorldFOV = safetyhook::create_inline(worldFOV.get_first(), SetWorldFOV);

    if (!shInitialize3DEnvironment || !shResetDevice || !shSetWorldFOV)
        spdlog::error("FOV: hook installation failed");
}
