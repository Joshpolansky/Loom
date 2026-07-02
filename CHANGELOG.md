# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.3.0] - 2026-07-02

### Added

#### WebAssembly / Browser Runtime
- **Runtime-as-a-service in the browser**: the full Loom runtime now compiles to WebAssembly via Emscripten and runs directly inside the browser tab; `fetch('/api/*')` calls are intercepted and served by the in-process runtime with no network round-trip
- **Real pthread threading for the WASM host**: replaced cooperative `tickOnce()` polling with real Worker-backed threads (requires `SharedArrayBuffer`/COOP+COEP headers); the scheduler and module `cyclic()` callbacks run on genuine background threads in the browser
- **`bootFromDataDir()`**: manifest-driven browser boot that fetches a `manifest.json`, loads listed `.wasm` side-modules, and instantiates all declared modules — enabling self-contained browser demos
- **`@joshpolansky/loom-wasm` npm package**: the WASM host, TypeScript bindings (`WasmMachine`, `LoomRuntime`), and type declarations are packaged for npm distribution; `bootWasmMachine()` is the public entry point
- **OPC-UA reflected node read/write**: `loom_read_node`/`loom_write_node` exported C entry points allow JavaScript to read and write any reflected tag by dotted path; `useVariable()` React hook provides live subscription with glaze round-trip
- **Emscripten CMake + Conan build**: `LOOM_WASM` CMake option builds the WASM host; demo modules compile as `SIDE_MODULE` via `loom_add_module()`; build auto-stages artifacts into `frontend/public/` for Vite dev server
- **WASM-mode frontend flag**: `VITE_WASM` build-time env var (sticky, no fragile query-param) switches the frontend to route all API calls through the in-browser runtime

#### Transport-Agnostic API Dispatch
- **`loom::dispatch()`**: a transport-free REST handler shared by the native Crow server and the WASM in-browser path; accepts `Request{method, path, body}` and returns `Response{status, body}` with no HTTP library dependency
- All REST routes migrated: scheduler/classes, scope, IO mappings, faults, bus topics/services, module detail/data/reload, system metrics, and all write (POST/PATCH/DELETE) routes now live exclusively in dispatch; `server.cpp` is a thin Crow adapter

#### SDK — Command Framework (PLCopen-style)
- **`loom::CommandFb<T>`**: PLCopen-compliant function-block base with full Execute/Busy/Done/Error output semantics, rising-edge execute detection, and one-cycle terminal visibility
- **`loom::EnableFb`**: companion always-on function block base with `IFunctionBlock` interface for uniform reflection
- **`loom::CommandClient`**: async command submission returning a `shared_ptr<CommandStatus>` handle; callers poll or await `phase` transitions without blocking cyclic code
- **`loom::CommandChannel`**: per-module typed pending-command queue backed by `RuntimeHeap`; thread-safe submit/consume with mutex-protected `std::vector`
- **`loom::RuntimeHeap`**: cross-module shared-object allocator using `std::shared_ptr` aliasing constructor; objects live in heap-allocated memory visible across `.so` boundaries without a shared allocator ABI

#### SDK — Port / Connection Points
- **`loom::Port<T>` / `loom::PortRef<T>`**: typed connection points with generation-counter validation; `PortRef` detects stale connections and self-corrects on next use; `connect()`/`disconnect()` are thread-safe via mutex + generation increment

#### SDK — Glaze / Reflection
- **`IFunctionBlock` Glaze metadata**: `glz::meta` specialisation for the `IFunctionBlock` base enables automatic reflection of `execute`, `busy`, `done`, `error`, `errorId` without macros
- **TagTable nested `glz::meta` indexing**: `TagTable` now correctly traverses types that expose a `glz::meta` specialisation (non-aggregate classes) in addition to plain aggregates; the `i++` counter aligns with `glz::reflect<T>::keys` for both

#### Diagnostics
- **Crash diagnostics**: `SIGSEGV`/`SIGABRT`/`SIGFPE`/`SIGILL` are caught by a POSIX signal handler; raw stack addresses are written async-signal-safely to `loom-crash-<id>.txt` before symbolization
- **cpptrace symbolization**: after the raw capture, `cpptrace` symbolizes addresses against `.dSYM` / DWARF debug info and writes a structured JSON report (`loom-crash-<id>.json`) with function name, file, line, and column
- **`/api/faults` + `loom/faults` bus topic**: structured `FaultReport` objects are published to the bus and exposed over REST; the frontend can display crash history without file system access
- **System metrics** (`SystemMetrics` module): tracks RSS memory (peak and current) and per-core CPU utilization at a configurable sample interval; data is reflected as normal runtime fields and visible on the dashboard

#### Scheduler
- **Cooperative `tickOnce()`**: single-call sweep across all classes for the WASM / thread-free host; `sweepClassOnce()` honours each class's individual period using `steady_clock` deadlines; missed-deadline re-anchoring prevents accumulated lag
- **`LOOM_HAS_THREADS` flag**: `thread_support.h` distinguishes Emscripten+pthreads (real Workers) from Emscripten without pthreads (cooperative-only) — finer-grained than a raw `#ifdef EMSCRIPTEN` guard
- **`applyAffinityPolicy` stub**: no-op stub provided for all non-Linux/non-Darwin targets (WASM, Windows); closes a missing-symbol link error on those platforms
- **`longRunning()` now optional**: modules that don't override `longRunning()` are detected at `init()` time; the scheduler skips spawning a background thread for them

### Changed
- `loom_runtime` CMake target split into `loom_core` (transport-free logic, `dispatch()`, data engine, scheduler, bus, module loader) and `loom_host_native` (Crow HTTP server, main entry point); downstream modules and tests link `loom_core`
- `loom_add_module()` CMake helper accepts `WASM` keyword to compile a module as an Emscripten `SIDE_MODULE` with the correct link flags; the same `CMakeLists.txt` builds both native `.so` and WASM `.wasm` targets
- `sdk/loom-config.cmake.in` updated to export `loom_core` instead of `loom_runtime`; downstream `find_package(loom)` consumers must link `loom::loom_core`

### Fixed
- **Scheduler tickOnce/classLoop race**: `tickOnce()` now holds `mutex_` for its entire duration, fully serializing cooperative calls against all structural `Scheduler` mutations; the last window for a data race between the cooperative and threaded modes is closed
- **`Method::DELETE_` rename**: `Method::DELETE` renamed to `Method::DELETE_` to avoid MSVC C2589 (`delete` is a reserved keyword in some MS headers); all switch cases and if-guards updated
- **macOS in-context unwind**: crash handler includes `<sys/ucontext.h>` (macOS) instead of `<ucontext.h>` (Linux) for the correct `mcontext_t` definition; the unwind now starts from the faulting instruction frame rather than the signal-handler frame
- **`lastFault` data race**: fault recording uses a release-store of `faulted` after all non-atomic writes; readers use acquire-load, ensuring full visibility of the fault record before observing `faulted == true`
- **`SystemMetrics` GCC/Clang ctor**: removed defaulted `Config` argument from the `SystemMetrics` constructor that triggered a compiler error under GCC/Clang strict-aggregate rules
- **POSIX affinity on all platforms**: `applyAffinityPolicy` now has a definition on every platform that doesn't support `pthread_setaffinity_np`, eliminating a link error on WASM and Windows
- **`.dSYM` CI build rule**: macOS debug-symbol generation moved to a CMake `add_custom_command(TARGET ... POST_BUILD)` dir-level rule so CI doesn't race on parallel directory creation

### Known Issues (non-blocking)
- **`build/Wasm` and `build/Release` share the `conan-release` CMake preset name**: running `just setup-wasm` and `just setup-release` in the same checkout overwrites `CMakeUserPresets.json`; the second `cmake --preset conan-release` uses whichever toolchain ran last. Workaround: run them in separate checkouts or clean between runs. Fix: add `tools.cmake.cmaketoolchain:presets_prefix=wasm-` to `conan/profiles/emscripten`.
- **POSIX signal handler may deadlock on heap-corruption crashes**: the crash handler calls `cpptrace` (which allocates) from the signal path. If the crash was triggered by a corrupted heap, the handler may hang rather than exit. The raw `.txt` report is always written first (async-signal-safe), so no crash data is lost; the process must be `SIGKILL`ed by a watchdog. Fix: defer symbolization to a post-mortem pass.
- **`WasmMachine.dispose()` does not stop the runtime tick timer**: the `setInterval` driving `loom_tick` in cooperative mode keeps running after `dispose()`; call `rt.stop()` manually if tearing down a cooperative-mode instance.
- **`GET /api/modules/:id/data/<badSection>` returns 404 instead of 400**: an unknown section name falls through to the catch-all 404 handler rather than returning a descriptive 400. Acceptable for v0.3.0; fix pending.
- **`lastTickStartMs` always 0 in `GET /api/scheduler/classes`**: the `ClassStatsDto` aggregate initializer is missing its 6th field; the value is correct over WebSocket. Fix pending.

## [0.1.10] - 2026-04-14

### Added
- Modular C++23 runtime with dynamic plugin loading (dlopen/LoadLibrary)
- Header-only SDK (`loom::Module<Config, Recipe, Runtime>`) with automatic struct reflection via glaze
- Real-time scheduler with configurable classes (period, CPU affinity, priority)
- Typed pub/sub bus and synchronous/async RPC services
- Data persistence for config and recipe sections
- HTTP REST API (Crow) for module management, scheduling, oscilloscope, IO mappings, and bus interaction
- WebSocket live streaming (`/ws` and `/ws/watch`)
- React 19 + TypeScript frontend debug IDE with dashboard, oscilloscope, scheduler view, bus explorer, and IO mapping editor
- Built-in example modules: ExampleMotor, PneumaticActuator, StackLight, Sequencer, EtherCAT
- Module template with documentation for creating new modules
- GTest unit and integration tests
- GitHub Actions CI/CD (Ubuntu, macOS, Windows)
- `just` task runner for build, test, run, and packaging workflows

### Security
- Default bind address changed to `127.0.0.1` (localhost only)
- File upload endpoint validates filenames and whitelists module extensions
