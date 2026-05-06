#pragma once

#include <mutex>
#include <shared_mutex>
#include <loom/bus.h>
#include "loom/types.h"
#include "loom/tag_table.hpp"
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <vector>
#include <functional>
#include <cctype>
#include <iterator>
#include <type_traits>
#include <utility>

namespace loom {

namespace detail {
/// Default empty summary struct used when a module doesn't define one.
struct EmptySummary {};
} // namespace detail

// Forward declaration needed by IModuleRegistry
class IModule;

/// Minimal interface for looking up sibling module instances by ID.
/// Implemented by RuntimeCore and injected into modules before init().
/// Use getRuntimeAs<T>() in Module rather than calling this directly.
class IModuleRegistry {
public:
    virtual ~IModuleRegistry() = default;
    /// Returns the IModule* for the given module ID, or nullptr if not found.
    virtual IModule* findModule(std::string_view id) = 0;
};

/// Type-erased interface for the runtime to interact with modules without
/// knowing their concrete Config/Recipe/Runtime types.
class IModule {
public:
    virtual ~IModule() = default;

    // Lifecycle
    virtual void init(const InitContext& ctx) = 0;

    /// Phase 1 of 3: runs BEFORE cyclic() on every member in the class.
    /// Override to snapshot hardware inputs or fast-read bus state.
    /// All members' preCyclic() complete before any member's cyclic() begins.
    virtual void preCyclic()  {}

    virtual void cyclic() = 0;

    /// Phase 3 of 3: runs AFTER cyclic() on every member in the class.
    /// Override to flush outputs (e.g. write hardware registers) after all
    /// modules have completed their work.
    virtual void postCyclic() {}

    virtual void exit() = 0;
    virtual void longRunning() = 0;

    /// Called by the scheduler instead of cyclic() directly.
    /// The Module<> base acquires runtimeMutex_ so that readField() / readSection(Runtime)
    /// from server/watch threads cannot race with cyclic() writes to runtime_.
    virtual void cyclicGuarded()    { cyclic();    }
    virtual void preCyclicGuarded() { preCyclic(); }
    virtual void postCyclicGuarded(){ postCyclic(); }

    /// Optional scheduling hint. Override to express a preference for class/order.
    /// The runtime may ignore these in favour of scheduler.json configuration.
    virtual TaskHint taskHint() const { return {}; }

    // Data access — JSON serialization via glaze (type-erased)
    virtual std::string readSection(DataSection section) const = 0;
    virtual bool writeSection(DataSection section, std::string_view json) = 0;
    /// Returns a JSON Schema string for the given section (Config or Recipe).
    virtual std::string schemaSection(DataSection section) const = 0;

    // Module identity
    virtual const ModuleHeader& header() const = 0;

    /// Returns the type ID string of the Runtime struct (e.g. "MotorRuntime/1").
    /// Used by getRuntimeAs<T>() to validate the cast before dereferencing.
    virtual std::string_view runtimeTypeId() const = 0;

    /// Raw pointer to the Runtime struct. Use getRuntimeAs<T>() instead of casting directly.
    virtual void* runtimePtr() = 0;
    virtual const void* runtimePtr() const = 0;

    // Optional: return a raw pointer to a traced field inside the given section.
    // Implemented by the `Module<>` base so modules automatically expose their
    // Config/Recipe/Runtime fields to same-process consumers (e.g. oscilloscope).
    virtual std::optional<void*> tracePtr(DataSection /*section*/, std::string_view /*name*/) { return std::nullopt; }
    virtual std::optional<std::string> traceTypeName(DataSection /*section*/, std::string_view /*name*/) { return std::nullopt; }
    /// Read a single field's current value as JSON. Path uses '/' separators (e.g. "status/position").
    virtual std::optional<std::string> readField(DataSection /*section*/, std::string_view /*path*/) { return std::nullopt; }
    /// Write a single field from JSON. Returns true on success.
    virtual bool writeField(DataSection /*section*/, std::string_view /*path*/, std::string_view /*json*/) { return false; }
    /// Invalidate and rebuild any internal trace caches for the given section.
    virtual void refreshTraceCache(DataSection /*section*/) {}

    /// Return pointer to the runtime mutex for cross-module field access.
    /// Used by IOMapper for safe concurrent reads while cyclic() writes.
    virtual std::shared_mutex* runtimeLock() { return nullptr; }

    // Bus injection (called by runtime before init)
    void setBus(Bus* bus, const std::string& moduleId) {
        bus_ = bus;
        moduleId_ = moduleId;
    }

    // Registry injection (called by runtime before init, after setBus)
    void setRegistry(IModuleRegistry* registry) {
        registry_ = registry;
    }

    Bus* bus() const { return bus_; }
    const std::string& moduleId() const { return moduleId_; }

    // Called by runtime before unload to tear down all subscriptions made by this module.
    void cleanupSubscriptions() {
        if (!bus_) return;
        for (auto id : subscriptionIds_) {
            bus_->unsubscribe(id);
        }
        subscriptionIds_.clear();
    }

protected:
    Bus* bus_ = nullptr;
    IModuleRegistry* registry_ = nullptr;
    std::vector<uint64_t> subscriptionIds_;
    std::string moduleId_;
};

/// Base class for user modules. Users inherit from this with their own
/// Config, Recipe, and Runtime aggregate structs.
///
/// Example:
///   struct MyConfig { int rate = 10; };
///   struct MyRecipe { double speed = 1.0; };
///   struct MyRuntime { double pos = 0.0; };
///   struct MySummary { bool running = false; double speed = 0.0; };  // optional
///   class MyModule : public loom::Module<MyConfig, MyRecipe, MyRuntime, MySummary> { ... };
///
template <typename Config, typename Recipe, typename Runtime,
          typename Summary = detail::EmptySummary>
class Module : public IModule {
public:
    using config_type  = Config;
    using recipe_type  = Recipe;
    using runtime_type = Runtime;
    using summary_type = Summary;

    // --- Accessors for module author use ---
    Config& config() { return config_; }
    const Config& config() const { return config_; }

    Recipe& recipe() { return recipe_; }
    const Recipe& recipe() const { return recipe_; }

    Runtime& runtime() { return runtime_; }
    const Runtime& runtime() const { return runtime_; }

    Summary& summary() { return summary_; }
    const Summary& summary() const { return summary_; }

    // --- Direct runtime access to sibling modules ---

    /// Returns the type ID of this module's Runtime struct (e.g. "MotorRuntime/1").
    /// Enabled only when Runtime defines a static constexpr kTypeId member.
    std::string_view runtimeTypeId() const override {
        if constexpr (requires { Runtime::kTypeId; }) {
            return Runtime::kTypeId;
        } else {
            return {};
        }
    }

    /// Zero-copy direct access to a sibling module's Runtime struct.
    ///
    /// Returns a pointer to the live Runtime struct of the module with ID `targetId`,
    /// or nullptr if the module is not found or the Runtime type doesn't match.
    ///
    /// IMPORTANT: Do NOT store this pointer across cyclic() calls. After a hot-reload
    /// the target module's instance is replaced and the pointer becomes dangling.
    /// Fetch it fresh at each call site.
    ///
    /// Thread safety: safe to call from cyclic() provided the target module is not
    /// concurrently being hot-reloaded. In normal operation this is always the case.
    template <typename T>
    T* getRuntimeAs(std::string_view targetId) {
        if (!registry_) return nullptr;
        auto* mod = registry_->findModule(targetId);
        if (!mod) return nullptr;
        if constexpr (requires { T::kTypeId; }) {
            if (mod->runtimeTypeId() != T::kTypeId) return nullptr;
        }
        return static_cast<T*>(mod->runtimePtr());
    }

    template <typename T>
    const T* getRuntimeAs(std::string_view targetId) const {
        if (!registry_) return nullptr;
        auto* mod = registry_->findModule(targetId);
        if (!mod) return nullptr;
        if constexpr (requires { T::kTypeId; }) {
            if (mod->runtimeTypeId() != T::kTypeId) return nullptr;
        }
        return static_cast<const T*>(mod->runtimePtr());
    }

    // --- Bus convenience helpers ---
    // These prefix the topic/service with this module's instance ID.

    /// Publish on a topic scoped to this module: "<moduleId>/<name>"
    size_t publishLocal(std::string_view name, std::string_view payload) {
        if (!bus_) return 0;
        return bus_->publish(moduleAddress(moduleId_, name), payload);
    }

    /// Typed publish: serializes value to JSON then publishes.
    template <typename T>
    size_t publishLocal(std::string_view name, const T& value) {
        if (!bus_) return 0;
        auto json = glz::write_json(value).value_or("{}");
        return bus_->publish(moduleAddress(moduleId_, name), json);
    }

    /// Subscribe to a topic scoped to another module: "<targetModuleId>/<name>"
    uint64_t subscribeTo(std::string_view targetModuleId, std::string_view name, TopicCallback cb) {
        if (!bus_) return 0;
        auto id = bus_->subscribe(moduleAddress(targetModuleId, name), std::move(cb));
        subscriptionIds_.push_back(id);
        return id;
    }

    /// Typed subscribe: auto-deserializes the payload into T before calling the handler.
    /// If deserialization fails the message is silently dropped.
    template <typename T>
    uint64_t subscribeTo(std::string_view targetModuleId, std::string_view name,
                         std::function<void(const T&)> handler) {
        return subscribeTo(targetModuleId, name,
            [h = std::move(handler)](std::string_view /*topic*/, std::string_view payload) {
                T value{};
                if (!glz::read_json(value, payload)) {
                    h(value);
                }
            });
    }

    // --- Lambda arg deduction helpers (inside Module so they resolve correctly) ---
    template <typename Lambda> struct lambda_arg;
    template <typename ClassType, typename Ret, typename Arg>
    struct lambda_arg<Ret(ClassType::*)(const Arg&) const> { using type = Arg; };
    template <typename Lambda>
    using lambda_arg_t = typename lambda_arg<decltype(&Lambda::operator())>::type;

    /// Typed subscribe (deduced from lambda): subscribeTo(id, "name", [](const T&) {...})
    template <typename Lambda,
              typename T = lambda_arg_t<Lambda>>
    uint64_t subscribeTo(std::string_view targetModuleId, std::string_view name, Lambda&& handler) {
        return subscribeTo<T>(targetModuleId, name,
            std::function<void(const T&)>(std::forward<Lambda>(handler)));
    }

    /// Typed register (deduced from lambda): registerLocalService("name", [](const Req&) -> CallResult {...})
    template <typename Lambda,
              typename Req = lambda_arg_t<Lambda>>
    bool registerLocalService(std::string_view name, Lambda&& handler) {
        return registerLocalService<Req>(name,
            std::function<CallResult(const Req&)>(std::forward<Lambda>(handler)));
    }

    /// Typed register: auto-deserializes request JSON into Req.
    template <typename Req>
    bool registerLocalService(std::string_view name, std::function<CallResult(const Req&)> handler) {
        if (!bus_) return false;
        auto schema = glz::write_json_schema<Req>().value_or("{}");
        return bus_->registerService(moduleAddress(moduleId_, name),
            [h = std::move(handler)](std::string_view payload) -> CallResult {
                Req req{};
                if (glz::read_json(req, payload)) {
                    return {false, {}, "failed to deserialize request"};
                }
                return h(req);
            }, std::move(schema));
    }

    /// Typed register: handler receives Req and returns Resp (auto-serialized to JSON).
    template <typename Req, typename Resp>
    bool registerLocalService(std::string_view name, std::function<Resp(const Req&)> handler) {
        if (!bus_) return false;
        auto schema = glz::write_json_schema<Req>().value_or("{}");
        return bus_->registerService(moduleAddress(moduleId_, name),
            [h = std::move(handler)](std::string_view payload) -> CallResult {
                Req req{};
                if (glz::read_json(req, payload)) {
                    return {false, {}, "failed to deserialize request"};
                }
                Resp resp = h(req);
                auto json = glz::write_json(resp).value_or("{}");
                return {true, std::move(json), ""};
            }, std::move(schema));
    }

    /// Register a service scoped to this module: "<moduleId>/<name>" (raw handler)
    bool registerLocalService(std::string_view name, ServiceHandler handler) {
        if (!bus_) return false;
        return bus_->registerService(moduleAddress(moduleId_, name), std::move(handler));
    }

    /// Call a service on another module (raw JSON string).
    CallResult callService(std::string_view targetModuleId, std::string_view name, std::string_view request = "{}") {
        if (!bus_) return {false, {}, "bus not available"};
        return bus_->call(moduleAddress(targetModuleId, name), request);
    }

    /// Type-safe call: serialize Req to JSON, auto-deserialize Resp from JSON.
    template <typename Resp, typename Req>
    std::pair<bool, Resp> callService(std::string_view targetModuleId, std::string_view name, const Req& req) {
        if (!bus_) return {false, Resp{}};
        auto req_json = glz::write_json(req).value_or("{}");
        auto result = bus_->call(moduleAddress(targetModuleId, name), req_json);
        if (!result.ok) return {false, Resp{}};
        Resp resp{};
        if (glz::read_json(resp, result.response)) return {false, Resp{}};
        return {true, std::move(resp)};
    }

    /// Type-safe call: serialize Req to JSON, ignore response.
    template <typename Req>
    CallResult callService(std::string_view targetModuleId, std::string_view name, const Req& req) {
        if (!bus_) return {false, {}, "bus not available"};
        auto req_json = glz::write_json(req).value_or("{}");
        return bus_->call(moduleAddress(targetModuleId, name), req_json);
    }

    /// Type-safe call: auto-deserialize Resp from JSON, send empty request.
    template <typename Resp>
    std::pair<bool, Resp> callService(std::string_view targetModuleId, std::string_view name) {
        if (!bus_) return {false, Resp{}};
        auto result = bus_->call(moduleAddress(targetModuleId, name), "{}");
        if (!result.ok) return {false, Resp{}};
        Resp resp{};
        if (glz::read_json(resp, result.response)) return {false, Resp{}};
        return {true, std::move(resp)};
    }

    /// Async call (raw string).
    std::future<CallResult> callServiceAsync(std::string_view targetModuleId, std::string_view name, std::string request = "{}") {
        if (!bus_) {
            std::promise<CallResult> p;
            p.set_value({false, {}, "bus not available"});
            return p.get_future();
        }
        return bus_->callAsync(moduleAddress(targetModuleId, name), std::move(request));
    }

    /// Async type-safe call: serialize Req to JSON, auto-deserialize Resp from JSON.
    template <typename Resp, typename Req>
    std::future<std::pair<bool, Resp>> callServiceAsync(std::string_view targetModuleId, std::string_view name, const Req& req) {
        if (!bus_) {
            std::promise<std::pair<bool, Resp>> p;
            p.set_value({false, Resp{}});
            return p.get_future();
        }
        auto req_json = glz::write_json(req).value_or("{}");
        auto fut = bus_->callAsync(moduleAddress(targetModuleId, name), req_json);
        return std::async(std::launch::async, [fut = std::move(fut)]() mutable {
            auto result = fut.get();
            if (!result.ok) return std::pair<bool, Resp>{false, Resp{}};
            Resp resp{};
            if (glz::read_json(resp, result.response)) return std::pair<bool, Resp>{false, Resp{}};
            return std::pair<bool, Resp>{true, std::move(resp)};
        });
    }

    /// Async type-safe call: serialize Req to JSON, ignore response.
    template <typename Req>
    std::future<CallResult> callServiceAsync(std::string_view targetModuleId, std::string_view name, const Req& req) {
        if (!bus_) {
            std::promise<CallResult> p;
            p.set_value({false, {}, "bus not available"});
            return p.get_future();
        }
        auto req_json = glz::write_json(req).value_or("{}");
        return bus_->callAsync(moduleAddress(targetModuleId, name), req_json);
    }

    /// Async type-safe call: auto-deserialize Resp from JSON, send empty request.
    template <typename Resp>
    std::future<std::pair<bool, Resp>> callServiceAsync(std::string_view targetModuleId, std::string_view name) {
        if (!bus_) {
            std::promise<std::pair<bool, Resp>> p;
            p.set_value({false, Resp{}});
            return p.get_future();
        }
        auto fut = bus_->callAsync(moduleAddress(targetModuleId, name), "{}");
        return std::async(std::launch::async, [fut = std::move(fut)]() mutable {
            auto result = fut.get();
            if (!result.ok) return std::pair<bool, Resp>{false, Resp{}};
            Resp resp{};
            if (glz::read_json(resp, result.response)) return std::pair<bool, Resp>{false, Resp{}};
            return std::pair<bool, Resp>{true, std::move(resp)};
        });
    }

    // --- Type-erased data access for the runtime ---

protected:
    Config config_   = {};
    Recipe recipe_   = {};
    Runtime runtime_ = {};
    Summary summary_ = {};

    // --- TagTables for all mutable sections (lazy-built, captures live references) ---
    mutable std::optional<TagTable<Config>>  config_tags_;
    mutable std::optional<TagTable<Recipe>>  recipe_tags_;
    mutable std::optional<TagTable<Runtime>> runtime_tags_;

    // Guards runtime_ against concurrent access between cyclic() (write) and
    // readField() / readSection(Runtime) (read) on different threads.
    // shared_mutex allows concurrent reads (WS broadcast + watch thread) while
    // still serialising against the cyclic write.
    mutable std::shared_mutex runtimeMutex_;

    void ensureTagTableBuilt(DataSection section = DataSection::Runtime) const {
        if (section == DataSection::Config && (!config_tags_ || config_tags_->needs_refresh()))
            config_tags_.emplace(const_cast<Config&>(config_));
        else if (section == DataSection::Recipe && (!recipe_tags_ || recipe_tags_->needs_refresh()))
            recipe_tags_.emplace(const_cast<Recipe&>(recipe_));
        else if (section == DataSection::Runtime && (!runtime_tags_ || runtime_tags_->needs_refresh()))
            runtime_tags_.emplace(const_cast<Runtime&>(runtime_));
    }

public:
    /// The scheduler calls this instead of cyclic() directly so that runtime_
    /// reads from other threads (readField, readSection) are not racing with writes.
    void cyclicGuarded() override {
        std::unique_lock lk(runtimeMutex_);
        cyclic();
    }

    void preCyclicGuarded() override {
        std::unique_lock lk(runtimeMutex_);
        preCyclic();
    }

    void postCyclicGuarded() override {
        std::unique_lock lk(runtimeMutex_);
        postCyclic();
    }

    std::shared_mutex* runtimeLock() override {
        return &runtimeMutex_;
    }

    std::string readSection(DataSection section) const override {
        switch (section) {
            case DataSection::Config:  return glz::write_json(config_).value_or("{}");
            case DataSection::Recipe:  return glz::write_json(recipe_).value_or("{}");
            case DataSection::Runtime: {
                // Copy runtime_ under a brief shared lock, then serialize the
                // snapshot outside the lock.  Cyclic() is only blocked for the
                // duration of the struct copy, not the (potentially slow)
                // JSON serialization.
                Runtime snap;
                { std::shared_lock lk(runtimeMutex_); snap = runtime_; }
                return glz::write_json(snap).value_or("{}");
            }
            case DataSection::Summary: return glz::write_json(summary_).value_or("{}");
        }
        return "{}";
    }

    std::string schemaSection(DataSection section) const override {
        switch (section) {
            case DataSection::Config:  return glz::write_json_schema<Config>().value_or("{}");
            case DataSection::Recipe:  return glz::write_json_schema<Recipe>().value_or("{}");
            default:                   return "{}";
        }
    }

    bool writeSection(DataSection section, std::string_view json) override {
        constexpr glz::opts kPermissiveReadOpts{
            .error_on_unknown_keys = false,
            .error_on_missing_keys = false,
        };

        switch (section) {
            case DataSection::Config:  {
                glz::context ctx{};
                auto ec = glz::read<kPermissiveReadOpts>(config_, json, ctx);
                if (!ec) refreshTraceCache(DataSection::Config);
                return !ec;
            }
            case DataSection::Recipe:  {
                glz::context ctx{};
                auto ec = glz::read<kPermissiveReadOpts>(recipe_, json, ctx);
                if (!ec) refreshTraceCache(DataSection::Recipe);
                return !ec;
            }
            case DataSection::Runtime: {
                std::unique_lock lk(runtimeMutex_);
                glz::context ctx{};
                auto ec = glz::read<kPermissiveReadOpts>(runtime_, json, ctx);
                if (!ec) {
                    refreshTraceCache(DataSection::Runtime);
                }
                return !ec;
            }
            case DataSection::Summary: return false; // read-only
        }
        return false;
    }

    // --- Tag APIs: per-field raw pointer, type, and JSON for same-process consumers ---
    std::optional<void*> tracePtr(DataSection section, std::string_view name) override {
        std::string key(name);
        if (!key.empty() && key.front() == '/') key.erase(0, 1);
        if (section == DataSection::Config)  { ensureTagTableBuilt(DataSection::Config);  return config_tags_->ptr(key); }
        if (section == DataSection::Recipe)  { ensureTagTableBuilt(DataSection::Recipe);  return recipe_tags_->ptr(key); }
        if (section == DataSection::Runtime) { ensureTagTableBuilt(DataSection::Runtime); return runtime_tags_->ptr(key); }
        return std::nullopt;
    }

    std::optional<std::string> traceTypeName(DataSection section, std::string_view name) override {
        std::string key(name);
        if (!key.empty() && key.front() == '/') key.erase(0, 1);
        if (section == DataSection::Config)  { ensureTagTableBuilt(DataSection::Config);  if (auto t = config_tags_->type_of(key))  return std::string(*t); return std::nullopt; }
        if (section == DataSection::Recipe)  { ensureTagTableBuilt(DataSection::Recipe);  if (auto t = recipe_tags_->type_of(key))  return std::string(*t); return std::nullopt; }
        if (section == DataSection::Runtime) { ensureTagTableBuilt(DataSection::Runtime); if (auto t = runtime_tags_->type_of(key)) return std::string(*t); return std::nullopt; }
        return std::nullopt;
    }

    bool writeField(DataSection section, std::string_view name, std::string_view json) override {
        std::string key(name);
        if (!key.empty() && key.front() == '/') key.erase(0, 1);
        if (section == DataSection::Config) {
            ensureTagTableBuilt(DataSection::Config);
            bool ok = config_tags_->write_json(key, json);
            if (ok) refreshTraceCache(DataSection::Config);
            return ok;
        }
        if (section == DataSection::Recipe) {
            ensureTagTableBuilt(DataSection::Recipe);
            bool ok = recipe_tags_->write_json(key, json);
            if (ok) refreshTraceCache(DataSection::Recipe);
            return ok;
        }
        if (section == DataSection::Runtime) {
            std::unique_lock lk(runtimeMutex_);
            ensureTagTableBuilt(DataSection::Runtime);
            bool ok = runtime_tags_->write_json(key, json);
            if (ok) refreshTraceCache(DataSection::Runtime);
            return ok;
        }
        return false;
    }

    std::optional<std::string> readField(DataSection section, std::string_view name) override {
        std::string key(name);
        if (!key.empty() && key.front() == '/') key.erase(0, 1);
        if (section == DataSection::Config) {
            ensureTagTableBuilt(DataSection::Config);
            return key.empty() ? std::optional<std::string>(glz::write_json(config_).value_or("{}"))
                               : config_tags_->read_json(key);
        }
        if (section == DataSection::Recipe) {
            ensureTagTableBuilt(DataSection::Recipe);
            return key.empty() ? std::optional<std::string>(glz::write_json(recipe_).value_or("{}"))
                               : recipe_tags_->read_json(key);
        }
        if (section == DataSection::Runtime) {
            std::shared_lock lk(runtimeMutex_);
            ensureTagTableBuilt(DataSection::Runtime);
            return key.empty() ? std::optional<std::string>(glz::write_json(runtime_).value_or("{}"))
                               : runtime_tags_->read_json(key);
        }
        return std::nullopt;
    }

    void refreshTraceCache(DataSection section) override {
        switch (section) {
            case DataSection::Config:  config_tags_.reset();  break;
            case DataSection::Recipe:  recipe_tags_.reset();  break;
            case DataSection::Runtime: runtime_tags_.reset(); break;
            default: break;
        }
    }
    void* configPtr()              { return &config_; }
    void* recipePtr()              { return &recipe_; }
    void* runtimePtr() override             { return &runtime_; }
    const void* configPtr()  const { return &config_; }
    const void* recipePtr()  const { return &recipe_; }
    const void* runtimePtr() const override { return &runtime_; }
};


} // namespace loom
