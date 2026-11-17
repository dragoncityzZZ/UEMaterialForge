#include "core/MaterialInstance.h"

#include "core/NodeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace mf {

namespace {
using json = nlohmann::json;

constexpr const char* kInstanceSchema = "materialforge.instance";

PinType TypeFromJson(const json& value, PinType fallback = PinType::Float) {
    if (!value.is_string()) return fallback;
    bool ok = false;
    const PinType type = PinTypeFromString(value.get<std::string>(), &ok);
    return ok ? type : fallback;
}

PropertyValue PropertyFromJson(const json& value, PinType type) {
    if (type == PinType::Float) {
        return PropertyValue::Float(value.is_number() ? value.get<float>() : 0.0f);
    }
    if (type == PinType::Vector2 && value.is_array() && value.size() >= 2) {
        return PropertyValue::Vec2Value(value[0].get<float>(), value[1].get<float>());
    }
    if ((type == PinType::Vector3 || type == PinType::Texture) && value.is_array() &&
        value.size() >= 3) {
        return PropertyValue::Vec3Value(value[0].get<float>(), value[1].get<float>(),
                                        value[2].get<float>());
    }
    if ((type == PinType::Vector4 || type == PinType::Texture) && value.is_array() &&
        value.size() >= 4) {
        return PropertyValue::Color4Value(value[0].get<float>(), value[1].get<float>(),
                                          value[2].get<float>(), value[3].get<float>());
    }
    if (value.is_string()) {
        return type == PinType::Texture ? PropertyValue::AssetPathValue(value.get<std::string>())
                                        : PropertyValue::StringValue(value.get<std::string>());
    }
    return PropertyValue::Float(0.0f);
}

json PropertyToJson(const PropertyValue& value) {
    switch (value.kind) {
    case PropertyKind::Float:
        return value.number;
    case PropertyKind::Vector2:
        return json::array({value.vec2.x, value.vec2.y});
    case PropertyKind::Vector3:
        return json::array({value.vec3.x, value.vec3.y, value.vec3.z});
    case PropertyKind::Vector4:
    case PropertyKind::Color4:
        return json::array({value.vec4.x, value.vec4.y, value.vec4.z, value.vec4.w});
    case PropertyKind::Color3:
        return json::array({value.vec3.x, value.vec3.y, value.vec3.z});
    case PropertyKind::Bool:
        return value.boolean;
    case PropertyKind::String:
    case PropertyKind::AssetPath:
        return value.text;
    }
    return 0.0f;
}
} // namespace

bool MaterialInstance::Load(const std::filesystem::path& path, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (error) *error = "Could not open material instance: " + path.u8string();
        return false;
    }
    json document;
    try {
        stream >> document;
    } catch (const std::exception& exception) {
        if (error) *error = std::string("Material instance parse error: ") + exception.what();
        return false;
    }
    if (!document.is_object() || document.value("schema", std::string()) != kInstanceSchema) {
        if (error) *error = "Not a MaterialForge material instance document.";
        return false;
    }
    if (document.value("version", 0) != 1) {
        if (error) *error = "Unsupported material instance version.";
        return false;
    }

    m_Name = document.value("name", path.stem().u8string());
    m_ParentMaterial = document.value("parentMaterial", std::string());
    m_Overrides.clear();
    if (document.contains("parameters") && document["parameters"].is_array()) {
        for (const json& item : document["parameters"]) {
            if (!item.is_object()) continue;
            MaterialInstanceOverride entry;
            entry.name = item.value("name", std::string());
            if (entry.name.empty()) continue;
            entry.type = TypeFromJson(item.value("type", json("Float")));
            entry.value = PropertyFromJson(item.value("value", json()), entry.type);
            m_Overrides.push_back(std::move(entry));
        }
    }
    if (error) error->clear();
    return true;
}

bool MaterialInstance::Save(const std::filesystem::path& path, std::string* error) const {
    json document;
    document["schema"] = kInstanceSchema;
    document["version"] = 1;
    document["name"] = m_Name;
    document["parentMaterial"] = m_ParentMaterial;
    document["parameters"] = json::array();
    for (const MaterialInstanceOverride& entry : m_Overrides) {
        document["parameters"].push_back({
            {"name", entry.name},
            {"type", PinTypeToString(entry.type)},
            {"value", PropertyToJson(entry.value)},
        });
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error) *error = "Could not write material instance: " + path.u8string();
        return false;
    }
    stream << document.dump(2) << "\n";
    if (!stream) {
        if (error) *error = "Failed while writing material instance: " + path.u8string();
        return false;
    }
    if (error) error->clear();
    return true;
}

bool MaterialInstance::IsOverridden(const std::string& name) const {
    return FindOverride(name) != nullptr;
}

const PropertyValue* MaterialInstance::FindOverride(const std::string& name) const {
    for (const MaterialInstanceOverride& entry : m_Overrides) {
        if (entry.name == name) return &entry.value;
    }
    return nullptr;
}

void MaterialInstance::SetOverride(const std::string& name, PinType type,
                                   const PropertyValue& value) {
    for (MaterialInstanceOverride& entry : m_Overrides) {
        if (entry.name == name) {
            entry.type = type;
            entry.value = value;
            return;
        }
    }
    m_Overrides.push_back({name, type, value});
}

bool MaterialInstance::ClearOverride(const std::string& name) {
    const auto it = std::find_if(m_Overrides.begin(), m_Overrides.end(),
                                 [&](const MaterialInstanceOverride& entry) {
                                     return entry.name == name;
                                 });
    if (it == m_Overrides.end()) return false;
    m_Overrides.erase(it);
    return true;
}

void MaterialInstance::ClearOverrides() {
    m_Overrides.clear();
}

bool MaterialInstance::ApplyToGraph(MaterialGraph& graph,
                                    std::vector<std::string>* warnings) const {
    bool changed = false;
    for (const auto& nodePtr : graph.Nodes()) {
        Node& node = *nodePtr;
        if (node.type != "ScalarParameter" && node.type != "VectorParameter") continue;
        const auto nameIt = node.properties.find("parameterName");
        if (nameIt == node.properties.end() || !IsStringProperty(nameIt->second.kind)) {
            continue;
        }
        const std::string& name = nameIt->second.text;
        const PropertyValue* override = FindOverride(name);
        if (!override) continue;
        const auto valueIt = node.properties.find("value");
        if (valueIt == node.properties.end()) continue;
        if (node.type == "ScalarParameter" && override->kind != PropertyKind::Float) {
            continue;
        }
        if (node.type == "VectorParameter" && override->kind != PropertyKind::Color4 &&
            override->kind != PropertyKind::Vector4) {
            continue;
        }
        valueIt->second = *override;
        changed = true;
    }

    if (warnings) {
        for (const MaterialInstanceOverride& entry : m_Overrides) {
            bool found = false;
            for (const auto& nodePtr : graph.Nodes()) {
                const Node& node = *nodePtr;
                if (node.type != "ScalarParameter" && node.type != "VectorParameter") {
                    continue;
                }
                const auto nameIt = node.properties.find("parameterName");
                if (nameIt != node.properties.end() && IsStringProperty(nameIt->second.kind) &&
                    nameIt->second.text == entry.name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                warnings->push_back("Instance override '" + entry.name +
                                    "' has no matching parent parameter.");
            }
        }
    }
    return changed;
}

} // namespace mf
