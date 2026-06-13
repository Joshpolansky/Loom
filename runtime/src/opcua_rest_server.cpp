#include "loom/opcua_rest_server.h"

#include "loom/runtime_core.h"

#include <crow.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace loom {

// Request DTOs — glaze reflects these, so they need EXTERNAL linkage (MSVC's
// compile-time reflection rejects anonymous-namespace types). Keep them in a
// named namespace; all other helpers stay in the anonymous namespace below.
namespace dto {
struct SessionBody   { double timeout = 30000.0; };
struct SubBody       { double publishingInterval = 1000.0; };
struct PutBody       { glz::raw_json value; };
struct ItemToMonitor { std::string nodeId; std::string attribute = "Value"; };
struct MonParams     { int64_t clientHandle = 0; double samplingInterval = 0.0; int queueSize = 1; };
struct MonItemBody   { ItemToMonitor itemToMonitor; MonParams monitoringParameters; };
struct BatchReq      { glz::raw_json id; std::string method; std::string url; glz::raw_json body; };
struct BatchReqs     { std::vector<BatchReq> requests; };
} // namespace dto

namespace {

using opcrest::ParsedNode;
using opcrest::parseNodeId;
using opcrest::makeNodeId;
using opcrest::urlDecode;
using opcrest::sectionName;

constexpr uint32_t kGood             = 0u;
constexpr uint32_t kBadNodeIdUnknown = 0x80340000u; // OPC-UA Bad_NodeIdUnknown

const char* statusString(uint32_t code) {
    if (code == kGood)             return "Good";
    if (code == kBadNodeIdUnknown) return "BadNodeIdUnknown";
    return "Bad";
}

// ISO-8601 UTC timestamp with millisecond precision.
std::string isoNow() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

// JSON-escape a string for embedding inside a JSON string literal.
std::string esc(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

crow::response jsonResp(int code, std::string body) {
    crow::response r(code, std::move(body));
    r.add_header("Content-Type", "application/json");
    r.add_header("Access-Control-Allow-Origin", "*");
    r.add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS");
    r.add_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    return r;
}

bool isOptions(const crow::request& req) { return req.method == "OPTIONS"_method; }

bool parseU64(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    try { out = std::stoull(s); return true; } catch (...) { return false; }
}

// --- value access (caller holds shared_lock on core.moduleMutex()) ---

uint32_t readValueLocked(RuntimeCore& core, const ParsedNode& p, std::string& outValue) {
    outValue = "null";
    if (p.kind == ParsedNode::Kind::Field) {
        auto* lm = core.loader().get(p.moduleId);
        if (!lm || !lm->instance) return kBadNodeIdUnknown;
        auto v = lm->instance->readField(p.section, p.fieldPointer);
        if (!v) return kBadNodeIdUnknown;
        outValue = *v;
        return kGood;
    }
    if (p.kind == ParsedNode::Kind::Section) {
        auto* lm = core.loader().get(p.moduleId);
        if (!lm || !lm->instance) return kBadNodeIdUnknown;
        outValue = lm->instance->readSection(p.section);
        return kGood;
    }
    return kBadNodeIdUnknown;
}

uint32_t writeValueLocked(RuntimeCore& core, const ParsedNode& p, std::string_view valueJson) {
    if (p.kind == ParsedNode::Kind::Field) {
        auto* lm = core.loader().get(p.moduleId);
        if (!lm || !lm->instance) return kBadNodeIdUnknown;
        return lm->instance->writeField(p.section, p.fieldPointer, valueJson) ? kGood : kBadNodeIdUnknown;
    }
    if (p.kind == ParsedNode::Kind::Section) {
        auto* lm = core.loader().get(p.moduleId);
        if (!lm || !lm->instance) return kBadNodeIdUnknown;
        return lm->instance->writeSection(p.section, valueJson) ? kGood : kBadNodeIdUnknown;
    }
    return kBadNodeIdUnknown;
}

// Build the JSON body for a read of {nodeId, attribute}. `nodeDecoded` is the
// already-percent-decoded NodeId string.
std::string buildReadBody(RuntimeCore& core, const std::string& nodeDecoded, const std::string& attr) {
    const std::string ts = isoNow();
    if (attr == "ValueRank")
        return "{\"status\":{\"code\":0,\"string\":\"Good\"},\"value\":-1,\"serverTimestamp\":\"" + ts + "\"}";
    if (attr == "ArrayDimensions")
        return "{\"status\":{\"code\":0,\"string\":\"Good\"},\"value\":[],\"serverTimestamp\":\"" + ts + "\"}";

    ParsedNode p = parseNodeId(nodeDecoded);
    uint32_t st;
    std::string val = "null";
    if (p.kind == ParsedNode::Kind::Foreign) {
        // NamespaceArray — return a minimal array (without the B&R PLC URI) so a
        // LuxConnect client falls back to ns=5; our nodes are reached via explicit
        // ns=1 NodeId overrides, which skip namespace resolution entirely.
        if (nodeDecoded == "ns=0;i=2255") {
            return "{\"status\":{\"code\":0,\"string\":\"Good\"},\"value\":"
                   "[\"http://opcfoundation.org/UA/\",\"urn:loom:runtime\"],"
                   "\"serverTimestamp\":\"" + ts + "\"}";
        }
        st = kBadNodeIdUnknown;
    } else if (p.kind == ParsedNode::Kind::Field || p.kind == ParsedNode::Kind::Section) {
        std::shared_lock<std::shared_mutex> ml(core.moduleMutex());
        st = readValueLocked(core, p, val);
    } else {
        st = kBadNodeIdUnknown; // containers are not value nodes
    }
    return "{\"status\":{\"code\":" + std::to_string(st) + ",\"string\":\"" + statusString(st) +
           "\"},\"value\":" + val + ",\"serverTimestamp\":\"" + ts + "\"}";
}

// Build the JSON body for a write of {nodeId} with the given raw-JSON value.
std::string buildWriteBody(RuntimeCore& core, const std::string& nodeDecoded, std::string_view valueJson) {
    ParsedNode p = parseNodeId(nodeDecoded);
    uint32_t st;
    if (p.kind == ParsedNode::Kind::Field || p.kind == ParsedNode::Kind::Section) {
        std::shared_lock<std::shared_mutex> ml(core.moduleMutex());
        st = writeValueLocked(core, p, valueJson.empty() ? std::string_view("null") : valueJson);
    } else {
        st = kBadNodeIdUnknown;
    }
    return "{\"status\":{\"code\":" + std::to_string(st) + ",\"string\":\"" + statusString(st) + "\"}}";
}

template <typename T>
glz::error_ctx readPermissive(T& v, std::string_view json) {
    constexpr glz::opts kOpts{ .error_on_unknown_keys = false, .error_on_missing_keys = false };
    glz::context ctx{};
    return glz::read<kOpts>(v, json, ctx);
}

std::string echoId(const glz::raw_json& id, size_t index) {
    if (!id.str.empty()) return id.str;
    return "\"" + std::to_string(index) + "\"";
}

// Extract the percent-decoded NodeId from a batch item's relative url of the
// form "/<enc(nodeId)>/attributes/<attr>". Returns {nodeDecoded, attr}.
std::pair<std::string, std::string> nodeFromBatchUrl(const std::string& url) {
    std::string path = url;
    if (!path.empty() && path.front() == '/') path.erase(0, 1);
    std::string attr = "Value";
    auto apos = path.find("/attributes/");
    std::string enc;
    if (apos != std::string::npos) {
        enc  = path.substr(0, apos);
        attr = path.substr(apos + std::string("/attributes/").size());
    } else {
        enc = path;
    }
    return { urlDecode(enc), attr };
}

} // namespace

// ---------------------------------------------------------------------------

OpcUaRestServer::OpcUaRestServer(RuntimeCore& core) : core_(core) {}

OpcUaRestServer::~OpcUaRestServer() { stopPump(); }

void OpcUaRestServer::startPump(int intervalMs) {
    pumpIntervalMs_ = intervalMs > 0 ? intervalMs : 50;
    pumpRunning_.store(true);
    pumpThread_ = std::thread([this]() { pumpLoop(); });
}

void OpcUaRestServer::stopPump() {
    pumpRunning_.store(false);
    if (pumpThread_.joinable()) pumpThread_.join();
}

// Single-producer notification pump. One thread services every session, so each
// pushchannel connection has exactly one writer — structurally avoiding the
// Crow multi-writer race that motivated this redesign.
void OpcUaRestServer::pumpLoop() {
    using namespace std::chrono;
    while (pumpRunning_.load()) {
        std::this_thread::sleep_for(milliseconds(pumpIntervalMs_));
        const auto now = steady_clock::now();
        const std::string ts = isoNow();

        // Frames are built under the locks, then flushed AFTER releasing the
        // module lock — mirroring /ws/watch, so a slow/busy socket can't block
        // module operations (reload, etc.) for the duration of the sends.
        std::vector<std::pair<crow::websocket::connection*, std::string>> outbox;

        // Lock ordering: SessionManager mutex BEFORE the module mutex (see
        // opcua_rest_session.h). The SessionManager mutex is held across the
        // flush too, so a disconnecting connection cannot be freed mid-send
        // (onclose also takes it).
        std::lock_guard<std::mutex> smlk(sm_.mutex());

        // Prune idle sessions that timed out without a DELETE. Sessions with a
        // live pushchannel are kept (their keep-alive HEADs refresh lastSeen).
        for (auto it = sm_.sessions().begin(); it != sm_.sessions().end();) {
            auto& s = it->second;
            const double ageMs = static_cast<double>(duration_cast<milliseconds>(now - s.lastSeen).count());
            if (!s.pushConn && ageMs > s.timeoutMs) it = sm_.sessions().erase(it);
            else ++it;
        }

        {
            std::shared_lock<std::shared_mutex> ml(core_.moduleMutex());
            for (auto& [sid, sess] : sm_.sessions()) {
                if (!sess.pushConn) continue;
                for (auto& [subId, sub] : sess.subs) {
                    if (now < sub.nextDue) continue;
                    sub.nextDue = now + milliseconds(static_cast<long long>(sub.publishingIntervalMs));

                    std::string notifs;
                    bool any = false;
                    for (auto& [mid, item] : sub.items) {
                        std::string val;
                        uint32_t st = readValueLocked(core_, item.parsed, val);
                        bool changed = !item.firstSent || st != item.lastStatus || val != item.lastJson;
                        if (!changed) continue;
                        item.firstSent  = true;
                        item.lastStatus = st;
                        item.lastJson   = val;

                        if (any) notifs += ",";
                        notifs += "{\"clientHandle\":" + std::to_string(item.clientHandle) +
                                  ",\"value\":" + val +
                                  ",\"status\":{\"code\":" + std::to_string(st) +
                                  ",\"symbol\":\"" + statusString(st) + "\"}" +
                                  ",\"sourceTimestamp\":\"" + ts + "\",\"serverTimestamp\":\"" + ts + "\"}";
                        any = true;
                    }
                    if (!any) continue;

                    outbox.emplace_back(sess.pushConn,
                        "{\"sessionId\":" + std::to_string(sid) +
                        ",\"subscriptionId\":" + std::to_string(subId) +
                        ",\"DataNotifications\":[" + notifs + "]}");
                }
            }
        } // module lock released here

        for (auto& [conn, frame] : outbox) {
            try { conn->send_text(frame); } catch (...) { /* closing; cleaned on onclose */ }
        }
    }
}

// ---------------------------------------------------------------------------

void OpcUaRestServer::registerRoutes(crow::SimpleApp& app) {
    using namespace dto;
    RuntimeCore& core = core_;
    opcrest::SessionManager& sm = sm_;

    // GET /api/1.0/auth — reachability + (stand-in) auth.
    CROW_ROUTE(app, "/api/1.0/auth")
        .methods("GET"_method, "OPTIONS"_method)
        ([](const crow::request& req) {
            if (isOptions(req)) return jsonResp(204, "");
            auto r = jsonResp(200, "{\"success\":true,\"roles\":[]}");
            r.add_header("Set-Cookie", "ClientId={loom}; Path=/; HttpOnly; SameSite=Strict");
            return r;
        });

    // POST /api/1.0/opcua/sessions — create session.
    CROW_ROUTE(app, "/api/1.0/opcua/sessions")
        .methods("POST"_method, "OPTIONS"_method)
        ([&sm](const crow::request& req) {
            if (isOptions(req)) return jsonResp(204, "");
            SessionBody body{};
            if (!req.body.empty() && readPermissive(body, req.body))
                return jsonResp(400, "{\"error\":\"malformed JSON body\"}");
            uint64_t id = sm.createSession(body.timeout);
            auto r = jsonResp(201, "{\"id\":" + std::to_string(id) +
                                   ",\"status\":{\"code\":0,\"string\":\"Good\"}}");
            r.add_header("Set-Cookie", "ClientId={" + std::to_string(id) + "}; Path=/; HttpOnly; SameSite=Strict");
            return r;
        });

    // GET/HEAD/DELETE/PATCH /api/1.0/opcua/sessions/<id>
    CROW_ROUTE(app, "/api/1.0/opcua/sessions/<string>")
        .methods("GET"_method, "HEAD"_method, "DELETE"_method, "PATCH"_method, "OPTIONS"_method)
        ([&sm](const crow::request& req, const std::string& sidStr) {
            if (isOptions(req)) return jsonResp(204, "");
            uint64_t sid = 0;
            if (!parseU64(sidStr, sid)) return jsonResp(404, "{\"error\":\"bad session id\"}");
            if (req.method == "DELETE"_method) {
                sm.deleteSession(sid);
                return jsonResp(204, "");
            }
            if (req.method == "PATCH"_method) {
                sm.touch(sid);
                return jsonResp(200, "{\"code\":0,\"string\":\"Good\"}");
            }
            // GET / HEAD — keep-alive.
            if (!sm.hasSession(sid)) return jsonResp(404, "{\"error\":\"no such session\"}");
            sm.touch(sid);
            if (req.method == "HEAD"_method) return jsonResp(204, "");
            return jsonResp(200, "{\"id\":" + std::to_string(sid) + ",\"status\":\"connected\"}");
        });

    // POST /api/1.0/opcua/sessions/<id>/nodes/$batch  (register before nodes/<string>/...)
    CROW_ROUTE(app, "/api/1.0/opcua/sessions/<string>/nodes/$batch")
        .methods("POST"_method, "OPTIONS"_method)
        ([&core, &sm](const crow::request& req, const std::string& sidStr) {
            if (isOptions(req)) return jsonResp(204, "");
            uint64_t sid = 0;
            if (parseU64(sidStr, sid)) sm.touch(sid);
            BatchReqs reqs{};
            readPermissive(reqs, req.body);
            std::string out = "{\"responses\":[";
            for (size_t i = 0; i < reqs.requests.size(); ++i) {
                auto& br = reqs.requests[i];
                auto [nodeDecoded, attr] = nodeFromBatchUrl(br.url);
                std::string body;
                int status;
                if (br.method == "PUT") {
                    PutBody pb{};
                    if (!br.body.str.empty()) readPermissive(pb, br.body.str);
                    body = buildWriteBody(core, nodeDecoded, pb.value.str);
                    status = 200;
                } else {
                    body = buildReadBody(core, nodeDecoded, attr);
                    status = 200;
                }
                if (i) out += ",";
                out += "{\"id\":" + echoId(br.id, i) + ",\"status\":" + std::to_string(status) +
                       ",\"body\":" + body + "}";
            }
            out += "]}";
            return jsonResp(200, out);
        });

    // GET /api/1.0/opcua/sessions/<id>/nodes/<nodeId>/references — Browse.
    CROW_ROUTE(app, "/api/1.0/opcua/sessions/<string>/nodes/<string>/references")
        .methods("GET"_method, "OPTIONS"_method)
        ([&core, &sm](const crow::request& req, const std::string& sidStr, const std::string& nodeRaw) {
            if (isOptions(req)) return jsonResp(204, "");
            uint64_t sid = 0;
            if (parseU64(sidStr, sid)) sm.touch(sid);

            const std::string nodeDecoded = urlDecode(nodeRaw);
            ParsedNode p = parseNodeId(nodeDecoded);
            std::string refs;
            bool any = false;
            auto addRef = [&](const std::string& childPath, const std::string& name, const char* nodeClass) {
                if (any) refs += ",";
                refs += "{\"referenceTypeId\":\"ns=0;i=47\",\"isForward\":true,\"nodeId\":\"" +
                        esc(makeNodeId(childPath)) + "\",\"browseName\":{\"namespaceIndex\":1,\"name\":\"" +
                        esc(name) + "\"},\"displayName\":{\"text\":\"" + esc(name) + "\"},\"nodeClass\":\"" +
                        nodeClass + "\"}";
                any = true;
            };

            if (p.kind == ParsedNode::Kind::Root) {
                addRef("/module", "module", "Object");
            } else if (p.kind == ParsedNode::Kind::ModuleContainer) {
                std::shared_lock<std::shared_mutex> ml(core.moduleMutex());
                for (auto& [id, mod] : core.loader().modules())
                    addRef("/module/" + id, id, "Object");
            } else if (p.kind == ParsedNode::Kind::Module) {
                for (const char* s : { "config", "recipe", "runtime", "summary" })
                    addRef("/module/" + p.moduleId + "/" + s, s, "Object");
            } else if (p.kind == ParsedNode::Kind::Section || p.kind == ParsedNode::Kind::Field) {
                std::string json;
                {
                    std::shared_lock<std::shared_mutex> ml(core.moduleMutex());
                    auto* lm = core.loader().get(p.moduleId);
                    if (lm && lm->instance) {
                        if (p.kind == ParsedNode::Kind::Section)
                            json = lm->instance->readSection(p.section);
                        else if (auto v = lm->instance->readField(p.section, p.fieldPointer))
                            json = *v;
                    }
                }
                glz::json_t doc;
                if (!json.empty() && !glz::read_json(doc, json) && doc.is_object()) {
                    const std::string base = (p.kind == ParsedNode::Kind::Section)
                        ? ("/module/" + p.moduleId + "/" + sectionName(p.section))
                        : ("/module/" + p.moduleId + "/" + sectionName(p.section) + "/" + p.fieldPointer);
                    for (auto& [k, v] : doc.get_object()) {
                        const char* nc = (v.is_object() || v.is_array()) ? "Object" : "Variable";
                        addRef(base + "/" + k, k, nc);
                    }
                }
            }
            return jsonResp(200, "{\"status\":{\"code\":0,\"string\":\"Good\"},\"references\":[" + refs + "]}");
        });

    // GET/PUT /api/1.0/opcua/sessions/<id>/nodes/<nodeId>/attributes/<attr>
    CROW_ROUTE(app, "/api/1.0/opcua/sessions/<string>/nodes/<string>/attributes/<string>")
        .methods("GET"_method, "PUT"_method, "OPTIONS"_method)
        ([&core, &sm](const crow::request& req, const std::string& sidStr,
                      const std::string& nodeRaw, const std::string& attr) {
            if (isOptions(req)) return jsonResp(204, "");
            uint64_t sid = 0;
            if (parseU64(sidStr, sid)) sm.touch(sid);
            const std::string nodeDecoded = urlDecode(nodeRaw);
            if (req.method == "PUT"_method) {
                PutBody pb{};
                if (readPermissive(pb, req.body) || pb.value.str.empty())
                    return jsonResp(400, "{\"status\":{\"code\":2147483648,\"string\":\"Bad\"},"
                                         "\"error\":\"malformed or missing value\"}");
                return jsonResp(200, buildWriteBody(core, nodeDecoded, pb.value.str));
            }
            return jsonResp(200, buildReadBody(core, nodeDecoded, attr));
        });

    // POST /api/1.0/opcua/sessions/<id>/subscriptions
    CROW_ROUTE(app, "/api/1.0/opcua/sessions/<string>/subscriptions")
        .methods("POST"_method, "OPTIONS"_method)
        ([&sm](const crow::request& req, const std::string& sidStr) {
            if (isOptions(req)) return jsonResp(204, "");
            uint64_t sid = 0;
            if (!parseU64(sidStr, sid)) return jsonResp(404, "{\"error\":\"bad session id\"}");
            SubBody body{};
            if (!req.body.empty() && readPermissive(body, req.body))
                return jsonResp(400, "{\"error\":\"malformed JSON body\"}");
            auto subId = sm.createSubscription(sid, body.publishingInterval);
            if (!subId) return jsonResp(404, "{\"error\":\"no such session\"}");
            return jsonResp(201, "{\"status\":{\"code\":0,\"string\":\"Good\"},\"subscriptionId\":" +
                                 std::to_string(*subId) + ",\"revisedPublishingInterval\":" +
                                 std::to_string(static_cast<long long>(body.publishingInterval)) + "}");
        });

    // POST .../subscriptions/<subId>/monitoredItems/$batch  (register before the /<itemId> route)
    CROW_ROUTE(app, "/api/1.0/opcua/sessions/<string>/subscriptions/<string>/monitoredItems/$batch")
        .methods("POST"_method, "OPTIONS"_method)
        ([&sm](const crow::request& req, const std::string& sidStr, const std::string& subStr) {
            if (isOptions(req)) return jsonResp(204, "");
            uint64_t sid = 0, subId = 0;
            if (!parseU64(sidStr, sid) || !parseU64(subStr, subId))
                return jsonResp(404, "{\"error\":\"bad id\"}");
            sm.touch(sid);
            BatchReqs reqs{};
            readPermissive(reqs, req.body);
            std::string out = "{\"responses\":[";
            for (size_t i = 0; i < reqs.requests.size(); ++i) {
                auto& br = reqs.requests[i];
                std::string body;
                int status;
                MonItemBody mib{};
                if (!br.body.str.empty()) readPermissive(mib, br.body.str);
                opcrest::MonitoredItem item;
                item.clientHandle = mib.monitoringParameters.clientHandle;
                item.nodeId       = mib.itemToMonitor.nodeId;     // JSON-body NodeId is plain (not URL-encoded)
                item.attribute    = mib.itemToMonitor.attribute;
                item.parsed       = parseNodeId(item.nodeId);
                auto mid = sm.addMonitoredItem(sid, subId, std::move(item));
                if (mid) {
                    body = "{\"monitoredItemId\":" + std::to_string(*mid) +
                           ",\"status\":{\"code\":0,\"string\":\"Good\"}}";
                    status = 201;
                } else {
                    body = "{\"message\":\"subscriptionId not found\"}";
                    status = 404;
                }
                if (i) out += ",";
                out += "{\"id\":" + echoId(br.id, i) + ",\"status\":" + std::to_string(status) +
                       ",\"body\":" + body + "}";
            }
            out += "]}";
            return jsonResp(200, out);
        });

    // POST .../subscriptions/<subId>/monitoredItems  (single create)
    CROW_ROUTE(app, "/api/1.0/opcua/sessions/<string>/subscriptions/<string>/monitoredItems")
        .methods("POST"_method, "OPTIONS"_method)
        ([&sm](const crow::request& req, const std::string& sidStr, const std::string& subStr) {
            if (isOptions(req)) return jsonResp(204, "");
            uint64_t sid = 0, subId = 0;
            if (!parseU64(sidStr, sid) || !parseU64(subStr, subId))
                return jsonResp(404, "{\"error\":\"bad id\"}");
            sm.touch(sid);
            MonItemBody mib{};
            if (readPermissive(mib, req.body))
                return jsonResp(400, "{\"error\":\"malformed JSON body\"}");
            opcrest::MonitoredItem item;
            item.clientHandle = mib.monitoringParameters.clientHandle;
            item.nodeId       = mib.itemToMonitor.nodeId;
            item.attribute    = mib.itemToMonitor.attribute;
            item.parsed       = parseNodeId(item.nodeId);
            auto mid = sm.addMonitoredItem(sid, subId, std::move(item));
            if (!mid) return jsonResp(404, "{\"message\":\"subscriptionId not found\"}");
            return jsonResp(201, "{\"status\":{\"code\":0,\"string\":\"Good\"},\"monitoredItemId\":" +
                                 std::to_string(*mid) +
                                 ",\"revisedSamplingInterval\":" +
                                 std::to_string(static_cast<long long>(mib.monitoringParameters.samplingInterval)) +
                                 ",\"revisedQueueSize\":1}");
        });

    // DELETE .../subscriptions/<subId>/monitoredItems/<itemId>
    CROW_ROUTE(app, "/api/1.0/opcua/sessions/<string>/subscriptions/<string>/monitoredItems/<string>")
        .methods("DELETE"_method, "OPTIONS"_method)
        ([&sm](const crow::request& req, const std::string& sidStr, const std::string& subStr,
               const std::string& itemStr) {
            if (isOptions(req)) return jsonResp(204, "");
            uint64_t sid = 0, subId = 0, itemId = 0;
            if (!parseU64(sidStr, sid) || !parseU64(subStr, subId) || !parseU64(itemStr, itemId))
                return jsonResp(404, "{\"error\":\"bad id\"}");
            sm.deleteMonitoredItem(sid, subId, itemId);
            return jsonResp(204, "");
        });

    // DELETE .../subscriptions/<subId>
    CROW_ROUTE(app, "/api/1.0/opcua/sessions/<string>/subscriptions/<string>")
        .methods("DELETE"_method, "OPTIONS"_method)
        ([&sm](const crow::request& req, const std::string& sidStr, const std::string& subStr) {
            if (isOptions(req)) return jsonResp(204, "");
            uint64_t sid = 0, subId = 0;
            if (!parseU64(sidStr, sid) || !parseU64(subStr, subId))
                return jsonResp(404, "{\"error\":\"bad id\"}");
            sm.deleteSubscription(sid, subId);
            return jsonResp(204, "");
        });

    // WebSocket /api/1.0/pushchannel?sessionid=<id> — notification stream.
    CROW_WEBSOCKET_ROUTE(app, "/api/1.0/pushchannel")
        .onaccept([&sm](const crow::request& req, void** userdata) -> bool {
            const char* p = req.url_params.get("sessionid");
            if (!p) return false;
            uint64_t sid = 0;
            if (!parseU64(p, sid) || !sm.hasSession(sid)) return false;
            // Stash the session id as a heap uint64_t (freed in onclose) — avoids
            // integer↔pointer aliasing / truncation on narrow-pointer platforms.
            *userdata = new uint64_t(sid);
            return true;
        })
        .onopen([&sm](crow::websocket::connection& conn) {
            auto* sidPtr = static_cast<uint64_t*>(conn.userdata());
            uint64_t sid = sidPtr ? *sidPtr : 0;
            sm.bindPushConn(sid, &conn);
            spdlog::info("opcua_rest pushchannel opened (session {})", sid);
        })
        .onclose([&sm](crow::websocket::connection& conn, const std::string& reason, unsigned short /*code*/) {
            sm.unbindPushConn(&conn);
            delete static_cast<uint64_t*>(conn.userdata());
            spdlog::info("opcua_rest pushchannel closed: {}", reason);
        })
        .onmessage([](crow::websocket::connection&, const std::string&, bool) { /* client→server unused */ });
}

} // namespace loom
