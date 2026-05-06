#include "foo.hpp"
#include <loom/module.h>
#include <loom/export.h>

// Module implementation: define the concrete class in the .cpp (pattern used across repo)
class ExampleModule : public loom::Module<ExampleConfig, ExampleRecipe, ExampleRuntime> {
public:
    LOOM_MODULE_HEADER("ExampleModule", "0.1.0")

    void init(const loom::InitContext& /*ctx*/) override {
        // Lightweight init. `config()` is already populated from disk by the runtime.
    }

    void cyclic() override {
        // Update `runtime()` each cycle.
    }

    void exit() override {
        // Cleanup before unload.
    }

    void longRunning() override {
        // Background thread work.
    }
};

LOOM_REGISTER_MODULE(ExampleModule)
