#include "loom/server.h"
#include "loom/api/json_build.h"   // shared jsonEscapeString / serializeCycleHistory / moduleInfoJson
#include "loom/api/router.h"       // api::dispatch — shared /api/* routing
#include "loom/diag/breadcrumb.h"
#include "loom/diag/fault_store.h"

// Crow static serving: define CROW_STATIC_DIRECTORY as a C++ variable expression
// so the path is resolved at runtime (when app.run() calls add_static_dir()).
// Must be defined before including crow.h so Crow's settings.h picks it up.
#include <crow/logging.h>
#include <string>
namespace { std::string g_crowStaticDir = "./data/UI/"; }
// Loom monitoring UI dir (normalized: trailing '/'), served at /_loom. Set in
// start() from config_.loomUiDir; read by the /_loom routes below.
namespace { std::string g_crowLoomUiDir = "./data/UI/"; }
#define CROW_STATIC_DIRECTORY g_crowStaticDir
#define CROW_STATIC_ENDPOINT "/<path>"
// We register our own "/<path>" catch-all in Server::start() that serves real
// files and falls back to index.html (generic SPA hosting). Disable Crow's
// built-in static catch-all so it doesn't duplicate/override that route.
#define CROW_DISABLE_STATIC_DIR

#include <crow.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>

namespace loom {

// ---------------------------------------------------------------------------
// Server-internal DTOs for scheduler endpoints
// Plain aggregate structs — glaze auto-reflects these with no macros.
// ---------------------------------------------------------------------------

struct PatchBody { std::string ptr; glz::raw_json value; };

/// Request body for POST /api/scope/probes.
struct AddProbeRequest {
    std::string moduleId;
    std::string path;
};

// Request payload for /ws/watch WebSocket messages.
struct WatchRequest {
    std::string type;
    std::string id;
    std::string moduleId;
    std::string path;
    bool        expand = false;
};

// Request payload for /ws subscription messages.
//   {"type":"subscribe","topics":["module/foo/runtime", ...]}
//   {"type":"unsubscribe","topics":[...]}
//   {"type":"unsubscribe_all"}
struct LiveSubscribeRequest {
    std::string              type;
    std::vector<std::string> topics;
};

/// Request body for POST /api/modules/instantiate.
struct InstantiateRequest {
    std::string id;
    std::string so;
};

/// Response item for GET /api/modules/available. NOTE: kept here (not migrated to
/// the catch-all) because the specific GET /api/modules/<string> route would
/// otherwise grab "/api/modules/available" as a module id. dispatch() has its own
/// copy (ordered before <id>) that serves WASM.
struct AvailableModuleDto {
    std::string filename;
    std::string className;
    std::string version;
};

// Escape a raw string for embedding inside a JSON string literal.
// jsonEscapeString, serializeCycleHistory and moduleInfoJson now live in
// loom/api/json_build.h (shared verbatim with the WASM api router).

// Convert DataSection string to enum
static std::optional<DataSection> parseSectionName(const std::string& name) {
    if (name == "config")  return DataSection::Config;
    if (name == "recipe")  return DataSection::Recipe;
    if (name == "runtime") return DataSection::Runtime;
    if (name == "summary") return DataSection::Summary;
    return std::nullopt;
}

template <typename T>
static glz::error_ctx readJsonPermissive(T& value, std::string_view json) {
    constexpr glz::opts kPermissiveReadOpts{
        .error_on_unknown_keys = false,
        .error_on_missing_keys = false,
    };
    glz::context ctx{};
    return glz::read<kPermissiveReadOpts>(value, json, ctx);
}

// serializeCycleHistory (vector + deque overloads) → loom/api/json_build.h

// Build a {"samples":[...],"latest":N} JSON body from raw samples.
// `since`   : drop samples with t <= since (unbinned) or whose bin start <= since (binned).
// `binMs`   : if > 0, group samples by floor(t/binMs)*binMs and emit max(cycle), max(jitter) per bin.
// `latest`  : returned to the client so the next poll only requests new data.
//             - unbinned: latest = max sample t emitted
//             - binned:   latest = (last fully-elapsed bin start) so the in-progress
//               bin is re-fetched (and re-aggregated) on the next poll.
static std::string buildHistoryBody(const std::vector<MetricSample>& samples,
                                    int64_t since,
                                    int64_t binMs) {
    std::string body = "{\"samples\":[";
    bool first = true;
    int64_t latest = since;

    if (binMs <= 0) {
        for (const auto& s : samples) {
            if (s.timestampMs <= since) continue;
            if (!first) body += ",";
            body += "{\"t\":" + std::to_string(s.timestampMs)
                + ",\"cycle\":" + std::to_string(s.cycleTimeUs)
                + ",\"jitter\":" + std::to_string(s.jitterUs) + "}";
            if (s.timestampMs > latest) latest = s.timestampMs;
            first = false;
        }
    } else {
        // Aggregate into bins: bin start = floor(t / binMs) * binMs.
        // Emit max cycle and max |jitter| per bin (jitter is signed; magnitude
        // is what's interesting on the chart).
        struct Agg { int64_t maxCycle = 0; int64_t maxJitter = 0; };
        std::map<int64_t, Agg> bins;
        int64_t lastBin = since;
        for (const auto& s : samples) {
            int64_t bin = (s.timestampMs / binMs) * binMs;
            if (bin <= since) continue;
            auto& a = bins[bin];
            if (s.cycleTimeUs > a.maxCycle) a.maxCycle = s.cycleTimeUs;
            int64_t aj = s.jitterUs < 0 ? -s.jitterUs : s.jitterUs;
            if (aj > a.maxJitter) a.maxJitter = aj;
        }
        // Don't emit the in-progress bin: it'll be re-aggregated on the next
        // poll, keeping each emitted bin a stable, finalized data point.
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t curBin = (now / binMs) * binMs;
        for (auto& [bin, a] : bins) {
            if (bin >= curBin) continue;
            if (!first) body += ",";
            body += "{\"t\":" + std::to_string(bin)
                + ",\"cycle\":" + std::to_string(a.maxCycle)
                + ",\"jitter\":" + std::to_string(a.maxJitter) + "}";
            if (bin > lastBin) lastBin = bin;
            first = false;
        }
        latest = lastBin;
    }

    body += "],\"latest\":" + std::to_string(latest) + "}";
    return body;
}

// Build JSON module info for a single module
// moduleInfoJson → loom/api/json_build.h (shared with the WASM api router)

Server::Server(RuntimeCore& core, const ServerConfig& config)
    : core_(core), config_(config) {}

Server::~Server() {
    stop();
}

static api::Method crowMethodToApi(crow::HTTPMethod m) {
    switch (m) {
        case crow::HTTPMethod::Get:    return api::Method::GET;
        case crow::HTTPMethod::Post:   return api::Method::POST;
        case crow::HTTPMethod::Put:    return api::Method::PUT;
        case crow::HTTPMethod::Patch:  return api::Method::PATCH;
        case crow::HTTPMethod::Delete: return api::Method::DELETE_;
        default:                       return api::Method::UNKNOWN;
    }
}

void Server::start() {
    if (running_.load()) return;
    running_.store(true);

    serverThread_ = std::thread([this]() {
        crow::SimpleApp app;
        app.loglevel(crow::LogLevel::Warning);
        // =====================================================================
        // GET /api/modules — List all modules
        // =====================================================================
        CROW_ROUTE(app, "/api/modules")
        ([this]() {
            std::shared_lock<std::shared_mutex> lock(core_.moduleMutex());
            std::string json = "[";
            bool first = true;
            for (auto& [id, mod] : core_.loader().modules()) {
                if (!first) json += ",";
                json += moduleInfoJson(mod, core_.scheduler());
                first = false;
            }
            json += "]";

            auto resp = crow::response(200, json);
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // GET /api/modules/available — List .so files with metadata across
        // all configured module directories. De-duplicated by filename;
        // primary moduleDir is enumerated first so a user's local build
        // shadows any system example sharing the same .so name.
        // =====================================================================
        CROW_ROUTE(app, "/api/modules/available")
        ([this]() {
            const auto& cfg = core_.config();
            auto available = ModuleLoader::queryAvailable(cfg.moduleDir);

            std::unordered_set<std::string> seen;
            seen.reserve(available.size() * 2);
            for (const auto& a : available) seen.insert(a.filename);

            for (const auto& extra : cfg.additionalModuleDirs) {
                auto more = ModuleLoader::queryAvailable(extra);
                for (auto& a : more) {
                    if (seen.insert(a.filename).second) {
                        available.push_back(std::move(a));
                    }
                }
            }

            std::vector<AvailableModuleDto> dtos;
            dtos.reserve(available.size());
            for (auto& a : available) {
                dtos.push_back({ a.filename, a.className, a.version });
            }
            auto json = glz::write_json(dtos).value_or("[]");
            auto resp = crow::response(200, json);
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // GET /api/faults — List fault reports (newest first). Includes this
        // run's exception faults and any persisted reports from prior runs
        // (signal-path crashes the process couldn't keep in memory).
        // =====================================================================
        // GET /api/faults — migrated to api::dispatch (router.cpp); served by the catch-all above.

        // =====================================================================
        // GET /api/faults/<id> — Full structured report for one fault.
        // =====================================================================
        CROW_ROUTE(app, "/api/faults/<string>")
        ([this](const std::string& id) {
            auto detail = core_.faultStore().detailJson(id);
            crow::response resp = detail
                ? crow::response(200, *detail)
                : crow::response(404, R"({"error":"fault not found"})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // POST /api/modules/instantiate — Create a new instance from a .so
        // Body: { "id": "left_motor", "so": "libexample_motor.so" }
        // =====================================================================
        CROW_ROUTE(app, "/api/modules/instantiate")
        .methods("POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }

            InstantiateRequest body{};
            auto err = readJsonPermissive(body, req.body);
            if (err || body.id.empty() || body.so.empty()) {
                auto resp = crow::response(400, R"({"error":"body must have non-empty 'id' and 'so'"})" );
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            auto resultId = core_.instantiateModule(body.so, body.id);
            if (resultId.empty()) {
                auto resp = crow::response(500, R"({"error":"instantiate failed"})" );
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            auto resp = crow::response(200, R"({"ok":true,"id":")"
                + resultId + R"("})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // GET /api/modules/:id — Module detail
        // =====================================================================
        CROW_ROUTE(app, "/api/modules/<string>")
        ([this](const std::string& id) {
            std::shared_lock<std::shared_mutex> lock(core_.moduleMutex());
            auto* mod = core_.loader().get(id);
            if (!mod) {
                auto resp = crow::response(404, R"({"error":"module not found"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            // Build detail with data sections included
            std::string json = "{";
            json += "\"id\":\"" + mod->id + "\"";
            json += ",\"name\":\"" + mod->nameStr + "\"";
            json += ",\"version\":\"" + mod->versionStr + "\"";
            json += ",\"state\":" + std::to_string(static_cast<int>(mod->state));
            json += ",\"path\":\"" + jsonEscapeString(mod->path.string()) + "\"";
            if (!mod->sourceFileStr.empty()) {
                json += ",\"sourceFile\":\"" + jsonEscapeString(mod->sourceFileStr) + "\"";
            }
            json += ",\"cyclicClass\":\"" + core_.scheduler().moduleClass(mod->id) + "\"";

            auto* ts = core_.scheduler().taskState(mod->id);
            if (ts) {
                json += ",\"stats\":{";
                json += "\"cycleCount\":" + std::to_string(ts->cycleCount.load());
                json += ",\"overrunCount\":" + std::to_string(ts->overrunCount.load());
                json += ",\"lastCycleTimeUs\":" + std::to_string(ts->lastCycleTimeUs.load());
                json += ",\"maxCycleTimeUs\":" + std::to_string(ts->maxCycleTimeUs.load());
                json += ",\"lastJitterUs\":" + std::to_string(ts->lastJitterUs.load());

                // Add cycle history
                {
                    std::lock_guard lk(ts->cycleHistoryMx);
                    auto histVec = ts->cycleHistory.getAll();
                    json += ",\"cycleHistory\":" + serializeCycleHistory(histVec);
                }

                json += "}";
            }

            // Include current data
            json += ",\"data\":{";
            json += "\"config\":"  + core_.dataEngine().readSection(id, DataSection::Config);
            json += ",\"recipe\":" + core_.dataEngine().readSection(id, DataSection::Recipe);
            json += ",\"runtime\":" + core_.dataEngine().readSection(id, DataSection::Runtime);
            json += ",\"summary\":" + core_.dataEngine().readSection(id, DataSection::Summary);
            json += "}";

            json += "}";

            auto resp = crow::response(200, json);
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // GET/POST /api/modules/:id/data/:section — Read or Write data section
        // =====================================================================
        CROW_ROUTE(app, "/api/modules/<string>/data/<string>")
        .methods("GET"_method, "POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req, const std::string& id, const std::string& sectionName) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }

            auto section = parseSectionName(sectionName);
            if (!section) {
                auto resp = crow::response(400, R"({"error":"invalid section. Use: config, recipe, runtime"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            if (req.method == "GET"_method) {
                std::shared_lock<std::shared_mutex> lock(core_.moduleMutex());
                auto json = core_.dataEngine().readSection(id, *section);
                auto resp = crow::response(200, json);
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            // POST — write section
            std::shared_lock<std::shared_mutex> lock(core_.moduleMutex());
            bool ok = core_.dataEngine().writeSection(id, *section, req.body);
            if (ok) {
                if (*section == DataSection::Config) {
                    core_.dataStore().saveConfig(id, core_.dataEngine());
                } else if (*section == DataSection::Recipe) {
                    core_.dataStore().saveRecipe(id, "default", core_.dataEngine());
                }
                auto resp = crow::response(200, R"({"ok":true})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            } else {
                auto resp = crow::response(400, R"({"error":"write failed"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
        });

        // =====================================================================
        // PATCH /api/modules/:id/data/:section — Update a single field
        // Body: {"ptr":"/field/0/sub","value":<any json>}
        // =====================================================================
        CROW_ROUTE(app, "/api/modules/<string>/data/<string>")
        .methods("PATCH"_method)
        ([this](const crow::request& req, const std::string& id, const std::string& sectionName) {
            auto section = parseSectionName(sectionName);
            if (!section) {
                auto resp = crow::response(400, R"({"error":"invalid section"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            // Parse body: extract "ptr" string and keep "value" as raw JSON.
            PatchBody body;
            if (auto ec = readJsonPermissive(body, req.body); ec || body.ptr.empty() || body.value.str.empty()) {
                auto resp = crow::response(400, R"({"error":"body must be {\"ptr\":\"...\",\"value\":...}"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            const auto& ptr      = body.ptr;
            const auto& valueJson = body.value.str;

            std::shared_lock<std::shared_mutex> lock(core_.moduleMutex());
            bool ok = core_.dataEngine().patchSection(id, *section, ptr, valueJson);
            if (!ok) {
                auto resp = crow::response(400, R"({"error":"patch failed"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
            auto resp = crow::response(200, R"({"ok":true})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // POST /api/modules/:id/config/save — Persist config to disk
        // =====================================================================
        CROW_ROUTE(app, "/api/modules/<string>/config/save")
        .methods("POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req, const std::string& id) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }
            std::shared_lock<std::shared_mutex> lock(core_.moduleMutex());
            bool ok = core_.dataStore().saveConfig(id, core_.dataEngine());
            auto resp = crow::response(ok ? 200 : 500, ok ? R"({"ok":true})" : R"({"error":"save failed"})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // POST /api/modules/:id/config/load — Reload config from disk
        // =====================================================================
        CROW_ROUTE(app, "/api/modules/<string>/config/load")
        .methods("POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req, const std::string& id) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }
            std::shared_lock<std::shared_mutex> lock(core_.moduleMutex());
            core_.dataStore().loadConfig(id, core_.dataEngine());
            auto json = core_.dataEngine().readSection(id, DataSection::Config);
            auto resp = crow::response(200, json);
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // POST /api/modules/:id/recipe/save/:name — Persist recipe to disk
        // =====================================================================
        CROW_ROUTE(app, "/api/modules/<string>/recipe/save/<string>")
        .methods("POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req, const std::string& id, const std::string& name) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }
            std::shared_lock<std::shared_mutex> lock(core_.moduleMutex());
            bool ok = core_.dataStore().saveRecipe(id, name, core_.dataEngine());
            auto resp = crow::response(ok ? 200 : 500, ok ? R"({"ok":true})" : R"({"error":"save failed"})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // POST /api/modules/:id/recipe/load/:name — Load recipe from disk
        // =====================================================================
        CROW_ROUTE(app, "/api/modules/<string>/recipe/load/<string>")
        .methods("POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req, const std::string& id, const std::string& name) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }
            std::shared_lock<std::shared_mutex> lock(core_.moduleMutex());
            core_.dataStore().loadRecipe(id, name, core_.dataEngine());
            auto json = core_.dataEngine().readSection(id, DataSection::Recipe);
            auto resp = crow::response(200, json);
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // DELETE /api/modules/:id — Remove a module instance
        // =====================================================================
        CROW_ROUTE(app, "/api/modules/<string>")
        .methods("DELETE"_method)
        ([this](const std::string& id) {
            bool ok = core_.removeInstance(id);
            if (!ok) {
                auto resp = crow::response(404, R"({"error":"instance not found"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
            auto resp = crow::response(200, R"({"ok":true})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // POST /api/modules/:id/reload — Warm-restart a module
        // =====================================================================
        CROW_ROUTE(app, "/api/modules/<string>/reload")
        .methods("POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req, const std::string& id) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }

            bool ok = core_.reloadModule(id);
            if (!ok) {
                auto resp = crow::response(500, R"({"error":"reload failed"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            auto resp = crow::response(200, R"({"ok":true,"message":"module reloaded"})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // POST /api/modules/upload — Upload a new or replacement .so module
        // Body: raw binary content of the .so file
        // Header X-Filename: required — the filename (e.g. "libmy_module.so")
        // =====================================================================
        CROW_ROUTE(app, "/api/modules/upload")
        .methods("POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type, X-Filename");
                return resp;
            }

            auto filename = req.get_header_value("X-Filename");
            if (filename.empty()) {
                auto resp = crow::response(400, R"({"error":"X-Filename header required"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            // Sanitize filename: extract basename only, reject path traversal attempts.
            auto sanitized = std::filesystem::path(filename).filename().string();
            if (sanitized.empty() || sanitized != filename || sanitized[0] == '.') {
                auto resp = crow::response(400, R"({"error":"invalid filename — must be a plain filename with no path separators"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            // Whitelist valid module extensions.
            auto ext = std::filesystem::path(sanitized).extension().string();
            if (ext != ".so" && ext != ".dylib" && ext != ".dll") {
                auto resp = crow::response(400, R"({"error":"invalid file extension — must be .so, .dylib, or .dll"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            if (req.body.empty()) {
                auto resp = crow::response(400, R"({"error":"empty body"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            // Write to a temp file, then call uploadModule.
            auto tmpPath = std::filesystem::temp_directory_path() / sanitized;
            {
                std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
                if (!ofs) {
                    auto resp = crow::response(500, R"({"error":"failed to write temp file"})");
                    resp.add_header("Content-Type", "application/json");
                    resp.add_header("Access-Control-Allow-Origin", "*");
                    return resp;
                }
                ofs.write(req.body.data(), static_cast<std::streamsize>(req.body.size()));
            }

            auto moduleId = core_.uploadModule(tmpPath);
            std::filesystem::remove(tmpPath);

            if (moduleId.empty()) {
                auto resp = crow::response(500, R"({"error":"upload failed"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            auto resp = crow::response(200, "{\"ok\":true,\"id\":\"" + moduleId + "\"}");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // GET /api/scope/schema — Nested runtime JSON for every module, keyed by
        // module ID. Returns `{ "moduleId": {...runtime...}, ... }`. The
        // frontend walks this tree directly to present probeable fields, so we
        // never ship a flat `[{moduleId, path}, ...]` representation.
        // =====================================================================
        // GET /api/scope/schema — migrated to api::dispatch (router.cpp).

        // =====================================================================
        // GET /api/scope/probes — List active probes
        // =====================================================================
        // GET /api/scope/probes — migrated to api::dispatch (router.cpp). POST/DELETE stay below.

        // =====================================================================
        // POST /api/scope/probes — Add a probe
        // Body: {"moduleId": "...", "path": "/fieldname"}
        // =====================================================================
        CROW_ROUTE(app, "/api/scope/probes")
        .methods("POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }

            AddProbeRequest req_body;
            if (auto err = readJsonPermissive(req_body, req.body); err) {
                auto resp = crow::response(400, R"({"error":"invalid JSON body"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
            if (req_body.moduleId.empty() || req_body.path.empty()) {
                auto resp = crow::response(400, R"({"error":"moduleId and path required"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            auto id = core_.oscilloscope().addProbe(req_body.moduleId, req_body.path);
            auto resp = crow::response(200, "{\"ok\":true,\"id\":" + std::to_string(id) + "}");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // DELETE /api/scope/probes/:id — Remove a probe
        // =====================================================================
        CROW_ROUTE(app, "/api/scope/probes/<string>")
        .methods("DELETE"_method, "OPTIONS"_method)
        ([this](const crow::request& req, const std::string& probeIdStr) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "DELETE, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }
            uint64_t probeId = 0;
            try { probeId = std::stoull(probeIdStr); } catch (...) {
                auto resp = crow::response(400, R"({"error":"invalid probe id"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
            bool ok = core_.oscilloscope().removeProbe(probeId);
            if (!ok) {
                auto resp = crow::response(404, R"({"error":"probe not found"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
            auto resp = crow::response(200, R"({"ok":true})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // GET /api/scope/data — Get all ring buffer data
        // =====================================================================
        // GET /api/scope/data — migrated to api::dispatch (router.cpp).

        // =====================================================================
        // GET /api/scheduler/classes — List class configs + live stats
        // =====================================================================
        // GET /api/scheduler/classes — migrated to api::dispatch (router.cpp).

        // =====================================================================
        // POST /api/scheduler/classes — Create a new class definition
        // Body: {"name": "myclass", "period_us": 20000, "priority": 50, "cpu_affinity": -1, "spin_us": 0}
        // =====================================================================
        CROW_ROUTE(app, "/api/scheduler/classes")
        .methods("POST"_method)
        ([this](const crow::request& req) {
            ClassDef def;
            if (auto err = readJsonPermissive(def, req.body); err) {
                auto resp = crow::response(400, R"({"error":"invalid JSON body"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
            if (def.name.empty()) {
                auto resp = crow::response(400, R"({"error":"name is required"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
            if (!core_.scheduler().addClassDef(def)) {
                auto resp = crow::response(409, R"({"error":"class already exists"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
            core_.saveSchedulerConfig();
            auto resp = crow::response(201, R"({"ok":true})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // PATCH /api/scheduler/classes/:name — Update class definition
        // Body: {"period_us": 10000, "priority": 50, "cpu_affinity": -1}
        // =====================================================================
        CROW_ROUTE(app, "/api/scheduler/classes/<string>")
        .methods("PATCH"_method, "OPTIONS"_method)
        ([this](const crow::request& req, const std::string& className) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "PATCH, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }

            // Find current definition
            auto defs = core_.scheduler().classConfigs();
            ClassDef updated;
            bool found = false;
            for (auto& d : defs) {
                if (d.name == className) { updated = d; found = true; break; }
            }
            if (!found) {
                auto resp = crow::response(404, R"({"error":"class not found"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            // Deserialize patch: glaze only overwrites fields present in the JSON body,
            // leaving all other fields at their current values (PATCH semantics for free).
            if (auto err = readJsonPermissive(updated, req.body); err) {
                auto resp = crow::response(400, R"({"error":"invalid JSON body"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
            // Never allow the client to rename a class via PATCH
            updated.name = className;

            core_.scheduler().updateClassDef(updated);
            core_.saveSchedulerConfig();

            auto resp = crow::response(200, R"({"ok":true})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // GET /api/scheduler/schema — JSON Schema for SchedulerConfig
        // =====================================================================
        CROW_ROUTE(app, "/api/scheduler/schema")
        ([this]() {
            auto schema = glz::write_json_schema<SchedulerConfig>().value_or("{}");
            auto resp = crow::response(200, schema);
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // GET /api/scheduler/modules/:id/history?since=<ms>&bin=<ms>
        //   Returns the per-module cycle/jitter ring buffer as a JSON array.
        //   `since` (optional) filters to samples newer than the given timestamp.
        //   `bin`   (optional) groups samples into fixed-width bins; each bin
        //           reports max cycle and max |jitter| within the window.
        // =====================================================================
        CROW_ROUTE(app, "/api/scheduler/modules/<string>/history")
        ([this](const crow::request& req, const std::string& id) {
            auto* ts = core_.scheduler().taskState(id);
            if (!ts) {
                auto resp = crow::response(404, "{\"error\":\"module not found\"}");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
            int64_t since = 0;
            int64_t binMs = 0;
            if (auto p = req.url_params.get("since")) { try { since = std::stoll(p); } catch (...) {} }
            if (auto p = req.url_params.get("bin"))   { try { binMs = std::stoll(p); } catch (...) {} }
            std::vector<MetricSample> samples;
            {
                std::lock_guard lk(ts->cycleHistoryMx);
                samples = ts->cycleHistory.getAll();
            }
            auto body = buildHistoryBody(samples, since, binMs);
            auto resp = crow::response(200, body);
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // GET /api/scheduler/classes/:name/history?since=<ms>&bin=<ms>
        //   Returns the per-class cycle/jitter ring buffer (binned if bin > 0).
        // =====================================================================
        CROW_ROUTE(app, "/api/scheduler/classes/<string>/history")
        ([this](const crow::request& req, const std::string& name) {
            auto deque = core_.scheduler().classHistory(name);
            std::vector<MetricSample> samples(deque.begin(), deque.end());
            int64_t since = 0;
            int64_t binMs = 0;
            if (auto p = req.url_params.get("since")) { try { since = std::stoll(p); } catch (...) {} }
            if (auto p = req.url_params.get("bin"))   { try { binMs = std::stoll(p); } catch (...) {} }
            auto body = buildHistoryBody(samples, since, binMs);
            auto resp = crow::response(200, body);
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // GET /api/io-mappings — List all I/O mappings with resolution status
        // =====================================================================
        // GET /api/io-mappings — migrated to api::dispatch (router.cpp). POST below stays.

        // =====================================================================
        // POST /api/io-mappings — Add a new mapping
        // Body: {"source": "left_motor.runtime.speed", "target": "controller.runtime.input", "enabled": true}
        // =====================================================================
        CROW_ROUTE(app, "/api/io-mappings")
        .methods("POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }

            IOMapEntry entry;
            if (auto err = readJsonPermissive(entry, req.body); err) {
                auto resp = crow::response(400, R"({"error":"invalid JSON body"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
            if (entry.source.empty() || entry.target.empty()) {
                auto resp = crow::response(400, R"({"error":"source and target are required"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            auto index = core_.ioMapper().addMapping(entry.source, entry.target, entry.enabled);
            core_.ioMapper().resolveAll(core_);

            auto resp = crow::response(201, "{\"ok\":true,\"index\":" + std::to_string(index) + "}");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // DELETE/PATCH /api/io-mappings/:index — Remove or update a mapping
        // =====================================================================
        CROW_ROUTE(app, "/api/io-mappings/<string>")
        .methods("DELETE"_method, "PATCH"_method, "OPTIONS"_method)
        ([this](const crow::request& req, const std::string& indexStr) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "DELETE, PATCH, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }

            size_t index = 0;
            try { index = std::stoull(indexStr); } catch (...) {
                auto resp = crow::response(400, R"({"error":"invalid index"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            if (req.method == "DELETE"_method) {
                // DELETE: remove mapping
                if (!core_.ioMapper().removeMapping(index)) {
                    auto resp = crow::response(404, R"({"error":"mapping not found"})");
                    resp.add_header("Content-Type", "application/json");
                    resp.add_header("Access-Control-Allow-Origin", "*");
                    return resp;
                }
                auto resp = crow::response(200, R"({"ok":true})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            } else if (req.method == "PATCH"_method) {
                // PATCH: update mapping
                if (index >= core_.ioMapper().entryCount()) {
                    auto resp = crow::response(404, R"({"error":"mapping not found"})");
                    resp.add_header("Content-Type", "application/json");
                    resp.add_header("Access-Control-Allow-Origin", "*");
                    return resp;
                }

                IOMapEntry entry;
                if (auto err = readJsonPermissive(entry, req.body); err) {
                    auto resp = crow::response(400, R"({"error":"invalid JSON body"})");
                    resp.add_header("Content-Type", "application/json");
                    resp.add_header("Access-Control-Allow-Origin", "*");
                    return resp;
                }
                if (entry.source.empty() || entry.target.empty()) {
                    auto resp = crow::response(400, R"({"error":"source and target are required"})");
                    resp.add_header("Content-Type", "application/json");
                    resp.add_header("Access-Control-Allow-Origin", "*");
                    return resp;
                }

                if (!core_.ioMapper().updateMapping(index, entry.source, entry.target, entry.enabled)) {
                    auto resp = crow::response(500, R"({"error":"update failed"})");
                    resp.add_header("Content-Type", "application/json");
                    resp.add_header("Access-Control-Allow-Origin", "*");
                    return resp;
                }

                core_.ioMapper().resolveAll(core_);

                auto resp = crow::response(200, R"({"ok":true})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            auto resp = crow::response(405, R"({"error":"method not allowed"})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // POST /api/io-mappings/resolve — Force re-resolve all mappings
        // =====================================================================
        CROW_ROUTE(app, "/api/io-mappings/resolve")
        .methods("POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }

            core_.ioMapper().resolveAll(core_);
            auto resp = crow::response(200, R"({"ok":true})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // POST /api/scheduler/reassign — Move a module to a different class
        // Body: {"moduleId": "...", "class": "...", "order": 0 (optional)}
        // =====================================================================
        CROW_ROUTE(app, "/api/scheduler/reassign")
        .methods("POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }

            // Parse body manually (keep the dep-light pattern of the rest of server.cpp).
            std::string moduleId, newClass;
            std::optional<int> newOrder;

            // Simple key extraction — body is expected to be compact JSON.
            auto extract = [&](const std::string& key) -> std::string {
                auto pos = req.body.find("\"" + key + "\"");
                if (pos == std::string::npos) return {};
                pos = req.body.find(':', pos);
                if (pos == std::string::npos) return {};
                ++pos;
                while (pos < req.body.size() && req.body[pos] == ' ') ++pos;
                if (pos >= req.body.size()) return {};
                if (req.body[pos] == '"') {
                    ++pos;
                    auto end = req.body.find('"', pos);
                    return (end != std::string::npos) ? req.body.substr(pos, end - pos) : "";
                }
                auto end = req.body.find_first_of(",}", pos);
                return (end != std::string::npos) ? req.body.substr(pos, end - pos) : "";
            };

            moduleId = extract("moduleId");
            newClass = extract("class");
            auto orderStr = extract("order");

            if (moduleId.empty() || newClass.empty()) {
                auto resp = crow::response(400, R"({"error":"moduleId and class are required"})");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }

            if (!orderStr.empty()) {
                try { newOrder = std::stoi(orderStr); } catch (...) {}
            }

            auto ec = core_.scheduler().reassignClass(moduleId, newClass, newOrder);
            if (ec) {
                auto resp = crow::response(400, "{\"error\":\"" + ec.message() + "\"}");
                resp.add_header("Content-Type", "application/json");
                resp.add_header("Access-Control-Allow-Origin", "*");
                return resp;
            }
            core_.saveSchedulerConfig();

            auto resp = crow::response(200, R"({"ok":true})");
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // GET /api/bus/topics — List bus topics
        // =====================================================================
        // GET /api/bus/topics — migrated to api::dispatch (router.cpp).

        // =====================================================================
        // GET /api/bus/services — List bus services with request schemas
        // =====================================================================
        // GET /api/bus/services — migrated to api::dispatch (router.cpp).

        // =====================================================================
        // POST /api/bus/services/:name — Call a bus service
        // =====================================================================
        CROW_ROUTE(app, "/api/bus/call/<path>")
        .methods("POST"_method, "OPTIONS"_method)
        ([this](const crow::request& req, const std::string& serviceName) {
            if (req.method == "OPTIONS"_method) {
                auto resp = crow::response(204);
                resp.add_header("Access-Control-Allow-Origin", "*");
                resp.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                resp.add_header("Access-Control-Allow-Headers", "Content-Type");
                return resp;
            }

            auto result = core_.bus().call(serviceName, req.body);
            std::string json = "{";
            json += "\"ok\":" + std::string(result.ok ? "true" : "false");
            if (!result.response.empty()) {
                json += ",\"response\":" + result.response;
            }
            if (!result.error.empty()) {
                json += ",\"error\":\"" + result.error + "\"";
            }
            json += "}";

            auto resp = crow::response(200, json);
            resp.add_header("Content-Type", "application/json");
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        // =====================================================================
        // WebSocket /ws — Live data streaming with topic subscriptions.
        //
        // Two message kinds the server emits:
        //   • {"type":"live", modules:{<id>:{summary,stats}}, classes:{...}}
        //     Always sent. Small. Used by Dashboard / SchedulerView.
        //   • {"type":"runtime", modules:{<id>:{runtime:...}, ...}}
        //     Sent only to connections that explicitly subscribed to
        //     "module/<id>/runtime". Used by ModuleDetail.
        //
        // Client → server messages (binary or text JSON):
        //   {"type":"subscribe","topics":["module/foo/runtime", ...]}
        //   {"type":"unsubscribe","topics":[...]}
        //   {"type":"unsubscribe_all"}
        // =====================================================================
        std::mutex wsMutex;
        std::set<crow::websocket::connection*> wsClients;
        // Per-connection subscription set. Topic strings, e.g. "module/<id>/runtime".
        std::unordered_map<crow::websocket::connection*, std::unordered_set<std::string>> liveSubs;

        // WebSocket /ws/watch — Dedicated variable subscriptions
        std::mutex watchMutex;
        // Per-connection subscriptions: connection* -> map(clientId -> subscription)
        struct WatchSub { std::string moduleId; std::string path; bool expand = false; };
        std::unordered_map<crow::websocket::connection*, std::unordered_map<std::string, WatchSub>> watchSessions;

        CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onopen([&](crow::websocket::connection& conn) {
            std::lock_guard lock(wsMutex);
            wsClients.insert(&conn);
            liveSubs.emplace(&conn, std::unordered_set<std::string>{});
            spdlog::info("WebSocket client connected (total: {})", wsClients.size());
        })
        .onclose([&](crow::websocket::connection& conn, const std::string& reason, unsigned short /*code*/) {
            std::lock_guard lock(wsMutex);
            wsClients.erase(&conn);
            liveSubs.erase(&conn);
            spdlog::info("WebSocket client disconnected: {} (total: {})", reason, wsClients.size());
        })
        .onmessage([&](crow::websocket::connection& conn, const std::string& data, bool /*is_binary*/) {
            LiveSubscribeRequest req;
            if (auto err = readJsonPermissive(req, data); err) {
                spdlog::debug("WS /ws: malformed subscribe request: {}", data);
                return;
            }
            std::lock_guard lock(wsMutex);
            auto it = liveSubs.find(&conn);
            if (it == liveSubs.end()) return;
            if (req.type == "subscribe") {
                for (auto& t : req.topics) it->second.insert(t);
            } else if (req.type == "unsubscribe") {
                for (auto& t : req.topics) it->second.erase(t);
            } else if (req.type == "unsubscribe_all") {
                it->second.clear();
            }
        });

        // Dedicated watch websocket
        CROW_WEBSOCKET_ROUTE(app, "/ws/watch")
        .onopen([&](crow::websocket::connection& conn) {
            std::lock_guard lg(watchMutex);
            watchSessions.emplace(&conn, std::unordered_map<std::string, WatchSub>());
            spdlog::info("Watch WS client connected (total watch sessions: {})", watchSessions.size());
        })
        .onclose([&](crow::websocket::connection& conn, const std::string& reason, unsigned short /*code*/) {
            std::lock_guard lg(watchMutex);
            watchSessions.erase(&conn);
            spdlog::info("Watch WS client disconnected: {} (total watch sessions: {})", reason, watchSessions.size());
        })
        .onmessage([&](crow::websocket::connection& conn, const std::string& data, bool /*is_binary*/) {
            // Expect messages of form: {"type":"subscribe","id":"c1","moduleId":"mod","path":"/status/pos","expand":false}
            WatchRequest req;
            if (auto err = readJsonPermissive(req, data); err) {
                spdlog::debug("Watch WS: malformed request: {}", data);
                return;
            }

            if (req.type == "subscribe") {
                if (req.id.empty() || req.moduleId.empty() || req.path.empty()) {
                    conn.send_text("{\"type\":\"error\",\"message\":\"id,moduleId,path required\"}");
                    return;
                }
                std::lock_guard lg(watchMutex);
                watchSessions[&conn][req.id] = WatchSub{req.moduleId, req.path, req.expand};
                conn.send_text(std::string("{\"type\":\"subscribed\",\"id\":\"") + jsonEscapeString(req.id) + "\"}");
                return;
            }

            if (req.type == "unsubscribe") {
                if (req.id.empty()) return;
                std::lock_guard lg(watchMutex);
                auto it = watchSessions.find(&conn);
                if (it != watchSessions.end()) it->second.erase(req.id);
                conn.send_text(std::string("{\"type\":\"unsubscribed\",\"id\":\"") + jsonEscapeString(req.id) + "\"}");
                return;
            }

            // unknown type: ignore
        });

        // Background thread to push live data to WebSocket clients.
        //
        // Two-frame model:
        //   1. "live" — small, broadcast to every connected client.
        //      Contains per-module summary+stats and per-class stats.
        //   2. "runtime" — built per connection from its subscription set.
        //      Only sent if the client subscribed to at least one
        //      "module/<id>/runtime" topic. Avoids serializing/sending
        //      large module runtime sections (e.g. EtherCAT PDO buffers)
        //      to clients that aren't displaying them.
        std::atomic<bool> wsRunning{true};
        std::thread wsThread([&]() {
            const std::string runtimeTopicPrefix = "module/";
            const std::string runtimeTopicSuffix = "/runtime";

            while (wsRunning.load() && running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(config_.wsUpdateIntervalMs));

                // Snapshot the union of runtime topics across all connections so
                // we only pay the serialization cost for modules someone is
                // actually watching.
                std::unordered_set<std::string> runtimeIdsWanted;
                {
                    std::lock_guard lock(wsMutex);
                    for (auto& [conn, topics] : liveSubs) {
                        for (auto& t : topics) {
                            if (t.size() > runtimeTopicPrefix.size() + runtimeTopicSuffix.size() &&
                                t.compare(0, runtimeTopicPrefix.size(), runtimeTopicPrefix) == 0 &&
                                t.compare(t.size() - runtimeTopicSuffix.size(), runtimeTopicSuffix.size(), runtimeTopicSuffix) == 0) {
                                runtimeIdsWanted.insert(t.substr(runtimeTopicPrefix.size(),
                                                                 t.size() - runtimeTopicPrefix.size() - runtimeTopicSuffix.size()));
                            }
                        }
                    }
                }

                // Build per-module base fragments + runtime sections under a shared
                // lock so reloadModule can't concurrently mutate the module map or
                // data engine.  Each fragment is the module's open JSON content
                // (no closing "}") so runtime data can be appended inline
                // per-connection without re-serializing the common fields.
                std::unordered_map<std::string, std::string> moduleFragments;
                std::unordered_map<std::string, std::string> runtimeFragments;
                std::string classesTail;
                {
                    std::shared_lock<std::shared_mutex> lock(core_.moduleMutex());

                    for (auto& id : runtimeIdsWanted) {
                        if (core_.loader().modules().find(id) == core_.loader().modules().end()) continue;
                        runtimeFragments.emplace(id, core_.dataEngine().readSection(id, DataSection::Runtime));
                    }

                    for (auto& [id, mod] : core_.loader().modules()) {
                        std::string frag = "\"" + id + "\":{";
                        frag += "\"summary\":" + core_.dataEngine().readSection(id, DataSection::Summary);
                        auto* ts = core_.scheduler().taskState(id);
                        if (ts) {
                            frag += ",\"stats\":{";
                            frag += "\"cycleCount\":" + std::to_string(ts->cycleCount.load());
                            frag += ",\"lastCycleTimeUs\":" + std::to_string(ts->lastCycleTimeUs.load());
                            frag += ",\"maxCycleTimeUs\":" + std::to_string(ts->maxCycleTimeUs.load());
                            frag += ",\"overrunCount\":" + std::to_string(ts->overrunCount.load());
                            frag += ",\"lastJitterUs\":" + std::to_string(ts->lastJitterUs.load());
                            // cycleHistory is intentionally NOT included here.
                            // Charts pull it via REST (/api/scheduler/modules/:id/history)
                            // because pushing it on every tick is far more bandwidth
                            // than any chart actually consumes.
                            frag += "}";
                        }
                        // No closing "}" — appended per-connection after optional runtime injection.
                        moduleFragments.emplace(id, std::move(frag));
                    }

                    classesTail = "},\"classes\":{";
                    bool firstClass = true;
                    for (auto& cs : core_.scheduler().allClassStats()) {
                        if (!firstClass) classesTail += ",";
                        classesTail += "\"" + cs.name + "\":{";
                        classesTail += "\"lastJitterUs\":" + std::to_string(cs.lastJitterUs);
                        classesTail += ",\"lastCycleTimeUs\":" + std::to_string(cs.lastCycleTimeUs);
                        classesTail += ",\"maxCycleTimeUs\":" + std::to_string(cs.maxCycleTimeUs);
                        classesTail += ",\"tickCount\":" + std::to_string(cs.tickCount);
                        classesTail += ",\"memberCount\":" + std::to_string(cs.memberCount);
                        classesTail += ",\"lastTickStartMs\":" + std::to_string(cs.lastTickStartMs);
                        // cycleHistory pulled via REST (/api/scheduler/classes/:name/history).
                        classesTail += "}";
                        firstClass = false;
                    }
                    classesTail += "}}";
                }

                // One send_binary per connection — runtime data embedded inline for
                // subscribers.  A single frame per connection means two rapid
                // send_binary calls cannot race inside Crow's do_write / ASIO buffer
                // chain.  Binary frames so non-UTF-8 bytes in device payloads
                // cannot kill the socket.
                static const std::unordered_set<std::string> kNoTopics;
                std::lock_guard lock(wsMutex);
                for (auto* conn : wsClients) {
                    const auto subsIt = liveSubs.find(conn);
                    const auto& connTopics = (subsIt != liveSubs.end()) ? subsIt->second : kNoTopics;

                    std::string msg = "{\"type\":\"live\",\"modules\":{";
                    bool first = true;
                    for (auto& [id, frag] : moduleFragments) {
                        if (!first) msg += ",";
                        msg += frag;
                        const std::string topic = runtimeTopicPrefix + id + runtimeTopicSuffix;
                        if (connTopics.count(topic)) {
                            auto fIt = runtimeFragments.find(id);
                            if (fIt != runtimeFragments.end())
                                msg += ",\"runtime\":" + fIt->second;
                        }
                        msg += "}";
                        first = false;
                    }
                    msg += classesTail;
                    conn->send_binary(msg);
                }
            }
        });

        // Background thread to sample and push watch values to /ws/watch clients
        std::atomic<bool> watchRunning{true};
        std::thread watchThread([&]() {
            while (watchRunning.load() && running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(config_.wsUpdateIntervalMs));

                // Snapshot current subscriptions
                struct Entry { crow::websocket::connection* conn; std::string clientId; std::string moduleId; std::string path; bool expand; };
                std::vector<Entry> entries;
                {
                    std::lock_guard lg(watchMutex);
                    for (auto& [conn, subs] : watchSessions) {
                        for (auto& [cid, sub] : subs) {
                            entries.push_back({conn, cid, sub.moduleId, sub.path, sub.expand});
                        }
                    }
                }
                if (entries.empty()) continue;

                // Group by moduleId -> path -> list of (conn, clientId)
                std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::pair<crow::websocket::connection*, std::string>>>> tasks;
                for (auto& e : entries) tasks[e.moduleId][e.path].push_back({e.conn, e.clientId});

                // Collect per-connection values
                std::unordered_map<crow::websocket::connection*, std::vector<std::string>> perConnVals;
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

                // Read values under shared module lock
                {
                    std::shared_lock<std::shared_mutex> lock(core_.moduleMutex());
                    for (auto& [moduleId, pathMap] : tasks) {
                        auto* lm = core_.loader().get(moduleId);
                        for (auto& [path, vec] : pathMap) {
                            if (!lm || !lm->instance) {
                                for (auto& [conn, clientId] : vec) {
                                    std::string item = "{\"id\":\"" + jsonEscapeString(clientId) + "\",\"moduleId\":\"" + jsonEscapeString(moduleId) + "\",\"path\":\"" + jsonEscapeString(path) + "\",\"value\":null,\"error\":\"module not found\"}";
                                    perConnVals[conn].push_back(item);
                                }
                                continue;
                            }
                            IModule* inst = lm->instance.get();
                            std::string callPath = path;
                            // if (!callPath.empty() && callPath.front() == '/') callPath.erase(0, 1);
                            auto maybe = inst->readField(DataSection::Runtime, callPath);
                            std::string val = maybe ? *maybe : "null";
                            for (auto& [conn, clientId] : vec) {
                                std::string item = "{\"id\":\"" + jsonEscapeString(clientId) + "\",\"moduleId\":\"" + jsonEscapeString(moduleId) + "\",\"path\":\"" + jsonEscapeString(path) + "\",\"value\":" + val + ",\"error\":null}";
                                perConnVals[conn].push_back(item);
                            }
                        }
                    }
                }

                // Send aggregated message per connection.
                // Hold watchMutex while sending: onclose also takes watchMutex, so a
                // disconnecting connection cannot be freed until after this loop finishes —
                // the conn* is guaranteed alive for the duration of the locked section.
                {
                    std::lock_guard lg(watchMutex);
                    for (auto& [conn, items] : perConnVals) {
                        if (!conn || watchSessions.find(conn) == watchSessions.end()) continue;
                        std::string joined;
                        for (size_t i = 0; i < items.size(); ++i) {
                            if (i) joined += ",";
                            joined += items[i];
                        }
                        std::string msg = std::string("{\"type\":\"watch\",\"ts\":") + std::to_string(nowMs) + ",\"values\":[" + joined + "]}";
                        try { conn->send_binary(msg); } catch (...) { /* connection closed mid-send, will be removed on next onclose */ }
                    }
                }
            }
        });

        // Set up static file serving from config_.staticDir. Crow resolves
        // CROW_STATIC_DIRECTORY (= g_crowStaticDir) at run() time, so point it at
        // the *configured* dir here — otherwise a non-default --data-dir serves
        // the build-default "./data/UI" (relative to cwd) and 404s. The index +
        // SPA-fallback routes below read g_crowStaticDir too.
        const std::string staticDir = crow::utility::normalize_path(config_.staticDir);
        g_crowStaticDir = staticDir; // normalized: forward slashes + trailing '/'
        if (!std::filesystem::exists(config_.staticDir)) {
            spdlog::warn("Static file directory does not exist: {}", config_.staticDir);
        } else {
            spdlog::info("Serving static files from: {}", config_.staticDir);
        }

        // Loom monitoring UI dir — resolved independently of staticDir (typically
        // install-relative) so /_loom works even when staticDir hosts a user app.
        g_crowLoomUiDir = crow::utility::normalize_path(config_.loomUiDir);
        if (!std::filesystem::exists(config_.loomUiDir)) {
            spdlog::warn("Loom UI directory does not exist: {}", config_.loomUiDir);
        } else {
            spdlog::info("Serving Loom UI (/_loom) from: {}", config_.loomUiDir);
        }

        // index.html, read directly into the response body. set_static_file_info
        // mangles absolute paths via sanitize_filename (→ 404), so we read the
        // file ourselves rather than relying on Crow's static helper here.
        auto readIndex = [](const std::string& dir) {
            std::ifstream f(dir + "index.html", std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            crow::response res(200, ss.str());
            res.add_header("Content-Type", "text/html; charset=utf-8");
            return res;
        };
        auto serveIndex     = [readIndex]() { return readIndex(g_crowStaticDir); };
        auto serveLoomIndex = [readIndex]() { return readIndex(g_crowLoomUiDir); };

        // In standby (no user app) staticDir *is* the Loom UI, which is built for
        // base '/_loom/' — serving it at '/' would render blank. So when the root
        // static dir resolves to the Loom UI, send '/' to the canonical /_loom/.
        // In user-project mode the dirs differ and the user app is served at '/'.
        // Compare by filesystem identity (handles relative-vs-absolute paths);
        // equivalent() needs both to exist, so a missing dir yields false.
        std::error_code rootEqEc;
        const bool rootIsLoomUi =
            std::filesystem::equivalent(config_.staticDir, config_.loomUiDir, rootEqEc) && !rootEqEc;

        // Serve index.html at root.
        CROW_ROUTE(app, "/")
        ([serveIndex, rootIsLoomUi](crow::response& res) {
            if (rootIsLoomUi) {
                res.code = 301;
                res.add_header("Location", "/_loom/");
            } else {
                res = serveIndex();
            }
            res.end();
        });

        // mapp Connect-compatible facade — additive REST + /api/1.0/pushchannel.
        // Registered alongside the legacy /ws and /api/* routes; shares core_.
        opcRest_ = std::make_unique<OpcUaRestServer>(core_);
        opcRest_->registerRoutes(app);
        opcRest_->startPump(config_.wsUpdateIntervalMs);

        // Loom monitoring/config UI, always mounted at /_loom (regardless of what
        // staticDir hosts). Same real-file-then-SPA-fallback behavior as the root
        // app, but rooted at g_crowLoomUiDir. Registered BEFORE the "/<path>"
        // catch-all so it wins (Crow resolves ambiguous matches to the lowest
        // rule index, i.e. earliest-registered). The Loom frontend is built with
        // Vite base '/_loom/', so its asset URLs resolve under this prefix. Crow
        // auto-registers a 301 from the slash-less '/_loom' to '/_loom/'.
        CROW_ROUTE(app, "/_loom/")
        ([serveLoomIndex]() { return serveLoomIndex(); });
        CROW_ROUTE(app, "/_loom/<path>")
        ([serveLoomIndex](crow::response& res, const std::string& filePathPartial) {
            std::error_code ec;
            const std::string full = g_crowLoomUiDir + filePathPartial;
            if (std::filesystem::is_regular_file(full, ec)) {
                res.set_static_file_info_unsafe(full);
                res.end();
            } else {
                res = serveLoomIndex();
                res.end();
            }
        });

        // Generic static + SPA history fallback (replaces Crow's built-in static
        // catch-all, which we disabled via CROW_DISABLE_STATIC_DIR). For any GET:
        //   - if it maps to a real file under the static dir, serve that file;
        //   - otherwise return index.html so the client-side router can handle it
        //     (a hard navigation/refresh on a client route returns the app, not 404).
        // This lets Loom host ANY single-page app without hardcoding its routes.
        //
        // IMPORTANT: this catch-all MUST be registered LAST. Crow resolves a
        // path that matches multiple rules to the LOWEST rule_index, i.e. the
        // earliest-registered rule wins. Registering this "/<path>" wildcard
        // after every real route (REST + WebSocket) gives it the highest index,
        // so it only wins when no literal route matches — otherwise it would
        // shadow the facade's GET endpoints and the pushchannel WS upgrade.
        CROW_ROUTE(app, "/<path>")
        ([serveIndex](crow::response& res, const std::string& filePathPartial) {
            std::error_code ec;
            const std::string full = g_crowStaticDir + filePathPartial;
            if (std::filesystem::is_regular_file(full, ec)) {
                res.set_static_file_info_unsafe(full);
                res.end();
            } else {
                res = serveIndex();
                res.end();
            }
        });

        // ---- /api/* catch-all → shared dispatch (registered LAST so the static
        // routes above take priority; this only handles paths none of them match,
        // i.e. routes already migrated into api::dispatch and shared with WASM).
        CROW_ROUTE(app, "/api/<path>").methods(
            crow::HTTPMethod::Get, crow::HTTPMethod::Post, crow::HTTPMethod::Put,
            crow::HTTPMethod::Patch, crow::HTTPMethod::Delete)
        ([this](const crow::request& creq, const std::string&) {
            api::Request areq;
            areq.method = crowMethodToApi(creq.method);
            areq.path   = creq.url;
            areq.body   = creq.body;
            api::Response ares = api::dispatch(core_, areq);
            crow::response resp(ares.status, ares.body);
            resp.add_header("Content-Type", ares.contentType);
            resp.add_header("Access-Control-Allow-Origin", "*");
            return resp;
        });

        spdlog::info("Starting HTTP server on {}:{}", config_.bindAddress, config_.port);
        app.bindaddr(config_.bindAddress)
           .port(config_.port)
           .multithreaded()
           .run();

        // Cleanup
        wsRunning.store(false);
        if (wsThread.joinable()) wsThread.join();
        watchRunning.store(false);
        if (watchThread.joinable()) watchThread.join();
        if (opcRest_) opcRest_->stopPump();
    });
}

void Server::stop() {
    if (!running_.load()) return;
    running_.store(false);
    // Crow's app.stop() would be called here, but we rely on the process signal
    if (serverThread_.joinable()) {
        serverThread_.detach(); // Crow manages its own lifecycle
    }
    spdlog::info("Server stopped");
}

} // namespace loom
