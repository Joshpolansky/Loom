#pragma once

/// thread_support.h — compile-time detection of real OS-thread availability.
///
/// LOOM_HAS_THREADS is 1 for native builds (std::thread always backed by a real
/// OS thread) and for Emscripten builds compiled with -pthread (real
/// Worker-backed pthreads over SharedArrayBuffer — see runtime/CMakeLists.txt
/// and the de-risking spike in spike/phaseC-pthread-dlopen/). It is 0 only for
/// an Emscripten build WITHOUT -pthread: there, spawning a std::thread that
/// does real concurrent work isn't available, so the scheduler must run
/// cooperatively via Scheduler::tickOnce() instead of class threads.
///
/// Check __EMSCRIPTEN_PTHREADS__ (defined only when -pthread was passed), NOT
/// __EMSCRIPTEN__ (defined for every Emscripten build regardless of threading)
/// — conflating the two was a bug in an earlier iteration of this codebase
/// (every wasm build, threaded or not, was treated as cooperative-only).
///
/// CMake currently always builds the wasm host with -pthread (no non-threaded
/// wasm target is wired up), so LOOM_HAS_THREADS is 1 in every build produced
/// today. This header exists so the source itself supports both variants —
/// wiring a second, non-pthread CMake target is a separable, deferred step.
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#  define LOOM_HAS_THREADS 1
#else
#  define LOOM_HAS_THREADS 0
#endif
