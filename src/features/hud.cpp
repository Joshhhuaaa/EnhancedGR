#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"

namespace
{
    // The interface is laid out against 640x480. Stock scales it by floor(width/640) across
    // and floor(height/480) down, which is the horizontal stretch; one factor taken from
    // height replaces both, applied before quantising rather than after.
    constexpr float LayoutHeight = 480.0f;

    using GetDimension = int(__thiscall*)(void*);
    using GetOptions = uint8_t*(__thiscall*)(void*);

    void** ppDisplay = nullptr;
    void** ppOptions = nullptr;
    char* pScaleHUD = nullptr;

    bool ScaleHUDEnabled()
    {
        void* manager = *ppOptions;

        return (*reinterpret_cast<GetOptions**>(manager))[88](manager)[0xbe] != 0;
    }

    // Replaces the engine's clearing of the live flag around the builds it excludes from scaling.
    void __cdecl SetElementScale()
    {
        *pScaleHUD = ScaleHUDEnabled() ? 1 : 0;
    }

    int Dimension(bool vertical)
    {
        void* display = *ppDisplay;

        return (*reinterpret_cast<GetDimension**>(display))[vertical ? 4 : 3](display);
    }

    // The live flag, not the option: the engine loads it from ScaleHUD or ScaleCommandMap per
    // element, so reading either option directly would scale one of them by the other.
    float Factor()
    {
        return *pScaleHUD != 0 ? Dimension(true) / LayoutHeight : 1.0f;
    }

    // The engine's own snap rule, so ScaleHUD off stays bit-identical to stock.
    float Snap(float value)
    {
        const float whole = std::trunc(value);
        const float fraction = value - whole;

        if (fraction > 0.25f && fraction < 0.75f)
            return whole + 0.5f;

        if (fraction < -0.25f && fraction > -0.75f)
            return whole - 0.5f;

        return whole;
    }

    struct Converter
    {
        float reference; // the anchored edge, in layout units: 0 top/left, 0.5 centre, 1 bottom/right
        float layout;    // 640 across, 480 down
        float direction;
    };

    Converter converters[6]{};
    size_t converterCount = 0;

    template<size_t Index>
    float __cdecl Convert(float coordinate)
    {
        const Converter& c = converters[Index];
        const float dimension = static_cast<float>(Dimension(c.layout == LayoutHeight));
        const float offset = c.direction * (coordinate - c.reference) * c.layout;

        return (c.reference * dimension + c.direction * Snap(Factor() * offset)) / dimension;
    }

    using ConvertFn = float(__cdecl*)(float);

    constexpr ConvertFn Replacements[]
    {
        Convert<0>, Convert<1>, Convert<2>, Convert<3>, Convert<4>, Convert<5>,
    };

    // Edge and axis are read out of the site's own operands rather than assumed from its address.
    bool Replace(const char* bytes, ptrdiff_t reference, ptrdiff_t layout, float direction)
    {
        auto sites = hook::pattern(bytes);
        if (sites.size() != 2)
            return false;

        sites.for_each_result([&](hook::pattern_match match)
        {
            Converter& c = converters[converterCount];
            c.reference = reference < 0 ? 0.0f : **match.get<float*>(reference);
            c.layout = **match.get<float*>(layout);
            c.direction = direction;

            injector::MakeJMP(match.get<void>(), reinterpret_cast<void*>(Replacements[converterCount]));
            ++converterCount;
        });

        return true;
    }

    // The inline sites all feed a font scale.
    float GlyphFactor()
    {
        return GlyphScale(Factor());
    }

    // Replaces an inline per-axis factor; the dimension the site fetched just above is left unused.
    int __cdecl FactorBits()
    {
        const float factor = GlyphFactor();

        return *reinterpret_cast<const int*>(&factor);
    }

    // The width is in backbuffer pixels but the X-centre converter scales the whole offset, so
    // the half-width is divided by the factor first. The integer halving is stock's own.
    int __fastcall NameCentre(int width)
    {
        const float centre = 320.0f - static_cast<float>(width / 2) / GlyphFactor();

        return *reinterpret_cast<const int*>(&centre);
    }

    // The font scales the glyph advance by its own +0x1c but uses the space (+0x14) and the gap
    // (+0x18) raw. Gap sites are topped up by (scale - 1) of the term they add, so nothing at 1.0.
    SafetyHookMid shGapForward{};
    SafetyHookMid shGapBack{};
    SafetyHookMid shGapMeasure{};

    void TopUp(const uint8_t* font, float* pen, float inverse, float direction)
    {
        const float scale = *reinterpret_cast<const float*>(font + 0x1c);

        *pen += direction * *reinterpret_cast<const int*>(font + 0x18) * inverse * (scale - 1.0f);
    }

    void GapForward(SafetyHookContext& ctx)
    {
        TopUp(reinterpret_cast<const uint8_t*>(ctx.ebp), reinterpret_cast<float*>(ctx.esp + 0x68),
              *reinterpret_cast<const float*>(ctx.esp + 0x7c), 1.0f);
    }

    // The command map's drawer lays its run out from the right.
    void GapBack(SafetyHookContext& ctx)
    {
        TopUp(reinterpret_cast<const uint8_t*>(ctx.edx), reinterpret_cast<float*>(ctx.esp + 0x64),
              *reinterpret_cast<const float*>(ctx.esp + 0x88), -1.0f);
    }

    // The measurer multiplies the gap out in one go, so it is scaled rather than topped up.
    void GapMeasure(SafetyHookContext& ctx)
    {
        ctx.eax = static_cast<uintptr_t>(static_cast<int>(ctx.eax)
                                         * *reinterpret_cast<const float*>(ctx.edi + 0x1c));
    }

    // Jumped to in place of the font's own MOV/RET for the space, so it returns for the font.
    int __fastcall SpaceAdvance(uint8_t* font, void* edx, int character)
    {
        return static_cast<int>(*reinterpret_cast<const int*>(font + 0x14)
                                * *reinterpret_cast<const float*>(font + 0x1c));
    }

    bool Rewrite(uint8_t* divide)
    {
        injector::MakeCALL(divide, reinterpret_cast<void*>(FactorBits));
        injector::WriteMemory<uint16_t>(divide + 5, 0xd08b, true); // MOV EDX, EAX
        injector::MakeNOP(divide + 7, 5, true);

        for (uint8_t* load = divide + 12; load < divide + 28; ++load)
        {
            if (load[0] != 0xdf || load[1] != 0x6c || load[2] != 0x24)
                continue;

            injector::WriteMemory<uint16_t>(load, 0x44d9, true);   // FILD qword -> FLD dword
            return true;
        }

        return false;
    }
}

FEATURE(Game, HUD)
{
    // The flag getter is MOV EAX,imm32; RET, and the display manager global follows the guard.
    auto guard = hook::pattern("E8 ? ? ? ? 80 38 00 74 ? 8B 0D ? ? ? ?");
    if (guard.empty())
    {
        spdlog::error("HUD: scale guard not found");
        return;
    }

    uint8_t* getter = guard.get(0).get<uint8_t>(5) + *guard.get(0).get<int32_t>(1);

    pScaleHUD = *reinterpret_cast<char**>(getter + 1);
    ppDisplay = *guard.get(0).get<void**>(12);

    auto options = hook::pattern("8B 0D ? ? ? ? 8B 11 FF 92 60 01 00 00 8A 98 BE 00 00 00");
    if (options.empty())
    {
        spdlog::error("HUD: options manager not found");
        return;
    }

    ppOptions = *options.get(0).get<void**>(2);

    if (!Replace("83 EC 0C D9 44 24 10 D8 0D ? ? ? ?", -1, 9, 1.0f) ||
        !Replace("83 EC 0C D9 05 ? ? ? ? D8 64 24 10 83 EC 08 D8 0D ? ? ? ?", 5, 18, -1.0f) ||
        !Replace("83 EC 0C D9 05 ? ? ? ? D8 64 24 10 D8 0D ? ? ? ?", 5, 15, -1.0f))
    {
        spdlog::error("HUD: expected six coordinate converters, replaced {}", converterCount);
        return;
    }

    // The horizontal factor and the vertical one, identical but for the divisor.
    for (const char* bytes : { "8B C8 B8 CD CC CC CC F7 E1 C1 EA 09",
                               "8B C8 B8 89 88 88 88 F7 E1 C1 EA 08" })
    {
        auto scale = hook::pattern(bytes);
        if (scale.size() != 10)
        {
            spdlog::error("HUD: expected 10 inline factors, found {}", scale.size());
            return;
        }

        scale.for_each_result([](hook::pattern_match match)
        {
            if (!Rewrite(match.get<uint8_t>()))
                spdlog::error("HUD: inline factor {:#x} has no integer load",
                              reinterpret_cast<uintptr_t>(match.get<void>()));
        });
    }

    // The reticle build clears the live flag: the getter call is repointed and the clearing
    // store dropped. Sibling elements share the idiom, so the match must be unique.
    auto reticle = hook::pattern("8A 18 E8 ? ? ? ? 8B 4C 24 0C C6 00 00 8B 44 24 10");
    if (reticle.size() != 1)
    {
        spdlog::error("HUD: expected one reticle scale override, found {}", reticle.size());
        return;
    }

    injector::MakeCALL(reticle.get(0).get<void>(2), reinterpret_cast<void*>(SetElementScale));
    injector::MakeNOP(reticle.get(0).get<void>(11), 3, true);

    // The reload ring's three build sites clear the flag the same way. The fourth clear,
    // around the per-frame draw, only rewrites UVs and is left alone.
    for (const char* bytes : { "E8 ? ? ? ? C6 00 00 8B 8E 2C 01 00 00 E8",
                               "E8 ? ? ? ? C6 00 00 8B 8E 1C 01 00 00 E8",
                               "E8 ? ? ? ? C6 00 00 8B 8E 2C 01 00 00 85 C9" })
    {
        auto icon = hook::pattern(bytes);
        if (icon.size() != 1)
        {
            spdlog::error("HUD: expected one reload ring scale override, found {}", icon.size());
            return;
        }

        injector::MakeCALL(icon.get(0).get<void>(), reinterpret_cast<void*>(SetElementScale));
        injector::MakeNOP(icon.get(0).get<void>(5), 3, true);
    }

    // Two centred-name sites, identical but for the result register; twelve bytes each.
    auto names = hook::pattern("99 2B C2 ? 40 01 00 00 D1 F8 2B ?");
    if (names.size() != 2)
    {
        spdlog::error("HUD: expected two centred name sites, found {}", names.size());
        return;
    }

    names.for_each_result([](hook::pattern_match match)
    {
        uint8_t* site = match.get<uint8_t>();

        // The site's own MOV r32, 320 names the register the result has to land in.
        const uint8_t destination = static_cast<uint8_t>(0xc0 | ((site[3] & 7) << 3));

        injector::WriteMemory<uint16_t>(site, 0xc88b, true);       // MOV ECX, EAX
        injector::MakeCALL(site + 2, reinterpret_cast<void*>(NameCentre));
        injector::WriteMemory<uint8_t>(site + 7, 0x8b, true);      // MOV EDX/ECX, EAX
        injector::WriteMemory<uint8_t>(site + 8, destination, true);
        injector::MakeNOP(site + 9, 3, true);

        for (uint8_t* load = site + 12; load < site + 40; ++load)
        {
            if (load[0] != 0xdb || load[1] != 0x44 || load[2] != 0x24)
                continue;

            injector::WriteMemory<uint8_t>(load, 0xd9, true);      // FILD dword -> FLD dword
            return;
        }

        spdlog::error("HUD: centred name site {:#x} has no integer load",
                      reinterpret_cast<uintptr_t>(site));
    });

    // A bare FILD or FMUL is no signature, so each pattern is the whole block and the offsets point into it.
    auto forward = hook::pattern("DB 45 18 D8 4C 24 7C D8 44 24 68 D9 5C 24 68");
    auto back = hook::pattern("8B D1 DB 42 18 D8 8C 24 88 00 00 00 D8 6C 24 64");
    auto measure = hook::pattern("8D 45 FF 0F AF 47 18 03 C3 5B 5F 5E 5D C2 04 00");
    auto space = hook::pattern("8B 44 24 04 66 3D 20 00 75 06 8B 41 14 C2 04 00");

    if (forward.size() != 1 || back.size() != 1 || measure.size() != 1 || space.size() != 1)
    {
        spdlog::error("HUD: expected one of each spacing site, found {}/{}/{}/{}", forward.size(),
                      back.size(), measure.size(), space.size());
        return;
    }

    shGapForward = safetyhook::create_mid(forward.get(0).get<void>(), GapForward);
    shGapBack = safetyhook::create_mid(back.get(0).get<void>(2), GapBack);
    shGapMeasure = safetyhook::create_mid(measure.get(0).get<void>(7), GapMeasure);

    injector::MakeJMP(space.get(0).get<void>(10), reinterpret_cast<void*>(SpaceAdvance), true);
}
