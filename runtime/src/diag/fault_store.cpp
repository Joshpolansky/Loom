#include "loom/diag/fault_store.h"

#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace loom::diag {

namespace {

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string jstr(glz::json_t& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return {};
    auto& v = j[key];
    return v.is_string() ? v.get<std::string>() : std::string{};
}

int64_t jnum(glz::json_t& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return 0;
    auto& v = j[key];
    return v.is_number() ? static_cast<int64_t>(v.get<double>()) : 0;
}

// Build a Summary by parsing a report JSON blob. Falls back to a minimal
// summary (id only) if the blob isn't our JSON shape.
FaultStore::Summary summarize(const std::string& id, const std::string& json) {
    FaultStore::Summary s;
    s.id = id;
    glz::json_t doc;
    if (json.empty() || glz::read_json(doc, json) || !doc.is_object()) {
        s.kind = "raw";
        return s;
    }
    s.tsMs   = jnum(doc, "ts");
    s.kind   = jstr(doc, "kind");
    s.reason = jstr(doc, "reason");
    if (doc.contains("breadcrumb") && doc["breadcrumb"].is_object()) {
        auto& bc    = doc["breadcrumb"];
        s.moduleId  = jstr(bc, "module");
        s.className = jstr(bc, "class");
        s.phase     = jstr(bc, "phase");
    }
    return s;
}

} // namespace

FaultStore::FaultStore(std::filesystem::path crashDir)
    : crashDir_(std::move(crashDir)) {
    std::error_code ec;
    std::filesystem::create_directories(crashDir_, ec);
    scanDir();
}

void FaultStore::scanDir() {
    std::error_code ec;
    if (!std::filesystem::exists(crashDir_, ec)) return;

    for (const auto& de : std::filesystem::directory_iterator(crashDir_, ec)) {
        if (ec || !de.is_regular_file()) continue;
        const auto& path = de.path();
        const std::string ext  = path.extension().string();
        const std::string stem = path.stem().string();

        if (ext == ".json") {
            std::string json = readFile(path);
            entries_.push_back({summarize(stem, json), std::move(json)});
        } else if (ext == ".txt" && stem.rfind("loom-crash-", 0) == 0) {
            // POSIX signal-path report (raw addresses) — wrap the text so the
            // viewer can show it; symbolize offline via `loom --symbolize`.
            std::string raw = readFile(path);
            std::string wrapped = glz::write_json(
                std::map<std::string, std::string>{{"id", stem},
                                                   {"kind", "raw"},
                                                   {"raw", raw}}).value_or("{}");
            Summary s;
            s.id = stem; s.kind = "raw"; s.reason = "raw report — symbolize offline";
            entries_.push_back({std::move(s), std::move(wrapped)});
        }
    }
    // Keep the invariant "newest at the back" (matches record()'s push_back), so
    // list()'s reverse walk yields newest-first. Raw .txt reports have ts 0.
    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& a, const Entry& b) { return a.summary.tsMs < b.summary.tsMs; });
}

std::string FaultStore::record(const FaultReport& report) noexcept {
    try {
        std::string json = toJson(report);
        {
            std::lock_guard lock(mx_);
            const auto path = crashDir_ / (report.id + ".json");
            bool persisted = false;
            {
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                if (f) { f << json; persisted = static_cast<bool>(f); }
            }
            // Keep the in-memory entry regardless (a fault that happened must stay
            // visible in /api/faults for this run), but surface a persistence
            // failure rather than silently implying the report was saved to disk.
            if (!persisted)
                spdlog::warn("FaultStore: fault '{}' kept in memory only — failed to write {}",
                             report.id, path.string());
            entries_.push_back({summarize(report.id, json), std::move(json)});
        }
        return report.id;
    } catch (const std::exception& e) {
        spdlog::error("FaultStore::record failed: {}", e.what());
        return {};
    } catch (...) {
        return {};
    }
}

std::vector<FaultStore::Summary> FaultStore::list() const {
    std::lock_guard lock(mx_);
    std::vector<Summary> out;
    out.reserve(entries_.size());
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        out.push_back(it->summary);
    return out;
}

std::optional<std::string> FaultStore::detailJson(const std::string& id) const {
    std::lock_guard lock(mx_);
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if (it->summary.id == id) return it->rawJson;
    return std::nullopt;
}

} // namespace loom::diag
