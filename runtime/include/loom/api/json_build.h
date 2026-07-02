#pragma once

// Shared, transport-agnostic JSON builders for the runtime's REST surface.
// Crow-free, so both the native HTTP server (server.cpp) and the WASM api
// router (api/router.cpp) build identical response bodies — one source of truth.

#include "loom/module_loader.h"
#include "loom/scheduler.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace loom {

/// JSON-escape a string value.
std::string jsonEscapeString(std::string s);

/// Serialize metric history to a JSON array. `maxSamples` caps the trailing
/// samples emitted (0 = unlimited).
std::string serializeCycleHistory(const std::vector<MetricSample>& samples, std::size_t maxSamples = 0);
std::string serializeCycleHistory(const std::deque<MetricSample>& samples, std::size_t maxSamples = 0);

/// Per-module info object (id, name, class, state, path, stats + cycle history).
/// `extraFields`, if non-empty, is spliced into the object as additional
/// members (raw JSON, without enclosing braces or a leading comma) — e.g.
/// `"data":{...}` to build the ModuleDetail shape. Kept inside the builder so
/// callers never have to do brace surgery on its output.
std::string moduleInfoJson(const LoadedModule& mod, const Scheduler& scheduler,
                           std::string_view extraFields = {});

/// Cycle/jitter history as `{"samples":[...], "latest":<ms>}`. `since` filters to
/// samples newer than the given timestamp; `binMs` > 0 aggregates into fixed-width
/// bins (max cycle, max |jitter| per bin) instead of emitting raw samples.
std::string buildHistoryBody(const std::vector<MetricSample>& samples, int64_t since, int64_t binMs);

} // namespace loom
