#include <loom/module.h>
#include <loom/export.h>
#include "stack_light.hpp"



// ---- Module ----
class StackLight : public loom::Module<StackLightConfig, StackLightRecipe, StackLightRuntime, StackLightCmd  > {
public:
    LOOM_MODULE_HEADER("StackLight", "1.0.0")

    void init(const loom::InitContext& /*ctx*/) override {
        runtime_ = StackLightRuntime{};

        // Service to set light pattern
        registerLocalService<StackLightCmd>("set", [this](const StackLightCmd& cmd) -> loom::CallResult {
            runtime_.cmd = cmd;
            return {true, R"({"ok":true})", ""};
        });
    }

    void cyclic() override {
        runtime_.cycle_count++;

        // Compute blink state
        int cyclesPerBlink = config_.blink_rate_ms / 100; // assuming 100ms cycle
        if (cyclesPerBlink < 1) cyclesPerBlink = 1;
        bool blinkOn = (runtime_.cycle_count / cyclesPerBlink) % 2 == 0;

        // Resolve each light
        runtime_.red_output = resolveLight(runtime_.cmd.red, blinkOn);
        runtime_.yellow_output = resolveLight(runtime_.cmd.yellow, blinkOn);
        runtime_.green_output = resolveLight(runtime_.cmd.green, blinkOn);
        runtime_.blue_output = resolveLight(runtime_.cmd.blue, blinkOn);
        runtime_.buzzer_output = resolveLight(runtime_.cmd.buzzer, blinkOn);

        summary_ = runtime_.cmd; // mirror runtime to summary for dashboard display

        // Publish status for the UI
        publishLocal("status", StackLightCmd{
            .red    = runtime_.red_output,
            .yellow = runtime_.yellow_output,
            .green  = runtime_.green_output,
            .blue   = runtime_.blue_output,
            .buzzer = runtime_.buzzer_output,
        });
    }

    void exit() override {
        // All lights off
        runtime_.cmd = StackLightCmd{};
    }

    void longRunning() override {}

private:
    static bool resolveLight(int state, bool blinkOn) {
        if (state == 0) return false;      // off
        if (state == 1) return true;       // on
        if (state == 2) return blinkOn;    // blink
        return false;
    }
};

LOOM_REGISTER_MODULE(StackLight)
