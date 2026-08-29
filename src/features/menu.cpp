#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"

namespace
{
    // The menu forces 640x480 and switches back on mission load. Pointing it at the session's
    // video settings makes the mode already match, so the setter's own early-out drops the switch.
    using GetVideoSettings = uint8_t*(__thiscall*)(void*);

    constexpr ptrdiff_t Width = 0x16;  // both u16, read at 0065f3a0 / 0065f3a6
    constexpr ptrdiff_t Height = 0x18;

    void** ppSession = nullptr;

    // Element rects are authored in 640x480 pixels and normalised against the backbuffer, so the
    // layout is scaled to fit at the one quad writer every element reaches.
    constexpr float LayoutWidth = 640.0f;
    constexpr float LayoutHeight = 480.0f;

    using GetDimension = int(__thiscall*)(void*);

    void** ppDisplay = nullptr;

    SafetyHookInline shWriteQuad{};

    float Dimension(bool vertical)
    {
        void* display = *ppDisplay;

        return static_cast<float>((*reinterpret_cast<GetDimension**>(display))[vertical ? 4 : 3](display));
    }

    SafetyHookInline shShellText{};

    // Text is positioned in 640x480 pixels and normalised by the font itself; glyph size is
    // scaled by handing the font a virtual backbuffer of display / glyph scale.
    void __fastcall ShellText(void* self, void* edx, void* text, int x, int y, uint32_t arg4,
                             uint32_t arg5, int* width, int* height, uint32_t maxX)
    {
        const float display = Dimension(false);
        const float factor = Dimension(true) / LayoutHeight;
        const float glyph = GlyphScale(factor);

        static int virtualWidth = 0;
        static int virtualHeight = 0;
        virtualWidth = static_cast<int>(display / glyph);
        virtualHeight = static_cast<int>(Dimension(true) / glyph);

        shShellText.thiscall<void>(self, text,
                                  static_cast<int>(x * factor + (display - LayoutWidth * factor) * 0.5f),
                                  static_cast<int>(y * factor),
                                  arg4, arg5,
                                  width != nullptr ? width : &virtualWidth,
                                  height != nullptr ? height : &virtualHeight,
                                  maxX);
    }

    SafetyHookInline shHitTest{};
    int hitDepth = 0;

    // Inverse of the layout transform. The container recurses through this function, so only
    // the outermost call arrives in screen pixels.
    int __fastcall HitTest(void* self, void* edx, int x, int y)
    {
        if (hitDepth == 0)
        {
            const float factor = Dimension(true) / LayoutHeight;

            x = static_cast<int>((x - (Dimension(false) - LayoutWidth * factor) * 0.5f) / factor);
            y = static_cast<int>(y / factor);
        }

        ++hitDepth;
        const int hit = shHitTest.thiscall<int>(self, x, y);
        --hitDepth;

        return hit;
    }

    // The character preview is a camera viewport: four floats in .rdata (left, right, top,
    // bottom) in 640x480 pixels, one set per box. Rewritten in place because the preview's
    // hit tests read the same floats.
    float* characterView[2]{};
    float characterDesign[2][4]{};

    void ScaleCharacterView()
    {
        const float factor = Dimension(true) / LayoutHeight;
        const float origin = (Dimension(false) - LayoutWidth * factor) * 0.5f;

        for (int box = 0; box < 2; ++box)
        {
            if (characterView[box] == nullptr)
                continue;

            for (int edge = 0; edge < 4; ++edge)
            {
                const float design = characterDesign[box][edge];

                injector::WriteMemory<float>(characterView[box] + edge,
                                             edge < 2 ? design * factor + origin : design * factor, true);
            }
        }
    }

    // The preview's FOV arrives multiplied by the layout factor, cancelling the viewport scale;
    // divide it back down for the draw. No-op at 640x480.
    constexpr ptrdiff_t PreviewFOV = 0x70;

    SafetyHookInline shCharacterView{};

    void __fastcall CharacterView(void* self, void* edx)
    {
        float* fov = reinterpret_cast<float*>(static_cast<uint8_t*>(self) + PreviewFOV);
        const float authored = *fov;

        *fov /= Dimension(true) / LayoutHeight;
        shCharacterView.thiscall<void>(self);
        *fov = authored;
    }

    // A copy rather than an edit in place: the callers that build a nine-slice frame derive
    // each of its rectangles from the last, and would compound the scale.
    void Scale(const float* quad, float* scaled)
    {
        const float factor = Dimension(true) / LayoutHeight;
        const float centre = 0.5f - LayoutWidth * 0.5f * factor / Dimension(false);

        static float applied = 0.0f;
        if (applied != factor)
        {
            applied = factor;
            spdlog::info("Menu: layout factor {:.4f}, left edge {:.4f}", factor, centre);

            ScaleCharacterView();
        }

        scaled[0] = quad[0] * factor + centre;
        scaled[1] = quad[1] * factor;
        scaled[2] = quad[2] * factor + centre;
        scaled[3] = quad[3] * factor;
    }

    void __fastcall WriteQuad(void* self, void* edx, const float* quad, void* uv, int index,
                              int arg4, void* colour)
    {
        // The first element's +0x1c is the root container; see notes/menu.md.
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            spdlog::info("Menu: first element {:#x}", reinterpret_cast<uintptr_t>(self));
        }

        float scaled[4];
        Scale(quad, scaled);

        shWriteQuad.thiscall<void>(self, scaled, uv, index, arg4, colour);
    }

    // The map preview writes its sprite through a private copy of the quad writer. Hooked via
    // its call site: the same body also exists as the credits backdrop writer.
    SafetyHookInline shSpriteQuad{};

    void __stdcall SpriteQuad(void* sprite, const float* quad, void* uv, uint32_t colour)
    {
        float scaled[4];
        Scale(quad, scaled);

        shSpriteQuad.stdcall<void>(sprite, scaled, uv, colour);
    }

    // Third copy of the quad writer, reached only by the map widget.
    SafetyHookInline shMapQuad{};

    void __stdcall MapQuad(void* sprite, const float* quad, void* uv, int index, void* colour)
    {
        float scaled[4];
        Scale(quad, scaled);

        shMapQuad.stdcall<void>(sprite, scaled, uv, index, colour);
    }

    // FUN_006c1d90 returns a fraction of the 640x480 design space while the rest of the widget
    // works in backbuffer pixels; rebase it so the layout transform covers the whole overlay.
    SafetyHookInline shMapPoint{};

    void __fastcall MapPoint(void* self, void* edx, float x, float y, float* outX, float* outY)
    {
        shMapPoint.thiscall<void>(self, x, y, outX, outY);

        *outX *= LayoutWidth / Dimension(false);
        *outY *= LayoutHeight / Dimension(true);
    }

    // Markers are written vertex by vertex and bypass the writers above.
    SafetyHookInline shMapMarker{};

    void __fastcall MapMarker(void* self, void* edx, const float* quad, void* uv, void* colour)
    {
        float scaled[4];
        Scale(quad, scaled);

        shMapMarker.thiscall<void>(self, scaled, uv, colour);
    }

    using GetOptions = uint8_t*(__thiscall*)(void*);

    void** ppOptions = nullptr;

    // The pane is an element and only scales under ScaleCommandMap (options+0xbd; +0xbe is
    // ScaleHUD), so the non-element pieces follow the same option.
    float MapFactor()
    {
        void* manager = *ppOptions;

        return (*reinterpret_cast<GetOptions**>(manager))[88](manager)[0xbd] != 0
                   ? Dimension(true) / LayoutHeight : 1.0f;
    }

    // The cursor is shared by the shell (always scaled) and the command map (scaled under its
    // option). The overlay's draw leaves a mark and Cursor() spends it.
    bool mapDrawn = false;

    SafetyHookMid shMapDraw{};

    void MapDraw(SafetyHookContext&)
    {
        mapDrawn = true;
    }

    // Icon sizes come in texels through these two accessors and nothing else calls them. The
    // caller draws extent + 1 pixels, so the +1 goes inside the multiply; factor 1.0 is then
    // an identity.
    SafetyHookInline shIconExtent[2]{};

    int Extent(SafetyHookInline& hook, int icon)
    {
        return static_cast<int>((hook.stdcall<int>(icon) + 1) * MapFactor()) - 1;
    }

    int __stdcall IconExtentX(int icon)
    {
        return Extent(shIconExtent[0], icon);
    }

    int __stdcall IconExtentY(int icon)
    {
        return Extent(shIconExtent[1], icon);
    }

    // The hover label is sized from the pane's viewport rect at [renderer+0x1f0..0x1fc]. Scaled
    // at the store, not the call: the width is reused to test for overrun of the pane's right edge.
    SafetyHookMid shMapLabelExtent{};

    void MapLabelExtent(SafetyHookContext& ctx)
    {
        const float factor = GlyphScale(MapFactor());
        int* extent = reinterpret_cast<int*>(ctx.esp + 8);

        extent[0] = static_cast<int>(extent[0] / factor);
        extent[1] = static_cast<int>(extent[1] / factor);
    }

    // The scene viewports build and submit their quad in one per-frame call, so the rect is
    // substituted before the call rather than the vertices rewritten after it.
    void Substitute(void* element, int* design)
    {
        int* rect = reinterpret_cast<int*>(static_cast<uint8_t*>(element) + 0x34);
        std::memcpy(design, rect, sizeof(int) * 4);

        const float factor = Dimension(true) / LayoutHeight;

        rect[0] = static_cast<int>(design[0] * factor
                                   + (Dimension(false) - LayoutWidth * factor) * 0.5f);
        rect[1] = static_cast<int>(design[1] * factor);
        rect[2] = static_cast<int>(design[2] * factor);
        rect[3] = static_cast<int>(design[3] * factor);
    }

    void Restore(void* element, const int* design)
    {
        std::memcpy(static_cast<uint8_t*>(element) + 0x34, design, sizeof(int) * 4);
    }

    SafetyHookInline shSceneQuad{};

    void __fastcall SceneQuad(void* self, void* edx)
    {
        int design[4];
        Substitute(self, design);

        shSceneQuad.thiscall<void>(self);

        Restore(self, design);
    }

    SafetyHookInline shSceneBands{};

    // Its band quads are insets off the same rect. Screen unverified; see notes.
    void __fastcall SceneBands(void* self, void* edx)
    {
        int design[4];
        Substitute(self, design);

        shSceneBands.thiscall<void>(self);

        Restore(self, design);
    }

    // The loading-screen chrome mixes element-rect coordinates with ones built from
    // displayWidth * 0.5. Presenting a 640x480 display for the duration puts all of them in the
    // design box, so one transform covers the lot. The dimensions are two ints on the renderer.
    constexpr ptrdiff_t DisplayWidth = 0x94d8;
    constexpr ptrdiff_t DisplayHeight = 0x94dc;

    bool chromeActive = false;
    float chromeWidth = 0.0f;
    float chromeFactor = 1.0f;

    SafetyHookInline shChromeBuild{};

    void __fastcall ChromeBuild(void* self, void* edx)
    {
        int* width = reinterpret_cast<int*>(static_cast<uint8_t*>(*ppDisplay) + DisplayWidth);
        int* height = reinterpret_cast<int*>(static_cast<uint8_t*>(*ppDisplay) + DisplayHeight);

        const int realWidth = *width;
        const int realHeight = *height;

        chromeWidth = static_cast<float>(realWidth);
        chromeFactor = static_cast<float>(realHeight) / LayoutHeight;

        *width = static_cast<int>(LayoutWidth);
        *height = static_cast<int>(LayoutHeight);
        chromeActive = true;

        shChromeBuild.thiscall<void>(self);

        chromeActive = false;
        *width = realWidth;
        *height = realHeight;
    }

    SafetyHookInline shChromeQuad{};

    void __fastcall ChromeQuad(void* self, void* edx, const float* quad, void* uv, int index,
                               int arg4, void* arg5)
    {
        if (!chromeActive)
        {
            shChromeQuad.thiscall<void>(self, quad, uv, index, arg4, arg5);
            return;
        }

        // Fractions of the design box now. The vertical is an identity: 480 * factor is the
        // display height.
        const float scale = LayoutWidth * chromeFactor / chromeWidth;
        const float centre = 0.5f - LayoutWidth * 0.5f * chromeFactor / chromeWidth;

        const float boxed[4]{ quad[0] * scale + centre, quad[1], quad[2] * scale + centre, quad[3] };

        shChromeQuad.thiscall<void>(self, boxed, uv, index, arg4, arg5);
    }

    // The console is a screen-edge overlay. Rather than exempt it from the transform, it gets a
    // design rect the transform maps onto the whole backbuffer:
    //     x = -origin / factor,  w = width / factor.
    using Layout = void(__thiscall*)(void*);

    SafetyHookInline shConsoleOpen{};

    void __fastcall ConsoleOpen(void* self, void* edx, char open)
    {
        shConsoleOpen.thiscall<void>(self, open);

        if (open == 0)
            return;

        const float factor = Dimension(true) / LayoutHeight;
        const float origin = (Dimension(false) - LayoutWidth * factor) * 0.5f;

        uint8_t* element = static_cast<uint8_t*>(self);

        // Truncation pulls both edges inside the screen; push each outward until it covers.
        int left = static_cast<int>(-origin / factor);
        if (left * factor + origin > 0.0f)
            --left;

        int right = static_cast<int>((Dimension(false) - origin) / factor);
        if (right * factor + origin < Dimension(false))
            ++right;

        *reinterpret_cast<int*>(element + 0x2c) = left;
        *reinterpret_cast<int*>(element + 0x3c) = right - left;

        // Slot +0x88 recomputes this element's absolute rect from the relative one and recurses
        // into its children, which is what carries the change to the rows.
        (*reinterpret_cast<Layout**>(self))[0x88 / sizeof(void*)](self);
    }

    // The cursor writes its own quad at backbuffer pixels, sized from a literal 32, so it needs
    // scale only. The two multiplies are repointed at cursorSize; the draw refreshes it.
    constexpr float CursorSize = 32.0f;

    float cursorSize = CursorSize;

    SafetyHookInline shCursor{};

    void __fastcall Cursor(void* self, void* edx, uint32_t arg)
    {
        cursorSize = CursorSize * (mapDrawn ? MapFactor() : Dimension(true) / LayoutHeight);
        mapDrawn = false;

        shCursor.thiscall<void>(self, arg);
    }

    // The credits backdrop is a 640x480 screenshot stretched over the whole backbuffer, so
    // it arrives in screen space; pillarbox it directly.
    SafetyHookInline shCreditsQuad{};

    void __stdcall CreditsQuad(void* sprite, const float* quad, void* uv, uint32_t colour)
    {
        const float factor = Dimension(true) / LayoutHeight;
        const float centre = 0.5f - LayoutWidth * 0.5f * factor / Dimension(false);

        const float boxed[4]
        {
            quad[0] * (1.0f - centre - centre) + centre,
            quad[1],
            quad[2] * (1.0f - centre - centre) + centre,
            quad[3],
        };

        shCreditsQuad.stdcall<void>(sprite, boxed, uv, colour);
    }

    // int64_t returns in EDX:EAX, the pair of pushes the patched site needs. The 640x480
    // fallback leaves the menu stock.
    int64_t __cdecl MenuResolution()
    {
        int32_t width = 640;
        int32_t height = 480;

        if (void* session = *ppSession)
        {
            const uint8_t* video = (*reinterpret_cast<GetVideoSettings**>(session))[3](session);

            if (video != nullptr && *reinterpret_cast<const uint16_t*>(video + Width) != 0)
            {
                width = *reinterpret_cast<const uint16_t*>(video + Width);
                height = *reinterpret_cast<const uint16_t*>(video + Height);
            }
        }

        return (static_cast<int64_t>(height) << 32) | static_cast<uint32_t>(width);
    }

    // Create the device using the configured resolution instead of hardcoded 640x480.
    // Keep the mode switch when the movie screen opens, since it rebuilds components
    // that capture the resolution at creation (including the UI camera viewport).
    SafetyHookMid shBootMode{};

    void BootMode(SafetyHookContext& ctx)
    {
        const int64_t mode = MenuResolution();
        uint8_t* renderer = reinterpret_cast<uint8_t*>(ctx.esi);

        *reinterpret_cast<int*>(renderer + DisplayWidth) = static_cast<int32_t>(mode);
        *reinterpret_cast<int*>(renderer + DisplayHeight) = static_cast<int32_t>(mode >> 32);
    }

    using SetResolution = void(__thiscall*)(void*, int, int);

    void** ppDisplayMgr = nullptr;

    SafetyHookInline shMovieOpen{};

    void __fastcall MovieOpen(void* self, void* edx)
    {
        if (void* manager = *ppDisplayMgr)
        {
            const int64_t mode = MenuResolution();

            // The setter early-outs on a matching mode; the width is cleared so it does not,
            // and SetDisplayMode writes it back before the reset.
            *reinterpret_cast<int*>(static_cast<uint8_t*>(*ppDisplay) + DisplayWidth) = 0;

            (*reinterpret_cast<SetResolution**>(manager))[0x124 / sizeof(void*)](
                manager, static_cast<int32_t>(mode), static_cast<int32_t>(mode >> 32));
        }

        shMovieOpen.thiscall<void>(self);
    }
}

FEATURE(Game, Menu)
{
    // The gameplay resolution site, which is also where the two field offsets come from.
    auto gameplay = hook::pattern("8B 0D ? ? ? ? 8B 11 FF 52 0C 8B 0D ? ? ? ? 33 FF 66 8B 78 18 33 D2 66 8B 50 16");
    if (gameplay.empty())
    {
        spdlog::error("Menu: session manager not found");
        return;
    }

    ppSession = *gameplay.get(0).get<void**>(2);

    auto sites = hook::pattern("8B 0D ? ? ? ? 68 E0 01 00 00 68 80 02 00 00");
    if (sites.size() != 2)
    {
        spdlog::error("Menu: expected two 640x480 sites, found {}", sites.size());
        return;
    }

    ppDisplayMgr = *sites.get(0).get<void**>(2);

    sites.for_each_result([](hook::pattern_match match)
    {
        uint8_t* site = match.get<uint8_t>();

        // The call clobbers ECX, so the load of the display manager moves after it.
        uint8_t manager[6];
        std::memcpy(manager, site, sizeof(manager));

        injector::MakeCALL(site, reinterpret_cast<void*>(MenuResolution));
        injector::WriteMemory<uint16_t>(site + 5, 0x5052, true);  // PUSH EDX; PUSH EAX
        injector::WriteMemoryRaw(site + 7, manager, sizeof(manager), true);
        injector::MakeNOP(site + 13, 3, true);
    });

    // The writer's own first act is to fetch the two dimensions, so the pattern carries the
    // display manager as well.
    auto quad = hook::pattern("83 EC 18 53 55 56 8B F1 33 DB 57 39 5E 68 0F 84 ? ? ? ? 8B 0D ? ? ? ?");
    if (quad.empty())
    {
        spdlog::error("Menu: element quad writer not found");
        return;
    }

    ppDisplay = *quad.get(0).get<void**>(22);

    shWriteQuad = safetyhook::create_inline(quad.get_first(), WriteQuad);

    auto movie = hook::pattern("64 A1 00 00 00 00 6A FF 68 ? ? ? ? 50 64 89 25 00 00 00 00 83 EC 1C 55 56 8B F1 "
                               "33 ED 8B 8E 18 01 00 00 C6 86 30 01 00 00 00");
    if (movie.size() != 1)
    {
        spdlog::error("Menu: expected one movie screen open, found {}", movie.size());
        return;
    }

    shMovieOpen = safetyhook::create_inline(movie.get_first(), MovieOpen);

    // Both stores of the 640x480 branch; the hook goes after the second.
    auto boot = hook::pattern("8B 4B 08 C7 02 80 02 00 00 89 8E E0 94 00 00 C7 86 DC 94 00 00 E0 01 00 00");
    if (boot.size() != 1)
    {
        spdlog::error("Menu: expected one boot-time 640x480 site, found {}", boot.size());
        return;
    }

    shBootMode = safetyhook::create_mid(boot.get(0).get<void>(25), BootMode);

    // Apply resolution in menus now that the menu is no longer hardcoded to 640x480.
    auto apply = hook::pattern("FF 50 64 84 C0 74 22 8B 8E 1C 01 00 00 33 C0 33 D2 66 8B 41 16");
    if (apply.size() != 1)
    {
        spdlog::error("Menu: expected one options apply mode switch, found {}", apply.size());
        return;
    }

    injector::MakeNOP(apply.get(0).get<void>(5), 2, true);

    auto text = hook::pattern("83 EC 10 56 8B F1 8A 86 F0 01 00 00 84 C0");
    if (text.empty())
    {
        spdlog::error("Menu: shell text drawer not found");
        return;
    }

    shShellText = safetyhook::create_inline(text.get_first(), ShellText);

    auto walk = hook::pattern("53 55 56 8B F1 57 8B 46 10 83 E0 01 3C 01");
    if (walk.empty())
    {
        spdlog::error("Menu: element hit test not found");
        return;
    }

    shHitTest = safetyhook::create_inline(walk.get_first(), HitTest);

    auto preview = hook::pattern("50 8D 54 24 24 51 52 53 8B CE D9 5C 24 2C E8");
    if (preview.empty())
    {
        spdlog::error("Menu: map preview draw not found");
        return;
    }

    uint8_t* call = preview.get(0).get<uint8_t>(14);

    shSpriteQuad = safetyhook::create_inline(call + 5 + *reinterpret_cast<int32_t*>(call + 1), SpriteQuad);

    // Second copy of the writer, again reached through its only call site.
    auto credits = hook::pattern("C7 44 24 24 00 00 70 3F E8");
    if (credits.empty())
    {
        spdlog::error("Menu: credits backdrop draw not found");
        return;
    }

    call = credits.get(0).get<uint8_t>(8);

    shCreditsQuad = safetyhook::create_inline(call + 5 + *reinterpret_cast<int32_t*>(call + 1), CreditsQuad);

    auto mapQuad = hook::pattern("83 EC 10 53 55 56 8B 74 24 20 33 DB 57 3B F3");
    if (mapQuad.empty())
    {
        spdlog::error("Menu: map pane quad writer not found");
        return;
    }

    shMapQuad = safetyhook::create_inline(mapQuad.get_first(), MapQuad);

    auto point = hook::pattern("D9 44 24 04 D8 A1 C0 00 00 00");
    auto marker = hook::pattern("83 EC 0C 56 8B F1 8B 86 04 01 00 00 83 F8 08");
    if (point.empty() || marker.empty())
    {
        spdlog::error("Menu: map overlay not found");
        return;
    }

    shMapPoint = safetyhook::create_inline(point.get_first(), MapPoint);
    shMapMarker = safetyhook::create_inline(marker.get_first(), MapMarker);

    auto options = hook::pattern("8B 0D ? ? ? ? 8B 11 FF 92 60 01 00 00 8A 98 BD 00 00 00");
    if (options.empty())
    {
        spdlog::error("Menu: options manager not found");
        return;
    }

    ppOptions = *options.get(0).get<void**>(2);

    auto extents = hook::pattern("D9 05 ? ? ? ? 56 8B 74 24 08 C1 E6 04 57 D8 8E");
    if (extents.size() != 2)
    {
        spdlog::error("Menu: expected two map icon extents, found {}", extents.size());
        return;
    }

    shIconExtent[0] = safetyhook::create_inline(extents.get(0).get<void>(), IconExtentX);
    shIconExtent[1] = safetyhook::create_inline(extents.get(1).get<void>(), IconExtentY);

    // The overlay's draw tests its own open flag first; the mark goes just past that test, so a
    // closed overlay leaves none.
    auto mapDraw = hook::pattern("83 EC 24 55 56 8B F1 57 8A 46 30 84 C0 0F 84");
    if (mapDraw.size() != 1)
    {
        spdlog::error("Menu: expected one command overlay draw, found {}", mapDraw.size());
        return;
    }

    shMapDraw = safetyhook::create_mid(mapDraw.get(0).get<void>(19), MapDraw);

    // Both dimensions are stored by the two instructions this straddles; the hook goes after the
    // second of them.
    auto label = hook::pattern("89 44 24 0C 8B 49 04 8B 42 04 81 C1 84 00 00 00");
    if (label.size() != 1)
    {
        spdlog::error("Menu: expected one map label extent, found {}", label.size());
        return;
    }

    shMapLabelExtent = safetyhook::create_mid(label.get(0).get<void>(4), MapLabelExtent);

    // Two matches, the large preview box and the small one, each pattern carrying the address of
    // its own first float.
    auto preview3d = hook::pattern("D9 05 ? ? ? ? D9 05 ? ? ? ? D9 05 ? ? ? ? 8B 45 28 D9 05 ? ? ? ?");
    if (preview3d.size() != 2)
    {
        spdlog::error("Menu: expected two character preview viewports, found {}", preview3d.size());
        return;
    }

    for (int box = 0; box < 2; ++box)
    {
        characterView[box] = *preview3d.get(box).get<float*>(23);
        std::memcpy(characterDesign[box], characterView[box], sizeof(characterDesign[box]));
    }

    auto character = hook::pattern("64 A1 00 00 00 00 6A FF 68 ? ? ? ? 50 64 89 25 00 00 00 00 83 EC 6C 53 55 8B E9 33 DB 39 5D 2C");
    if (character.empty())
    {
        spdlog::error("Menu: character preview draw not found");
        return;
    }

    shCharacterView = safetyhook::create_inline(character.get_first(), CharacterView);

    auto scene = hook::pattern("6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 34 A0 ? ? ? ? 53 55 56");
    auto bands = hook::pattern("6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 4C 53 55 8B D9 BD 02 00 00 00");
    if (scene.empty() || bands.empty())
    {
        spdlog::error("Menu: scene viewport builders not found");
        return;
    }

    shSceneQuad = safetyhook::create_inline(scene.get_first(), SceneQuad);
    shSceneBands = safetyhook::create_inline(bands.get_first(), SceneBands);

    // The F4 is what separates this writer from its byte-for-byte twin 0068b950, which tests +0xf0
    // instead; do not match it by body without that byte.
    auto chrome = hook::pattern("83 EC 18 53 55 56 8B F1 33 DB 57 39 9E F4 00 00 00");
    auto chromeBuild = hook::pattern("64 A1 00 00 00 00 6A FF 68 ? ? ? ? 50 64 89 25 00 00 00 00 81 EC DC 00 00 00 56 8B F1 8B 86 F4 00 00 00");
    if (chrome.empty() || chromeBuild.empty())
    {
        spdlog::error("Menu: loading screen chrome not found");
        return;
    }

    shChromeQuad = safetyhook::create_inline(chrome.get_first(), ChromeQuad);
    shChromeBuild = safetyhook::create_inline(chromeBuild.get_first(), ChromeBuild);

    auto console = hook::pattern("53 8B 5C 24 08 56 8B F1 53 E8 ? ? ? ? 84 DB 74 0E 8B 0D ? ? ? ? 8B 01 FF 50 14 89 46 3C");
    if (console.empty())
    {
        spdlog::error("Menu: console open handler not found");
        return;
    }

    shConsoleOpen = safetyhook::create_inline(console.get_first(), ConsoleOpen);

    auto cursor = hook::pattern("83 EC 0C 56 8B F1 8B 0D ? ? ? ? 8B 01 FF 50 64");
    auto size = hook::pattern("D9 44 24 04 D8 0D ? ? ? ? D9 44 24 08 D8 0D ? ? ? ?");
    if (cursor.empty() || size.empty())
    {
        spdlog::error("Menu: cursor draw not found");
        return;
    }

    for (ptrdiff_t operand : { 6, 16 })
        injector::WriteMemory<float*>(size.get(0).get<void>(operand), &cursorSize, true);

    shCursor = safetyhook::create_inline(cursor.get_first(), Cursor);

    // These screens ask the display for its size and lay themselves out in it, outside the scaled
    // 640x480 space. Each five-byte fetch becomes MOV EAX, imm32; the manager load between them
    // stays as a dead load. `height` is the height fetch's offset, -1 for a width-only site.
    static constexpr struct { const char* what; const char* pattern; ptrdiff_t height; } screens[]
    {
        { "options",              "8B 01 FF 50 14 8B 0D ? ? ? ? 8B E8 8B 11 FF 52 18 8B 56 40",    13 },
        { "pause",                "8B 11 FF 52 14 8B 0D ? ? ? ? 8B F0 8B 01 FF 50 18 8B 5F 3C",    13 },
        { "loading",              "8B 01 FF 50 14 8B 0D ? ? ? ? 8B F0 89 74 24 48 8B 11 FF 52 18", 17 },
        { "save game",            "8B 11 FF 52 14 8B 0D ? ? ? ? 8B F8 8B 01 FF 50 18 8B 5E 3C",    13 },
        { "confirm from save",    "8B 01 FF 50 14 8B 0D ? ? ? ? 8B D8 8B 11 FF 52 18 8B 57 3C",    13 },
        { "confirm from pause",   "8B 01 FF 50 14 8B 0D ? ? ? ? 8B F8 8B 11 FF 52 18 8B 56 3C",    13 },
        { "prompt",               "8B 01 FF 50 14 8B 0D ? ? ? ? 8B F8 8B 11 FF 52 18 8B 46 38",    13 },
        // A widget looked up by name; no height fetch.
        { "action info",          "8B 11 FF 52 14 8B 56 48 8B 4E 3C 03 56 38 8B 3E 2B C1",        -1 },
    };

    // One screen missing is one screen off-screen, not a reason to drop the other seven.
    for (const auto& screen : screens)
    {
        auto site = hook::pattern(screen.pattern);
        if (site.empty())
        {
            spdlog::error("Menu: {} screen layout not found", screen.what);
            continue;
        }

        injector::WriteMemory<uint8_t>(site.get(0).get<void>(0), 0xb8, true);
        injector::WriteMemory<uint32_t>(site.get(0).get<void>(1), 640, true);

        if (screen.height < 0)
            continue;

        injector::WriteMemory<uint8_t>(site.get(0).get<void>(screen.height), 0xb8, true);
        injector::WriteMemory<uint32_t>(site.get(0).get<void>(screen.height + 1), 480, true);
    }
}
