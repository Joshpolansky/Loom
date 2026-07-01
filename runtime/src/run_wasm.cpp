// run_wasm.cpp — Emscripten host for the Loom runtime.
//
// The native host (run.cpp + server.cpp) drives the runtime with class threads
// and a Crow HTTP/WebSocket server. A browser can't run a socket server, but
// CAN run real threads (SharedArrayBuffer + Worker-backed std::thread, opted
// into via -pthread in CMakeLists.txt — see spike/phaseC-pthread-dlopen/ for
// the de-risking spike that proved dlopen + real worker threads + the
// pauseClass()/unpauseClass() mutex+condvar handshake all work together).
// So this host loads modules NON-cooperatively: RuntimeConfig::cooperative is
// false, so RuntimeCore::loadModules() calls Scheduler::startClasses() (spawns
// real class threads running classLoop() — the SAME code path as native,
// unmodified) AND setupWatcher() (a real file-watcher thread over MEMFS; harmless
// if nothing ever writes into moduleDir post-boot, but no longer special-cased
// away). No Crow, no signals — but the scheduler and module watcher are the
// real thing, not a JS-driven cooperative stand-in. State is read back as JSON
// through the same DataEngine the server exposes.
//
// Built only for Emscripten and linked against loom_core (NOT loom_runtime).
#ifdef __EMSCRIPTEN__

#include "loom/runtime_core.h"
#include "loom/api/router.h"
#include "loom/opcua_rest_nodeid.h"   // opcrest::parseNodeId (OPC-UA node addressing)
#include "loom/types.h"

#include <spdlog/spdlog.h>
#include <emscripten/emscripten.h>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {
std::unique_ptr<loom::RuntimeCore> g_core;
std::vector<std::string>           g_ids;   // ids of modules loaded by loom_init

// Copy a std::string into a malloc'd C buffer the JS side reads then free()s.
char* dupString(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out) std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}
} // namespace

extern "C" {

// Boot the runtime. `moduleDir`/`dataDir` are paths in the Emscripten FS.
// Modules already present in moduleDir are discovered and loaded onto real
// class threads (startClasses()) — same as native. Returns the number of
// modules loaded, or -1 on error.
EMSCRIPTEN_KEEPALIVE
int loom_init(const char* moduleDir, const char* dataDir) {
    spdlog::set_level(spdlog::level::info);
    try {
        loom::RuntimeConfig cfg;
        cfg.moduleDir   = moduleDir ? moduleDir : "/modules";
        cfg.dataDir     = dataDir   ? dataDir   : "/data";
        cfg.cooperative = false;   // real class threads + file watcher (-pthread)
        g_core = std::make_unique<loom::RuntimeCore>(cfg);
        g_ids = g_core->loadModules();
        spdlog::info("loom_init: {} module(s) loaded", g_ids.size());
        return static_cast<int>(g_ids.size());
    } catch (const std::exception& e) {
        spdlog::error("loom_init failed: {}", e.what());
        g_core.reset();
        return -1;
    }
}

// Cooperative-mode-only: advance every due class one pass. Real class threads
// (the current, non-cooperative default — see loom_init) self-drive via
// classLoop() and must NEVER be swept concurrently by tickOnce() too — both
// would call sweepClassOnce() on the same runner.members with no mutual
// exclusion between them. Kept exported (a lighter-weight cooperative mode may
// still be useful, e.g. where -pthread's COOP/COEP hosting requirement can't be
// met) but is a safe no-op unless the runtime was actually booted cooperative.
EMSCRIPTEN_KEEPALIVE
void loom_tick() {
    if (!g_core) return;
    if (!g_core->config().cooperative) {
        spdlog::warn("loom_tick() called on a non-cooperative runtime (real class "
                     "threads are already driving it) -- ignored.");
        return;
    }
    g_core->scheduler().tickOnce();
}

// Comma-separated ids of the modules loaded by loom_init. malloc'd; caller free()s.
EMSCRIPTEN_KEEPALIVE
char* loom_module_ids() {
    std::string s;
    for (std::size_t i = 0; i < g_ids.size(); ++i) { if (i) s += ','; s += g_ids[i]; }
    return dupString(s);
}

// Route a REST request (method, path, body) through the same transport-agnostic
// dispatcher the native HTTP server uses. Returns a malloc'd JSON envelope
// {"status":<int>,"body":<raw json>} (caller free()s) so the JS fetch shim can
// build a Response with the right status. Body is embedded raw (every handler
// returns valid JSON).
EMSCRIPTEN_KEEPALIVE
char* loom_request(const char* method, const char* path, const char* body) {
    if (!g_core) return dupString("{\"status\":503,\"body\":null}");
    loom::api::Request req;
    req.method = loom::api::methodFromString(method ? method : "GET");
    req.path   = path ? path : "";
    req.body   = body ? body : "";
    loom::api::Response resp = loom::api::dispatch(*g_core, req);
    std::string env = "{\"status\":" + std::to_string(resp.status) +
                      ",\"body\":" + (resp.body.empty() ? "null" : resp.body) + "}";
    return dupString(env);
}

// Load an arbitrary user module from a file already written into the Emscripten
// FS (the JS service FS.writeFile()s the bytes, then calls this). Copies it into
// the module dir, loads + starts it cooperatively. Returns the loaded module id
// (empty string on failure). malloc'd; caller free()s.
EMSCRIPTEN_KEEPALIVE
char* loom_load_module(const char* path) {
    if (!g_core || !path) return dupString("");
    try {
        std::string id = g_core->uploadModule(path);
        if (!id.empty()) g_ids.push_back(id);
        return dupString(id);
    } catch (const std::exception& e) {
        spdlog::error("loom_load_module('{}') failed: {}", path, e.what());
        return dupString("");
    }
}

// Reflected runtime state of one module instance, as a JSON string. Returns a
// malloc'd C string the caller must free() (null on error).
EMSCRIPTEN_KEEPALIVE
char* loom_state_json(const char* moduleId) {
    if (!g_core || !moduleId) return nullptr;
    std::string js = g_core->dataEngine().readSection(moduleId, loom::DataSection::Runtime);
    return dupString(js);
}

// --- OPC-UA-style reflected tag access (backs the WasmMachine) ---------------
// A node is "ns=1;s=/module/<id>/<section>/<field-path>" (same address space the
// native OPC-UA facade serves). These map to IModule readField/writeField, so a
// WasmMachine can satisfy lux-react's useVariable()/writeVariable() in-browser.

// Read a node's reflected value as JSON. Returns "null" for unknown/non-value
// nodes. malloc'd; caller free()s.
EMSCRIPTEN_KEEPALIVE
char* loom_read_node(const char* nodeId) {
    if (!g_core || !nodeId) return dupString("null");
    auto p = loom::opcrest::parseNodeId(loom::opcrest::urlDecode(nodeId));
    using K = loom::opcrest::ParsedNode::Kind;
    if (p.kind != K::Field && p.kind != K::Section) return dupString("null");
    std::shared_lock<std::shared_mutex> lock(g_core->moduleMutex());
    auto* mod = g_core->loader().get(p.moduleId);
    if (!mod || !mod->instance) return dupString("null");
    if (p.kind == K::Section) return dupString(mod->instance->readSection(p.section));
    auto v = mod->instance->readField(p.section, p.fieldPointer);
    return dupString(v ? *v : std::string("null"));
}

// Write a node's reflected value from JSON. Returns 1 on success, 0 on failure.
EMSCRIPTEN_KEEPALIVE
int loom_write_node(const char* nodeId, const char* valueJson) {
    if (!g_core || !nodeId || !valueJson) return 0;
    auto p = loom::opcrest::parseNodeId(loom::opcrest::urlDecode(nodeId));
    using K = loom::opcrest::ParsedNode::Kind;
    std::shared_lock<std::shared_mutex> lock(g_core->moduleMutex());
    auto* mod = g_core->loader().get(p.moduleId);
    if (!mod || !mod->instance) return 0;
    if (p.kind == K::Section) return mod->instance->writeSection(p.section, valueJson) ? 1 : 0;
    if (p.kind == K::Field)   return mod->instance->writeField(p.section, p.fieldPointer, valueJson) ? 1 : 0;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void loom_shutdown() {
    if (g_core) { g_core->shutdown(); g_core.reset(); }
}

} // extern "C"

#endif // __EMSCRIPTEN__
