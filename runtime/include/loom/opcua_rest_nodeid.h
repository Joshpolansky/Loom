#pragma once

#include "loom/types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// mapp Connect facade — NodeId addressing.
//
// A node is addressed as an OPC-UA string NodeId: "ns=1;s=<path>" where <path>
// mirrors the runtime's data tree:
//
//   /module                                   — container of all modules
//   /module/<id>                              — one module (container of sections)
//   /module/<id>/<section>                    — a whole section (config|recipe|runtime|summary)
//   /module/<id>/<section>/<field-pointer>    — a leaf/sub-tree within a section
//   /scheduler/...                            — scheduler stats (Browse only, later)
//
// <field-pointer> is the '/'-separated path handed to IModule::readField /
// writeField (empty = whole section). Anything not "ns=1;s=/..." (e.g. the
// client's B&R "ns=5;..." names, or "ns=0;i=2255") parses as Foreign and is
// handled explicitly by the caller — we never crash on an unknown NodeId.
namespace loom::opcrest {

/// Percent-decode a URL-encoded path segment (handles %XX). '+' is left as-is
/// (NodeIds never contain '+'; this avoids qs_decode's '+'→space semantics).
inline std::string urlDecode(std::string_view in) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int h = hex(in[i + 1]);
            int l = hex(in[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>((h << 4) | l));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

inline std::optional<DataSection> sectionFromName(std::string_view s) {
    if (s == "config")  return DataSection::Config;
    if (s == "recipe")  return DataSection::Recipe;
    if (s == "runtime") return DataSection::Runtime;
    if (s == "summary") return DataSection::Summary;
    return std::nullopt;
}

inline const char* sectionName(DataSection s) {
    switch (s) {
        case DataSection::Config:  return "config";
        case DataSection::Recipe:  return "recipe";
        case DataSection::Runtime: return "runtime";
        case DataSection::Summary: return "summary";
    }
    return "runtime";
}

struct ParsedNode {
    enum class Kind {
        Root,             ///< ns=1;s=/  (or empty path)
        ModuleContainer,  ///< ns=1;s=/module
        Module,           ///< ns=1;s=/module/<id>
        Section,          ///< ns=1;s=/module/<id>/<section>  (fieldPointer empty)
        Field,            ///< ns=1;s=/module/<id>/<section>/<field-pointer>
        Scheduler,        ///< ns=1;s=/scheduler/...
        Foreign,          ///< anything else (e.g. ns=5;..., ns=0;i=2255)
    } kind = Kind::Foreign;

    std::string moduleId;
    DataSection section = DataSection::Runtime;
    std::string fieldPointer;  ///< '/'-path for readField; empty = whole section
    std::string raw;           ///< the original NodeId string
};

/// Parse an already-percent-decoded NodeId. Returns Foreign for anything that
/// is not one of our "ns=1;s=/..." paths.
inline ParsedNode parseNodeId(std::string_view nodeId) {
    ParsedNode p;
    p.raw = std::string(nodeId);

    constexpr std::string_view kPrefix = "ns=1;s=";
    if (nodeId.substr(0, kPrefix.size()) != kPrefix) {
        p.kind = ParsedNode::Kind::Foreign;
        return p;
    }
    std::string_view path = nodeId.substr(kPrefix.size());
    if (path.empty() || path[0] != '/') {
        p.kind = ParsedNode::Kind::Foreign;
        return p;
    }

    // Split path (after the leading '/') on '/'.
    std::vector<std::string_view> seg;
    for (size_t i = 1; i <= path.size();) {
        size_t j = path.find('/', i);
        if (j == std::string_view::npos) j = path.size();
        if (j > i) seg.push_back(path.substr(i, j - i));
        i = j + 1;
    }

    if (seg.empty()) { p.kind = ParsedNode::Kind::Root; return p; }

    if (seg[0] == "module") {
        if (seg.size() == 1) { p.kind = ParsedNode::Kind::ModuleContainer; return p; }
        p.moduleId = std::string(seg[1]);
        if (seg.size() == 2) { p.kind = ParsedNode::Kind::Module; return p; }
        auto sec = sectionFromName(seg[2]);
        if (!sec) { p.kind = ParsedNode::Kind::Foreign; return p; }
        p.section = *sec;
        if (seg.size() == 3) { p.kind = ParsedNode::Kind::Section; return p; }
        std::string fp;
        for (size_t k = 3; k < seg.size(); ++k) {
            if (k > 3) fp += "/";
            fp += std::string(seg[k]);
        }
        p.fieldPointer = std::move(fp);
        p.kind = ParsedNode::Kind::Field;
        return p;
    }

    if (seg[0] == "scheduler") { p.kind = ParsedNode::Kind::Scheduler; return p; }

    p.kind = ParsedNode::Kind::Foreign;
    return p;
}

/// Build a NodeId string from a "/..." path.
inline std::string makeNodeId(std::string_view path) {
    return "ns=1;s=" + std::string(path);
}

} // namespace loom::opcrest
