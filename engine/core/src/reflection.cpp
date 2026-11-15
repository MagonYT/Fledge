#include "fledge/core/reflection.h"

#include <format>

namespace fledge {

const char* toString(PropertyType t) {
    switch (t) {
        case PropertyType::Bool: return "bool";
        case PropertyType::Int: return "int";
        case PropertyType::Float: return "float";
        case PropertyType::Vec3: return "vec3";
        case PropertyType::Color: return "color";
        case PropertyType::Quat: return "quat";
        case PropertyType::String: return "string";
        case PropertyType::AssetRef: return "asset";
        case PropertyType::Enum: return "enum";
        case PropertyType::Json: return "json";
    }
    return "?";
}

Json PropertyInfo::toJson(const void* obj) const {
    PropertyValue v = get(obj);
    switch (type) {
        case PropertyType::Bool: return std::get<bool>(v);
        case PropertyType::Int: return std::get<int>(v);
        case PropertyType::Float: return std::get<float>(v);
        case PropertyType::Vec3:
        case PropertyType::Color: return Json(std::get<Vec3>(v));
        case PropertyType::Quat: return Json(std::get<Quat>(v));
        case PropertyType::String:
        case PropertyType::AssetRef: return std::get<std::string>(v);
        case PropertyType::Enum: {
            int i = std::get<int>(v);
            if (i >= 0 && i < static_cast<int>(enumNames.size())) return enumNames[static_cast<size_t>(i)];
            return i;
        }
        case PropertyType::Json: return std::get<Json>(v);
    }
    return nullptr;
}

static bool isNumberArray(const Json& j, size_t n) {
    if (!j.is_array() || j.size() != n) return false;
    for (auto& e : j)
        if (!e.is_number()) return false;
    return true;
}

Status PropertyInfo::fromJson(void* obj, const Json& j) const {
    auto bad = [&](const char* expected) {
        return Status::failure(std::format("property '{}' expects {}, got {}", name, expected, j.dump()));
    };
    switch (type) {
        case PropertyType::Bool:
            if (!j.is_boolean()) return bad("a boolean");
            set(obj, j.get<bool>());
            return {};
        case PropertyType::Int:
            if (!j.is_number()) return bad("an integer");
            set(obj, j.get<int>());
            return {};
        case PropertyType::Float:
            if (!j.is_number()) return bad("a number");
            set(obj, j.get<float>());
            return {};
        case PropertyType::Vec3:
            if (!isNumberArray(j, 3)) return bad("[x, y, z]");
            set(obj, j.get<Vec3>());
            return {};
        case PropertyType::Color:
            if (isNumberArray(j, 3) || isNumberArray(j, 4)) {
                set(obj, Vec3{j[0].get<float>(), j[1].get<float>(), j[2].get<float>()});
                return {};
            }
            return bad("[r, g, b] in 0..1");
        case PropertyType::Quat:
            if (isNumberArray(j, 4)) {
                set(obj, j.get<Quat>().normalized());
                return {};
            }
            if (isNumberArray(j, 3)) { // euler degrees (pitch, yaw, roll)
                Vec3 e = j.get<Vec3>();
                set(obj, Quat::fromEuler({radians(e.x), radians(e.y), radians(e.z)}));
                return {};
            }
            return bad("[x, y, z, w] quaternion or [pitch, yaw, roll] degrees");
        case PropertyType::String:
        case PropertyType::AssetRef:
            if (!j.is_string()) return bad("a string");
            set(obj, j.get<std::string>());
            return {};
        case PropertyType::Enum:
            if (j.is_number_integer()) {
                set(obj, j.get<int>());
                return {};
            }
            if (j.is_string()) {
                for (size_t i = 0; i < enumNames.size(); ++i)
                    if (toLower(enumNames[i]) == toLower(j.get<std::string>())) {
                        set(obj, static_cast<int>(i));
                        return {};
                    }
            }
            return bad("one of the enum names");
        case PropertyType::Json:
            set(obj, j);
            return {};
    }
    return Status::failure("unknown property type");
}

Json PropertyInfo::schema() const {
    Json s;
    switch (type) {
        case PropertyType::Bool: s = {{"type", "boolean"}}; break;
        case PropertyType::Int: s = {{"type", "integer"}}; break;
        case PropertyType::Float: s = {{"type", "number"}}; break;
        case PropertyType::Vec3:
        case PropertyType::Color:
            s = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};
            break;
        case PropertyType::Quat:
            s = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 4},
                 {"description", "quaternion [x,y,z,w] or euler degrees [pitch,yaw,roll]"}};
            break;
        case PropertyType::String:
        case PropertyType::AssetRef: s = {{"type", "string"}}; break;
        case PropertyType::Enum: s = {{"type", "string"}, {"enum", enumNames}}; break;
        case PropertyType::Json: s = {{"type", {"array", "object"}}}; break;
    }
    if (!description.empty() && !s.contains("description")) s["description"] = description;
    return s;
}

const PropertyInfo* TypeInfo::find(std::string_view propName) const {
    for (auto& p : properties)
        if (p.name == propName) return &p;
    return nullptr;
}

Json TypeInfo::toJson(const void* obj) const {
    Json j = Json::object();
    for (auto& p : properties) {
        if (p.omitWhenEmpty) {
            PropertyValue v = p.get(obj);
            if (auto* str = std::get_if<std::string>(&v); str && str->empty()) continue;
        }
        j[p.name] = p.toJson(obj);
    }
    return j;
}

void TypeInfo::clearOmitted(void* obj, const Json& j) const {
    for (auto& p : properties)
        if (p.omitWhenEmpty && !(j.is_object() && j.contains(p.name))) p.set(obj, PropertyValue(std::string()));
}

Status TypeInfo::fromJson(void* obj, const Json& j) const {
    if (!j.is_object()) return Status::failure(std::format("component '{}' expects an object", name));
    for (auto& [key, value] : j.items()) {
        const PropertyInfo* p = find(key);
        if (!p) return Status::failure(std::format("component '{}' has no property '{}'", name, key));
        if (auto st = p->fromJson(obj, value); !st) return st;
    }
    return {};
}

Json TypeInfo::describe() const {
    Json props = Json::array();
    for (auto& p : properties) {
        Json pj = {{"name", p.name}, {"type", toString(p.type)}, {"schema", p.schema()}};
        if (!p.description.empty()) pj["description"] = p.description;
        if (p.replicated) pj["replicated"] = true;
        if (p.omitWhenEmpty) pj["optional"] = true;  // saved only when set
        props.push_back(pj);
    }
    return {{"name", name}, {"description", description}, {"properties", props}};
}

} // namespace fledge
