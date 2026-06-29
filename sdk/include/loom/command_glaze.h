#pragma once

#include <glaze/glaze.hpp>

#include "loom/command.h"

// ============================================================================
// Glaze metadata for the function-block bases — OPT-IN (keeps command.h itself
// glaze-free, so the core control header has no serialization dependency).
//
// Include this header where you want function blocks to be reflectable, e.g. to
// place them directly in a module's Runtime struct so the watch tree / HMI sees
// their live PLCopen I/O. The bases are abstract, so these specializations are
// never serialized on their own — the value here is the field-list macros, which
// a concrete FB's meta expands to reuse the base I/O surface without repeating it:
//
//   #include <loom/command_glaze.h>
//   template <> struct glz::meta<MC_MoveAbsolute> {
//       using T = MC_MoveAbsolute;
//       static constexpr auto value = glz::object(
//           LOOM_COMMAND_FB_FIELDS(T),                 // execute/done/busy/...
//           "position", &T::position, "velocity", &T::velocity);
//   };
//
// Only the public I/O is exposed; private edge/latch state and any bound handles
// (a CommandClient, a status cell) are intentionally omitted.
// ============================================================================

/// Common IFunctionBlock outputs.
#define LOOM_IFUNCTION_BLOCK_FIELDS(T) \
    "busy", &T::busy, "error", &T::error, "error_id", &T::error_id

/// CommandFb surface: the Execute input + the PLCopen command outputs.
#define LOOM_COMMAND_FB_FIELDS(T)                                          \
    "execute", &T::execute, "buffer_mode", &T::buffer_mode,               \
    "busy", &T::busy, "active", &T::active, "done", &T::done,             \
    "command_aborted", &T::command_aborted, "error", &T::error,          \
    "error_id", &T::error_id

/// EnableFb surface: the Enable level input + common outputs.
#define LOOM_ENABLE_FB_FIELDS(T) \
    "enable", &T::enable, LOOM_IFUNCTION_BLOCK_FIELDS(T)

template <>
struct glz::meta<loom::BufferMode> {
    using enum loom::BufferMode;
    static constexpr auto value = glz::enumerate("aborting", Aborting, "buffered", Buffered);
};

template <>
struct glz::meta<loom::IFunctionBlock> {
    using T = loom::IFunctionBlock;
    static constexpr auto value = glz::object(LOOM_IFUNCTION_BLOCK_FIELDS(T));
};

template <>
struct glz::meta<loom::CommandFb> {
    using T = loom::CommandFb;
    static constexpr auto value = glz::object(LOOM_COMMAND_FB_FIELDS(T));
};

template <>
struct glz::meta<loom::EnableFb> {
    using T = loom::EnableFb;
    static constexpr auto value = glz::object(LOOM_ENABLE_FB_FIELDS(T));
};
