#include <loom/module.h>
#include <loom/export.h>

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

// ============================================================================
// Crasher — a deliberately-faulting module for exercising crash diagnostics.
//
// Config picks the fault and when it fires:
//   fault: none | throw | segfault | fpe | abort | loop
//   phase: init | cyclic
//   after_ticks: for phase=cyclic, fault on the Nth cyclic tick (lets the
//                module load + run first, so the breadcrumb shows phase=cyclic).
// ============================================================================

struct CrasherConfig {
    std::string fault       = "none";
    std::string phase       = "cyclic";
    uint64_t    after_ticks = 50;
};
struct CrasherRecipe  { int _unused = 0; };
struct CrasherRuntime { uint64_t cycle = 0; };

namespace {
[[maybe_unused]] void doFault(const std::string& f) {
    if (f == "throw")    throw std::runtime_error("crasher: intentional std::runtime_error");
    if (f == "segfault") { volatile int* p = nullptr; *p = 1; }          // SIGSEGV / AV
    if (f == "fpe")      { volatile int a = 1, b = 0; volatile int c = a / b; (void)c; } // SIGFPE
    if (f == "abort")    std::abort();                                    // SIGABRT
    if (f == "loop")     { volatile bool spin = true; while (spin) {} }   // hang (watchdog)
}
} // namespace

class Crasher : public loom::Module<CrasherConfig, CrasherRecipe, CrasherRuntime> {
public:
    LOOM_MODULE_HEADER("Crasher", "1.0.0")

    void init(const loom::InitContext&) override {
        if (config_.phase == "init") doFault(config_.fault);
    }
    void cyclic() override {
        runtime_.cycle++;
        if (config_.phase == "cyclic" && runtime_.cycle >= config_.after_ticks)
            doFault(config_.fault);
    }
    void exit() override {}
    void longRunning() override {}
};

LOOM_REGISTER_MODULE(Crasher)
