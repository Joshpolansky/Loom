#include "loom/diag/fault_report.h"

#include <cstdio>
#include <string>

namespace loom::diag {

const char* faultKindName(FaultKind k) {
    switch (k) {
        case FaultKind::Exception: return "exception";
        case FaultKind::Signal:    return "signal";
    }
    return "unknown";
}

namespace {

// Minimal JSON string escaper (RFC 8259). We hand-roll the report JSON so the
// captured `sections` can embed as real nested JSON rather than escaped strings.
void appendEscaped(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void appendKV(std::string& out, const char* key, std::string_view val, bool& first) {
    if (!first) out += ',';
    first = false;
    out += '"'; out += key; out += "\":";
    appendEscaped(out, val);
}

void appendKVRaw(std::string& out, const char* key, std::string_view rawJson, bool& first) {
    if (!first) out += ',';
    first = false;
    out += '"'; out += key; out += "\":";
    out += (rawJson.empty() ? std::string_view{"null"} : rawJson);
}

void appendKVNum(std::string& out, const char* key, long long val, bool& first) {
    if (!first) out += ',';
    first = false;
    out += '"'; out += key; out += "\":";
    out += std::to_string(val);
}

} // namespace

std::string toJson(const FaultReport& r) {
    std::string out;
    out.reserve(1024 + r.frames.size() * 128);
    out += '{';
    bool first = true;

    appendKV   (out, "id",           r.id, first);
    appendKVNum(out, "ts",           r.tsMs, first);
    appendKV   (out, "kind",         faultKindName(r.kind), first);
    appendKVNum(out, "signalOrCode", r.signalOrCode, first);
    appendKV   (out, "reason",       r.reason, first);

    // build identity
    out += ",\"build\":{";
    {
        bool bfirst = true;
        appendKV(out, "sdkVersion", r.sdkVersion, bfirst);
        appendKV(out, "gitSha",     r.gitSha, bfirst);
        appendKV(out, "buildType",  r.buildType, bfirst);
    }
    out += '}';

    // breadcrumb
    out += ",\"breadcrumb\":{";
    {
        bool cfirst = true;
        appendKV   (out, "module", r.moduleId.empty() ? "" : r.moduleId, cfirst);
        appendKV   (out, "class",  r.className, cfirst);
        appendKV   (out, "phase",  phaseName(r.phase), cfirst);
        appendKVNum(out, "cycle",  static_cast<long long>(r.cycle), cfirst);
    }
    out += '}';

    // frames
    out += ",\"frames\":[";
    for (std::size_t i = 0; i < r.frames.size(); ++i) {
        const SymFrame& f = r.frames[i];
        if (i) out += ',';
        out += '{';
        bool ffirst = true;
        char addr[24];
        std::snprintf(addr, sizeof addr, "0x%016llx",
                      static_cast<unsigned long long>(f.address));
        appendKVNum(out, "idx",     static_cast<long long>(i), ffirst);
        appendKV   (out, "address", addr, ffirst);
        appendKV   (out, "function", f.symbol, ffirst);
        appendKV   (out, "file",    f.filename, ffirst);
        appendKVNum(out, "line",    f.line, ffirst);
        out += '}';
    }
    out += ']';

    // sections (exception path only)
    if (r.sections) {
        out += ",\"sections\":{";
        bool sfirst = true;
        appendKVRaw(out, "config",  r.sections->config,  sfirst);
        appendKVRaw(out, "recipe",  r.sections->recipe,  sfirst);
        appendKVRaw(out, "runtime", r.sections->runtime, sfirst);
        appendKVRaw(out, "summary", r.sections->summary, sfirst);
        out += '}';
    }

    out += '}';
    return out;
}

} // namespace loom::diag
