#pragma once
// Reflection-lite: a TypeInfo lists named, typed properties with type-erased
// getters/setters. It is the single source of truth for:
//   - the editor Inspector (widgets per PropertyType)
//   - scene serialization (.fscene)
//   - MCP tools (get/set_component_property, describe_component)
//   - networking: properties marked .replicated() are sent to clients (fledge_net)
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "fledge/core/json.h"
#include "fledge/core/math.h"
#include "fledge/core/utility.h"

namespace fledge {

enum class PropertyType { Bool, Int, Float, Vec3, Color, Quat, String, AssetRef, Enum, Json };
const char* toString(PropertyType t);

using PropertyValue = std::variant<bool, int, float, Vec3, Quat, std::string, Json>;

struct PropertyInfo {
    std::string name;
    PropertyType type = PropertyType::Float;
    std::string description;
    float minValue = 0, maxValue = 0;          // 0/0 = unbounded (UI hint only)
    std::vector<std::string> enumNames;        // for Enum (stored as int)
    bool replicated = false;                   // sent server -> clients by fledge_net (docs/NETWORKING.md)
    bool omitWhenEmpty = false;                // String / AssetRef: TypeInfo::toJson leaves it out while empty (optional field)
    std::function<PropertyValue(const void*)> get;
    std::function<void(void*, const PropertyValue&)> set;

    Json toJson(const void* obj) const;
    // Accepts the canonical form plus friendly forms (Quat from 3 euler degrees,
    // Enum from its name, Color from 3 or 4 numbers).
    Status fromJson(void* obj, const Json& j) const;
    Json schema() const;  // JSON-schema fragment used by MCP tool descriptions
};

struct TypeInfo {
    std::string name;
    std::string description;
    std::vector<PropertyInfo> properties;

    const PropertyInfo* find(std::string_view propName) const;
    Json toJson(const void* obj) const;
    Status fromJson(void* obj, const Json& j) const; // missing keys keep current values
    // Resets every omitWhenEmpty property that j does not name to "" (a saved state without the key means "not set"):
    // applyEntityState (undo / redo, Time Machine) calls it before fromJson, so restoring an older state clears the field.
    void clearOmitted(void* obj, const Json& j) const;
    Json describe() const;
};

namespace detail {
template <typename M> struct PropTypeOf;
template <> struct PropTypeOf<bool> { static constexpr PropertyType value = PropertyType::Bool; };
template <> struct PropTypeOf<int> { static constexpr PropertyType value = PropertyType::Int; };
template <> struct PropTypeOf<float> { static constexpr PropertyType value = PropertyType::Float; };
template <> struct PropTypeOf<Vec3> { static constexpr PropertyType value = PropertyType::Vec3; };
template <> struct PropTypeOf<Quat> { static constexpr PropertyType value = PropertyType::Quat; };
template <> struct PropTypeOf<std::string> { static constexpr PropertyType value = PropertyType::String; };
template <> struct PropTypeOf<Json> { static constexpr PropertyType value = PropertyType::Json; };
} // namespace detail

// Fluent builder:  TypeBuilder<Light>("Light").field("intensity", &Light::intensity, "lux-ish").build();
template <typename T>
class TypeBuilder {
public:
    explicit TypeBuilder(std::string name, std::string description = {}) {
        m_info.name = std::move(name);
        m_info.description = std::move(description);
    }

    template <typename M>
    TypeBuilder& field(std::string name, M T::*member, std::string description = {}, float minV = 0, float maxV = 0) {
        return add(std::move(name), member, detail::PropTypeOf<M>::value, std::move(description), minV, maxV);
    }
    TypeBuilder& color(std::string name, Vec3 T::*member, std::string description = {}) {
        return add(std::move(name), member, PropertyType::Color, std::move(description), 0, 1);
    }
    TypeBuilder& assetRef(std::string name, std::string T::*member, std::string description = {}) {
        return add(std::move(name), member, PropertyType::AssetRef, std::move(description), 0, 0);
    }
    template <typename E>
    TypeBuilder& enumeration(std::string name, E T::*member, std::vector<std::string> names, std::string description = {}) {
        PropertyInfo p;
        p.name = std::move(name);
        p.type = PropertyType::Enum;
        p.description = std::move(description);
        p.enumNames = std::move(names);
        p.get = [member](const void* o) -> PropertyValue { return static_cast<int>(static_cast<const T*>(o)->*member); };
        p.set = [member](void* o, const PropertyValue& v) {
            if (auto* i = std::get_if<int>(&v)) static_cast<T*>(o)->*member = static_cast<E>(*i);
        };
        m_info.properties.push_back(std::move(p));
        return *this;
    }
    // Marks the property added last as replicated (networking: server -> clients).
    TypeBuilder& replicated() {
        if (!m_info.properties.empty()) m_info.properties.back().replicated = true;
        return *this;
    }
    // Marks the (string) property added last as optional: not written while empty, so files without it stay unchanged.
    TypeBuilder& omitWhenEmpty() {
        if (!m_info.properties.empty()) m_info.properties.back().omitWhenEmpty = true;
        return *this;
    }
    TypeInfo build() { return std::move(m_info); }

private:
    template <typename M>
    TypeBuilder& add(std::string name, M T::*member, PropertyType type, std::string description, float minV, float maxV) {
        PropertyInfo p;
        p.name = std::move(name);
        p.type = type;
        p.description = std::move(description);
        p.minValue = minV;
        p.maxValue = maxV;
        p.get = [member](const void* o) -> PropertyValue { return static_cast<const T*>(o)->*member; };
        p.set = [member](void* o, const PropertyValue& v) {
            if (auto* x = std::get_if<M>(&v)) static_cast<T*>(o)->*member = *x;
        };
        m_info.properties.push_back(std::move(p));
        return *this;
    }
    TypeInfo m_info;
};

} // namespace fledge
