#pragma once
#include <string>
#include <vector>

// ---- Data structs ----
struct PneumaticConfig {
    std::string actuator_name = "Actuator_1";
    double extend_time_ms = 500.0;     // Time to fully extend
    double retract_time_ms = 500.0;    // Time to fully retract
    double position_tolerance = 0.02;  // Position tolerance for "at position"
};

struct PneumaticRecipe {
    bool auto_retract = false;         // Auto-retract after extend completes
    double auto_retract_delay_ms = 0;  // Delay before auto-retract
};

struct PneumaticRuntime {
    static constexpr std::string_view kTypeId = "PneumaticRuntime/1";
    // Commands (written by other modules via Bus)
    bool cmd_extend = false;
    bool cmd_retract = false;

    // State
    double position = 0.0;       // 0.0 = retracted, 1.0 = extended
    bool extended = false;        // At full extension
    bool retracted = true;        // At full retraction
    bool moving = false;
    bool fault = false;

    uint64_t cycle_count = 0;
    uint64_t extend_count = 0;
    uint64_t retract_count = 0;
    std::vector<PneumaticRecipe> history = {{},{}}; // History of received commands
};

/// Published on the "status" topic for other modules to subscribe to
struct PneumaticStatus {
    std::string state;
    double position = 0.0;
};

inline constexpr const char* PneumaticIdentifier = "PneumaticActuator";