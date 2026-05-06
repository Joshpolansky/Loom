#include <loom/module.h>
#include <loom/export.h>

#include <stack_light/stack_light.hpp>
#include <pneumatic_actuator/pneumatic_actuator.hpp>
#include <example_motor/motor_module.hpp>

#include "sequencer.hpp"

/// ---- Sequence Steps ----
// 0: idle         — waiting for start command
// 1: starting     — set stack light, start motor
// 2: wait_motor   — wait for motor to reach speed
// 3: clamp        — extend the pneumatic clamp
// 4: wait_clamp   — wait for clamp extended
// 5: process      — hold for processing (simulated dwell)
// 6: unclamp      — retract the clamp
// 7: wait_unclamp — wait for clamp retracted
// 8: complete     — increment count, loop or finish

class Sequencer : public loom::Module<SequencerConfig, SequencerRecipe, SequencerRuntime, SequencerStatus> {
public:
    LOOM_MODULE_HEADER("Sequencer", "1.0.0")

    void init(const loom::InitContext& /*ctx*/) override {
        runtime_ = SequencerRuntime{};
        summary_ = SequencerStatus{};
        config_.light_id = StackLightIdentifier; // default to known stack light ID
        config_.motor_id = MotorIdentifier; // default to known motor ID
        config_.clamp_id = PneumaticIdentifier; // default to known pneumatic ID

        // Register start/stop/reset services
        registerLocalService("start", [this](std::string_view) -> loom::CallResult {
            if (runtime_.running) return {false, {}, "already running"};
            runtime_.running = true;
            runtime_.complete = false;
            runtime_.fault = false;
            runtime_.fault_message.clear();
            runtime_.step = 1; // go to "starting"
            runtime_.parts_produced = 0;
            return {true, R"({"accepted":true})", ""};
        });

        registerLocalService("stop", [this](std::string_view) -> loom::CallResult {
            runtime_.running = false;
            runtime_.step = 0;
            runtime_.step_name = "idle";
            // Tell motor to stop
            auto MotorResponse = callService(config_.motor_id, "set_speed", MotorRecipe{.target_speed = 0.0});
            // Update stack light
            auto StackLightResponse = callService(config_.light_id, "set", StackLightCmd{});
            return {true, R"({"accepted":true})", ""};
        });

        registerLocalService("reset", [this](std::string_view) -> loom::CallResult {
            runtime_ = SequencerRuntime{};
            return {true, R"({"accepted":true})", ""};
        });

        // Subscribe to motor status updates
        subscribeTo(config_.motor_id, "status", [this](const MotorStatus& s) {
            runtime_.motor_at_speed = s.at_speed;
        });

        // Subscribe to pneumatic status updates
        subscribeTo(config_.clamp_id, "status", [this](const PneumaticStatus& s) {
            runtime_.clamp_extended  = (s.state == "extended");
            runtime_.clamp_retracted = (s.state == "retracted");
        });
    }

    void cyclic() override {
        runtime_.cycle_count++;

        if (!runtime_.running) {
            runtime_.step_name = "idle";
            return;
        }


        runtime_.step_timer_ms += 100; // assuming 100ms cycle
        auto pneumaticRuntime = getRuntimeAs<PneumaticRuntime>(config_.clamp_id);
        if(pneumaticRuntime){
            runtime_.clamp_extended  = pneumaticRuntime->extended;
            runtime_.clamp_retracted = pneumaticRuntime->retracted;
        }
        switch (runtime_.step) {
            case 1: stepStarting(); break;
            case 2: stepWaitMotor(); break;
            case 3: stepClamp(); break;
            case 4: stepWaitClamp(); break;
            case 5: stepProcess(); break;
            case 6: stepUnclamp(); break;
            case 7: stepWaitUnclamp(); break;
            case 8: stepComplete(); break;
            default: break;
        }

        // Publish status
        publishLocal("status", summary_);
    }

    void exit() override {
        // Emergency stop everything
        runtime_.running = false;
    }

    void longRunning() override {}

private:
    void goToStep(int step, const std::string& name) {
        runtime_.step = step;
        runtime_.step_name = name;
        runtime_.step_timer_ms = 0;
        summary_.step = step;
        summary_.step_name = name;
    }

    void checkTimeout() {
        if (runtime_.step_timer_ms > config_.step_timeout_ms) {
            runtime_.fault = true;
            runtime_.fault_message = "Timeout in step: " + runtime_.step_name;
            runtime_.running = false;
            // Set stack light to red blink + buzzer
            auto StackLightResponse = callService(config_.light_id, "set", StackLightCmd{.red = 2, .buzzer = 2});
            goToStep(0, "fault");
            return;
        }
    }

    void stepStarting() {
        // Set stack light to green (running)
        auto StackLightResponse = callService(config_.light_id, "set",StackLightCmd{.green = 1});

        // Command motor to target speed
        auto MotorResponse = callService(config_.motor_id, "set_speed", MotorRecipe{
            .target_speed = recipe_.motor_speed,
            .acceleration = recipe_.motor_acceleration,
            .direction_cw = true,
        });
        if(MotorResponse.ok){
            // Proceed to next step
            goToStep(2, "wait_motor");
        } else {
            runtime_.fault = true;
            runtime_.fault_message = "Failed to start motor";
            runtime_.running = false;
            // Set stack light to red blink + buzzer
            auto StackLightResponse = callService(config_.light_id, "set", StackLightCmd{.red = 2, .buzzer = 2});
            goToStep(0, "fault");
        }
    }

    void stepWaitMotor() {
        runtime_.step_name = "wait_motor";
        if (runtime_.motor_at_speed) {
            goToStep(3, "clamp");
        }
        checkTimeout();
    }

    void stepClamp() {
        runtime_.step_name = "clamp";
        // Command pneumatic to extend
        auto extendResponse = callService(config_.clamp_id, "extend", "{}");
        if(!extendResponse.ok){
            runtime_.fault = true;
            runtime_.fault_message = "Failed to extend clamp";
            runtime_.running = false;
            // Set stack light to red blink + buzzer
            auto StackLightResponse = callService(config_.light_id, "set", StackLightCmd{.red = 2, .buzzer = 2});
            goToStep(0, "fault");
            return;
        }

        // Set light to yellow (processing)
        callService(config_.light_id, "set", StackLightCmd{.yellow = 1, .green = 1});

        goToStep(4, "wait_clamp");
    }

    void stepWaitClamp() {
        runtime_.step_name = "wait_clamp";
        if (runtime_.clamp_extended) {
            goToStep(5, "process");
        }
        checkTimeout();
    }

    void stepProcess() {
        runtime_.step_name = "process";
        // Dwell for 1 second (10 cycles at 100ms)
        if (runtime_.step_timer_ms >= 1000) {
            goToStep(6, "unclamp");
        }
    }

    void stepUnclamp() {
        runtime_.step_name = "unclamp";
        callService(config_.clamp_id, "retract", "{}");
        goToStep(7, "wait_unclamp");
    }

    void stepWaitUnclamp() {
        runtime_.step_name = "wait_unclamp";
        if (runtime_.clamp_retracted) {
            goToStep(8, "complete");
        }
        checkTimeout();
    }

    void stepComplete() {
        runtime_.step_name = "complete";
        runtime_.parts_produced++;

        // Check if target reached
        if (recipe_.cycle_target > 0 &&
            static_cast<int>(runtime_.parts_produced) >= recipe_.cycle_target) {
            runtime_.complete = true;
            runtime_.running = false;
            runtime_.step = 0;
            runtime_.step_name = "done";
            // Blue light = complete
            callService(config_.light_id, "set", StackLightCmd{.blue = 1});
        } else {
            // Loop back to starting
            goToStep(1, "starting");
        }
    }
};

LOOM_REGISTER_MODULE(Sequencer)
