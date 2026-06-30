#include "loom/api/router.h"
#include "loom/api/json_build.h"

#include "loom/runtime_core.h"
#include "loom/opcua_rest_nodeid.h"  // opcrest::sectionFromName
#include "loom/diag/breadcrumb.h"   // diag::phaseName

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

    return notFound();
}

} // namespace api
} // namespace loom
