// run_wasm.cpp — Emscripten host for the Loom runtime.
//
// The native host (run.cpp + server.cpp) drives the runtime with class threads
// and a Crow HTTP/WebSocket server. A browser can do neither, so this host
// drives RuntimeCore *cooperatively*: it loads modules in cooperative mode (no
// class threads, no file watcher) and steps the scheduler from JavaScript via
// Scheduler::tickOnce(). State is read back as JSON through the same DataEngine
// the server would expose. No Crow, no signals, no threads.
//
// Built only for Emscripten and linked against loom_core (NOT loom_runtime).
#ifdef __EMSCRIPTEN__

#include "loom/runtime_core.h"
#include "loom/types.h"

#include <spdlog/spdlog.h>
#include <emscripten/emscripten.h>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

namespace {
std::unique_ptr<loom::RuntimeCore> g_core;

// Copy a std::string into a malloc'd C buffer the JS side reads then free()s.
char* dupString(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out) std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}
} // namespace

extern "C" {

// Boot the runtime. `moduleDir`/`dataDir` are paths in the Emscripten FS.
// Modules already present in moduleDir are discovered and loaded cooperatively.
// Returns the number of modules loaded, or -1 on error.
EMSCRIPTEN_KEEPALIVE
int loom_init(const char* moduleDir, const char* dataDir) {
    spdlog::set_level(spdlog::level::info);
    try {
        loom::RuntimeConfig cfg;
        cfg.moduleDir   = moduleDir ? moduleDir : "/modules";
        cfg.dataDir     = dataDir   ? dataDir   : "/data";
        cfg.cooperative = true;   // no class threads, no file watcher
        g_core = std::make_unique<loom::RuntimeCore>(cfg);
        auto ids = g_core->loadModules();
        spdlog::info("loom_init: {} module(s) loaded", ids.size());
        return static_cast<int>(ids.size());
    } catch (const std::exception& e) {
        spdlog::error("loom_init failed: {}", e.what());
        g_core.reset();
        return -1;
    }
}

// Advance every due class one cooperative pass. Call from JS on a timer / rAF.
EMSCRIPTEN_KEEPALIVE
void loom_tick() {
    if (g_core) g_core->scheduler().tickOnce();
}

// Reflected runtime state of one module instance, as a JSON string. Returns a
// malloc'd C string the caller must free() (null on error).
EMSCRIPTEN_KEEPALIVE
char* loom_state_json(const char* moduleId) {
    if (!g_core || !moduleId) return nullptr;
    std::string js = g_core->dataEngine().readSection(moduleId, loom::DataSection::Runtime);
    return dupString(js);
}

EMSCRIPTEN_KEEPALIVE
void loom_shutdown() {
    if (g_core) { g_core->shutdown(); g_core.reset(); }
}

} // extern "C"

#endif // __EMSCRIPTEN__
