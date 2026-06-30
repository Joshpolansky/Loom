#pragma once

// Transport-agnostic dispatch over the Loom runtime's REST surface. The native
// Crow server (server.cpp) and the WASM host (run_wasm.cpp) both route /api/*
// requests through dispatch(), so there is a single source of truth for the API
// regardless of transport (HTTP socket vs. in-process JS call).

#include <string>
#include <string_view>

namespace loom {
class RuntimeCore;

namespace api {

// DELETE_ (not DELETE) because <windows.h>/winnt.h #defines DELETE as
// (0x00010000L) — MSVC then macro-expands Method::DELETE into a syntax error
// (C2589) regardless of namespacing, since the preprocessor runs first.
enum class Method { GET, POST, PUT, PATCH, DELETE_, UNKNOWN };

Method methodFromString(std::string_view s);

struct Request {
    Method      method = Method::GET;
    std::string path;   ///< full path, e.g. "/api/modules" or "/api/modules/foo"
    std::string body;   ///< raw request body (JSON), empty for GET
};

struct Response {
    int         status      = 200;
    std::string contentType = "application/json";
    std::string body;
};

/// Route a request to the matching handler. Returns 404 for unmatched paths.
Response dispatch(RuntimeCore& core, const Request& req);

} // namespace api
} // namespace loom
