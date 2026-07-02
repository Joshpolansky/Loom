#pragma once

#include "loom/diag/fault_report.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// ============================================================================
// loom::diag — fault store
//
// Thread-safe registry of fault reports for this run, backed by JSON files in
// <crashDir>. On construction it scans the directory so crashes from PRIOR runs
// (including the signal-path text reports the process couldn't keep in memory)
// are visible too. The server's /api/faults[/:id] routes delegate here.
// ============================================================================

namespace loom::diag {

class FaultStore {
public:
    /// One row in the fault list — enough to render the LoomUI tree without
    /// fetching every full report.
    struct Summary {
        std::string id;
        int64_t     tsMs = 0;
        std::string kind;       ///< "exception" | "signal" | "raw"
        std::string moduleId;   ///< "" → runtime code
        std::string className;
        std::string phase;
        std::string reason;
    };

    /// Retention cap: newest kMaxEntries faults are kept, in memory AND on
    /// disk (older report files are deleted as new ones arrive). Without a cap
    /// a fault-looping module grows the store without bound — each entry holds
    /// the full report JSON including stack traces — and the crash dir
    /// compounds across runs (scanDir() reloads every file at boot).
    static constexpr std::size_t kMaxEntries = 200;

    explicit FaultStore(std::filesystem::path crashDir);

    /// Persist a live fault (JSON file + in-memory summary). Returns its id.
    /// Safe to call from any worker thread; never throws.
    std::string record(const FaultReport& report) noexcept;

    /// All known faults, newest first.
    std::vector<Summary> list() const;

    /// Full report JSON for one id ("" stem), or nullopt if unknown.
    std::optional<std::string> detailJson(const std::string& id) const;

    const std::filesystem::path& crashDir() const { return crashDir_; }

private:
    struct Entry {
        Summary     summary;
        std::string rawJson;   ///< full report JSON served by detailJson()
    };

    /// Load existing reports from disk (called once at construction).
    void scanDir();

    /// Drop oldest entries (and their disk files) beyond kMaxEntries. Caller
    /// holds mx_ (or is the single-threaded constructor).
    void enforceCap();

    std::filesystem::path crashDir_;
    mutable std::mutex    mx_;
    std::vector<Entry>    entries_;   ///< newest at the back; capped at kMaxEntries
};

} // namespace loom::diag
