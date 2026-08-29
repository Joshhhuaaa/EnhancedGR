#include "stdafx.h"
#include "config.hpp"
#include "feature.hpp"

static Config::Value bScaleHUD("HUD", "ScaleHUD", true);

namespace
{
    // Element geometry is baked at build time, so a changed option needs a relayout. The
    // engine's resolution-change relayout already handles every element's flag itself.
    using Relayout = void(__thiscall*)(void*);

    using GetOptions = uint8_t*(__thiscall*)(void*);

    Relayout pRelayout = nullptr;
    void** ppHUD = nullptr;
    void** ppOptions = nullptr;

    // The options screen edits a working copy that Apply copies over the live options, so the
    // rebuild follows Apply rather than the checkbox.
    SafetyHookInline shApplyOptions{};

    void Rebuild()
    {
        if (*ppHUD == nullptr || *ppOptions == nullptr)
            return;

        void* manager = *ppOptions;
        uint8_t* options = (*reinterpret_cast<GetOptions**>(manager))[88](manager);

        // Re-forced here because Apply has just copied the screen's values over the live ones.
        if (bScaleHUD)
        {
            options[0xbd] = 1;
            options[0xbe] = 1;
        }

        pRelayout(*ppHUD);
    }

    void __fastcall ApplyOptions(void* self, void* edx)
    {
        shApplyOptions.thiscall<void>(self);

        Rebuild();
    }

    // Elements built on the HUD state change were relaid out by the menu's 640x480 mode switch
    // a moment later; menu.cpp removes that switch, so the relayout is asked for here.
    SafetyHookInline shHUDState{};

    void __fastcall HUDState(void* self, void* edx, void* node)
    {
        shHUDState.thiscall<void>(self, node);

        Rebuild();
    }

    // Forced once per parsed settings key, so the last leaves both set whatever the file said,
    // and only at the parse, so the in-game toggles keep working.
    SafetyHookInline shSettingsKey{};

    void __fastcall SettingsKey(uint8_t* options, void* edx, void* node)
    {
        shSettingsKey.thiscall<void>(options, node);

        options[0xbd] = 1;
        options[0xbe] = 1;
    }

    // The live options object is replaced behind the forces above, so the option is also
    // stamped where the HUD draw reads it.
    SafetyHookMid shDrawScale[2]{};

    void DrawScale(SafetyHookContext& ctx)
    {
        uint8_t* options = reinterpret_cast<uint8_t*>(ctx.eax);

        options[0xbd] = 1;
        options[0xbe] = 1;
    }

}

FEATURE(Game, HUDRebuild)
{
    // The relayout's one call site names both the routine and the HUD singleton it runs on.
    auto relayout = hook::pattern("8B 0D ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 6A 02 8B 88 08 02 00 00");
    if (relayout.empty())
    {
        spdlog::error("HUDRebuild: element relayout not found");
        return;
    }

    ppHUD = *relayout.get(0).get<void**>(2);
    pRelayout = reinterpret_cast<Relayout>(relayout.get(0).get<uint8_t>(11)
                                           + *relayout.get(0).get<int32_t>(7));

    auto options = hook::pattern("8B 0D ? ? ? ? 8B 11 FF 92 60 01 00 00 8A 98 BE 00 00 00");
    if (options.empty())
    {
        spdlog::error("HUDRebuild: options manager not found");
        return;
    }

    ppOptions = *options.get(0).get<void**>(2);

    auto apply = hook::pattern("6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 5C "
                               "53 55 56 8B F1 8B 0D ? ? ? ? 57 33 FF 8B 01");
    if (apply.size() != 1)
    {
        spdlog::error("HUDRebuild: expected one options apply, found {}", apply.size());
        return;
    }

    shApplyOptions = safetyhook::create_inline(apply.get_first(), ApplyOptions);

    auto state = hook::pattern("64 A1 00 00 00 00 6A FF 68 ? ? ? ? 50 64 89 25 00 00 00 00 53 56 "
                               "57 8B 7C 24 1C 8B F1 57 E8 ? ? ? ? 8B 47 24 8B 08 8B 11 FF 52 3C");
    if (state.empty())
    {
        spdlog::error("HUDRebuild: HUD state change not found");
        return;
    }

    shHUDState = safetyhook::create_inline(state.get_first(), HUDState);

    if (!bScaleHUD)
        return;

    auto settings = hook::pattern("6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
                                  "83 EC 1C 55 56 8B 74 24 34 8B E9");
    if (settings.empty())
    {
        spdlog::error("HUDRebuild: settings key handler not found");
        return;
    }

    shSettingsKey = safetyhook::create_inline(settings.get_first(), SettingsKey);

    // The two option loads in the HUD draw; EAX holds the options object at both.
    auto draw = hook::pattern("8B 01 FF 90 60 01 00 00 8A 98 BE 00 00 00");
    if (draw.size() != 2)
    {
        spdlog::error("HUDRebuild: expected two HUD draw option loads, found {}", draw.size());
        return;
    }

    for (size_t i = 0; i < 2; ++i)
        shDrawScale[i] = safetyhook::create_mid(draw.get(i).get<void>(8), DrawScale);
}
