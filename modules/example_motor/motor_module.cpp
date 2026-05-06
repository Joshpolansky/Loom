#include <loom/module.h>
#include <loom/export.h>

#include "motor_module.hpp"



// ---- Module implementation ----

class ExampleMotor : public loom::Module<MotorConfig, MotorRecipe, MotorRuntime, MotorSummary> {
public:
    LOOM_MODULE_HEADER("ExampleMotor", "1.0.0")

    void init(const loom::InitContext& /*ctx*/) override {
        runtime_ = MotorRuntime{};

        // Register service for external speed commands
        registerLocalService("set_speed", [this](const MotorRecipe& req) -> loom::CallResult {
            recipe_ = req;
            return {true, R"({"accepted":true})", ""};
        });
        runtime_.status.fault.message.push_back("Motor initialized");
        runtime_.status.fault.message.push_back("Motor ready");
        runtime_.history.push_back(MotorState{.current_speed = runtime_.current_speed, .position = runtime_.position, .at_speed = runtime_.at_speed});
        runtime_.history.push_back(MotorState{.current_speed = runtime_.current_speed, .position = runtime_.position, .at_speed = runtime_.at_speed});
    }

    void cyclic() override {
        runtime_.cycle_count++;

        // Simple motor simulation: ramp speed towards target
        double dt = config_.cycle_rate_ms / 1000.0; // seconds
        double target = recipe_.direction_cw ? recipe_.target_speed : -recipe_.target_speed;
        double accel = recipe_.acceleration;

        // Clamp target to max speed
        if (std::abs(target) > config_.max_speed) {
            target = (target > 0) ? config_.max_speed : -config_.max_speed;
        }

        // Clamp acceleration to max
        if (accel > config_.max_acceleration) {
            accel = config_.max_acceleration;
        }

        // Ramp towards target speed
        double diff = target - runtime_.current_speed;
        double maxDelta = accel * dt;
        if (std::abs(diff) <= maxDelta) {
            runtime_.current_speed = target;
        } else {
            runtime_.current_speed += (diff > 0 ? maxDelta : -maxDelta);
        }

        // Update position (integrate speed)
        runtime_.position += (runtime_.current_speed / 60.0) * dt; // RPM -> revolutions/s

        // Check if at target speed
        runtime_.at_speed = std::abs(runtime_.current_speed - target) < 0.01;

        // Update summary (shown on dashboard card)
        summary_.speed    = runtime_.current_speed;
        summary_.target   = recipe_.target_speed;
        summary_.at_speed = runtime_.at_speed;
        summary_.fault    = runtime_.status.fault.active;

         // Simulate a fault if speed exceeds max by a large margin

        // Publish status for other modules (e.g., sequencer)
        publishLocal("status", MotorStatus{
            .at_speed = runtime_.at_speed,
            .speed    = runtime_.current_speed,
            .position = runtime_.position,
        });
    }

    void exit() override {
        runtime_.current_speed = 0.0;
    }

    void longRunning() override {
        // No long-running work for this example module
    }
};

LOOM_REGISTER_MODULE(ExampleMotor)
