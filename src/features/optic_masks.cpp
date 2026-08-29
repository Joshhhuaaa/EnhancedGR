#include "stdafx.h"
#include "config.hpp"
#include "feature.hpp"

static Config::Value nOpticMaskAspect("HUD", "OpticMaskAspect", 1);

namespace
{
    enum { Stock, FitHeight, FillWidth };

    // Each mask is a nine-vertex 3x3 grid whose positions are rewritten over the backbuffer
    // every frame; its shape comes from the authored texture coordinates alone (one quadrant
    // mirrored, 0.01 at the edges, 0.99 at the centre), so scaling one axis is the correction.
    constexpr ptrdiff_t NightVisionMask = 0x2e2c;
    constexpr ptrdiff_t OpticMask = 0x2e30;

    constexpr ptrdiff_t Vertices = 0x180;
    constexpr ptrdiff_t Changed = 0x7f;
    constexpr ptrdiff_t Stride = 0x40 / sizeof(float);

    constexpr float Edge = 0.01f;
    constexpr float Centre = 0.99f;
    constexpr float LayoutAspect = 4.0f / 3.0f;

    using GetDimension = int(__thiscall*)(void*);

    void** ppDisplay = nullptr;

    SafetyHookInline shDrawMasks{};

    float Dimension(bool vertical)
    {
        void* display = *ppDisplay;

        return static_cast<float>((*reinterpret_cast<GetDimension**>(display))[vertical ? 4 : 3](display));
    }

    // A coordinate pushed past the edge must sample the mask's black border, which needs CLAMP;
    // the engine never sets an address mode, so the default WRAP stands. Stage 0 and the
    // scaled axis only: the nvnoise layer on stage 1 relies on wrapping.
    enum { AddressU = 13, AddressV = 14, Wrap = 1, Clamp = 3 };

    using SetTextureStageState = HRESULT(__stdcall*)(void*, uint32_t, uint32_t, uint32_t);
    constexpr ptrdiff_t StageState = 0xfc / sizeof(void*); // IDirect3DDevice8 vtable

    // Set around the flush's own DrawIndexedPrimitive: the mask draw only registers into a
    // render list, so a sampler change there reaches only some frames. Device at renderer+0xc.
    constexpr ptrdiff_t Device = 0x0c;

    void* masks[2]{};
    bool clamping = false;

    SafetyHookInline shDrawMesh{};

    void Address(void* device, uint32_t mode)
    {
        (*reinterpret_cast<SetTextureStageState**>(device))[StageState]
            (device, 0, nOpticMaskAspect == FitHeight ? AddressU : AddressV, mode);
    }

    // RET 8: the second argument (a counter index) is real even though the decompiler drops it.
    void __fastcall DrawMesh(uint8_t* self, void* edx, void* mesh, uint32_t counter)
    {
        if (!clamping || (mesh != masks[0] && mesh != masks[1]))
        {
            shDrawMesh.thiscall<void>(self, mesh, counter);
            return;
        }

        void* device = *reinterpret_cast<void**>(self + Device);

        Address(device, Clamp);
        shDrawMesh.thiscall<void>(self, mesh, counter);
        Address(device, Wrap);
    }

    void Correct(void* mask, float edge)
    {
        if (mask == nullptr)
            return;

        float* vertex = *reinterpret_cast<float**>(static_cast<uint8_t*>(mask) + Vertices);
        if (vertex == nullptr)
            return;

        const ptrdiff_t axis = nOpticMaskAspect == FitHeight ? 0 : 1;

        for (int i = 0; i < 9; ++i, vertex += Stride)
        {
            const float value = (axis == 0 ? i % 3 : i / 3) == 1 ? Centre : edge;

            if (vertex[axis] == value)
                continue;

            vertex[axis] = value;
            *(static_cast<uint8_t*>(mask) + Changed) = 1;
        }
    }

    void __fastcall DrawMasks(uint8_t* self, void* edx, void* arg)
    {
        const float width = Dimension(false);
        const float height = Dimension(true);
        const float aspect = width / height;
        const float edge = Centre - (Centre - Edge) *
                           (nOpticMaskAspect == FitHeight ? aspect / LayoutAspect : LayoutAspect / aspect);

        static float applied = 0.0f;
        if (applied != edge)
        {
            applied = edge;
            spdlog::info("OpticMasks: {}x{}, {} edge at {:.4f}", static_cast<int>(width),
                         static_cast<int>(height),
                         nOpticMaskAspect == FitHeight ? "horizontal" : "vertical", edge);
        }

        masks[0] = *reinterpret_cast<void**>(self + NightVisionMask);
        masks[1] = *reinterpret_cast<void**>(self + OpticMask);

        Correct(masks[0], edge);
        Correct(masks[1], edge);

        // Only a coordinate that has left the mask needs the clamp. Stands until the next frame
        // because the draw it is meant for happens after this returns.
        clamping = edge < 0.0f;

        shDrawMasks.thiscall<void>(self, arg);
    }
}

FEATURE(Game, OpticMasks)
{
    if (nOpticMaskAspect == Stock)
        return;

    // The mask draw. RET 4, so the unused argument must be forwarded; the display manager
    // load sits at offset 53.
    auto draw = hook::pattern("83 EC 18 53 56 8B F1 57 8B 0D ? ? ? ? 8B 01 FF 50 54 84 C0 0F 85 ? ? ? ? "
                              "8B 0D ? ? ? ? E8 ? ? ? ? 8B 0D ? ? ? ? 8A D8 E8 ? ? ? ? 8B 0D ? ? ? ?");
    if (draw.empty())
    {
        spdlog::error("OpticMasks: mask draw not found");
        return;
    }

    auto mesh = hook::pattern("53 55 56 57 8B 7C 24 14 8B F1 8B 87 94 01 00 00 8B 9F 98 01 00 00 8B AF A0 01 00 00");
    if (mesh.empty())
    {
        spdlog::error("OpticMasks: mesh draw not found");
        return;
    }

    ppDisplay = *draw.get(0).get<void**>(53);

    shDrawMasks = safetyhook::create_inline(draw.get_first(), DrawMasks);
    shDrawMesh = safetyhook::create_inline(mesh.get_first(), DrawMesh);

    if (!shDrawMasks || !shDrawMesh)
        spdlog::error("OpticMasks: hook installation failed");
}
