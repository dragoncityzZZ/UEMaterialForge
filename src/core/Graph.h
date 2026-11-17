#pragma once

#include "core/Node.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mf {

class MaterialGraph {
public:
    MaterialGraph();
    ~MaterialGraph() = default;

    MaterialGraph(const MaterialGraph& other);
    MaterialGraph& operator=(const MaterialGraph& other);
    MaterialGraph(MaterialGraph&&) noexcept = default;
    MaterialGraph& operator=(MaterialGraph&&) noexcept = default;

    void Clear();

    Node& AddNode(const NodeDefinition& definition, const Vec2& position);
    Node* AddNodeByType(const std::string& type, const Vec2& position, std::string* error = nullptr);
    bool RemoveNode(NodeId nodeId);

    Link* Connect(PinId outputPinId, PinId inputPinId, std::string* error = nullptr);
    bool CanConnect(PinId outputPinId, PinId inputPinId, std::string* reason = nullptr) const;
    bool RemoveLink(LinkId linkId);
    bool DisconnectInput(PinId inputPinId);

    Node* FindNode(NodeId nodeId);
    const Node* FindNode(NodeId nodeId) const;
    Pin* FindPin(PinId pinId);
    const Pin* FindPin(PinId pinId) const;
    Link* FindLink(LinkId linkId);
    const Link* FindLink(LinkId linkId) const;
    const Link* FindInputLink(PinId inputPinId) const;
    bool IsPinConnected(PinId pinId) const;

    Node* FindMaterialOutput();
    const Node* FindMaterialOutput() const;

    // Closest upstream node connected to the given input, if any.
    const Node* FindSourceNode(PinId inputPinId) const;

    const std::vector<std::unique_ptr<Node>>& Nodes() const { return m_Nodes; }
    std::vector<std::unique_ptr<Node>>& Nodes() { return m_Nodes; }
    const std::vector<Link>& Links() const { return m_Links; }
    std::vector<Link>& Links() { return m_Links; }

    void SetNodePosition(NodeId nodeId, const Vec2& position);

    // Call after editing a node property / material setting outside the graph.
    void MarkContentChanged();

    // Document state can change while the compiled material stays identical
    // (for example when only positions move).
    void MarkLayoutChanged();

    std::uint64_t ContentRevision() const { return m_ContentRevision; }
    std::uint64_t LayoutRevision() const { return m_LayoutRevision; }
    bool IsDirty() const { return m_Dirty; }
    void MarkSaved() { m_Dirty = false; }

    MaterialSettings& Settings() { return m_Settings; }
    const MaterialSettings& Settings() const { return m_Settings; }

    GraphStats ComputeStats() const;

    // After deserialization, moves internal counters past every loaded id.
    void RebuildIdCounters();

    NodeId NextNodeId() { return m_NextNodeId++; }
    PinId NextPinId() { return m_NextPinId++; }
    LinkId NextLinkId() { return m_NextLinkId++; }

private:
    void CopyFrom(const MaterialGraph& other);
    bool WouldCreateCycle(NodeId sourceNode, NodeId targetNode) const;
    void RemoveLinksForNode(NodeId nodeId);
    void ResetIdCounters();

    std::vector<std::unique_ptr<Node>> m_Nodes;
    std::vector<Link> m_Links;
    MaterialSettings m_Settings;

    NodeId m_NextNodeId = 1;
    PinId m_NextPinId = 1000;
    LinkId m_NextLinkId = 100000;

    std::uint64_t m_ContentRevision = 1;
    std::uint64_t m_LayoutRevision = 1;
    bool m_Dirty = false;
};

} // namespace mf
