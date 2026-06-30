#pragma once

// Shared, transport-agnostic JSON builders for the runtime's REST surface.
// Crow-free, so both the native HTTP server (server.cpp) and the WASM api
// router (api/router.cpp) build identical response bodies — one source of truth.

#include "loom/module_loader.h"
#include "loom/scheduler.h"

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace loom {

/// JSON-escape a string value.
std::string jsonEscapeString(std::string s);

/// Serialize metric history to a JSON array. `maxSamples` caps the trailing
/// samples emitted (0 = unlimited).
std::string serializeCycleHistory(const std::vector<MetricSample>& samples, std::size_t maxSamples = 0);
std::string serializeCycleHistory(const std::deque<MetricSample>& samples, std::size_t maxSamples = 0);

/// Per-module info object (id, name, class, state, path, stats + cycle history).
std::string moduleInfoJson(const LoadedModule& mod, const Scheduler& scheduler);

} // namespace loom
