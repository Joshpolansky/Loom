#pragma once

#include <string>

// --- POD structs only: config, recipe, runtime ---
struct ExampleConfig {
    int rate = 10;
};

struct ExampleRecipe {
    double speed = 1.0;
};

struct ExampleRuntime {
    double pos = 0.0;
};

inline constexpr const char* ExampleIdentifier = "ExampleModule";
