#include "loom/api/router.h"
#include "loom/api/json_build.h"

#include "loom/runtime_core.h"
#include "loom/opcua_rest_nodeid.h"  // opcrest::sectionFromName
#include "loom/diag/breadcrumb.h"   // diag::phaseName

#include <glaze/glaze.hpp>

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace loom {

// ---------------------------------------------------------------------------
// Shared JSON builders (moved verbatim from server.cpp so the native server and
// the WASM api router emit identical bodies). Crow-free.
// ---------------------------------------------------------------------------

std::string jsonEscapeString(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\')      out += "\\\\";
        else if (c == '"')  out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += c;
    }
    return out;
}

std::string serializeCycleHistory(const std::vector<MetricSample>& samples, std::size_t maxSamples) {
    std::string json = "[";
    bool first = true;
    std::size_t startIdx = (maxSamples > 0 && samples.size() > maxSamples) ? samples.size() - maxSamples : 0;
    for (std::size_t i = startIdx; i < samples.size(); ++i) {
        const auto& s = samples[i];
        if (!first) json += ",";
        json += "{\"t\":" + std::to_string(s.timestampMs)
            + ",\"cycle\":" + std::to_string(s.cycleTimeUs)
            + ",\"jitter\":" + std::to_string(s.jitterUs) + "}";
        first = false;
    }
    json += "]";
    return json;
}

std::string serializeCycleHistory(const std::deque<MetricSample>& samples, std::size_t maxSamples) {
    std::string json = "[";
    bool first = true;
    std::size_t startIdx = (maxSamples > 0 && samples.size() > maxSamples) ? samples.size() - maxSamples : 0;
    std::size_t i = 0;
    for (const auto& s : samples) {
        if (i++ < startIdx) continue;
        if (!first) json += ",";
        json += "{\"t\":" + std::to_string(s.timestampMs)
            + ",\"cycle\":" + std::to_string(s.cycleTimeUs)
            + ",\"jitter\":" + std::to_string(s.jitterUs) + "}";
        first = false;
    }
    json += "]";
    return json;
}

std::string moduleInfoJson(const LoadedModule& mod, const Scheduler& scheduler) {
    auto* ts = scheduler.taskState(mod.id);

    std::string json = "{";
    json += "\"id\":\"" + mod.id + "\"";
    json += ",\"name\":\"" + mod.nameStr + "\"";
    json += ",\"className\":\"" + mod.className + "\"";
    json += ",\"version\":\"" + mod.versionStr + "\"";
    json += ",\"state\":" + std::to_string(static_cast<int>(mod.state));
    json += ",\"path\":\"" + jsonEscapeString(mod.path.string()) + "\"";
    if (!mod.sourceFileStr.empty()) {
        json += ",\"sourceFile\":\"" + jsonEscapeString(mod.sourceFileStr) + "\"";
    }
    json += ",\"cyclicClass\":\"" + scheduler.moduleClass(mod.id) + "\"";
    if (ts) {
        json += ",\"stats\":{";
        json += "\"cycleCount\":" + std::to_string(ts->cycleCount.load());
        json += ",\"overrunCount\":" + std::to_string(ts->overrunCount.load());
        json += ",\"lastCycleTimeUs\":" + std::to_string(ts->lastCycleTimeUs.load());
        json += ",\"maxCycleTimeUs\":" + std::to_string(ts->maxCycleTimeUs.load());
        json += ",\"lastJitterUs\":" + std::to_string(ts->lastJitterUs.load());

        const bool faulted = ts->faulted.load(std::memory_order_acquire);
        json += ",\"faulted\":" + std::string(faulted ? "true" : "false");
        if (faulted) {
            json += ",\"lastFaultMs\":" + std::to_string(ts->lastFaultMs.load());
            json += ",\"lastFaultPhase\":\"" +
                    std::string(diag::phaseName(static_cast<diag::Phase>(ts->lastFaultPhase.load()))) + "\"";
            json += ",\"lastFaultMsg\":\"" + jsonEscapeString(ts->lastFaultMsg) + "\"";
        }

        {
            std::lock_guard lk(ts->cycleHistoryMx);
            auto histVec = ts->cycleHistory.getAll();
            json += ",\"cycleHistory\":" + serializeCycleHistory(histVec);
        }

        json += "}";
    }
    json += "}";
    return json;
}

std::string buildHistoryBody(const std::vector<MetricSample>& samples, int64_t since, int64_t binMs) {
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

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

namespace api {

Method methodFromString(std::string_view s) {
    if (s == "GET")    return Method::GET;
    if (s == "POST")   return Method::POST;
    if (s == "PUT")    return Method::PUT;
    if (s == "PATCH")  return Method::PATCH;
    if (s == "DELETE") return Method::DELETE_;
    return Method::UNKNOWN;
}

// Scheduler DTOs (glaze auto-reflects these aggregates). In a NAMED namespace
// because glaze reflection requires the reflected type to have linkage. This is
// the single copy — server.cpp's /api/scheduler/classes route and its DTOs were
// removed; native reaches this handler via the /api/<path> catch-all.
struct ClassStatsDto {
    int64_t  lastJitterUs    = 0;
    int64_t  lastCycleTimeUs = 0;
    int64_t  maxCycleTimeUs  = 0;
    uint64_t tickCount       = 0;
    int      memberCount     = 0;
    int64_t  lastTickStartMs = 0;
};
struct ClassInfoDto {
    std::string              name;
    int                      period_us    = 10000;
    int                      cpu_affinity = -1;
    int                      priority     = 50;
    int                      spin_us      = 0;
    ClassStatsDto            stats;
    std::vector<std::string> modules;
};
ClassInfoDto makeClassInfoDto(const ClassDef& def, const std::vector<ClassStats>& allStats) {
    ClassInfoDto dto;
    dto.name         = def.name;
    dto.period_us    = def.period_us;
    dto.cpu_affinity = def.cpu_affinity;
    dto.priority     = def.priority;
    dto.spin_us      = def.spin_us;
    for (const auto& s : allStats) {
        if (s.name != def.name) continue;
        dto.stats   = { s.lastJitterUs, s.lastCycleTimeUs, s.maxCycleTimeUs, s.tickCount, s.memberCount };
        dto.modules = s.moduleIds;
        break;
    }
    return dto;
}

// Response item for GET /api/modules/available (glaze-reflected; named namespace).
struct AvailableModuleDto {
    std::string filename;
    std::string className;
    std::string version;
};

// Request-body shapes (glaze-reflected; named namespace, same reason as above).
struct PatchBody { std::string ptr; glz::raw_json value; };
struct AddProbeRequest { std::string moduleId; std::string path; };
struct InstantiateRequest { std::string id; std::string so; };

namespace {
Response json(int status, std::string body) { return {status, "application/json", std::move(body)}; }
Response notFound() { return json(404, "{\"error\":\"no matching route\"}"); }
Response badRequest(std::string msg) { return json(400, "{\"error\":\"" + std::move(msg) + "\"}"); }

// Permissive JSON read: unknown/missing keys are not errors (PATCH-style partial
// bodies, forward-compatible clients).
template <typename T>
glz::error_ctx readJsonPermissive(T& value, std::string_view jsonStr) {
    constexpr glz::opts kPermissiveReadOpts{
        .error_on_unknown_keys = false,
        .error_on_missing_keys = false,
    };
    glz::context ctx{};
    return glz::read<kPermissiveReadOpts>(value, jsonStr, ctx);
}

// Split "path?query" into (path, query); query is empty if there's no '?'. Both
// transports hand dispatch() a path that may carry a query suffix: the wasm host
// passes JS's `pathname + search` verbatim, and the native catch-all passes
// Crow's raw_url (which already includes it) — so the split happens once, here,
// rather than at each call site.
std::pair<std::string, std::string> splitPathQuery(const std::string& raw) {
    auto pos = raw.find('?');
    if (pos == std::string::npos) return {raw, {}};
    return {raw.substr(0, pos), raw.substr(pos + 1)};
}

// Extract one value from a raw "a=1&b=2" query string ("" if key is absent).
std::string queryParam(const std::string& query, std::string_view key) {
    std::size_t pos = 0;
    while (pos < query.size()) {
        auto amp = query.find('&', pos);
        auto piece = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        auto eq = piece.find('=');
        auto k = eq == std::string::npos ? piece : piece.substr(0, eq);
        if (k == key) return eq == std::string::npos ? std::string{} : piece.substr(eq + 1);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}
}

Response dispatch(RuntimeCore& core, const Request& req) {
    auto [path, query] = splitPathQuery(req.path);
    const auto  method = req.method;
    const auto& body   = req.body;

    // GET /api/modules — list all loaded modules
    if (method == Method::GET && path == "/api/modules") {
        std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
        std::string out = "[";
        bool first = true;
        for (auto& [id, mod] : core.loader().modules()) {
            if (!first) out += ",";
            out += moduleInfoJson(mod, core.scheduler());
            first = false;
        }
        out += "]";
        return json(200, std::move(out));
    }

    // GET /api/modules/available — installable module .so files. MUST precede the
    // /api/modules/<id> prefix check below (else "available" is read as a module id).
    if (method == Method::GET && path == "/api/modules/available") {
        const auto& cfg = core.config();
        auto available = ModuleLoader::queryAvailable(cfg.moduleDir);
        std::unordered_set<std::string> seen;
        seen.reserve(available.size() * 2);
        for (const auto& a : available) seen.insert(a.filename);
        for (const auto& extra : cfg.additionalModuleDirs) {
            for (auto& a : ModuleLoader::queryAvailable(extra))
                if (seen.insert(a.filename).second) available.push_back(std::move(a));
        }
        std::vector<AvailableModuleDto> dtos;
        dtos.reserve(available.size());
        for (auto& a : available) dtos.push_back({ a.filename, a.className, a.version });
        return json(200, glz::write_json(dtos).value_or("[]"));
    }

    // POST /api/modules/instantiate — create a new instance from a .so
    // Body: {"id":"left_motor","so":"example_motor.so"}. MUST precede the
    // /api/modules/<id> prefix check below for the same reason as "available".
    if (method == Method::POST && path == "/api/modules/instantiate") {
        InstantiateRequest reqBody{};
        auto err = readJsonPermissive(reqBody, body);
        if (err || reqBody.id.empty() || reqBody.so.empty())
            return badRequest("body must have non-empty 'id' and 'so'");
        auto resultId = core.instantiateModule(reqBody.so, reqBody.id);
        if (resultId.empty()) return json(500, "{\"error\":\"instantiate failed\"}");
        return json(200, "{\"ok\":true,\"id\":\"" + resultId + "\"}");
    }

    // /api/modules/<id>[/...] — single-module GET/DELETE, and the /data, /config,
    // /recipe, /reload sub-routes (all keyed off the same modId/tail split).
    if (path.rfind("/api/modules/", 0) == 0) {
        std::string rest = path.substr(std::string_view("/api/modules/").size());
        auto slash = rest.find('/');
        std::string modId = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        std::string tail   = (slash == std::string::npos) ? std::string{} : rest.substr(slash + 1);

        if (!modId.empty() && tail.empty()) {
            if (method == Method::GET) {
                std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
                auto* mod = core.loader().get(modId);
                if (!mod) return json(404, "{\"error\":\"module not found\"}");
                return json(200, moduleInfoJson(*mod, core.scheduler()));
            }
            if (method == Method::DELETE_) {
                if (!core.removeInstance(modId)) return json(404, "{\"error\":\"instance not found\"}");
                return json(200, "{\"ok\":true}");
            }
        } else if (!modId.empty() && tail.rfind("data/", 0) == 0) {
            // GET/POST/PUT/PATCH /api/modules/<id>/data/<section>
            auto sec = opcrest::sectionFromName(tail.substr(5));
            if (sec) {
                if (method == Method::GET) {
                    std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
                    return json(200, core.dataEngine().readSection(modId, *sec));
                }
                if (method == Method::POST || method == Method::PUT) {
                    std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
                    if (!core.dataEngine().writeSection(modId, *sec, body)) return badRequest("write failed");
                    if (*sec == DataSection::Config)      core.dataStore().saveConfig(modId, core.dataEngine());
                    else if (*sec == DataSection::Recipe) core.dataStore().saveRecipe(modId, "default", core.dataEngine());
                    return json(200, "{\"ok\":true}");
                }
                if (method == Method::PATCH) {
                    PatchBody patch;
                    auto err = readJsonPermissive(patch, body);
                    if (err || patch.ptr.empty() || patch.value.str.empty())
                        return badRequest(R"(body must be {"ptr":"...","value":...})");
                    std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
                    if (!core.dataEngine().patchSection(modId, *sec, patch.ptr, patch.value.str))
                        return badRequest("patch failed");
                    return json(200, "{\"ok\":true}");
                }
            }
        } else if (!modId.empty() && tail == "config/save" && method == Method::POST) {
            std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
            bool ok = core.dataStore().saveConfig(modId, core.dataEngine());
            return json(ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"save failed\"}");
        } else if (!modId.empty() && tail == "config/load" && method == Method::POST) {
            std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
            core.dataStore().loadConfig(modId, core.dataEngine());
            return json(200, core.dataEngine().readSection(modId, DataSection::Config));
        } else if (!modId.empty() && tail.rfind("recipe/save/", 0) == 0 && method == Method::POST) {
            auto name = tail.substr(std::string_view("recipe/save/").size());
            std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
            bool ok = core.dataStore().saveRecipe(modId, name, core.dataEngine());
            return json(ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"save failed\"}");
        } else if (!modId.empty() && tail.rfind("recipe/load/", 0) == 0 && method == Method::POST) {
            auto name = tail.substr(std::string_view("recipe/load/").size());
            std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
            core.dataStore().loadRecipe(modId, name, core.dataEngine());
            return json(200, core.dataEngine().readSection(modId, DataSection::Recipe));
        } else if (!modId.empty() && tail == "reload" && method == Method::POST) {
            if (!core.reloadModule(modId)) return json(500, "{\"error\":\"reload failed\"}");
            return json(200, R"({"ok":true,"message":"module reloaded"})");
        }
    }

    // GET /api/scheduler/classes — class definitions + live stats
    if (method == Method::GET && path == "/api/scheduler/classes") {
        auto allStats = core.scheduler().allClassStats();
        auto defs     = core.scheduler().classConfigs();
        std::vector<ClassInfoDto> resp;
        resp.reserve(defs.size());
        for (const auto& def : defs) resp.push_back(makeClassInfoDto(def, allStats));
        return json(200, glz::write_json(resp).value_or("[]"));
    }

    // POST /api/scheduler/classes — create a new class definition
    if (method == Method::POST && path == "/api/scheduler/classes") {
        ClassDef def;
        if (auto err = readJsonPermissive(def, body); err) return badRequest("invalid JSON body");
        if (def.name.empty()) return badRequest("name is required");
        if (!core.scheduler().addClassDef(def)) return json(409, "{\"error\":\"class already exists\"}");
        core.saveSchedulerConfig();
        return json(201, "{\"ok\":true}");
    }

    // /api/scheduler/classes/<name>[/history] — update a class, or its history
    if (path.rfind("/api/scheduler/classes/", 0) == 0) {
        std::string rest = path.substr(std::string_view("/api/scheduler/classes/").size());
        auto slash = rest.find('/');
        std::string className = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        std::string tail       = (slash == std::string::npos) ? std::string{} : rest.substr(slash + 1);

        if (!className.empty() && tail.empty() && method == Method::PATCH) {
            auto defs = core.scheduler().classConfigs();
            ClassDef updated;
            bool found = false;
            for (auto& d : defs) { if (d.name == className) { updated = d; found = true; break; } }
            if (!found) return json(404, "{\"error\":\"class not found\"}");
            if (auto err = readJsonPermissive(updated, body); err) return badRequest("invalid JSON body");
            updated.name = className;  // never let the client rename a class via PATCH
            core.scheduler().updateClassDef(updated);
            core.saveSchedulerConfig();
            return json(200, "{\"ok\":true}");
        }
        if (!className.empty() && tail == "history" && method == Method::GET) {
            auto deque = core.scheduler().classHistory(className);
            std::vector<MetricSample> samples(deque.begin(), deque.end());
            int64_t since = 0, binMs = 0;
            if (auto p = queryParam(query, "since"); !p.empty()) try { since = std::stoll(p); } catch (...) {}
            if (auto p = queryParam(query, "bin");   !p.empty()) try { binMs = std::stoll(p); } catch (...) {}
            return json(200, buildHistoryBody(samples, since, binMs));
        }
    }

    // GET /api/scheduler/schema — JSON Schema for SchedulerConfig
    if (method == Method::GET && path == "/api/scheduler/schema") {
        return json(200, glz::write_json_schema<SchedulerConfig>().value_or("{}"));
    }

    // GET /api/scheduler/modules/<id>/history?since=&bin=
    if (method == Method::GET && path.rfind("/api/scheduler/modules/", 0) == 0
        && path.size() > std::string_view("/api/scheduler/modules/").size()) {
        std::string rest = path.substr(std::string_view("/api/scheduler/modules/").size());
        static constexpr std::string_view kSuffix = "/history";
        if (rest.size() > kSuffix.size() && rest.compare(rest.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
            std::string id = rest.substr(0, rest.size() - kSuffix.size());
            auto* ts = core.scheduler().taskState(id);
            if (!ts) return json(404, "{\"error\":\"module not found\"}");
            int64_t since = 0, binMs = 0;
            if (auto p = queryParam(query, "since"); !p.empty()) try { since = std::stoll(p); } catch (...) {}
            if (auto p = queryParam(query, "bin");   !p.empty()) try { binMs = std::stoll(p); } catch (...) {}
            std::vector<MetricSample> samples;
            { std::lock_guard lk(ts->cycleHistoryMx); samples = ts->cycleHistory.getAll(); }
            return json(200, buildHistoryBody(samples, since, binMs));
        }
    }

    // POST /api/scheduler/reassign — move a module to a different class
    // Body: {"moduleId":"...","class":"...","order":0 (optional)}
    if (method == Method::POST && path == "/api/scheduler/reassign") {
        // glaze can't bind to the C++ keyword "class" via the default member name,
        // so pull moduleId/class/order with the same dependency-light manual
        // extraction server.cpp used (this body is tiny and fixed-shape).
        auto extract = [&](const std::string& key) -> std::string {
            auto pos = body.find("\"" + key + "\"");
            if (pos == std::string::npos) return {};
            pos = body.find(':', pos);
            if (pos == std::string::npos) return {};
            ++pos;
            while (pos < body.size() && body[pos] == ' ') ++pos;
            if (pos >= body.size()) return {};
            if (body[pos] == '"') {
                ++pos;
                auto end = body.find('"', pos);
                return (end != std::string::npos) ? body.substr(pos, end - pos) : "";
            }
            auto end = body.find_first_of(",}", pos);
            return (end != std::string::npos) ? body.substr(pos, end - pos) : "";
        };
        std::string moduleId = extract("moduleId");
        std::string newClass = extract("class");
        auto orderStr = extract("order");
        if (moduleId.empty() || newClass.empty()) return badRequest("moduleId and class are required");
        std::optional<int> newOrder;
        if (!orderStr.empty()) { try { newOrder = std::stoi(orderStr); } catch (...) {} }
        auto ec = core.scheduler().reassignClass(moduleId, newClass, newOrder);
        if (ec) return json(400, "{\"error\":\"" + ec.message() + "\"}");
        core.saveSchedulerConfig();
        return json(200, "{\"ok\":true}");
    }

    // GET /api/faults — fault reports (newest first)
    if (method == Method::GET && path == "/api/faults") {
        std::string out = "[";
        bool first = true;
        for (const auto& s : core.faultStore().list()) {
            if (!first) out += ",";
            first = false;
            out += "{\"id\":\"" + jsonEscapeString(s.id) + "\""
                 + ",\"ts\":" + std::to_string(s.tsMs)
                 + ",\"kind\":\"" + jsonEscapeString(s.kind) + "\""
                 + ",\"module\":\"" + jsonEscapeString(s.moduleId) + "\""
                 + ",\"class\":\"" + jsonEscapeString(s.className) + "\""
                 + ",\"phase\":\"" + jsonEscapeString(s.phase) + "\""
                 + ",\"reason\":\"" + jsonEscapeString(s.reason) + "\"}";
        }
        out += "]";
        return json(200, std::move(out));
    }

    // GET /api/faults/<id> — single fault detail
    if (method == Method::GET && path.rfind("/api/faults/", 0) == 0) {
        std::string id = path.substr(std::string_view("/api/faults/").size());
        auto detail = core.faultStore().detailJson(id);
        return detail ? json(200, *detail) : json(404, "{\"error\":\"fault not found\"}");
    }

    // GET /api/bus/topics
    if (method == Method::GET && path == "/api/bus/topics") {
        auto topics = core.bus().topics();
        std::string out = "[";
        for (std::size_t i = 0; i < topics.size(); ++i) { if (i) out += ","; out += "\"" + topics[i] + "\""; }
        out += "]";
        return json(200, std::move(out));
    }

    // GET /api/bus/services
    if (method == Method::GET && path == "/api/bus/services") {
        auto infos = core.bus().serviceInfos();
        std::string out = "[";
        for (std::size_t i = 0; i < infos.size(); ++i) {
            if (i) out += ",";
            out += "{\"name\":\"" + infos[i].name + "\",\"schema\":";
            out += infos[i].schema.empty() ? "null" : infos[i].schema;
            out += "}";
        }
        out += "]";
        return json(200, std::move(out));
    }

    // POST /api/bus/call/<serviceName> — invoke a bus service (name may contain '/')
    if (method == Method::POST && path.rfind("/api/bus/call/", 0) == 0) {
        std::string serviceName = path.substr(std::string_view("/api/bus/call/").size());
        auto result = core.bus().call(serviceName, body);
        std::string out = "{\"ok\":" + std::string(result.ok ? "true" : "false");
        if (!result.response.empty()) out += ",\"response\":" + result.response;
        if (!result.error.empty())    out += ",\"error\":\"" + result.error + "\"";
        out += "}";
        return json(200, std::move(out));
    }

    // GET /api/scope/schema — each module's runtime section snapshot
    if (method == Method::GET && path == "/api/scope/schema") {
        std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
        std::string out = "{";
        bool first = true;
        for (const auto& entry : core.loader().modules()) {
            const auto& id = entry.first;
            auto runtime = core.dataEngine().readSection(id, DataSection::Runtime);
            if (runtime.empty()) runtime = "{}";
            if (!first) out += ",";
            out += "\"" + id + "\":" + runtime;
            first = false;
        }
        out += "}";
        return json(200, std::move(out));
    }

    // GET /api/scope/probes — configured probes
    if (method == Method::GET && path == "/api/scope/probes") {
        auto probes = core.oscilloscope().listProbes();
        std::string out = "[";
        bool first = true;
        for (auto& p : probes) {
            if (!first) out += ",";
            out += "{\"id\":" + std::to_string(p.id)
                 + ",\"moduleId\":\"" + p.moduleId + "\""
                 + ",\"path\":\"" + p.path + "\""
                 + ",\"label\":\"" + p.label + "\"}";
            first = false;
        }
        out += "]";
        return json(200, std::move(out));
    }

    // POST /api/scope/probes — add a probe. Body: {"moduleId":"...","path":"/field"}
    if (method == Method::POST && path == "/api/scope/probes") {
        AddProbeRequest reqBody;
        if (auto err = readJsonPermissive(reqBody, body); err) return badRequest("invalid JSON body");
        if (reqBody.moduleId.empty() || reqBody.path.empty()) return badRequest("moduleId and path required");
        auto id = core.oscilloscope().addProbe(reqBody.moduleId, reqBody.path);
        return json(200, "{\"ok\":true,\"id\":" + std::to_string(id) + "}");
    }

    // DELETE /api/scope/probes/<id>
    if (method == Method::DELETE_ && path.rfind("/api/scope/probes/", 0) == 0) {
        std::string idStr = path.substr(std::string_view("/api/scope/probes/").size());
        uint64_t probeId = 0;
        try { probeId = std::stoull(idStr); } catch (...) { return badRequest("invalid probe id"); }
        if (!core.oscilloscope().removeProbe(probeId)) return json(404, "{\"error\":\"probe not found\"}");
        return json(200, "{\"ok\":true}");
    }

    // GET /api/scope/data — all captured probe samples
    if (method == Method::GET && path == "/api/scope/data") {
        return json(200, core.oscilloscope().allDataToJson());
    }

    // GET /api/io-mappings — I/O mappings with resolution status
    if (method == Method::GET && path == "/api/io-mappings") {
        auto& mapper = core.ioMapper();
        std::string out = "[";
        bool first = true;
        for (std::size_t i = 0; i < mapper.entryCount(); ++i) {
            auto* entry = mapper.getMapping(i);
            if (!entry) continue;
            if (!first) out += ",";
            out += "{\"index\":" + std::to_string(i);
            out += ",\"source\":\"" + (i < mapper.getMappings().size() ? mapper.getMappings()[i].src_module_id : "") + "\"";
            out += ",\"target\":\"" + (i < mapper.getMappings().size() ? mapper.getMappings()[i].dst_module_id : "") + "\"";
            out += ",\"enabled\":" + std::string(entry->valid ? "true" : "false");
            out += ",\"resolved\":" + std::string(entry->valid ? "true" : "false");
            out += ",\"stable\":" + std::string(entry->stable ? "true" : "false");
            out += ",\"error\":\"" + entry->error + "\"}";
            first = false;
        }
        out += "]";
        return json(200, std::move(out));
    }

    // POST /api/io-mappings — add a mapping. Body: {"source":"...","target":"...","enabled":true}
    if (method == Method::POST && path == "/api/io-mappings") {
        IOMapEntry entry;
        if (auto err = readJsonPermissive(entry, body); err) return badRequest("invalid JSON body");
        if (entry.source.empty() || entry.target.empty()) return badRequest("source and target are required");
        auto index = core.ioMapper().addMapping(entry.source, entry.target, entry.enabled);
        core.ioMapper().resolveAll(core);
        return json(201, "{\"ok\":true,\"index\":" + std::to_string(index) + "}");
    }

    // POST /api/io-mappings/resolve — force re-resolve all mappings. MUST precede
    // the /api/io-mappings/<index> prefix check below (else "resolve" parses as an index).
    if (method == Method::POST && path == "/api/io-mappings/resolve") {
        core.ioMapper().resolveAll(core);
        return json(200, "{\"ok\":true}");
    }

    // DELETE/PATCH /api/io-mappings/<index> — remove or update a mapping
    if (path.rfind("/api/io-mappings/", 0) == 0
        && (method == Method::DELETE_ || method == Method::PATCH)) {
        std::string idxStr = path.substr(std::string_view("/api/io-mappings/").size());
        std::size_t index = 0;
        try { index = std::stoull(idxStr); } catch (...) { return badRequest("invalid index"); }

        if (method == Method::DELETE_) {
            if (!core.ioMapper().removeMapping(index)) return json(404, "{\"error\":\"mapping not found\"}");
            return json(200, "{\"ok\":true}");
        }
        // PATCH
        if (index >= core.ioMapper().entryCount()) return json(404, "{\"error\":\"mapping not found\"}");
        IOMapEntry entry;
        if (auto err = readJsonPermissive(entry, body); err) return badRequest("invalid JSON body");
        if (entry.source.empty() || entry.target.empty()) return badRequest("source and target are required");
        if (!core.ioMapper().updateMapping(index, entry.source, entry.target, entry.enabled))
            return json(500, "{\"error\":\"update failed\"}");
        core.ioMapper().resolveAll(core);
        return json(200, "{\"ok\":true}");
    }

    return notFound();
}

} // namespace api
} // namespace loom
