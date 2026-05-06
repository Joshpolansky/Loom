#include <string>

// ---- Data structs ----

// Light states: 0=off, 1=on, 2=blink
struct StackLightConfig {
    std::string light_name = "StackLight_1";
    int blink_rate_ms = 500;  // Blink half-period
};

struct StackLightRecipe {
    // No recipe params for stack light
    int _pad = 0;
};

/// Received via the "set" service — sets the desired state of each light
struct StackLightCmd {
    int red = 0;
    int yellow = 0;
    int green = 0;
    int blue = 0;
    int buzzer = 0;
};

struct StackLightRuntime {
    static constexpr std::string_view kTypeId = "StackLightRuntime/1";
    // Individual light states (0=off, 1=on, 2=blink)
    StackLightCmd cmd{};

    // Computed output (after blink processing)
    bool red_output = false;
    bool yellow_output = false;
    bool green_output = false;
    bool blue_output = false;
    bool buzzer_output = false;

    uint64_t cycle_count = 0;
};

inline constexpr const char* StackLightIdentifier = "StackLight";