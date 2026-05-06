#pragma once

#include <glaze/glaze.hpp>
#include <string>
#include <type_traits>

namespace loom {

namespace detail {

/// Map a C++ type to a human-readable string for the field descriptor.
template <typename T>
constexpr const char* typeName() {
    if constexpr (std::is_same_v<T, bool>) return "bool";
    else if constexpr (std::is_same_v<T, int8_t>) return "int8";
    else if constexpr (std::is_same_v<T, uint8_t>) return "uint8";
    else if constexpr (std::is_same_v<T, int16_t>) return "int16";
    else if constexpr (std::is_same_v<T, uint16_t>) return "uint16";
    else if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, int>) return "int32";
    else if constexpr (std::is_same_v<T, uint32_t>) return "uint32";
    else if constexpr (std::is_same_v<T, int64_t>) return "int64";
    else if constexpr (std::is_same_v<T, uint64_t>) return "uint64";
    else if constexpr (std::is_same_v<T, float>) return "float";
    else if constexpr (std::is_same_v<T, double>) return "double";
    else if constexpr (std::is_same_v<T, std::string>) return "string";
    else return "object";
}

/// Build a SectionDescriptor for a struct type T using glaze reflection.
template <typename T>
SectionDescriptor buildSectionDescriptor(DataSection section) {
    SectionDescriptor desc;
    desc.section = section;

    // Use glaze reflection to enumerate fields
    constexpr auto N = glz::reflect<T>::size;

    if constexpr (N > 0) {
        constexpr auto keys = glz::reflect<T>::keys;

        T instance{};

        // Use glz::for_each to iterate over reflected members
        auto processField = [&]<size_t I>() {
            auto& val = glz::get<I>(instance);
            using FieldType = std::remove_cvref_t<decltype(val)>;

            FieldDescriptor fd;
            fd.name = std::string(keys[I]);
            fd.typeName = typeName<FieldType>();
            fd.offset = reinterpret_cast<uintptr_t>(&val) - reinterpret_cast<uintptr_t>(&instance);
            fd.size = sizeof(FieldType);
            desc.fields.push_back(std::move(fd));
        };

        [&]<size_t... Is>(std::index_sequence<Is...>) {
            (processField.template operator()<Is>(), ...);
        }(std::make_index_sequence<N>{});
    }

    return desc;
}

} // namespace detail

template <typename Config, typename Recipe, typename Runtime>
ModuleDataDescriptor buildDescriptor(const std::string& moduleId) {
    ModuleDataDescriptor desc;
    desc.moduleId = moduleId;
    desc.config = detail::buildSectionDescriptor<Config>(DataSection::Config);
    desc.recipe = detail::buildSectionDescriptor<Recipe>(DataSection::Recipe);
    desc.runtime = detail::buildSectionDescriptor<Runtime>(DataSection::Runtime);
    return desc;
}

} // namespace loom
