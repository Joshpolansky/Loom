#include "loom/api/router.h"
#include "loom/api/json_build.h"

#include "loom/runtime_core.h"
#include "loom/opcua_rest_nodeid.h"  // opcrest::sectionFromName
#include "loom/diag/breadcrumb.h"   // diag::phaseName

#include <glaze/glaze.hpp>

#include <deque>
#include <shared_mutex>
#include <string>
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

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

namespace api {

Method methodFromString(std::string_view s) {
    if (s == "GET")    return Method::GET;
    if (s == "POST")   return Method::POST;
    if (s == "PUT")    return Method::PUT;
    if (s == "PATCH")  return Method::PATCH;
    if (s == "DELETE") return Method::DELETE;
    return Method::UNKNOWN;
}

// Scheduler DTOs (glaze auto-reflects these aggregates). In a NAMED namespace
// because glaze reflection requires the reflected type to have linkage. NOTE:
// duplicated with server.cpp for now — the right consolidation is to make
// server.cpp delegate wholesale to dispatch() and host the one copy here.
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

namespace {
Response json(int status, std::string body) { return {status, "application/json", std::move(body)}; }
Response notFound() { return json(404, "{\"error\":\"no matching route\"}"); }
}

Response dispatch(RuntimeCore& core, const Request& req) {
    // GET /api/modules — list all loaded modules
    if (req.method == Method::GET && req.path == "/api/modules") {
        std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
        std::string body = "[";
        bool first = true;
        for (auto& [id, mod] : core.loader().modules()) {
            if (!first) body += ",";
            body += moduleInfoJson(mod, core.scheduler());
            first = false;
        }
        body += "]";
        return json(200, std::move(body));
    }

    // GET /api/modules/<id> — one module
    if (req.method == Method::GET && req.path.rfind("/api/modules/", 0) == 0) {
        std::string id = req.path.substr(std::string_view("/api/modules/").size());
        if (!id.empty() && id.find('/') == std::string::npos) {
            std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
            auto* mod = core.loader().get(id);
            if (!mod) return json(404, "{\"error\":\"module not found\"}");
            return json(200, moduleInfoJson(*mod, core.scheduler()));
        }

        // GET /api/modules/<id>/data/<section> — reflected section JSON
        auto slash = id.find('/');
        if (slash != std::string::npos) {
            std::string modId = id.substr(0, slash);
            std::string tail  = id.substr(slash + 1);  // "data/<section>"
            if (!modId.empty() && tail.rfind("data/", 0) == 0) {
                if (auto sec = opcrest::sectionFromName(tail.substr(5))) {
                    std::shared_lock<std::shared_mutex> lock(core.moduleMutex());
                    return json(200, core.dataEngine().readSection(modId, *sec));
                }
            }
        }
    }

    // GET /api/scheduler/classes — class definitions + live stats
    if (req.method == Method::GET && req.path == "/api/scheduler/classes") {
        auto allStats = core.scheduler().allClassStats();
        auto defs     = core.scheduler().classConfigs();
        std::vector<ClassInfoDto> resp;
        resp.reserve(defs.size());
        for (const auto& def : defs) resp.push_back(makeClassInfoDto(def, allStats));
        return json(200, glz::write_json(resp).value_or("[]"));
    }

    // GET /api/faults — fault reports (newest first)
    if (req.method == Method::GET && req.path == "/api/faults") {
        std::string body = "[";
        bool first = true;
        for (const auto& s : core.faultStore().list()) {
            if (!first) body += ",";
            first = false;
            body += "{\"id\":\"" + jsonEscapeString(s.id) + "\""
                  + ",\"ts\":" + std::to_string(s.tsMs)
                  + ",\"kind\":\"" + jsonEscapeString(s.kind) + "\""
                  + ",\"module\":\"" + jsonEscapeString(s.moduleId) + "\""
                  + ",\"class\":\"" + jsonEscapeString(s.className) + "\""
                  + ",\"phase\":\"" + jsonEscapeString(s.phase) + "\""
                  + ",\"reason\":\"" + jsonEscapeString(s.reason) + "\"}";
        }
        body += "]";
        return json(200, std::move(body));
    }

    // GET /api/bus/topics
    if (req.method == Method::GET && req.path == "/api/bus/topics") {
        auto topics = core.bus().topics();
        std::string body = "[";
        for (std::size_t i = 0; i < topics.size(); ++i) { if (i) body += ","; body += "\"" + topics[i] + "\""; }
        body += "]";
        return json(200, std::move(body));
    }

    // GET /api/bus/services
    if (req.method == Method::GET && req.path == "/api/bus/services") {
        auto infos = core.bus().serviceInfos();
        std::string body = "[";
        for (std::size_t i = 0; i < infos.size(); ++i) {
            if (i) body += ",";
            body += "{\"name\":\"" + infos[i].name + "\",\"schema\":";
            body += infos[i].schema.empty() ? "null" : infos[i].schema;
            body += "}";
        }
        body += "]";
        return json(200, std::move(body));
    }

    return notFound();
}

} // namespace api
} // namespace loom
