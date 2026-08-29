#include "stdafx.h"
#include "common.hpp"
#include "feature.hpp"

namespace
{
    struct Entry
    {
        GameModule module;
        FeatureFn  fn;
    };

    std::vector<Entry>& List()
    {
        static std::vector<Entry> list;
        return list;
    }

    // An empty name runs the feature immediately, which is what GameModule::Game wants.
    const wchar_t* ModuleName(GameModule module)
    {
        switch (module)
        {
        case GameModule::D3D8: return L"d3d8.dll";
        default:               return L"";
        }
    }
}

Feature::Feature(GameModule module, FeatureFn fn)
{
    List().push_back({ module, fn });
}

void RegisterFeatures()
{
    // CallbackHandler keys module callbacks in a std::map, so registering one per feature
    // silently drops every fix past the first for a given module. Group by module and
    // register a single callback that runs them all.
    std::map<std::wstring, std::vector<FeatureFn>> byModule;

    for (auto& entry : List())
        byModule[ModuleName(entry.module)].push_back(entry.fn);

    for (auto& [name, fns] : byModule)
    {
        CallbackHandler::RegisterCallback(name, [fns]()
        {
            for (auto fn : fns)
                fn();
        });
    }
}
