#pragma once

#include "core/Graph.h"
#include "core/Property.h"

#include <string>
#include <vector>

namespace mf {

// A parameter exposed by a material graph, together with the editor metadata
// used by Material Instances.
struct MaterialParameterInfo {
    NodeId nodeId = 0;
    std::string name;
    std::string nodeType;
    PinType type = PinType::Float;
    std::string group = "Default";
    int sortPriority = 0;
    std::string description;
    float sliderMin = 0.0f;
    float sliderMax = 1.0f;
    PropertyValue defaultValue{};
};

// Scalar/Vector parameters are returned first, grouped and sorted by priority.
// Texture parameters are included as read-only metadata entries.
std::vector<MaterialParameterInfo> CollectMaterialParameters(
    const MaterialGraph& graph, std::vector<std::string>* warnings = nullptr);

MaterialParameterInfo* FindMaterialParameter(std::vector<MaterialParameterInfo>& parameters,
                                             const std::string& name);
const MaterialParameterInfo* FindMaterialParameter(
    const std::vector<MaterialParameterInfo>& parameters, const std::string& name);

} // namespace mf
