#include "loom/oscilloscope.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string_view>

namespace loom {

Oscilloscope::~Oscilloscope() {
    workerRunning_.store(false);
    queueCv_.notify_all();
    if (workerThread_.joinable()) workerThread_.join();
}


// ---------------------------------------------------------------------------
// Probe management
// ---------------------------------------------------------------------------

uint64_t Oscilloscope::addProbe(const std::string& moduleId, const std::string& path) {
    std::lock_guard lock(mutex_);
    uint64_t id = nextId_++;
    Probe p;
    p.id       = id;
    p.moduleId = moduleId;
    p.path     = path.starts_with('/') ? path : "/" + path;
    probes_.emplace(id, std::move(p));
    spdlog::info("Oscilloscope: added probe {} -> {}{}", id, moduleId, path);
    return id;
}

bool Oscilloscope::removeProbe(uint64_t id) {
    std::lock_guard lock(mutex_);
    auto it = probes_.find(id);
    if (it == probes_.end()) return false;
    probes_.erase(it);
    spdlog::info("Oscilloscope: removed probe {}", id);
    return true;
}

std::vector<ProbeInfo> Oscilloscope::listProbes() const {
    std::lock_guard lock(mutex_);
    std::vector<ProbeInfo> out;
    out.reserve(probes_.size());
    for (auto& [id, p] : probes_) {
        out.push_back({ p.id, p.moduleId, p.path,
                        p.moduleId + p.path });
    }
    return out;
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

void Oscilloscope::sample(const DataEngine& engine, const ModuleLoader& loader) {
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::lock_guard lock(mutex_);
    for (auto& [id, probe] : probes_) {
        // Verify module still exists and has a live instance.
        auto lm = loader.get(probe.moduleId);
        if (!lm || !lm->instance) continue;
        auto *mod = lm->instance.get();

        // Try fast resolution using full JSON-pointer path (without leading '/').
        std::string path = probe.path;
        if (!path.empty() && path[0] == '/') path.erase(0, 1);

        auto ptrOpt = mod->tracePtr(DataSection::Runtime, path);
        auto tnOpt  = mod->traceTypeName(DataSection::Runtime, path);

        bool pushed = false;
        if (ptrOpt && tnOpt) {
            void* p = *ptrOpt;
            const std::string tn = *tnOpt;
            double v = 0.0;
            if (tn == "double") { v = *reinterpret_cast<double*>(p); pushed = true; }
            else if (tn == "float") { v = *reinterpret_cast<float*>(p); pushed = true; }
            else if (tn == "long") { v = static_cast<double>(*reinterpret_cast<long*>(p)); pushed = true; }
            else if (tn == "unsigned long") { v = static_cast<double>(*reinterpret_cast<unsigned long*>(p)); pushed = true; }
            else if (tn == "int") { v = static_cast<double>(*reinterpret_cast<int*>(p)); pushed = true; }
            else if (tn == "unsigned int") { v = static_cast<double>(*reinterpret_cast<unsigned int*>(p)); pushed = true; }
            else if (tn == "short") { v = static_cast<double>(*reinterpret_cast<short*>(p)); pushed = true; }
            else if (tn == "unsigned short") { v = static_cast<double>(*reinterpret_cast<unsigned short*>(p)); pushed = true; }
            else if (tn == "char") { v = static_cast<double>(*reinterpret_cast<char*>(p)); pushed = true; }
            else if (tn == "unsigned char") { v = static_cast<double>(*reinterpret_cast<unsigned char*>(p)); pushed = true; }
            else if (tn == "bool") { v = *reinterpret_cast<bool*>(p) ? 1.0 : 0.0; pushed = true; }

            if (pushed) {
                probe.buffer.push_back({ nowMs, v });
                if (probe.buffer.size() > kDefaultMaxSamples) probe.buffer.pop_front();
                continue;
            }
        }

        // Fallback: read full runtime JSON and extract by path (original behaviour).
        auto json = engine.readSection(probe.moduleId, DataSection::Runtime);
        auto val  = extractByPath(json, probe.path);
        if (!val) continue;

        probe.buffer.push_back({ nowMs, *val });
        if (probe.buffer.size() > kDefaultMaxSamples) {
            probe.buffer.pop_front();
        }
    }
}

// enqueueSnapshot implemented inline in header.

// ---------------------------------------------------------------------------
// Data access
// ---------------------------------------------------------------------------

std::vector<Sample> Oscilloscope::getData(uint64_t probeId) const {
    std::lock_guard lock(mutex_);
    auto it = probes_.find(probeId);
    if (it == probes_.end()) return {};
    return { it->second.buffer.begin(), it->second.buffer.end() };
}

std::string Oscilloscope::allDataToJson() const {
    std::lock_guard lock(mutex_);
    std::string json = "{";
    bool firstProbe = true;
    for (auto& [id, probe] : probes_) {
        if (!firstProbe) json += ",";
        firstProbe = false;
        json += "\"" + std::to_string(id) + "\":[";
        bool firstSample = true;
        for (auto& s : probe.buffer) {
            if (!firstSample) json += ",";
            firstSample = false;
            json += "[" + std::to_string(s.timestampMs) + "," +
                    std::to_string(s.value) + "]";
        }
        json += "]";
    }
    json += "}";
    return json;
}

// ---------------------------------------------------------------------------
// Field enumeration helpers
// ---------------------------------------------------------------------------

// Lightweight JSON value extractor — navigates a flat or nested JSON object
// by a JSON-Pointer path (e.g. "/speed" or "/status/running").
//
// Handles: numbers (including integers), booleans (true/false).
// Returns nullopt for strings, arrays, or objects.
// extractByPath implementation moved inline into header to satisfy inlining
// for the snapshot worker.

// Recursively collect all numeric leaf paths from a JSON string.
// Only handles top-level and one level of nesting for simplicity.
void Oscilloscope::collectFields(const std::string& json,
                                 const std::string& prefix,
                                 const std::string& moduleId,
                                 const std::string& label_prefix,
                                 std::vector<ScopeField>& out) {
    std::string_view sv = json;

    auto skipWS = [](std::string_view& s) {
        while (!s.empty() && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r'))
            s.remove_prefix(1);
    };

    skipWS(sv);
    if (sv.empty() || sv[0] != '{') return;
    sv.remove_prefix(1); // skip '{'

    while (!sv.empty()) {
        skipWS(sv);
        if (sv.empty() || sv[0] == '}') break;
        if (sv[0] == ',') { sv.remove_prefix(1); continue; }

        // Parse key
        if (sv[0] != '"') break;
        sv.remove_prefix(1);
        auto keyEnd = sv.find('"');
        if (keyEnd == std::string_view::npos) break;
        std::string key(sv.substr(0, keyEnd));
        sv = sv.substr(keyEnd + 1);

        skipWS(sv);
        if (sv.empty() || sv[0] != ':') break;
        sv.remove_prefix(1);
        skipWS(sv);

        if (sv.empty()) break;

        std::string fieldPath = prefix + "/" + key;
        std::string fieldLabel = label_prefix + " / " + key;

        if (sv[0] == '{') {
            // Nested object — recurse one level by extracting the sub-object string.
            size_t depth = 0, i = 0;
            for (; i < sv.size(); ++i) {
                if (sv[i] == '{') ++depth;
                else if (sv[i] == '}') { --depth; if (depth == 0) { ++i; break; } }
            }
            std::string subJson(sv.substr(0, i));
            collectFields(subJson, fieldPath, moduleId, fieldLabel, out);
            sv = sv.substr(i);
        } else if (sv[0] == '[') {
            // Array — iterate elements and recurse into objects or capture numeric primitives.
            size_t pos = 0;
            if (sv[0] != '[') break;
            // find matching closing bracket for overall array slice
            size_t arrEnd = 0;
            int arrDepth = 0;
            for (size_t ii = 0; ii < sv.size(); ++ii) {
                if (sv[ii] == '[') ++arrDepth;
                else if (sv[ii] == ']') { --arrDepth; if (arrDepth == 0) { arrEnd = ii + 1; break; } }
            }
            if (arrEnd == 0) break;
            // process elements between [1 .. arrEnd-2]
            pos = 1;
            int idx = 0;
            while (pos < arrEnd) {
                // skip whitespace and commas
                while (pos < arrEnd && (sv[pos] == ' ' || sv[pos] == '\n' || sv[pos] == '\r' || sv[pos] == '\t' || sv[pos] == ',')) ++pos;
                if (pos >= arrEnd || sv[pos] == ']') break;
                if (sv[pos] == '{') {
                    // find end of object
                    size_t depth = 0, i = pos;
                    for (; i < arrEnd; ++i) {
                        if (sv[i] == '{') ++depth;
                        else if (sv[i] == '}') { --depth; if (depth == 0) { ++i; break; } }
                    }
                    std::string subJson(sv.substr(pos, i - pos));
                    std::string idxPath = fieldPath + "/" + std::to_string(idx);
                    std::string idxLabel = fieldLabel + " / " + std::to_string(idx);
                    collectFields(subJson, idxPath, moduleId, idxLabel, out);
                    pos = i;
                } else if (sv[pos] == '[') {
                    // nested array — find its end and recurse by treating index as element
                    size_t depth = 0, i = pos;
                    for (; i < arrEnd; ++i) {
                        if (sv[i] == '[') ++depth;
                        else if (sv[i] == ']') { --depth; if (depth == 0) { ++i; break; } }
                    }
                    std::string subArr(sv.substr(pos, i - pos));
                    std::string idxPath = fieldPath + "/" + std::to_string(idx);
                    std::string idxLabel = fieldLabel + " / " + std::to_string(idx);
                    // recurse into nested array by calling collectFields on an artificial object
                    collectFields(subArr, idxPath, moduleId, idxLabel, out);
                    pos = i;
                } else if (sv[pos] == '"') {
                    // string element — skip
                    ++pos; bool esc = false;
                    while (pos < arrEnd) {
                        char c = sv[pos++];
                        if (esc) { esc = false; continue; }
                        if (c == '\\') { esc = true; continue; }
                        if (c == '"') break;
                    }
                } else {
                    // primitive value — capture until comma or closing bracket
                    size_t i = pos;
                    int depth = 0; bool inStr = false;
                    for (; i < arrEnd; ++i) {
                        char c = sv[i];
                        if (inStr) { if (c == '\\') { ++i; continue; } if (c == '"') inStr = false; }
                        else {
                            if (c == '"') { inStr = true; }
                            else if (c == '{' || c == '[') ++depth;
                            else if (c == '}' || c == ']') { if (depth == 0) break; --depth; }
                            else if (c == ',') break;
                        }
                    }
                    std::string_view valSv = sv.substr(pos, i - pos);
                    // trim whitespace
                    size_t a = 0, b = valSv.size();
                    while (a < b && (valSv[a] == ' ' || valSv[a] == '\n' || valSv[a] == '\r' || valSv[a] == '\t')) ++a;
                    while (b > a && (valSv[b-1] == ' ' || valSv[b-1] == '\n' || valSv[b-1] == '\r' || valSv[b-1] == '\t')) --b;
                    bool isNumeric = false;
                    if (b > a) {
                        std::string_view tok = valSv.substr(a, b-a);
                        if (tok == "true" || tok == "false") isNumeric = true;
                        else {
                            char* end = nullptr;
                            std::string tmp(tok);
                            std::strtod(tmp.c_str(), &end);
                            isNumeric = (end != tmp.c_str() && *end == '\0');
                        }
                    }
                    if (isNumeric) {
                        std::string idxPath = fieldPath + "/" + std::to_string(idx);
                        out.push_back({ moduleId, idxPath, moduleId + idxPath });
                    }
                    pos = i;
                }
                // skip optional comma
                while (pos < arrEnd && (sv[pos] == ' ' || sv[pos] == '\n' || sv[pos] == '\r' || sv[pos] == '\t')) ++pos;
                if (pos < arrEnd && sv[pos] == ',') ++pos;
                ++idx;
            }
            // advance main cursor past the whole array
            sv = sv.substr(arrEnd);
        } else if (sv[0] == '"') {
            // String — skip
            sv.remove_prefix(1);
            bool escaped = false;
            while (!sv.empty()) {
                char c = sv[0]; sv.remove_prefix(1);
                if (escaped) { escaped = false; continue; }
                if (c == '\\') { escaped = true; continue; }
                if (c == '"') break;
            }
        } else {
            // Leaf value — try to parse as numeric
            auto start = sv;
            // Find end of value (comma, '}', ']', whitespace)
            size_t i = 0;
            while (i < sv.size() && sv[i] != ',' && sv[i] != '}' &&
                   sv[i] != ']' && sv[i] != ' ' && sv[i] != '\n') {
                ++i;
            }
            std::string_view valStr = sv.substr(0, i);
            sv = sv.substr(i);

            bool isNumeric = false;
            if (valStr == "true" || valStr == "false") {
                isNumeric = true;
            } else if (!valStr.empty()) {
                char* end = nullptr;
                std::strtod(valStr.data(), &end);
                isNumeric = (end != valStr.data() && end == valStr.data() + valStr.size());
            }

            if (isNumeric) {
                out.push_back({ moduleId, fieldPath, moduleId + fieldPath });
            }
        }
    }
}

std::vector<ScopeField> Oscilloscope::listFields(const DataEngine& engine,
                                                  const ModuleLoader& loader) {
    std::vector<ScopeField> fields;
    for (auto& [id, _] : loader.modules()) {
        auto json = engine.readSection(id, DataSection::Runtime);
        collectFields(json, "", id, id, fields);
    }
    return fields;
}

} // namespace loom
