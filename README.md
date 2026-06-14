# Loom

A modular C++ PLC-like runtime that dynamically loads shared library based module plugins, reflects user-defined data structs via [glaze](https://github.com/stephenberry/glaze), and exposes them over REST/WebSocket. It ships a React debug IDE frontend and a **mapp Connect-compatible, OPC-UA-style facade** (`/api/1.0`) so standard OPC-UA web clients — e.g. [LuxConnect](https://github.com/loupeteam) — can browse, read/write, and subscribe to live data.

---

## Architecture

```
┌────────────────────────────────────────────────────────┐
│  React Frontend  (Vite + TypeScript)                   │
│  LuxConnect + LuxReact (live data via OPC-UA facade)   │
│  DataTree  │  SectionPanel  │  REST for metadata        │
└───────────────────────┬────────────────────────────────┘
            REST + /api/1.0/pushchannel (WebSocket), Crow
┌───────────────────────┴────────────────────────────────┐
│  loom::Server  (HTTP)                                    │
│   ├─ OpcUaRestServer  — mapp Connect facade /api/1.0     │
│   │                     (REST + /pushchannel)            │
│   └─ legacy /api/* REST + /ws, /ws/watch                 │
├────────────────────────────────────────────────────────┤
│  loom::ModuleLoader  ·  loom::Scheduler                  │
│  loom::DataEngine  ·  loom::DataStore                    │
│  loom::Bus  (pub/sub + RPC)                             │
├────────────────────────────────────────────────────────┤
│  Module Plugins  (.so/.dll, loaded via dlopen)              │
│  ExampleMotor  │  Sequencer  │  StackLight  │  PneumaticActuator │
└────────────────────────────────────────────────────────┘
```

The frontend consumes **live** module data (runtime, summary, scheduler stats) through the OPC-UA facade via LuxConnect/LuxReact; metadata, history, config save/load, bus, oscilloscope, and IO mappings still use the plain `/api/*` REST endpoints.

### Layers

| Layer | Path | Description |
|-------|------|-------------|
| **SDK** | `sdk/` | Header-only API — `loom::Module<Config,Recipe,Runtime>` |
| **Core Runtime** | `runtime/` | Loader, scheduler, data engine, persistence, HTTP server |
| **Example Modules** | `modules/` | Motor, sequencer, stack light, pneumatic actuator |
| **Frontend** | `frontend/` | React + TypeScript + Vite debug IDE |
| **Tests** | `tests/` | GTest unit + integration tests |

---

## Module Data Sections

| Section | Purpose | Persistence |
|---------|---------|-------------|
| **Config** | Module parameters | Saved to disk, restored on boot |
| **Recipe** | Product / batch parameters | Loaded on user selection |
| **Runtime** | Live process variables | Not persisted; read/written every cycle |
| **Summary** | Read-only dashboard snapshot | Not persisted; recomputed each cycle |

---

## Quick Start

### Prerequisites

- CMake ≥ 3.25, C++23 compiler (clang++ 17+ or g++ 13+)
- [Conan](https://conan.io/) 2.x
- [just](https://github.com/casey/just)
- Node.js 18+

### Run in 3 steps

```bash
just frontend    # Build the React UI into frontend/dist/
just build       # Install deps, configure, and build the C++ runtime
just run         # Start the runtime — open http://localhost:8080
```

The runtime serves the built UI at `/`, the legacy REST/WebSocket API at `/api` and `/ws`, and the mapp Connect-compatible OPC-UA facade at `/api/1.0` (REST + `/api/1.0/pushchannel`). No separate web server needed. Client-side routes (e.g. `/module/<id>`) are served `index.html` so deep links and refreshes work.

### Frontend development (live reload)

When editing the frontend you don't need to rebuild after every change. Keep the runtime running in one terminal and start the Vite dev server in another:

```bash
just run         # Terminal 1 — runtime on :8080
just dev         # Terminal 2 — Vite on :5173, proxies /api and /ws to :8080
```

Edit files under `frontend/src/` and the browser reloads instantly. When you're done, run `just frontend` once to rebuild the static bundle so `just run` picks up your changes without Vite.

### Tests

```bash
just test
```

---

## Writing a Module

### 1 — Define your data structs (plain C++23 aggregates)

```cpp
struct MyConfig  { int rate = 10; std::string label = "default"; };
struct MyRecipe  { double target_speed = 1.0; };
struct MyRuntime { double position = 0.0; bool fault = false; };
```

Glaze reflects these automatically — no macros, no registration.

### 2 — Inherit from `loom::Module`

```cpp
#include <loom/module.h>
#include <loom/export.h>

struct SpeedCmd { double speed = 0.0; };  // request payload for set_speed service

class MyModule : public loom::Module<MyConfig, MyRecipe, MyRuntime> {
public:
    LOOM_MODULE_HEADER("MyModule", "1.0.0")

    void init(const loom::InitContext& /*ctx*/) override {
        // config_ is already populated from disk.
        // bus_ is injected and ready.

        // Typed service — JSON is auto-deserialized into SpeedCmd
        registerLocalService("set_speed", [this](const SpeedCmd& cmd) -> loom::CallResult {
            recipe_.target_speed = cmd.speed;
            return {true, "{}", ""};
        });

        // Typed subscription — T is deduced from the lambda argument
        subscribeTo("other_module", "status", [this](const MyStatus& s) {
            runtime_.fault = !s.ok;
        });
    }

    void cyclic() override {
        runtime_.position += 0.01;
        publishLocal("status", runtime_);
    }

    void exit() override {}
    void longRunning() override {}  // required; leave empty if unused
};

LOOM_REGISTER_MODULE(MyModule)
```

### 3 — CMakeLists.txt

```cmake
add_library(my_module SHARED my_module.cpp)
target_link_libraries(my_module PRIVATE loom_sdk)
set_target_properties(my_module PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules")
```

---

## Bus Patterns

```cpp
// Publish a typed struct as JSON
publishLocal("status", runtime_);

// Subscribe — T is deduced from the lambda argument
subscribeTo("other_module", "status", [this](const SomeStatus& s) { ... });

// Synchronous RPC call (raw JSON)
auto result = callService("other_module", "command", R"({"speed":5})");

// Typed call — serializes Req, deserializes Resp
auto [ok, resp] = callService<SomeResp>("other_module", "command", SomeReq{.speed = 5});

// Async call
auto fut = callServiceAsync("other_module", "command", payload);
```

---

## REST API

### Modules

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/modules` | List all loaded module instances |
| GET | `/api/modules/available` | List available `.so` files with metadata |
| GET | `/api/modules/:id` | Module detail (header, state, stats, all data) |
| GET | `/api/modules/:id/data/:section` | Read `config`, `recipe`, or `runtime` as JSON |
| POST | `/api/modules/:id/data/:section` | Replace a data section |
| PATCH | `/api/modules/:id/data/:section` | JSON Patch a data section |
| POST | `/api/modules/:id/config/save` | Persist config to disk |
| POST | `/api/modules/:id/config/load` | Reload config from disk |
| POST | `/api/modules/:id/recipe/save/:name` | Save recipe by name |
| POST | `/api/modules/:id/recipe/load/:name` | Load recipe by name |
| POST | `/api/modules/:id/reload` | Hot-reload a module (warm restart) |
| POST | `/api/modules/instantiate` | Create a new instance from an available `.so` |
| POST | `/api/modules/upload` | Upload a new `.so` file |
| DELETE | `/api/modules/:id` | Remove a module instance |

### Scheduler

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/scheduler/classes` | List scheduler classes with stats |
| POST | `/api/scheduler/classes` | Create a scheduler class |
| PATCH | `/api/scheduler/classes/:name` | Update class parameters |
| GET | `/api/scheduler/schema` | JSON schema for scheduler config |
| POST | `/api/scheduler/reassign` | Reassign modules to classes |

### Oscilloscope

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/scope/schema` | Nested runtime JSON for every module: `{ moduleId: {...}, ... }` |
| GET | `/api/scope/probes` | List active probes |
| POST | `/api/scope/probes` | Add a probe (`{ moduleId, path }`) |
| DELETE | `/api/scope/probes/:id` | Remove a probe |
| GET | `/api/scope/data` | Latest sampled values for all probes |

### IO Mappings

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/io-mappings` | List all IO mappings |
| POST | `/api/io-mappings` | Create an IO mapping |
| PATCH | `/api/io-mappings/:id` | Update an IO mapping |
| DELETE | `/api/io-mappings/:id` | Delete an IO mapping |
| POST | `/api/io-mappings/resolve` | Resolve current mapping values |

### Bus

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/bus/topics` | Active pub/sub topics |
| GET | `/api/bus/services` | Registered RPC services |
| POST | `/api/bus/call/:service` | Call a service — `:service` can contain slashes (e.g. `motor/set_speed`) |

### WebSocket (legacy)

The original live channels. The bundled frontend no longer uses these — it streams via the OPC-UA facade `/api/1.0/pushchannel` (below) — but they remain for compatibility.

| Endpoint | Description |
|----------|-------------|
| `/ws` | Live runtime data + scheduler stats stream |
| `/ws/watch` | Subscribe to specific field paths (`{ type, id, moduleId, path }`) |

---

## mapp Connect / OPC-UA facade (`/api/1.0`)

An OPC-UA-style projection of the runtime, wire-compatible with B&R's mapp Connect REST API, so OPC-UA web clients (e.g. LuxConnect/LuxReact) connect with no custom code. This is the channel the bundled frontend uses for all live data. Auth is a minimal stand-in (no real authentication yet).

**Address space** — nodes are addressed by string NodeId `ns=1;s=<path>`:

| Path | Node |
|------|------|
| `/module` | Container of all modules |
| `/module/<id>` | Container of one module's sections |
| `/module/<id>/<section>` | A whole section (`config`/`recipe`/`runtime`/`summary`) |
| `/module/<id>/<section>/<field>` | A leaf / sub-tree within a section (JSON-pointer path) |
| `/module/<id>/stats` | Scheduler task stats (read-only) |
| `/scheduler/classes/<name>` | Per-class scheduler stats (read-only) |

**Endpoints** (the subset of mapp Connect exercised by LuxConnect):

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/1.0/auth` | Reachability / stand-in auth |
| POST | `/api/1.0/opcua/sessions` | Create a session → `{ id }` |
| GET/DELETE/PATCH | `/api/1.0/opcua/sessions/:id` | Session info / delete / keep-alive (HEAD derives from GET) |
| GET/PUT | `…/nodes/:nodeId/attributes/:attr` | Read / write a node value |
| POST | `…/nodes/$batch` | Batch read/write |
| GET | `…/nodes/:nodeId/references` | Browse children |
| POST/DELETE | `…/subscriptions[/:subId]` | Create / delete a subscription |
| POST | `…/subscriptions/:subId/monitoredItems` (+ `/$batch`) | Add monitored items |
| DELETE | `…/subscriptions/:subId/monitoredItems/:itemId` | Remove a monitored item |
| WS | `/api/1.0/pushchannel?sessionid=:id` | DataChange notification stream |

Change detection runs on a sample thread; a missing node reports `Bad_NodeIdUnknown`, so subscriptions transparently recover as modules load/unload.

---

## Example Modules

| Module | Bus Services | Bus Topics |
|--------|-------------|------------|
| **ExampleMotor** | `set_speed` | `status` (at_speed, speed, position) |
| **PneumaticActuator** | `extend`, `retract`, `state` | `status` (state, position) |
| **StackLight** | `set` | `status` (red, yellow, green, blue, buzzer) |
| **Sequencer** | `start`, `stop`, `reset` | `status` (step, step_name, running, parts_produced) |

---

## Dependencies

| Library | Purpose |
|---------|---------|
| [glaze](https://github.com/stephenberry/glaze) | JSON + C++23 struct reflection (header-only) |
| [Crow](https://github.com/CrowCpp/Crow) | HTTP / WebSocket server |
| [spdlog](https://github.com/gabime/spdlog) | Structured logging |
| [GTest](https://github.com/google/googletest) | Unit testing |

**Frontend:** React + Vite + TypeScript, with [`@loupeteam/lux-connect`](https://www.npmjs.com/package/@loupeteam/lux-connect) (OPC-UA client) and [`@loupeteam/lux-react`](https://www.npmjs.com/package/@loupeteam/lux-react) (React hooks) consuming the `/api/1.0` facade; [recharts](https://recharts.org) for history charts.
