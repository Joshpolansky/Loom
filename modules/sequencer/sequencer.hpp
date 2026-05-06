#pragma once

#include <string>

// ---- Data structs ----
struct SequencerConfig {
    std::string sequence_name = "MainSequence";
    // Module IDs of connected modules (configured at deploy time)
    std::string motor_id = "example_motor";
    std::string clamp_id = "pneumatic_actuator";
    std::string light_id = "stack_light";
    int step_timeout_ms = 5000;
};

struct SequencerRecipe {
    double motor_speed = 500.0;       // RPM for the run step
    double motor_acceleration = 200.0;
    int cycle_target = 0;             // 0 = run forever
};

struct SequencerRuntime {
    static constexpr std::string_view kTypeId = "SequencerRuntime/1";
    // Sequence state
    int step = 0;                     // Current step index
    std::string step_name = "idle";   // Human-readable step name
    bool running = false;
    bool complete = false;
    bool fault = false;
    std::string fault_message;

    // Stats
    uint64_t cycle_count = 0;
    uint64_t parts_produced = 0;
    int step_timer_ms = 0;

    // Last known connected module states (updated via subscriptions)
    bool motor_at_speed = false;
    bool clamp_extended = false;
    bool clamp_retracted = true;
};

struct SequencerStatus {
    int step;
    std::string step_name;
    bool running;
    uint64_t parts_produced;
};

