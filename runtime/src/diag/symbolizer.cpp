#include "loom/diag/symbolizer.h"

#include <cpptrace/cpptrace.hpp>

#include <utility>

namespace loom::diag {

std::vector<SymFrame> symbolize(const void* const* addrs, std::size_t n) {
    cpptrace::raw_trace raw;
    raw.frames.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        raw.frames.push_back(
            static_cast<cpptrace::frame_ptr>(reinterpret_cast<std::uintptr_t>(addrs[i])));

    cpptrace::stacktrace st = raw.resolve();

    std::vector<SymFrame> out;
    out.reserve(st.frames.size());
    for (const auto& f : st.frames) {
        SymFrame sf;
        sf.address  = static_cast<std::uintptr_t>(f.raw_address);
        sf.symbol   = f.symbol;
        sf.filename = f.filename;
        sf.line     = f.line.has_value() ? static_cast<std::uint32_t>(f.line.value()) : 0u;
        out.push_back(std::move(sf));
    }
    return out;
}

} // namespace loom::diag
