# Loom — C++ PLC-Like Runtime

## Project Overview
Loom is a modular C++ PLC-like runtime that dynamically loads `.so` module plugins, automatically reflects user-defined data structs via [glaze](https://github.com/stephenberry/glaze) (C++23, no macros), and exposes them over REST/WebSocket for a React debug IDE frontend.

## Architecture

### Layers
1. **SDK** (`sdk/`) — Header-only module author API: `loom::Module<Config, Recipe, Runtime>` template
2. **Core Runtime** (`runtime/`) — Module loader (dlopen), scheduler, data engine (glaze reflection), data store (persistence), HTTP/WebSocket server (Crow)
3. **Example Modules** (`modules/`) — Motor, sequencer, stack light, pneumatic actuator — all interconnected via Bus
4. **Frontend** (`frontend/`) — React + TypeScript + Vite debug IDE
5. **Tests** (`tests/`) — GTest unit tests

### Key Types
- `loom::Module<Config, Recipe, Runtime>` — Base class for all modules. Users define plain C++23 aggregate structs.
- `loom::IModule` — Type-erased virtual interface used by the runtime.
- `LOOM_REGISTER_MODULE(ClassName)` — Macro that exports `extern "C"` factory functions.
- `loom::ModuleHeader` — Version/identity info exported by each module.
- `loom::ModuleLoader` — Discovers, loads, unloads `.so` files via dlopen.
- `loom::Scheduler` — Runs cyclic + long-running tasks on per-module threads.
- `loom::DataEngine` — Registry for module data; JSON read/write via glaze.
- `loom::DataStore` — Persists Config/Recipe to JSON files on disk.
- `loom::Bus` — Header-only inter-module communication bus (topic pub/sub, service RPC, async RPC).
- `loom::Server` — REST + WebSocket server (Crow) for the debug IDE frontend.
- `loom::CallResult` — Result of a service call (ok, response, error).
- `loom::moduleAddress(moduleId, name)` — Helper to build namespaced addresses.

### Module Data Sections
| Section | Purpose | Persistence |
|---------|---------|-------------|
| **Config** | Module parameters | Saved to file, loaded on boot, survives restarts |
| **Recipe** | Product/batch parameters | Loaded from file on user selection |
| **Runtime** | Live process variables | Not persisted, read/written every cycle |

### Module Lifecycle
1. `init()` — Called once after load (config already populated from disk, bus injected)
2. `cyclic()` — Called at fixed interval by scheduler
3. `exit()` — Called before unload
4. `longRunning()` — Runs on background thread for async work

### Inter-Module Communication (`loom::Bus`)
Three patterns available via `bus_` (injected before `init()`):

| Pattern | Method | Use Case |
|---------|--------|----------|
| **Topic pub/sub** | `publishLocal()` / `subscribeTo()` | Broadcast events, status updates |
| **Service RPC** | `registerLocalService()` / `callService()` | Request/response commands |
| **Async RPC** | `callServiceAsync()` | Non-blocking calls returning `std::future` |

Namespace convention: addresses are `"<moduleId>/<name>"` (e.g., `"left_motor/status"`).
Global topics (e.g., `"estop"`) use `bus_->publish()`/`bus_->subscribe()` directly.

### REST API (`loom::Server`)
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/modules` | List all loaded modules |
| GET | `/api/modules/:id` | Module detail with all data sections |
| GET/POST | `/api/modules/:id/data/:section` | Read/write config, recipe, or runtime |
| POST | `/api/modules/:id/reload` | Warm-restart a module |
| GET | `/api/bus/topics` | List active bus topics |
| GET | `/api/bus/services` | List registered bus services |
| POST | `/api/bus/call/:name` | Call a bus service |
| WS | `/ws` | Live data streaming (runtime + stats) |

### Example Modules
| Module | Description | Bus Services | Bus Topics |
|--------|-------------|-------------|------------|
| **ExampleMotor** | Motor simulation with speed ramp | `set_speed` | `status` (at_speed, speed, position) |
| **PneumaticActuator** | Pneumatic cylinder extend/retract | `extend`, `retract` | `status` (state, position) |
| **StackLight** | Red/yellow/green/blue/buzzer | `set` | `status` (light outputs) |
| **Sequencer** | 8-step orchestrator | `start`, `stop`, `reset` | `status` (step, running, parts) |

## Build System
- **CMake** with presets (`CMakePresets.json`)
- **Conan** for dependencies (`conanfile.py`)
- **just** task runner (`justfile`)

### Dependencies
- `glaze` — JSON serialization + C++23 struct reflection (header-only)
- `spdlog` — Structured logging
- `gtest` — Unit testing
- `crowcpp-crow` — HTTP/WebSocket server

### Build Commands
```bash
just setup       # Install Conan deps
just build       # Configure + build (Debug)
just test        # Run tests
just run         # Run runtime with example modules
just frontend    # Build React frontend
just dev         # Start frontend dev server
just clean       # Clean build artifacts
```

## Conventions

### C++ Style
- C++23 standard, no extensions
- Namespace: `loom` for all runtime/SDK code
- Header-only SDK in `sdk/include/loom/` (including Bus)
- Module plugins use `extern "C"` exports (C ABI boundary)
- Use aggregate structs for user data (enables glaze auto-reflection)
- No RTTI or exceptions in module cyclic code (performance-sensitive)

### Module Authoring Pattern
```cpp
// my_module.hpp
#pragma once

// 1. Define plain aggregate structs for your data sections
struct MyConfig { int rate = 10; };
struct MyRecipe { double speed = 1.0; };
struct MyRuntime { double pos = 0.0; bool fault = false; };

// my_module.cpp
#include "my_module.hpp"
#include <loom/module.h>

// 2. Inherit from loom::Module with your types
class MyModule : public loom::Module<MyConfig, MyRecipe, MyRuntime> {
public:
    static loom::ModuleHeader moduleHeader() {
        return { .api_version = loom::kApiVersion, .name = "MyModule", .version = "1.0.0" };
    }
    const loom::ModuleHeader& header() const override { ... }
    loom::ModuleError init() override { ... }
    loom::ModuleError cyclic() override { ... }
    loom::ModuleError exit() override { ... }
    void longRunning() override { ... }
};

// 3. Register the module
LOOM_REGISTER_MODULE(MyModule)
```

### File Organization
- One module per directory under `modules/`
- Each module directory has its own `CMakeLists.txt`, a .hpp file for interface structs, and a .cpp file for implementation
- Module `.so` files output to `build/<config>/modules/`
- Config/recipe persistence under `data/<moduleId>/`
- Frontend source in `frontend/`, builds to `frontend/dist/`

### Testing
- Unit tests in `tests/` using GTest
- Integration tests load actual `.so` modules via ModuleLoader
- Run with `just test` or `ctest --preset debug`

## Completion Checklist
Before marking any task as done, verify:
- [ ] C++ builds cleanly (`just build`)
- [ ] All tests pass (`just test`)
- [ ] Frontend type-checks (`cd frontend && npx tsc --noEmit`)
- [ ] Frontend builds (`cd frontend && npm run build`)
- [ ] No remaining TODOs in the task list
