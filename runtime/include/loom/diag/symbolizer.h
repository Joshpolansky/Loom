#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// loom::diag — stack symbolizer
//
// Resolves raw instruction addresses to function + file:line. Backed by cpptrace
// in symbolizer.cpp (the ONLY TU that includes cpptrace — it stays out of every
// header so consumers/SDK never see it). NOT async-signal-safe (it allocates and
// reads debug info): call only off the signal path — the Windows unhandled-
// exception filter, the std::set_terminate handler, or the offline --symbolize
// tool. The POSIX fatal-signal handler captures raw addresses and symbolizes
// later / offline.
// ============================================================================

namespace loom::diag {

struct SymFrame {
    uintptr_t   address = 0;
    std::string symbol;    // function name ("" if unresolved)
    std::string filename;  // source file ("" if unavailable)
    uint32_t    line = 0;  // 0 if unknown
};

std::vector<SymFrame> symbolize(const void* const* addrs, std::size_t n);

} // namespace loom::diag
