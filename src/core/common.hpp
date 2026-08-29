#pragma once

inline HMODULE baseModule = nullptr;
inline std::filesystem::path sExePath;
inline std::string sExeName;
inline std::filesystem::path sAsiPath;

void InitPaths();

// A bitmap font only looks right at a whole multiple of its size. The nearest one is taken unless
// it overshoots the layout by more than a tenth, where the one below fits instead.
inline float GlyphScale(float factor)
{
    const float nearest = std::round(factor);
    const float glyph = nearest > factor * 1.1f ? std::floor(factor) : nearest;

    return std::max(1.0f, glyph);
}

namespace Memory
{
    void* ReadIAT(HMODULE callerModule, const char* targetModule, const char* targetFunction);
    bool  WriteIAT(HMODULE callerModule, const char* targetModule, const char* targetFunction, void* detour);
}
