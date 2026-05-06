#include <glaze/core/reflect.hpp>
#include <string>
#include <vector>

class testClass{
    public:
    int x = 0;
    std::string y = "hello";
    testClass(){
        x = 42;
    };
    ~testClass() = default;
    virtual void testFunc(){

    };
};

class testClass2 : public testClass{
    public:
    int a = 0;
    std::string b = "world";
    testClass2(){
        a = 24;
    };
    ~testClass2() = default;
    virtual void testFunc2(){

    };
};

template <>
struct glz::meta<testClass2>
{
   static constexpr auto value = object(&testClass::x, &testClass::y, &testClass2::a, &testClass2::b);
};

// ---- User-defined data structs (plain aggregates, no macros needed) ----
struct MotorConfig {
    std::string device_name = "Motor_1";
    int cycle_rate_ms = 100;
    double max_speed = 1000.0;       // RPM
    double max_acceleration = 500.0; // RPM/s
    testClass2 test_obj; // Example of a non-POD config field that should be ignored by CrunTime
};

struct MotorRecipe {
    double target_speed = 0.0;       // RPM
    double acceleration = 100.0;     // RPM/s
    bool direction_cw = true;        // Clockwise
};
struct MotorRuntime_status{
    bool PowerOn = false;
    bool Running = false;
    struct Fault{
        bool active = false;
        std::vector<std::string> message = {};
    } fault;    
};
struct MotorState{
    double current_speed = 0.0;      // RPM
    double position = 0.0;           // Revolutions
    bool at_speed = false;
};

struct MotorRuntime {
    static constexpr std::string_view kTypeId = "MotorRuntime/1";
    double current_speed = 0.0;      // RPM
    double position = 0.0;           // Revolutions
    bool at_speed = false;
    uint64_t cycle_count = 0;
    MotorRuntime_status status;
    std::vector<MotorState> history = {{}, {}, {}}; // last 3 states for simple trace
};

/// Published on the "status" topic for other modules to subscribe to
struct MotorStatus {
    bool at_speed = false;
    double speed = 0.0;
    double position = 0.0;
};

/// Summary: key KPIs shown on the dashboard module card
struct MotorSummary {
    double speed = 0.0;      // current RPM
    double target = 0.0;     // target RPM
    bool   at_speed = false;
    bool   fault = false;
};

inline constexpr const char* MotorIdentifier = "ExampleMotor";