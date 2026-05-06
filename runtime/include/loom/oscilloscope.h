#pragma once

#include "loom/data_engine.h"
#include "loom/module_loader.h"

#include <deque>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <tuple>
#include <string>
#include <unordered_map>
#include <vector>

namespace loom {

/// A browsable field available for oscilloscope tracing.
struct ScopeField {
    std::string moduleId;
    std::string path;    ///< JSON Pointer path, e.g. "/current_speed"
    std::string label;   ///< Human-readable label, e.g. "example_motor / current_speed"
};


/// A single sampled data point.
struct Sample {
    int64_t timestampMs;  ///< Milliseconds since epoch
    double  value;
};

/// Metadata for an active probe (without the buffer).
struct ProbeInfo {
    uint64_t    id;
    std::string moduleId;
    std::string path;
    std::string label;
};

/// Manages oscilloscope probes: field selection, ring-buffer sampling, and data access.
///
/// Sampling is driven externally (e.g. from the WS push thread) by calling sample().
/// Field enumeration uses the DataEngine to read the current runtime JSON and extract
/// all top-level and nested numeric leaf values.
class Oscilloscope {
public:
    static constexpr size_t kDefaultMaxSamples = 5000;

    Oscilloscope() = default;
    ~Oscilloscope();

    // --- Probe management ---

    /// Add a probe for (moduleId, path). Returns the new probe ID. Thread-safe.
    uint64_t addProbe(const std::string& moduleId, const std::string& path);

    /// Remove a probe by ID. Returns false if not found. Thread-safe.
    bool removeProbe(uint64_t id);

    /// List all active probe metadata. Thread-safe.
    std::vector<ProbeInfo> listProbes() const;

    // --- Sampling ---

    /// Sample all active probes. Call periodically from the WS push thread.
    /// Uses the DataEngine to read current runtime values via JSON.
    /// @param engine  DataEngine used to read runtime sections.
    /// @param loader  ModuleLoader used to verify module existence.
    void sample(const DataEngine& engine, const ModuleLoader& loader);

    /// Enqueue a runtime JSON snapshot for asynchronous processing by the
    /// oscilloscope worker. This is intended to be fast from the caller's
    /// perspective (push + notify) so callers can sample from a hot loop.
    void enqueueSnapshot(std::string moduleId, std::string json, int64_t timestampMs);

    /// Sample probes for a single module ID. `timestampMs` is used for all
    /// appended samples. This method will attempt fast pointer-based reads and
    /// only use `engine` to read JSON for probes that require fallback.
    void sampleModule(const std::string& moduleId, const DataEngine& engine, const ModuleLoader& loader, int64_t timestampMs);

    /// Overload that takes the live IModule directly — avoids the ModuleLoader
    /// lookup and does not require moduleMutex_ to be held. Safe to call from
    /// classLoop / isolatedLoop where the module's lifetime is guaranteed by
    /// the class pause protocol.
    void sampleModule(const std::string& moduleId, const DataEngine& engine, IModule& mod, int64_t timestampMs);

    // --- Data access ---

    /// Get all buffered samples for a probe. Returns empty vector if not found.
    std::vector<Sample> getData(uint64_t probeId) const;

    /// Serialize all probe data to a JSON object: {"probeId": [[t,v],...], ...}
    std::string allDataToJson() const;

    // --- Field enumeration ---

    /// Enumerate all numeric leaf fields across all loaded modules' runtime sections.
    /// Traverses the runtime JSON and returns one ScopeField per numeric leaf.
    static std::vector<ScopeField> listFields(const DataEngine& engine,
                                              const ModuleLoader& loader);

private:
    void processSnapshots();
    struct Probe {
        uint64_t    id;
        std::string moduleId;
        std::string path;
        std::deque<Sample> buffer;
    };

    /// Extract a numeric value from a parsed runtime JSON by path segments.
    /// Returns nullopt if the path doesn't exist or the leaf isn't numeric.
    static std::optional<double> extractByPath(const std::string& json,
                                               const std::string& path);

    /// Recursively collect all numeric leaf paths from a JSON object string.
    /// Produces paths like "/speed", "/status/running" (using JSON Pointer syntax).
    static void collectFields(const std::string& json,
                              const std::string& prefix,
                              const std::string& moduleId,
                              const std::string& label_prefix,
                              std::vector<ScopeField>& out);

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Probe> probes_;
    uint64_t nextId_ = 1;

    // Background processing of runtime snapshots pushed from the scheduler.
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::tuple<std::string, std::string, int64_t>> snapshotQueue_; // (moduleId, json, ts)
    std::atomic<bool> workerRunning_{false};
    std::thread workerThread_;
};

// Inline implementations for lightweight snapshot worker to avoid link-order
// issues when scheduler code is compiled into different targets (tests).

inline void Oscilloscope::processSnapshots() {
    while (workerRunning_.load()) {
        std::unique_lock<std::mutex> qlk(queueMutex_);
        if (snapshotQueue_.empty()) {
            queueCv_.wait_for(qlk, std::chrono::milliseconds(100));
        }
        std::deque<std::tuple<std::string, std::string, int64_t>> items;
        items.swap(snapshotQueue_);
        qlk.unlock();

        for (auto& it : items) {
            const std::string& moduleId = std::get<0>(it);
            const std::string& json = std::get<1>(it);
            int64_t ts = std::get<2>(it);

            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, probe] : probes_) {
                if (probe.moduleId != moduleId) continue;
                auto val = Oscilloscope::extractByPath(json, probe.path);
                if (!val) continue;
                probe.buffer.push_back({ ts, *val });
                if (probe.buffer.size() > Oscilloscope::kDefaultMaxSamples) {
                    probe.buffer.pop_front();
                }
            }
        }
    }
}

inline void Oscilloscope::enqueueSnapshot(std::string moduleId, std::string json, int64_t timestampMs) {
    {
        std::lock_guard<std::mutex> qlk(queueMutex_);
        snapshotQueue_.emplace_back(std::move(moduleId), std::move(json), timestampMs);
        if (!workerRunning_.load()) {
            workerRunning_.store(true);
            workerThread_ = std::thread(&Oscilloscope::processSnapshots, this);
        }
    }
    queueCv_.notify_one();
}

inline void Oscilloscope::sampleModule(const std::string& moduleId, const DataEngine& engine, const ModuleLoader& loader, int64_t timestampMs) {
    auto lm = loader.get(moduleId);
    if (!lm || !lm->instance) return;
    sampleModule(moduleId, engine, *lm->instance, timestampMs);
}

inline void Oscilloscope::sampleModule(const std::string& moduleId, const DataEngine& engine, IModule& mod, int64_t timestampMs) {
    std::lock_guard lock(mutex_);

    // Collect probes for this module
    std::vector<Probe*> targets;
    targets.reserve(8);
    for (auto& [id, probe] : probes_) {
        if (probe.moduleId == moduleId) targets.push_back(&probe);
    }
    if (targets.empty()) return;

    // Lazily fetch JSON only when a probe needs fallback
    bool needJson = false;
    std::string json;

    for (auto* probe : targets) {
        std::string path = probe->path;
        if (!path.empty() && path[0] == '/') path.erase(0, 1);

        auto ptrOpt = mod.tracePtr(DataSection::Runtime, path);
        auto tnOpt  = mod.traceTypeName(DataSection::Runtime, path);

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
                probe->buffer.push_back({ timestampMs, v });
                if (probe->buffer.size() > kDefaultMaxSamples) probe->buffer.pop_front();
                continue;
            }
        }

        needJson = true;
    }

    if (needJson) {
        json = engine.readSection(moduleId, DataSection::Runtime);
        for (auto* probe : targets) {
            if (!probe->buffer.empty() && probe->buffer.back().timestampMs == timestampMs) continue;
            auto val = extractByPath(json, probe->path);
            if (!val) continue;
            probe->buffer.push_back({ timestampMs, *val });
            if (probe->buffer.size() > kDefaultMaxSamples) probe->buffer.pop_front();
        }
    }
}

inline std::optional<double> Oscilloscope::extractByPath(const std::string& json,
                                                       const std::string& path) {
    std::string_view sv = json;
    std::string_view remaining = path;

    if (remaining.empty() || remaining[0] != '/') return std::nullopt;
    remaining.remove_prefix(1);

    auto skipWhitespace = [](std::string_view& s) {
        while (!s.empty() && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r'))
            s.remove_prefix(1);
    };

    auto parseSegment = [&](std::string_view& rem) -> std::string_view {
        auto slash = rem.find('/');
        if (slash == std::string_view::npos) {
            auto seg = rem;
            rem = {};
            return seg;
        }
        auto seg = rem.substr(0, slash);
        rem = rem.substr(slash + 1);
        return seg;
    };

    std::string_view cursor = sv;
    while (!remaining.empty()) {
        auto seg = parseSegment(remaining);
        skipWhitespace(cursor);
        if (cursor.empty()) return std::nullopt;

        if (cursor[0] == '{') {
            // object — find the member with name == seg
            cursor.remove_prefix(1);
            bool found = false;
            while (!cursor.empty()) {
                skipWhitespace(cursor);
                if (cursor.empty() || cursor[0] == '}') break;
                if (cursor[0] == ',') { cursor.remove_prefix(1); continue; }
                if (cursor[0] != '"') return std::nullopt;
                cursor.remove_prefix(1);
                auto keyEnd = cursor.find('"');
                if (keyEnd == std::string_view::npos) return std::nullopt;
                auto key = cursor.substr(0, keyEnd);
                cursor = cursor.substr(keyEnd + 1);
                skipWhitespace(cursor);
                if (cursor.empty() || cursor[0] != ':') return std::nullopt;
                cursor.remove_prefix(1);
                skipWhitespace(cursor);
                if (key == seg) { found = true; break; }

                // skip this value
                int depth = 0; bool inStr = false; size_t i = 0;
                for (; i < cursor.size(); ++i) {
                    char c = cursor[i];
                    if (inStr) { if (c == '\\') { ++i; continue; } if (c == '"') inStr = false; }
                    else {
                        if (c == '"') inStr = true;
                        else if (c == '{' || c == '[') ++depth;
                        else if (c == '}' || c == ']') { if (depth == 0) { break; } --depth; }
                        else if ((c == ',' || c == '}') && depth == 0) break;
                    }
                }
                cursor = cursor.substr(i);
            }
            if (!found) return std::nullopt;
        } else if (cursor[0] == '[') {
            // array — seg must be integer index
            long idx = -1;
            try { idx = std::stol(std::string(seg)); } catch (...) { return std::nullopt; }

            // find matching closing bracket for this array slice
            size_t arrEnd = 0; int depth = 0;
            for (size_t ii = 0; ii < cursor.size(); ++ii) {
                if (cursor[ii] == '[') ++depth;
                else if (cursor[ii] == ']') { --depth; if (depth == 0) { arrEnd = ii + 1; break; } }
            }
            if (arrEnd == 0) return std::nullopt;

            size_t pos = 1; int curIdx = 0; bool foundElem = false;
            while (pos < arrEnd) {
                while (pos < arrEnd && (cursor[pos] == ' ' || cursor[pos] == '\n' || cursor[pos] == '\r' || cursor[pos] == '\t' || cursor[pos] == ',')) ++pos;
                if (pos >= arrEnd || cursor[pos] == ']') break;
                size_t start = pos;
                if (cursor[pos] == '{') {
                    int d = 0; size_t i = pos;
                    for (; i < arrEnd; ++i) {
                        if (cursor[i] == '{') ++d;
                        else if (cursor[i] == '}') { --d; if (d == 0) { ++i; break; } }
                    }
                    size_t end = i;
                    if (curIdx == idx) { cursor = cursor.substr(start, end - start); foundElem = true; break; }
                    pos = end;
                } else if (cursor[pos] == '[') {
                    int d = 0; size_t i = pos;
                    for (; i < arrEnd; ++i) {
                        if (cursor[i] == '[') ++d;
                        else if (cursor[i] == ']') { --d; if (d == 0) { ++i; break; } }
                    }
                    size_t end = i;
                    if (curIdx == idx) { cursor = cursor.substr(start, end - start); foundElem = true; break; }
                    pos = end;
                } else if (cursor[pos] == '"') {
                    ++pos; bool esc = false;
                    while (pos < arrEnd) { char c = cursor[pos++]; if (esc) { esc = false; continue; } if (c == '\\') { esc = true; continue; } if (c == '"') break; }
                    size_t end = pos;
                    if (curIdx == idx) { cursor = cursor.substr(start, end - start); foundElem = true; break; }
                } else {
                    size_t i = pos; int d = 0; bool inStr = false;
                    for (; i < arrEnd; ++i) {
                        char c = cursor[i];
                        if (inStr) { if (c == '\\') { ++i; continue; } if (c == '"') inStr = false; }
                        else {
                            if (c == '"') inStr = true;
                            else if (c == '{' || c == '[') ++d;
                            else if (c == '}' || c == ']') { if (d == 0) break; --d; }
                            else if (c == ',') break;
                        }
                    }
                    size_t end = i;
                    if (curIdx == idx) { cursor = cursor.substr(start, end - start); foundElem = true; break; }
                    pos = end;
                }
                while (pos < arrEnd && (cursor[pos] == ' ' || cursor[pos] == '\n' || cursor[pos] == '\r' || cursor[pos] == '\t')) ++pos;
                if (pos < arrEnd && cursor[pos] == ',') ++pos;
                ++curIdx;
            }
            if (!foundElem) return std::nullopt;
        } else {
            return std::nullopt;
        }
    }

    std::string_view v = cursor;
    while (!v.empty() && (v[0] == ' ' || v[0] == '\t' || v[0] == '\n' || v[0] == '\r')) v.remove_prefix(1);
    if (v.empty()) return std::nullopt;

    if (v[0] == 't' && v.size() >= 4 && v.substr(0,4) == "true") return 1.0;
    if (v[0] == 'f' && v.size() >= 5 && v.substr(0,5) == "false") return 0.0;

    if (v[0] == '"' || v[0] == '{' || v[0] == '[') return std::nullopt;

    size_t i = 0; int depth = 0; bool inStr = false;
    for (; i < v.size(); ++i) {
        char c = v[i];
        if (inStr) { if (c == '\\') { ++i; continue; } if (c == '"') { inStr = false; } }
        else {
            if (c == '"') { inStr = true; }
            else if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (depth == 0) break; --depth; }
            else if ((c == ',') && depth == 0) break;
        }
    }

    std::string_view token = v.substr(0, i);
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t' || token.back() == '\n' || token.back() == '\r')) token.remove_suffix(1);
    if (token.empty()) return std::nullopt;

    char* end = nullptr;
    double result = std::strtod(token.data(), &end);
    if (end != token.data()) return result;
    return std::nullopt;
}

} // namespace loom
