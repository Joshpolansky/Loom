# Skill: Loom Module Authoring

## Purpose
Guide the creation of new Loom module plugins. Modules are dynamically-loaded shared libraries that the runtime discovers, loads, auto-serializes, auto-deserializes, and schedules.

## When to Use
- User wants to create a new module
- User asks how to expose data from a module
- User wants to add config/recipe/runtime fields
- User asks about the module lifecycle (init, cyclic, exit, longRunning)

## Module Authoring Steps

### 1. Create Module Directory
```
modules/<module_name>/
├── CMakeLists.txt
├── <module_name>.hpp
└── <module_name>.cpp
```

### 2. CMakeLists.txt Template
```cmake
add_library(<module_name> MODULE
    <module_name>.cpp
)

target_link_libraries(<module_name> PRIVATE
    loom::sdk
)

if(WIN32)
    set(_module_suffix ".dll")
else()
    set(_module_suffix ".so")
endif()

set_target_properties(<module_name> PROPERTIES
    PREFIX ""
    SUFFIX "${_module_suffix}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
    CXX_VISIBILITY_PRESET hidden
)
```

### 3. Module Source Template
```cpp
#include "<module_name>.hpp"
#include <loom/module.h>
#include <loom/export.h>

// Step 1: Define data structs as plain C++23 aggregates
// - Use default member initializers for sensible defaults
// - Supported types: bool, int/uint (8-64), float, double, std::string,
//   std::vector, std::array, std::optional, std::map, nested structs
// - Max 128 fields per struct (glaze C++23 limit) — use nesting for more

struct <Name>Config {
    // Parameters that persist across restarts
    double homeOffset = 0.0;
};

struct <Name>Recipe {
    // Product/batch parameters, swappable at runtime
    double targetPosition = 0.0;
};

struct <Name>Runtime {
    // Live process variables, updated every cycle
    bool homed = false;
    bool peerReady = false;
    bool ready = false;
    double position = 0.0;
    double measuredTemperature = 0.0;
};

struct HomeRequest {
    bool force = false;
};

struct HomeResponse {
    bool accepted = true;
};

struct TemperatureUpdate {
    double celsius = 0.0;
};

struct StatusMessage {
    bool homed = false;
    double position = 0.0;
};

// Step 2: Implement the module class
class <Name>Module : public loom::Module<<Name>Config, <Name>Recipe, <Name>Runtime> {
public:
    // Declares moduleHeader() + header() override in one line:
    LOOM_MODULE_HEADER("<Name>", "1.0.0")

    void init(const loom::InitContext& ctx) override {
        // ctx.reason: InitReason::Boot | Reload | Recovery
        // Called once after load. Config is already populated from disk.
        // bus_ is available for inter-module communication.
        // Initialize runtime state here.

        // Register services that other modules can call.
        // The SDK auto-deserializes HomeRequest and auto-serializes HomeResponse.
        registerLocalService("home", [this](const HomeRequest& req) -> HomeResponse {
            runtime().homed = req.force;
            return {.accepted = true};
        });

        // Subscribe to topics from other modules.
        // The SDK auto-deserializes TemperatureUpdate before invoking the handler.
        subscribeTo("sensor_1", "temperature", [this](const TemperatureUpdate& update) {
            runtime().measuredTemperature = update.celsius;
        });
    }

    void cyclic() override {
        // Called at fixed interval. Access data through config(), recipe(), runtime().

        // Publish typed status for other modules.
        // The SDK auto-serializes StatusMessage to JSON.
        publishLocal("status", StatusMessage{
            .homed = runtime().homed,
            .position = runtime().position,
        });

        // Call typed services on other modules.
        // The SDK auto-serializes the request and auto-deserializes the response.
        auto [ok, result] = callService<HomeResponse>("other_module", "home", HomeRequest{.force = false});
        if (ok && result.accepted) {
            runtime().peerReady = true;
        }
    }

    void exit() override {
        // Called before unload. Clean up resources.
    }

    void longRunning() override {
        // Runs on background thread. For async I/O, communication, etc.
        // Return periodically — the runtime calls this in a loop.
    }
};

// Step 3: Register the module
LOOM_REGISTER_MODULE(<Name>Module)
```

### 4. Register in Build
Add to `modules/CMakeLists.txt`:
```cmake
add_subdirectory(<module_name>)
```

## Data Section Guidelines

### Config (persists across restarts)
- Device addresses, communication settings
- Timing parameters (cycle rate)
- Physical limits (max speed, max force)
- Calibration values

### Recipe (user-selectable)
- Product-specific setpoints
- Process parameters
- Motion profiles
- Batch quantities

### Runtime (live, not persisted)
- Current sensor values
- Actuator states
- Counters, positions, velocities
- Fault flags and status

## Key Rules
1. **Aggregate structs only** — No constructors, no private members, no virtual methods in data structs
2. **Default initializers** — Always provide sensible defaults
3. **No raw pointers** in data structs — use `std::string`, `std::vector`, `std::optional`
4. **Keep cyclic() fast** — No blocking I/O, no allocations, no exceptions
5. **Use longRunning() for async work** — File I/O, network, heavy computation
6. **Unique module names** — The `name` field in `moduleHeader()` must be unique across all modules
7. **Prefer typed bus helpers** — `publishLocal()`, `subscribeTo()`, `registerLocalService()`, and typed `callService()` automatically serialize and deserialize aggregate payloads via glaze
8. **Namespace Bus addresses** — Use `publishLocal()`, `registerLocalService()` etc. which auto-prefix with the module instance ID. Use `bus_->publish()`/`bus_->subscribe()` for global topics.

## Inter-Module Communication

The `bus_` pointer is injected before `init()`. Three patterns:

### Topic Pub/Sub
```cpp
struct SpeedStatus {
    double rpm = 0.0;
};

// Publish (scoped to this module): "<moduleId>/speed"
// The SDK auto-serializes SpeedStatus.
publishLocal("speed", SpeedStatus{.rpm = 1000.0});

// Subscribe to another module's topic:
subscribeTo("sensor_1", "temperature", [this](const TemperatureUpdate& update) {
    runtime().measuredTemperature = update.celsius;
});

// Global topic (no module prefix):
bus_->publish("estop", "{}");
bus_->subscribe("estop", [this](auto, auto payload) { /* handle */ });
```

### Service RPC (synchronous)
```cpp
struct HomeRequest {
    bool force = false;
};

struct HomeResponse {
    bool done = false;
};

// Register (scoped): "<moduleId>/home"
// Request and response are handled as typed aggregates.
registerLocalService("home", [this](const HomeRequest& req) -> HomeResponse {
    return {.done = req.force || runtime().ready};
});

// Call another module's service:
auto [ok, response] = callService<HomeResponse>("motor_1", "home", HomeRequest{.force = true});
if (ok && response.done) { /* handle success */ }
```

### Async RPC
```cpp
auto future = callServiceAsync<HomeResponse>("motor_1", "home", HomeRequest{.force = true});
// ... do other work ...
auto [ok, response] = future.get(); // blocks until response
```
