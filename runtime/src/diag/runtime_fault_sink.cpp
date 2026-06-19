#include "loom/diag/runtime_fault_sink.h"

#include "loom/diag/fault_report.h"
#include "loom/bus.h"
#include "loom/data_engine.h"
#include "loom/types.h"
#include "loom/version.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <string>
#include <string_view>

#ifndef LOOM_BUILD_TYPE
#define LOOM_BUILD_TYPE "unknown"
#endif

namespace loom::diag {

RuntimeFaultSink::RuntimeFaultSink(FaultStore& store, DataEngine& engine, Bus& bus)
    : store_(store), engine_(engine), bus_(bus) {}

namespace {
std::string safeRead(DataEngine& engine, const std::string& id, DataSection sec) {
    try {
        return engine.readSection(id, sec);
    } catch (...) {
        return {};
    }
}

// The report id becomes a filename (<crashDir>/<id>.json), and the module
// portion ultimately comes from the HTTP instantiate request body — so a '/' or
// '..' could escape the crash directory. Map anything outside a safe set to '_'.
std::string sanitizeForFilename(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out += ok ? c : '_';
    }
    return out.empty() ? std::string{"module"} : out;
}
} // namespace

void RuntimeFaultSink::onModuleFault(const FaultEvent& ev) {
    try {
        const int64_t nowMs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        FaultReport r;
        r.id           = sanitizeForFilename(ev.moduleId) + "-" + std::to_string(nowMs) + "-" +
                         std::to_string(seq_.fetch_add(1, std::memory_order_relaxed));
        r.tsMs         = nowMs;
        r.kind         = FaultKind::Exception;
        r.signalOrCode = 0;
        r.reason       = ev.message;
        r.sdkVersion   = loom::kSdkVersion;
        r.gitSha       = loom::kGitSha;
        r.buildType    = LOOM_BUILD_TYPE;
        r.moduleId     = ev.moduleId;
        r.className    = ev.className;
        r.phase        = ev.phase;
        r.cycle        = ev.cycle;

        // Capture the module's live data sections — safe off the signal path.
        FaultSections sections;
        sections.config  = safeRead(engine_, ev.moduleId, DataSection::Config);
        sections.recipe  = safeRead(engine_, ev.moduleId, DataSection::Recipe);
        sections.runtime = safeRead(engine_, ev.moduleId, DataSection::Runtime);
        sections.summary = safeRead(engine_, ev.moduleId, DataSection::Summary);
        r.sections = std::move(sections);

        std::string json = toJson(r);
        store_.record(r);
        bus_.publish("loom/faults", json);
    } catch (const std::exception& e) {
        spdlog::error("RuntimeFaultSink failed to record fault: {}", e.what());
    } catch (...) {
        // never propagate out of the worker thread
    }
}

} // namespace loom::diag
