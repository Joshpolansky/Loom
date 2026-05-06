#include <loom/module.h>
#include <loom/export.h>
#include "pneumatic_actuator.hpp"

// ---- Module ----
class PneumaticActuator : public loom::Module<PneumaticConfig, PneumaticRecipe, PneumaticRuntime, PneumaticStatus> {
public:
    LOOM_MODULE_HEADER("PneumaticActuator", "1.0.0")

    void init(const loom::InitContext& /*ctx*/) override {
        runtime_ = PneumaticRuntime{};

        // Register extend/retract services
        registerLocalService("extend", [this](std::string_view) -> loom::CallResult {
            runtime_.cmd_extend = true;
            runtime_.cmd_retract = false;
            return {true, R"({"accepted":true})", ""};
        });

        registerLocalService("retract", [this](std::string_view) -> loom::CallResult {
            runtime_.cmd_retract = true;
            runtime_.cmd_extend = false;
            return {true, R"({"accepted":true})", ""};
        });

        registerLocalService("state", [this](std::string_view) -> loom::CallResult {
            auto json = glz::write_json(PneumaticStatus{
                .state = runtime_.extended ? "extended" : (runtime_.retracted ? "retracted" : "moving"),
                .position = runtime_.position,
            }).value_or("{}");
            return {true, std::move(json), ""};
        });

    }

    void cyclic() override {
        runtime_.cycle_count++;

        double dt_ms = 100.0; // assuming 100ms cycle

        // Determine motion direction
        double target = 0.0;
        if (runtime_.cmd_extend) {
            target = 1.0;
        } else if (runtime_.cmd_retract) {
            target = 0.0;
        } else {
            target = runtime_.position; // hold
        }

        // Simulate motion
        if (target > runtime_.position) {
            double rate = 1.0 / config_.extend_time_ms; // per ms
            runtime_.position += rate * dt_ms;
            if (runtime_.position >= 1.0) {
                runtime_.position = 1.0;
                runtime_.cmd_extend = false;
            }
            runtime_.moving = true;
        } else if (target < runtime_.position) {
            double rate = 1.0 / config_.retract_time_ms;
            runtime_.position -= rate * dt_ms;
            if (runtime_.position <= 0.0) {
                runtime_.position = 0.0;
                runtime_.cmd_retract = false;
            }
            runtime_.moving = true;
        } else {
            runtime_.moving = false;
        }

        // Update state flags
        runtime_.extended = runtime_.position >= (1.0 - config_.position_tolerance);
        runtime_.retracted = runtime_.position <= config_.position_tolerance;

        if (runtime_.extended && !wasExtended_) {
            runtime_.extend_count++;
        }
        if(runtime_.retracted && wasExtended_) {
            runtime_.retract_count++;
        }
        wasExtended_ = runtime_.extended;

        // Publish status
        std::string state = runtime_.extended ? "extended" : (runtime_.retracted ? "retracted" : "moving");
        summary_ = PneumaticStatus{
            .state = state,
            .position = runtime_.position,
        };
        publishLocal("status", summary_);
    }

    void exit() override {}

    void longRunning() override {}

private:
    bool wasExtended_ = false;
};

LOOM_REGISTER_MODULE(PneumaticActuator)
