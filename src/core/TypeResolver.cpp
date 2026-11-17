#include "core/TypeResolver.h"

#include "core/NodeRegistry.h"

#include <algorithm>

namespace mf {

namespace {
PinType FunctionPinType(const Node& node) {
    const auto it = node.properties.find("pinType");
    if (it == node.properties.end() || !IsStringProperty(it->second.kind)) {
        return PinType::Float;
    }
    bool ok = false;
    const PinType type = PinTypeFromString(it->second.text, &ok);
    return ok && IsNumericPin(type) ? type : PinType::Float;
}
} // namespace

TypeResolver::TypeResolver(const MaterialGraph& graph)
    : m_Graph(graph) {}

PinType TypeResolver::GetEffectiveType(const Pin& pin) const {
    return pin.direction == PinDirection::Input ? ResolveInput(pin) : ResolveOutput(pin);
}

PinType TypeResolver::DefaultTypeForPin(const Pin& pin) const {
    const PinType type = pin.defaultValue.NumericPinType();
    if (type == PinType::Wildcard) {
        return PinType::Float;
    }
    return type;
}

PinType TypeResolver::ResolveInput(const Pin& pin) const {
    const Node* owner = m_Graph.FindNode(pin.nodeId);
    if (owner && owner->type == "FunctionOutput") {
        return FunctionPinType(*owner);
    }
    if (pin.type != PinType::Wildcard) {
        return pin.type;
    }
    if (const auto cached = m_Cache.find(pin.id); cached != m_Cache.end()) {
        return cached->second;
    }
    if (!m_Visiting.insert(pin.id).second) {
        return PinType::Float;
    }

    PinType result = DefaultTypeForPin(pin);
    if (const Link* link = m_Graph.FindInputLink(pin.id)) {
        if (const Pin* source = m_Graph.FindPin(link->fromPin)) {
            const PinType sourceType = ResolveOutput(*source);
            if (sourceType != PinType::Wildcard && IsNumericPin(sourceType)) {
                result = sourceType;
            }
        }
    }

    m_Visiting.erase(pin.id);
    m_Cache[pin.id] = result;
    return result;
}

PinType TypeResolver::ResolveOutput(const Pin& pin) const {
    if (pin.type != PinType::Wildcard) {
        return pin.type;
    }
    if (const auto cached = m_Cache.find(pin.id); cached != m_Cache.end()) {
        return cached->second;
    }
    if (!m_Visiting.insert(pin.id).second) {
        return PinType::Float;
    }

    PinType result = PinType::Float;
    const Node* node = m_Graph.FindNode(pin.nodeId);
    const NodeDefinition* definition = node ? NodeRegistry::Get().Find(node->type) : nullptr;
    if (!node || !definition) {
        m_Visiting.erase(pin.id);
        m_Cache[pin.id] = result;
        return result;
    }
    if (node->type == "FunctionInput") {
        result = FunctionPinType(*node);
        m_Visiting.erase(pin.id);
        m_Cache[pin.id] = result;
        return result;
    }

    switch (definition->typeRule) {
    case PinTypeRule::Fixed:
        result = PinType::Float;
        break;
    case PinTypeRule::PromoteInputs: {
        PinType promoted = PinType::Float;
        for (const Pin& input : node->inputs) {
            PinType type = ResolveInput(input);
            if (IsNumericPin(type)) {
                promoted = PromotePinTypes(promoted, type);
            }
        }
        result = promoted;
        break;
    }
    case PinTypeRule::MatchFirstInput: {
        PinType first = PinType::Wildcard;
        for (const Pin& input : node->inputs) {
            const PinType type = ResolveInput(input);
            if (IsNumericPin(type)) {
                first = type;
                break;
            }
        }
        result = (first == PinType::Wildcard) ? PinType::Float : first;
        break;
    }
    case PinTypeRule::AppendInputs: {
        int components = 0;
        for (const Pin& input : node->inputs) {
            components += PinComponentCount(ResolveInput(input));
        }
        result = NumericPinType(std::max(1, std::min(4, components)));
        break;
    }
    case PinTypeRule::ComponentMask: {
        struct MaskChannel {
            const char* key;
        };
        static const MaskChannel channels[] = {{"r"}, {"g"}, {"b"}, {"a"}};
        int components = 0;
        for (const MaskChannel& channel : channels) {
            const auto it = node->properties.find(channel.key);
            if (it != node->properties.end() && it->second.kind == PropertyKind::Bool &&
                it->second.boolean) {
                ++components;
            }
        }
        result = NumericPinType(std::max(1, std::min(4, components)));
        break;
    }
    }

    if (!IsNumericPin(result)) {
        result = PinType::Float;
    }

    m_Visiting.erase(pin.id);
    m_Cache[pin.id] = result;
    return result;
}

} // namespace mf
