#pragma once

#include "core/Property.h"
#include "core/Types.h"
#include "math/Math.h"

#include <map>
#include <string>
#include <vector>

namespace mf {

struct PinDefinition {
    std::string name;
    PinType type = PinType::Float;
    PropertyValue defaultValue{};
    std::string tooltip;
};

// Controls how a polymorphic (Wildcard) output pin resolves its concrete type.
enum class PinTypeRule : std::uint8_t {
    Fixed = 0,
    PromoteInputs,
    MatchFirstInput,
    AppendInputs,
    ComponentMask
};

struct NodeDefinition {
    std::string type;              // Matches the UE material expression suffix,
                                   // e.g. "Constant3Vector", "TextureSample".
    std::string displayName;
    std::string category;
    std::string description;
    std::string ueClass;           // e.g. "MaterialExpressionTextureSample".
    std::vector<PinDefinition> inputs;
    std::vector<PinDefinition> outputs;
    std::vector<PropertySpec> properties;
    PinTypeRule typeRule = PinTypeRule::Fixed;
    bool isMaterialOutput = false;
    bool isFunctionOnly = false;
    bool hiddenInLibrary = false;
    bool isFunctionCall = false;
    bool isComment = false;
};

struct Pin {
    PinId id = 0;
    NodeId nodeId = 0;
    std::string name;
    PinDirection direction = PinDirection::Input;
    PinType type = PinType::Float;
    PropertyValue defaultValue{};
    std::string tooltip;
    std::string splitFrom;   // Base output pin name when this is a split component.
    int splitIndex = -1;     // 0=R, 1=G, 2=B, 3=A; -1 for a normal pin.
    bool hidden = false;     // Base output hidden while its components are split out.

    bool IsSplitComponent() const { return !splitFrom.empty() && splitIndex >= 0; }
};

struct Node {
    NodeId id = 0;
    std::string type;
    Vec2 position{};
    Vec2 size{280.0f, 160.0f};  // Used by comment group nodes.
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    std::map<std::string, PropertyValue> properties;

    Pin* FindInput(const std::string& name);
    const Pin* FindInput(const std::string& name) const;
    Pin* FindOutput(const std::string& name);
    const Pin* FindOutput(const std::string& name) const;
    Pin* FindPin(PinId id);
    const Pin* FindPin(PinId id) const;

    bool IsMaterialOutput() const { return type == "MaterialOutput"; }
};

struct Link {
    LinkId id = 0;
    PinId fromPin = 0; // Output pin.
    PinId toPin = 0;   // Input pin.
};

struct MaterialSettings {
    std::string name = "M_MaterialForge";
    std::string domain = "Surface";
    std::string blendMode = "Opaque";
    std::string shadingModel = "DefaultLit";
    std::string translucencyLightingMode = "SurfaceForwardShading";
    bool twoSided = false;
    bool ditheredLodTransition = false;
    bool castDynamicShadowAsMasked = false;
    bool useEmissiveForStaticLighting = false;
    float opacityMaskClip = 0.333f;
};

struct GraphStats {
    std::size_t nodeCount = 0;
    std::size_t linkCount = 0;
    std::size_t parameterCount = 0;
    std::size_t textureCount = 0;
};

} // namespace mf
