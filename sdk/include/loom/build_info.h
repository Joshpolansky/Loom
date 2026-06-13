#pragma once

#include <cstdint>
// <version> pulls in the implementation's config macros (on MSVC this defines
// _ITERATOR_DEBUG_LEVEL) without dragging in a heavy container header.
#include <version>

namespace loom {

/// ABI-relevant compile-time build signature for a translation unit.
///
/// This is a plain C-ABI POD (integers only) so the runtime can read a
/// module's signature across the shared-library boundary *even when the two
/// sides were built with incompatible C++ runtimes / STLs* — which is exactly
/// the situation this struct exists to detect. Heap-corruption crashes happen
/// when a `std::string`/`std::vector` allocated by one side is freed by the
/// other; if we transported the signature in such a type, reading it would
/// itself crash. So: NEVER add std::string, std::vector, or any non-trivial
/// type to this struct.
///
/// On MSVC the two properties that actually cause cross-heap corruption when
/// mismatched are `iterator_debug_level` (Debug STL = 2, Release STL = 0) and
/// the CRT flavor (`debug_crt` / `dynamic_crt`). `msc_ver` is informational:
/// the 14.x toolset family (VS 2015–2026) is binary compatible, so a differing
/// `msc_ver` is not by itself a reason to reject a module.
struct BuildInfo {
    uint32_t struct_size;          ///< sizeof(BuildInfo) at the producer — forward-compat guard
    uint32_t msc_ver;              ///< _MSC_VER, or 0 if not MSVC
    uint32_t msc_full_ver;         ///< _MSC_FULL_VER, or 0
    int32_t  iterator_debug_level; ///< _ITERATOR_DEBUG_LEVEL, or -1 if undefined
    uint8_t  is_msvc;              ///< 1 if compiled with MSVC
    uint8_t  debug_crt;            ///< 1 if _DEBUG (debug CRT) was defined
    uint8_t  dynamic_crt;          ///< 1 if _DLL (dynamic /MD CRT) was defined
    uint8_t  pointer_bits;         ///< 8 * sizeof(void*) (32 or 64)
    uint64_t cpp_standard;         ///< __cplusplus
};

/// Capture the build signature of the *current* translation unit.
///
/// Defined inline so each binary bakes in its own compile flags: the runtime
/// EXE captures the runtime's flags, and each module DLL captures its own
/// (because LOOM_REGISTER_MODULE expands the call in the module's .cpp).
/// Comparing two results detects an ABI-incompatible build before it crashes.
inline BuildInfo loomCurrentBuildInfo() {
    BuildInfo b{};
    b.struct_size  = static_cast<uint32_t>(sizeof(BuildInfo));
    b.pointer_bits = static_cast<uint8_t>(sizeof(void*) * 8);
    b.cpp_standard = static_cast<uint64_t>(__cplusplus);
#if defined(_MSC_VER)
    b.is_msvc = 1;
    b.msc_ver = static_cast<uint32_t>(_MSC_VER);
#  if defined(_MSC_FULL_VER)
    b.msc_full_ver = static_cast<uint32_t>(_MSC_FULL_VER);
#  endif
#else
    b.is_msvc = 0;
#endif
#if defined(_ITERATOR_DEBUG_LEVEL)
    b.iterator_debug_level = static_cast<int32_t>(_ITERATOR_DEBUG_LEVEL);
#else
    b.iterator_debug_level = -1;
#endif
#if defined(_DEBUG)
    b.debug_crt = 1;
#endif
#if defined(_DLL)
    b.dynamic_crt = 1;
#endif
    return b;
}

} // namespace loom
