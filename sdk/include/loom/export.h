#pragma once

#include "loom/types.h"

/// Visibility attribute for exported symbols.
#if defined(_WIN32) || defined(__CYGWIN__)
    #define LOOM_EXPORT __declspec(dllexport)
#else
    #define LOOM_EXPORT __attribute__((visibility("default")))
#endif

/// Declare the module name/version and implement the mandatory header() override
/// in one line. Place inside the class body.
///
/// Usage:
///   LOOM_MODULE_HEADER("MyModule", "1.0.0")
///
#define LOOM_MODULE_HEADER(name_, version_)                                    \
    static loom::ModuleHeader moduleHeader() {                                 \
        return {.api_version = loom::kApiVersion,                              \
                .name = (name_), .version = (version_),                       \
                .sdk_version = loom::kSdkVersion};                             \
    }                                                                         \
    const loom::ModuleHeader& header() const override {                        \
        static const auto hdr = moduleHeader();                               \
        return hdr;                                                           \
    }

/// Register a module class with the Loom runtime.
///
/// This macro generates the extern "C" factory functions that the runtime
/// uses to create/destroy module instances and query metadata.
///
/// Usage:
///   LOOM_REGISTER_MODULE(MyModule)
///
/// The module class must inherit from loom::Module<Config, Recipe, Runtime[, Summary]>.
///
#define LOOM_REGISTER_MODULE(ModuleClass)                                      \
    static_assert(                                                            \
        std::is_base_of_v<                                                    \
            loom::Module<                                                      \
                typename ModuleClass::config_type,                            \
                typename ModuleClass::recipe_type,                            \
                typename ModuleClass::runtime_type,                           \
                typename ModuleClass::summary_type>,                          \
            ModuleClass>,                                                     \
        #ModuleClass " must inherit from loom::Module<Config, Recipe, Runtime>"); \
                                                                              \
    extern "C" {                                                              \
                                                                              \
    LOOM_EXPORT loom::IModule* loom_module_create() {                            \
        return new ModuleClass();                                             \
    }                                                                         \
                                                                              \
    LOOM_EXPORT void loom_module_destroy(loom::IModule* m) {                     \
        delete m;                                                             \
    }                                                                         \
                                                                              \
    LOOM_EXPORT const loom::ModuleHeader* loom_query_header() {                  \
        static const loom::ModuleHeader hdr = ModuleClass::moduleHeader();     \
        return &hdr;                                                          \
    }                                                                         \
                                                                              \
    } /* extern "C" */
