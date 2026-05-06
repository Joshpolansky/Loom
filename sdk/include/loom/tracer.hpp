#pragma once
// Renamed to tag_table.hpp — this header now simply re-exports it.
#include "loom/tag_table.hpp"

// Legacy aliases so any existing code using the old names still compiles.
using TracedVar  = loom::Tag;
template<typename T> using Tracer = loom::TagTable<T>;

// -----------------------------------------------------------------------
// TracedVar - type-erased accessors for a single (possibly nested) field
// -----------------------------------------------------------------------
struct TracedVar {
    std::string      path;       // e.g. "player/health"
    std::string_view type_name;  // e.g. "int"
    std::function<std::string()>          get_json;
    std::function<void(std::string_view)> set_json;
    std::function<void*()>                ptr;        // raw pointer to field
};

// -----------------------------------------------------------------------
// Detail: recursively walk reflected fields, registering leaves
// -----------------------------------------------------------------------
namespace detail {

// Forward declaration so visit_field can recurse into register_fields
template<typename T>
void register_fields(T& obj,
                     const std::string& prefix,
                     std::unordered_map<std::string, TracedVar>& cache);

template<typename FieldType>
void visit_field(FieldType& field,
                 const std::string& path,
                 std::unordered_map<std::string, TracedVar>& cache)
{
    if constexpr (glz::reflectable<FieldType>) {
        // Nested struct — recurse, using current path as prefix
        register_fields(field, path, cache);
    } else {
        // Leaf — register get/set/ptr lambdas
        TracedVar var;
        var.path      = path;
        var.type_name = glz::name_v<FieldType>;

        var.get_json = [&field]() -> std::string {
            return glz::write_json(field).value_or("<e>");
        };
        var.set_json = [&field](std::string_view json) {
            glz::read_json(field, json);
        };
        var.ptr = [&field]() -> void* { return std::addressof(field); };

        cache.emplace(path, std::move(var));
    }
}

template<typename T>
void register_fields(T& obj,
                     const std::string& prefix,
                     std::unordered_map<std::string, TracedVar>& cache)
{
    // glz::for_each_field calls the lambda with just the field value (no index).
    // Track the field index manually with a mutable counter.
    std::size_t i = 0;
    glz::for_each_field(obj, [&](auto& field) {
        std::string_view name = glz::reflect<T>::keys[i++];

        std::string path = prefix.empty()
            ? std::string(name)
            : prefix + "/" + std::string(name);

        visit_field(field, path, cache);
    });
}

} // namespace detail

// -----------------------------------------------------------------------
// Tracer - built once, queried by flattened path string
// -----------------------------------------------------------------------
template<typename T>
class Tracer {
public:
    explicit Tracer(T& obj) {
        detail::register_fields(obj, "", cache_);
    }

    // Get current value as JSON string, e.g. "player/health"
    std::optional<std::string> get(std::string_view path) const {
        auto it = cache_.find(std::string(path));
        if (it == cache_.end()) return std::nullopt;
        return it->second.get_json();
    }

    // Set value from a JSON string
    bool set(std::string_view path, std::string_view json_value) {
        auto it = cache_.find(std::string(path));
        if (it == cache_.end()) return false;
        it->second.set_json(json_value);
        return true;
    }

    // Get raw pointer to field (caller must know the type)
    void* get_ptr(std::string_view path) const {
        auto it = cache_.find(std::string(path));
        if (it == cache_.end()) return nullptr;
        return it->second.ptr();
    }

    // Get the C++ type name of a field
    std::optional<std::string_view> type_of(std::string_view path) const {
        auto it = cache_.find(std::string(path));
        if (it == cache_.end()) return std::nullopt;
        return it->second.type_name;
    }

    bool contains(std::string_view path) const {
        return cache_.contains(std::string(path));
    }

    // Iterate all traced vars: fn(TracedVar const&)
    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (auto& [key, var] : cache_)
            fn(var);
    }

    // Dump all fields: "path (type) = value"
    std::string dump() const {
        std::string out;
        for (auto& [key, var] : cache_) {
            out += var.path;
            out += " (";
            out += var.type_name;
            out += ") = ";
            out += var.get_json();
            out += "\n";
        }
        return out;
    }

private:
    std::unordered_map<std::string, TracedVar> cache_;
};

template<typename T>
Tracer<T> make_tracer(T& obj) {
    return Tracer<T>(obj);
}
