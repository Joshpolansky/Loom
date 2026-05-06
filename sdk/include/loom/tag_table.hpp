#pragma once

#include <glaze/glaze.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <optional>
#include <vector>
#include <array>
#include <deque>
#include <list>
#include <map>
#include <type_traits>

namespace loom {

// -----------------------------------------------------------------------
// Tag - type-erased accessors for a single reflected leaf field
// -----------------------------------------------------------------------
struct Tag {
    std::string      path;       // e.g. "status/position"
    std::string_view type_name;  // e.g. "double"
    std::function<std::string()>          get_json;
    std::function<void(std::string_view)> set_json;
    std::function<void*()>                ptr;   // raw pointer to live field
    bool             stable = true;        // false if path goes through dynamic container
};

// -----------------------------------------------------------------------
// Trait helpers
// -----------------------------------------------------------------------
namespace detail {

template<typename T> struct is_std_vector : std::false_type {};
template<typename V, typename A>
struct is_std_vector<std::vector<V, A>> : std::true_type {};

template<typename T> struct is_std_array : std::false_type {};
template<typename V, std::size_t N>
struct is_std_array<std::array<V, N>> : std::true_type {};

template<typename T> struct is_std_deque : std::false_type {};
template<typename V, typename A>
struct is_std_deque<std::deque<V, A>> : std::true_type {};

template<typename T> struct is_std_list : std::false_type {};
template<typename V, typename A>
struct is_std_list<std::list<V, A>> : std::true_type {};

// All indexed sequence containers — iterable with a counter
template<typename T> struct is_sequence_container : std::disjunction<
    is_std_vector<T>, is_std_array<T>, is_std_deque<T>, is_std_list<T>> {};

template<typename T> struct is_std_map : std::false_type {};
template<typename K, typename V, typename C, typename A>
struct is_std_map<std::map<K, V, C, A>> : std::true_type {};

template<typename T> struct is_std_unordered_map : std::false_type {};
template<typename K, typename V, typename H, typename E, typename A>
struct is_std_unordered_map<std::unordered_map<K, V, H, E, A>> : std::true_type {};

template<typename T> struct is_map_container : std::disjunction<
    is_std_map<T>, is_std_unordered_map<T>> {};

template<typename T> struct is_std_optional : std::false_type {};
template<typename V>
struct is_std_optional<std::optional<V>> : std::true_type {};

// std::string and std::string_view are reflectable in glaze but should be
// treated as opaque leaves, not recursed into.
template<typename T> struct is_string_like : std::false_type {};
template<> struct is_string_like<std::string>            : std::true_type {};
template<> struct is_string_like<std::string_view>       : std::true_type {};
template<> struct is_string_like<std::wstring>           : std::true_type {};

// Convert a map key to a path segment string
template<typename K>
std::string key_to_segment(const K& k) {
    if constexpr (std::is_same_v<std::decay_t<K>, std::string>)
        return k;
    else if constexpr (std::is_same_v<std::decay_t<K>, std::string_view>)
        return std::string(k);
    else if constexpr (std::is_integral_v<K> || std::is_floating_point_v<K>)
        return std::to_string(k);
    else
        return glz::write_json(k).value_or("?");
}

template<typename T>
void tag_register_fields(T& obj,
                         const std::string& prefix,
                         std::unordered_map<std::string, Tag>& cache,
                         std::vector<std::function<bool()>>& stale_checkers,
                         bool in_dynamic = false);

template<typename FieldType>
void tag_visit_field(FieldType& field,
                     const std::string& path,
                     std::unordered_map<std::string, Tag>& cache,
                     std::vector<std::function<bool()>>& stale_checkers,
                     bool in_dynamic = false)
{
    if constexpr (is_sequence_container<FieldType>::value) {
        // Register the container itself as a leaf (JSON = full sequence)
        Tag seq_tag;
        seq_tag.path      = path;
        seq_tag.type_name = glz::name_v<FieldType>;
        seq_tag.stable    = !in_dynamic;  // containers themselves are dynamic
        seq_tag.get_json  = [&field]() -> std::string {
            return glz::write_json(field).value_or("<e>");
        };
        seq_tag.set_json  = [&field](std::string_view json) {
            glz::read_json(field, json);
        };
        seq_tag.ptr = [&field]() -> void* { return std::addressof(field); };
        cache.emplace(path, std::move(seq_tag));

        // Staleness: stale if size changed since build
        const std::size_t size_at_build = field.size();
        stale_checkers.push_back([&field, size_at_build]() {
            return field.size() != size_at_build;
        });

        // Also expand current elements as indexed sub-paths (e.g. "history/0/speed").
        // If the container grows later, call refreshTraceCache() to rebuild.
        std::size_t idx = 0;
        for (auto& elem : field) {
            std::string elem_path = path + "/" + std::to_string(idx++);
            tag_visit_field(elem, elem_path, cache, stale_checkers, true);  // mark descendants as dynamic
        }
    } else if constexpr (is_map_container<FieldType>::value) {
        // Register the map itself as a leaf (JSON = full object)
        Tag map_tag;
        map_tag.path      = path;
        map_tag.type_name = glz::name_v<FieldType>;
        map_tag.stable    = !in_dynamic;  // maps are dynamic
        map_tag.get_json  = [&field]() -> std::string {
            return glz::write_json(field).value_or("<e>");
        };
        map_tag.set_json  = [&field](std::string_view json) {
            glz::read_json(field, json);
        };
        map_tag.ptr = [&field]() -> void* { return std::addressof(field); };
        cache.emplace(path, std::move(map_tag));

        // Staleness: stale if key count changed since build
        const std::size_t size_at_build = field.size();
        stale_checkers.push_back([&field, size_at_build]() {
            return field.size() != size_at_build;
        });

        // Expand each entry as "map_path/key"
        for (auto& [k, v] : field) {
            std::string entry_path = path + "/" + key_to_segment(k);
            tag_visit_field(v, entry_path, cache, stale_checkers, true);  // mark descendants as dynamic
        }
    } else if constexpr (is_std_optional<FieldType>::value) {
        // Register the optional itself at its own path (JSON = null or the wrapped value)
        Tag opt_tag;
        opt_tag.path      = path;
        opt_tag.type_name = glz::name_v<FieldType>;
        opt_tag.stable    = !in_dynamic;  // optional is dynamic
        opt_tag.get_json  = [&field]() -> std::string {
            return glz::write_json(field).value_or("<e>");
        };
        opt_tag.set_json  = [&field](std::string_view json) {
            glz::read_json(field, json);
        };
        opt_tag.ptr = [&field]() -> void* { return std::addressof(field); };
        cache.emplace(path, std::move(opt_tag));

        // Staleness: stale if engagement state changed since build.
        // Registered PRE-ORDER so it fires before any inner checkers on rebuild.
        const bool engaged_at_build = field.has_value();
        stale_checkers.push_back([&field, engaged_at_build]() {
            return field.has_value() != engaged_at_build;
        });

        // Transparent recursion: if currently engaged, walk inner value using
        // the same path as the optional (optional is invisible in the path).
        // e.g. optional<Inner>{x,y} → "maybe/x", "maybe/y" not "maybe/value/x"
        if (field.has_value()) {
            tag_visit_field(*field, path, cache, stale_checkers, true);  // mark descendants as dynamic
        }
    } else if constexpr (glz::reflectable<FieldType> && !is_string_like<FieldType>::value) {
        Tag struct_tag;
        struct_tag.path      = path;
        struct_tag.type_name = glz::name_v<FieldType>;
        struct_tag.stable    = !in_dynamic;  // inherit dynamic state
        struct_tag.get_json  = [&field]() -> std::string {
            return glz::write_json(field).value_or("<e>");
        };
        struct_tag.set_json  = [&field](std::string_view json) {
            glz::read_json(field, json);
        };
        struct_tag.ptr = [&field]() -> void* { return std::addressof(field); };
        cache.emplace(path, std::move(struct_tag));         
        // Nested struct — recurse, using current path as prefix
        tag_register_fields(field, path, cache, stale_checkers, in_dynamic);
    } else {
        // Leaf — register lambdas that reference the live field
        Tag tag;
        tag.path      = path;
        tag.type_name = glz::name_v<FieldType>;
        tag.stable    = !in_dynamic;  // inherit dynamic state
        tag.get_json  = [&field]() -> std::string {
            return glz::write_json(field).value_or("<e>");
        };
        tag.set_json  = [&field](std::string_view json) {
            glz::read_json(field, json);
        };
        tag.ptr = [&field]() -> void* { return std::addressof(field); };

        cache.emplace(path, std::move(tag));
    }
}

template<typename T>
void tag_register_fields(T& obj,
                         const std::string& prefix,
                         std::unordered_map<std::string, Tag>& cache,
                         std::vector<std::function<bool()>>& stale_checkers,
                         bool in_dynamic)
{
    // glz::for_each_field calls the lambda with just the field value (no index).
    // Track the field index manually with a mutable counter.
    std::size_t i = 0;
    glz::for_each_field(obj, [&](auto& field) {
        std::string_view name = glz::reflect<T>::keys[i++];
        std::string path = prefix.empty()
            ? std::string(name)
            : prefix + "/" + std::string(name);
        tag_visit_field(field, path, cache, stale_checkers, in_dynamic);
    });
}

} // namespace detail

// -----------------------------------------------------------------------
// TagTable<T> — built once against a live object, queried by path
//
// Lambdas inside each Tag capture references to the fields of the object
// passed to the constructor. That object must outlive the TagTable.
// In Module<>, TagTable<Runtime> is built directly on runtime_ so it
// always reflects current values with no copying or offset arithmetic.
// -----------------------------------------------------------------------
template<typename T>
class TagTable {
public:
    explicit TagTable(T& obj) {
        detail::tag_register_fields(obj, "", tags_, stale_checkers_);
    }

    // Returns true if any dynamic container (vector, map, optional) has changed
    // size or engagement state since this TagTable was built. When true, the
    // caller should rebuild (e.g. via Module::refreshTraceCache()).
    bool needs_refresh() const {
        for (auto& check : stale_checkers_)
            if (check()) return true;
        return false;
    }

    // Read the current value of a field as JSON (e.g. "status/position")
    std::optional<std::string> read_json(std::string_view path) const {
        auto it = tags_.find(std::string(path));
        if (it == tags_.end()) return std::nullopt;
        return it->second.get_json();
    }

    // Write a field from a JSON string
    bool write_json(std::string_view path, std::string_view json) {
        auto it = tags_.find(std::string(path));
        if (it == tags_.end()) return false;
        it->second.set_json(json);
        return true;
    }

    // Raw pointer to the live field (caller must know the type)
    void* ptr(std::string_view path) const {
        auto it = tags_.find(std::string(path));
        if (it == tags_.end()) return nullptr;
        return it->second.ptr();
    }

    // C++ type name of the field, e.g. "double", "bool"
    std::optional<std::string_view> type_of(std::string_view path) const {
        auto it = tags_.find(std::string(path));
        if (it == tags_.end()) return std::nullopt;
        return it->second.type_name;
    }

    bool contains(std::string_view path) const {
        return tags_.contains(std::string(path));
    }

    // Check if a path is stable (doesn't go through dynamic containers)
    bool is_stable(std::string_view path) const {
        auto it = tags_.find(std::string(path));
        return it != tags_.end() && it->second.stable;
    }

    // Find and return the Tag struct for inspection (e.g. to capture JSON lambdas)
    const Tag* find(std::string_view path) const {
        auto it = tags_.find(std::string(path));
        return it != tags_.end() ? &it->second : nullptr;
    }

    // Iterate all tags: fn(Tag const&)
    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (auto& [key, tag] : tags_)
            fn(tag);
    }

    // Dump all fields as "path (type) = json\n"
    std::string dump() const {
        std::string out;
        for (auto& [key, tag] : tags_) {
            out += tag.path;
            out += " (";
            out += tag.type_name;
            out += ") = ";
            out += tag.get_json();
            out += "\n";
        }
        return out;
    }

private:
    std::unordered_map<std::string, Tag>    tags_;
    std::vector<std::function<bool()>>      stale_checkers_;
};

} // namespace loom
