#include "core/MaterialParameters.h"

#include "core/NodeRegistry.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace mf {

namespace {
PropertyValue ReadProperty(const Node& node, const char* key) {
    const auto it = node.properties.find(key);
    return it == node.properties.end() ? PropertyValue{} : it->second;
}

std::string ReadString(const Node& node, const char* key, const std::string& fallback) {
    const PropertyValue value = ReadProperty(node, key);
    if (IsStringProperty(value.kind) && !value.text.empty()) return value.text;
    return fallback;
}

float ReadFloat(const Node& node, const char* key, float fallback) {
    const PropertyValue value = ReadProperty(node, key);
    if (value.kind == PropertyKind::Float) return value.number;
    return fallback;
}

bool IsParameterNode(const std::string& type) {
    return type == "ScalarParameter" || type == "VectorParameter" ||
           type == "TextureObjectParameter" || type == "TextureSampleParameter2D";
}
} // namespace

std::vector<MaterialParameterInfo> CollectMaterialParameters(
    const MaterialGraph& graph, std::vector<std::string>* warnings) {
    std::vector<MaterialParameterInfo> result;
    std::unordered_set<std::string> seenNames;
    for (const auto& nodePtr : graph.Nodes()) {
        const Node& node = *nodePtr;
        if (!IsParameterNode(node.type)) continue;

        MaterialParameterInfo info;
        info.nodeId = node.id;
        info.nodeType = node.type;
        info.name = ReadString(node, "parameterName",
                               node.type == "ScalarParameter" ? "Scalar" : "Parameter");
        if (node.type == "ScalarParameter") {
            info.type = PinType::Float;
            info.defaultValue = PropertyValue::Float(ReadFloat(node, "value", 0.0f));
            info.sliderMin = ReadFloat(node, "sliderMin", 0.0f);
            info.sliderMax = ReadFloat(node, "sliderMax", 1.0f);
        } else if (node.type == "VectorParameter") {
            info.type = PinType::Vector4;
            const PropertyValue value = ReadProperty(node, "value");
            info.defaultValue = value.kind == PropertyKind::Color4 ||
                                        value.kind == PropertyKind::Vector4
                                    ? value
                                    : PropertyValue::Color4Value(1.0f, 1.0f, 1.0f, 1.0f);
        } else {
            info.type = PinType::Texture;
            info.defaultValue = PropertyValue::AssetPathValue(
                ReadString(node, "texture", ""));
        }
        info.group = ReadString(node, "parameterGroup", "Default");
        info.sortPriority =
            static_cast<int>(std::lround(ReadFloat(node, "sortPriority", 0.0f)));
        info.description = ReadString(node, "description", "");

        if (!seenNames.insert(info.name).second) {
            if (warnings) {
                warnings->push_back("Duplicate material parameter name '" + info.name +
                                    "' was ignored in the instance list.");
            }
            continue;
        }
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(),
              [](const MaterialParameterInfo& a, const MaterialParameterInfo& b) {
                  if (a.group != b.group) return a.group < b.group;
                  if (a.sortPriority != b.sortPriority) {
                      return a.sortPriority < b.sortPriority;
                  }
                  return a.name < b.name;
              });
    return result;
}

MaterialParameterInfo* FindMaterialParameter(std::vector<MaterialParameterInfo>& parameters,
                                             const std::string& name) {
    for (MaterialParameterInfo& parameter : parameters) {
        if (parameter.name == name) return &parameter;
    }
    return nullptr;
}

const MaterialParameterInfo* FindMaterialParameter(
    const std::vector<MaterialParameterInfo>& parameters, const std::string& name) {
    for (const MaterialParameterInfo& parameter : parameters) {
        if (parameter.name == name) return &parameter;
    }
    return nullptr;
}

} // namespace mf
