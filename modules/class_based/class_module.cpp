#include <loom/module.h>
#include <loom/export.h>

#include "class_module.hpp"

// ---- Module implementation ----

class ClassBasedModule : public loom::Module<Configuration, Recipe, Runtime> {
public:
    LOOM_MODULE_HEADER("ClassExample", "1.0.0")

    void init(const loom::InitContext& /*ctx*/) override {
        Configuration().reset();
    }

    void cyclic() override {
    }

    void exit() override {

    }

    void longRunning() override {
        // No long-running work for this example module
    }
};

LOOM_REGISTER_MODULE(ClassBasedModule)
