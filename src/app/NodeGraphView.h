#pragma once

#include "core/Graph.h"

#include <imgui.h>
#include <imgui_node_editor.h>

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace mf {

// Owns the ax::NodeEditor canvas and translates its transient interactions into
// MaterialGraph mutations (create / replace links, delete nodes and links, add
// nodes from the searchable palette).
class NodeGraphView {
public:
    explicit NodeGraphView(MaterialGraph& graph);
    ~NodeGraphView();

    NodeGraphView(const NodeGraphView&) = delete;
    NodeGraphView& operator=(const NodeGraphView&) = delete;

    enum class AlignMode {
        Left,
        CenterHorizontal,
        Right,
        Top,
        CenterVertical,
        Bottom
    };

    struct Bookmark {
        std::string name;
        NodeId nodeId = 0;
    };

    void Initialize();
    void Shutdown();

    // Call after loading or replacing the whole graph. Rebuilds the editor
    // context so stale IDs/selection from the previous document are discarded.
    void OnGraphReplaced();

    void Draw(const ImVec2& size);

    void SetFunctionMode(bool functionMode) { m_FunctionMode = functionMode; }
    bool IsFunctionMode() const { return m_FunctionMode; }

    Node* AddNodeAtViewCenter(const std::string& type, std::string* error = nullptr);
    Node* CreateFunctionCallNodeAt(const std::string& functionPath,
                                   const ImVec2& canvasPosition,
                                   std::string* error = nullptr);
    Node* AddFunctionCallAtViewCenter(const std::string& functionPath,
                                      std::string* error = nullptr);
    void FrameAll();
    void FrameSelection();
    void SelectNode(NodeId id);
    void SelectAllNodes();
    std::string TakeFunctionOpenRequest();
    bool TakeTexturePickerRequest(NodeId* nodeId, PinId* pinId);
    bool TakeNodePreviewRequest(NodeId* nodeId, PinId* pinId);
    bool ApplyTextureToInput(NodeId nodeId, PinId pinId, const std::string& texturePath,
                             std::string* error = nullptr);
    bool ConvertConstantNodeToParameter(NodeId nodeId, std::string* error = nullptr);

    NodeId SelectedNodeId() const { return m_SelectedNodeId; }
    std::string TakeLastError();
    std::string TakeLastMessage();

    std::vector<NodeId> SelectedNodeIds() const;
    int SelectedNodeCount() const;
    void AlignSelected(AlignMode mode);
    void DistributeSelected(bool horizontal);
    void AddCommentAtViewCenter();
    void GroupSelectedNodes();
    bool CollapseSelectedToFunction(const std::string& functionPath,
                                    std::string* error = nullptr);
    void AddBookmarkFromSelection();
    void GoToBookmark(std::size_t index);
    void RemoveBookmark(std::size_t index);
    const std::vector<Bookmark>& Bookmarks() const { return m_Bookmarks; }

private:
    void CreateEditorContext();
    void DestroyEditorContext();
    void DrawNode(Node& node, class TypeResolver& resolver);
    void DrawCreateAndDeleteInteractions();
    void DrawAddNodePopup();
    void DrawNodeDropTarget();
    void TryNodeShortcuts();
    void TryClipboardShortcuts();
    void CopySelectedNodes();
    void PasteNodesAtMouse();
    void PasteUe5NodesAtCanvas(const std::string& text, const ImVec2& canvasPosition);
    Node* CreateNodeAt(const std::string& type, const ImVec2& canvasPosition,
                       std::string* error = nullptr);
    void CreateTextureNodesAt(const std::string& texturePath,
                              const ImVec2& canvasPosition);
    void RequestTexturePicker(NodeId nodeId, PinId pinId);
    bool AttachTextureToPin(Node& node, Pin& pin, const std::string& texturePath,
                            std::string* error);

    struct ClipboardNode {
        std::string type;
        Vec2 position;
        Vec2 size;
        std::map<std::string, PropertyValue> properties;
        std::vector<Pin> inputs;
        std::vector<Pin> outputs;
    };
    struct ClipboardLink {
        std::size_t fromNode = 0;
        std::size_t toNode = 0;
        std::string fromPin;
        std::string toPin;
    };
    std::vector<ClipboardNode> m_ClipboardNodes;
    std::vector<ClipboardLink> m_ClipboardLinks;
    void SyncPositions();
    void UpdateSelection();
    void ApplyPendingSelection();
    void SplitOutputPin(Node& node, Pin& base);
    void MergeOutputPin(Node& node, const Pin& base);

    MaterialGraph& m_Graph;
    ax::NodeEditor::EditorContext* m_Context = nullptr;
    ax::NodeEditor::Config m_EditorConfig;

    NodeId m_SelectedNodeId = 0;
    NodeId m_PendingSelectNode = 0;
    std::vector<NodeId> m_PendingSelectNodes;
    NodeId m_FocusNode = 0;
    ax::NodeEditor::NodeId m_ContextNode = 0;
    ax::NodeEditor::PinId m_ContextPin = 0;
    ax::NodeEditor::LinkId m_ContextLink = 0;
    ImVec2 m_ContextCanvasPos{};
    ImVec2 m_AddNodeCanvasPos{};
    ImVec2 m_ViewCenterCanvas{};
    ImVec2 m_EditorScreenPos{};
    ImVec2 m_EditorSize{};
    std::unordered_set<NodeId> m_PendingPositions;
    bool m_PendingFrameAll = true;
    bool m_PendingFrameSelection = false;
    bool m_FocusAddSearch = false;
    bool m_FunctionMode = false;
    char m_AddSearch[128] = {};
    std::string m_LastError;
    std::string m_LastMessage;
    std::string m_FunctionOpenRequest;
    NodeId m_TexturePickerNode = 0;
    PinId m_TexturePickerPin = 0;
    NodeId m_NodePreviewRequestNode = 0;
    PinId m_NodePreviewRequestPin = 0;
    bool m_TexturePinDropHandled = false;
    std::vector<Bookmark> m_Bookmarks;
    std::vector<NodeId> m_SelectedNodes;
};

} // namespace mf
