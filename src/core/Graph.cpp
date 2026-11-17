#include "core/Graph.h"

#include "core/NodeRegistry.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace mf {

MaterialGraph::MaterialGraph() = default;

MaterialGraph::MaterialGraph(const MaterialGraph& other) {
    CopyFrom(other);
}

MaterialGraph& MaterialGraph::operator=(const MaterialGraph& other) {
    if (this != &other) {
        CopyFrom(other);
    }
    return *this;
}

void MaterialGraph::CopyFrom(const MaterialGraph& other) {
    m_Nodes.clear();
    m_Nodes.reserve(other.m_Nodes.size());
    for (const auto& node : other.m_Nodes) {
        m_Nodes.push_back(std::make_unique<Node>(*node));
    }
    m_Links = other.m_Links;
    m_Settings = other.m_Settings;
    m_NextNodeId = other.m_NextNodeId;
    m_NextPinId = other.m_NextPinId;
    m_NextLinkId = other.m_NextLinkId;
    m_ContentRevision = other.m_ContentRevision;
    m_LayoutRevision = other.m_LayoutRevision;
    m_Dirty = other.m_Dirty;
}

void MaterialGraph::Clear() {
    m_Nodes.clear();
    m_Links.clear();
    m_Settings = MaterialSettings{};
    ResetIdCounters();
    ++m_ContentRevision;
    ++m_LayoutRevision;
    m_Dirty = false;
}

void MaterialGraph::ResetIdCounters() {
    m_NextNodeId = 1;
    m_NextPinId = 1000;
    m_NextLinkId = 100000;
}

Node& MaterialGraph::AddNode(const NodeDefinition& definition, const Vec2& position) {
    auto node = std::make_unique<Node>();
    node->id = NextNodeId();
    node->type = definition.type;
    node->position = position;

    for (const PinDefinition& input : definition.inputs) {
        Pin pin;
        pin.id = NextPinId();
        pin.nodeId = node->id;
        pin.name = input.name;
        pin.direction = PinDirection::Input;
        pin.type = input.type;
        pin.defaultValue = input.defaultValue;
        pin.tooltip = input.tooltip;
        node->inputs.push_back(std::move(pin));
    }

    for (const PinDefinition& output : definition.outputs) {
        Pin pin;
        pin.id = NextPinId();
        pin.nodeId = node->id;
        pin.name = output.name;
        pin.direction = PinDirection::Output;
        pin.type = output.type;
        pin.defaultValue = output.defaultValue;
        pin.tooltip = output.tooltip;
        node->outputs.push_back(std::move(pin));
    }

    for (const PropertySpec& property : definition.properties) {
        node->properties.emplace(property.key, property.defaultValue);
    }

    Node& ref = *node;
    m_Nodes.push_back(std::move(node));
    MarkContentChanged();
    return ref;
}

Node* MaterialGraph::AddNodeByType(const std::string& type, const Vec2& position, std::string* error) {
    const NodeDefinition* definition = NodeRegistry::Get().Find(type);
    if (!definition) {
        if (error) *error = "Unknown node type: " + type;
        return nullptr;
    }
    if (definition->isMaterialOutput && FindMaterialOutput()) {
        if (error) *error = "The graph already contains a Material Output node.";
        return nullptr;
    }
    return &AddNode(*definition, position);
}

bool MaterialGraph::RemoveNode(NodeId nodeId) {
    auto it = std::find_if(m_Nodes.begin(), m_Nodes.end(),
                           [nodeId](const std::unique_ptr<Node>& node) { return node->id == nodeId; });
    if (it == m_Nodes.end()) {
        return false;
    }

    RemoveLinksForNode(nodeId);
    m_Nodes.erase(it);
    MarkContentChanged();
    return true;
}

void MaterialGraph::RemoveLinksForNode(NodeId nodeId) {
    m_Links.erase(std::remove_if(m_Links.begin(), m_Links.end(), [&](const Link& link) {
        const Pin* from = FindPin(link.fromPin);
        const Pin* to = FindPin(link.toPin);
        return (from && from->nodeId == nodeId) || (to && to->nodeId == nodeId);
    }), m_Links.end());
}

bool MaterialGraph::CanConnect(PinId outputPinId, PinId inputPinId, std::string* reason) const {
    const Pin* outputPin = FindPin(outputPinId);
    const Pin* inputPin = FindPin(inputPinId);

    if (!outputPin || !inputPin) {
        if (reason) *reason = "Pin does not exist.";
        return false;
    }
    if (outputPin->direction != PinDirection::Output || inputPin->direction != PinDirection::Input) {
        if (reason) *reason = "Links must go from an output pin to an input pin.";
        return false;
    }
    if (outputPin->nodeId == inputPin->nodeId) {
        if (reason) *reason = "A node cannot be connected to itself.";
        return false;
    }
    if (!ArePinTypesLinkCompatible(outputPin->type, inputPin->type)) {
        if (reason) {
            *reason = std::string("Incompatible pin types: ") + PinTypeToString(outputPin->type) +
                      " -> " + PinTypeToString(inputPin->type) + ".";
        }
        return false;
    }
    if (WouldCreateCycle(outputPin->nodeId, inputPin->nodeId)) {
        if (reason) *reason = "That connection would create a cycle.";
        return false;
    }
    return true;
}

bool MaterialGraph::WouldCreateCycle(NodeId sourceNode, NodeId targetNode) const {
    if (sourceNode == targetNode) {
        return true;
    }

    // Follow existing edges downstream from the target. If we can reach the
    // source node, adding source -> target would close a cycle.
    std::vector<NodeId> stack{targetNode};
    std::unordered_set<NodeId> visited;
    while (!stack.empty()) {
        const NodeId current = stack.back();
        stack.pop_back();
        if (!visited.insert(current).second) {
            continue;
        }
        for (const Link& link : m_Links) {
            const Pin* from = FindPin(link.fromPin);
            const Pin* to = FindPin(link.toPin);
            if (!from || !to || from->nodeId != current) {
                continue;
            }
            if (to->nodeId == sourceNode) {
                return true;
            }
            stack.push_back(to->nodeId);
        }
    }
    return false;
}

Link* MaterialGraph::Connect(PinId outputPinId, PinId inputPinId, std::string* error) {
    if (!CanConnect(outputPinId, inputPinId, error)) {
        return nullptr;
    }

    // UE material inputs accept a single connection; a new link replaces the
    // old one.
    DisconnectInput(inputPinId);

    Link link;
    link.id = NextLinkId();
    link.fromPin = outputPinId;
    link.toPin = inputPinId;
    m_Links.push_back(link);
    MarkContentChanged();
    return &m_Links.back();
}

bool MaterialGraph::RemoveLink(LinkId linkId) {
    auto it = std::find_if(m_Links.begin(), m_Links.end(),
                           [linkId](const Link& link) { return link.id == linkId; });
    if (it == m_Links.end()) {
        return false;
    }
    m_Links.erase(it);
    MarkContentChanged();
    return true;
}

bool MaterialGraph::DisconnectInput(PinId inputPinId) {
    const auto it = std::remove_if(m_Links.begin(), m_Links.end(),
                                   [inputPinId](const Link& link) { return link.toPin == inputPinId; });
    if (it == m_Links.end()) {
        return false;
    }
    m_Links.erase(it, m_Links.end());
    MarkContentChanged();
    return true;
}

Node* MaterialGraph::FindNode(NodeId nodeId) {
    for (auto& node : m_Nodes) {
        if (node->id == nodeId) return node.get();
    }
    return nullptr;
}

const Node* MaterialGraph::FindNode(NodeId nodeId) const {
    for (const auto& node : m_Nodes) {
        if (node->id == nodeId) return node.get();
    }
    return nullptr;
}

Pin* MaterialGraph::FindPin(PinId pinId) {
    for (auto& node : m_Nodes) {
        if (Pin* pin = node->FindPin(pinId)) {
            return pin;
        }
    }
    return nullptr;
}

const Pin* MaterialGraph::FindPin(PinId pinId) const {
    for (const auto& node : m_Nodes) {
        if (const Pin* pin = node->FindPin(pinId)) {
            return pin;
        }
    }
    return nullptr;
}

Link* MaterialGraph::FindLink(LinkId linkId) {
    for (Link& link : m_Links) {
        if (link.id == linkId) return &link;
    }
    return nullptr;
}

const Link* MaterialGraph::FindLink(LinkId linkId) const {
    for (const Link& link : m_Links) {
        if (link.id == linkId) return &link;
    }
    return nullptr;
}

const Link* MaterialGraph::FindInputLink(PinId inputPinId) const {
    for (const Link& link : m_Links) {
        if (link.toPin == inputPinId) return &link;
    }
    return nullptr;
}

bool MaterialGraph::IsPinConnected(PinId pinId) const {
    for (const Link& link : m_Links) {
        if (link.fromPin == pinId || link.toPin == pinId) return true;
    }
    return false;
}

Node* MaterialGraph::FindMaterialOutput() {
    for (auto& node : m_Nodes) {
        if (node->IsMaterialOutput()) return node.get();
    }
    return nullptr;
}

const Node* MaterialGraph::FindMaterialOutput() const {
    for (const auto& node : m_Nodes) {
        if (node->IsMaterialOutput()) return node.get();
    }
    return nullptr;
}

const Node* MaterialGraph::FindSourceNode(PinId inputPinId) const {
    const Link* link = FindInputLink(inputPinId);
    if (!link) {
        return nullptr;
    }
    const Pin* from = FindPin(link->fromPin);
    return from ? FindNode(from->nodeId) : nullptr;
}

void MaterialGraph::SetNodePosition(NodeId nodeId, const Vec2& position) {
    Node* node = FindNode(nodeId);
    if (!node) {
        return;
    }
    if (node->position.x == position.x && node->position.y == position.y) {
        return;
    }
    node->position = position;
    MarkLayoutChanged();
}

void MaterialGraph::MarkContentChanged() {
    ++m_ContentRevision;
    m_Dirty = true;
}

void MaterialGraph::MarkLayoutChanged() {
    ++m_LayoutRevision;
    m_Dirty = true;
}

GraphStats MaterialGraph::ComputeStats() const {
    GraphStats stats;
    stats.nodeCount = m_Nodes.size();
    stats.linkCount = m_Links.size();
    for (const auto& node : m_Nodes) {
        if (node->type == "ScalarParameter" || node->type == "VectorParameter") {
            ++stats.parameterCount;
        }
        if (node->type == "TextureObject") {
            ++stats.textureCount;
        }
    }
    return stats;
}

void MaterialGraph::RebuildIdCounters() {
    NodeId maxNode = 0;
    PinId maxPin = 0;
    LinkId maxLink = 0;
    for (const auto& node : m_Nodes) {
        maxNode = std::max(maxNode, node->id);
        for (const Pin& pin : node->inputs) maxPin = std::max(maxPin, pin.id);
        for (const Pin& pin : node->outputs) maxPin = std::max(maxPin, pin.id);
    }
    for (const Link& link : m_Links) {
        maxLink = std::max(maxLink, link.id);
    }
    m_NextNodeId = maxNode + 1;
    m_NextPinId = maxPin + 1;
    m_NextLinkId = maxLink + 1;
}

} // namespace mf
