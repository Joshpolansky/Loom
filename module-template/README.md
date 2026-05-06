Module Template
===============

A minimal starting point for building a Loom module plugin.

## Prerequisites

The Loom SDK must be in your local Conan cache before you build. From the
Loom repo root:

```bash
just package
```

This creates `loom/<version>@local/stable` in your local Conan cache, which
your module will depend on.

## Files

| File | Purpose |
|------|---------|
| `foo.hpp` | Define your `Config`, `Recipe`, and `Runtime` structs here |
| `foo.cpp` | Implement your module class here |
| `CMakeLists.txt` | CMake build — finds `loom`, builds the module `.so` |
| `conanfile.py` | Conan recipe template — copy and adjust for your module |

## Quick start (CMake only)

If you have the SDK installed (via `just package` or `cmake --install`), you can
build directly with CMake:

```bash
cd module-template
conan install . -s compiler.cppstd=23 -s build_type=Debug --build=missing
cmake --preset conan-debug
cmake --build --preset conan-debug
```

The built `.so` is placed in `build/Debug/modules/`.

## Quick start (Conan package)

Copy the module template into a new directory, adjust `conanfile.py` (name,
version, loom version), then:

```bash
conan install . -s compiler.cppstd=23 -s build_type=Debug --build=missing
cmake --preset conan-debug
cmake --build --preset conan-debug
```

To package your module as a Conan recipe:

```bash
conan create . --user=local --channel=stable -s compiler.cppstd=23 -s build_type=Debug
```

## Authoring a module

**1. Define your data structs in `foo.hpp`:**

```cpp
struct MyConfig  { int rate = 10; };
struct MyRecipe  { double speed = 1.0; };
struct MyRuntime { double pos = 0.0; bool fault = false; };
```

Structs must be plain C++23 aggregates (no custom constructors) so that glaze
can reflect them automatically.

**2. Implement your module class in `foo.cpp`:**

```cpp
#include "foo.hpp"
#include <loom/module.h>
#include <loom/export.h>

class MyModule : public loom::Module<MyConfig, MyRecipe, MyRuntime> {
public:
    LOOM_MODULE_HEADER("MyModule", "1.0.0")

    void init(const loom::InitContext& ctx) override { /* one-time setup */ }
    void cyclic() override                          { /* runs every cycle */ }
    void exit() override                            { /* cleanup */ }
    void longRunning() override                     { /* background thread */ }
};

LOOM_REGISTER_MODULE(MyModule)
```

**3. Inside the lifecycle methods, access your data sections:**

```cpp
void cyclic() override {
    runtime().pos += config().rate * 0.001;
}
```

**4. Use the bus for inter-module communication:**

```cpp
void init(const loom::InitContext& ctx) override {
    bus_->publishLocal("status", runtime());

    bus_->subscribeLocal<SomeOtherRuntime>(
        "other_module/status",
        [this](const SomeOtherRuntime& data) { /* handle */ }
    );
}
```

## Loading the module at runtime

Copy the built `.so` into the directory you pass to `--module-dir`:

```bash
./loom --module-dir ./modules
```

The runtime discovers and loads all `.so` files in that directory on startup.

