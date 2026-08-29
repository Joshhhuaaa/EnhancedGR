#pragma once

// A fix has to wait for the module that owns the code it patches. GameModule::Game is the
// executable itself, which is always mapped before the asi loads, so those run immediately.
// Add an entry here and in ModuleName() in feature.cpp for any dll the game loads later.
enum class GameModule
{
    Game,
    D3D8,
};

using FeatureFn = void(*)();

struct Feature
{
    Feature(GameModule module, FeatureFn fn);
};

void RegisterFeatures();

#define FEATURE(module, name)                                   \
    static void name();                                         \
    static Feature name##_Feature(GameModule::module, name);    \
    static void name()
