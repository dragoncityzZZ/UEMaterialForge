#include "app/NodeGraphView.h"

#include "app/Localization.h"
#include "app/NodeSearch.h"
#include "app/NodeThumbnail.h"
#include "app/UiWidgets.h"
#include "core/NodeRegistry.h"
#include "core/TypeResolver.h"
#include "io/GraphSerializer.h"
#include "io/Ue5Clipboard.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ed = ax::NodeEditor;

namespace mf {
namespace {


ed::NodeId ToEditorNode(NodeId id) {
    return ed::NodeId(static_cast<uintptr_t>(id));
}

ed::PinId ToEditorPin(PinId id) {
    return ed::PinId(static_cast<uintptr_t>(id));
}

ed::LinkId ToEditorLink(LinkId id) {
    return ed::LinkId(static_cast<uintptr_t>(id));
}

std::string Trim(const std::string& input) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

std::string LocalizedPopupId(const char* label, const char* stableId) {
    return std::string(Tr(label)) + "###" + stableId;
}

float CanvasUiScale() {
    return std::max(0.25f, Ui::Scale());
}

std::string UniquePinName(const std::vector<Pin>& pins, std::string base) {
    if (base.empty()) base = "Value";
    std::string candidate = base;
    int suffix = 2;
    auto taken = [&](const std::string& name) {
        return std::any_of(pins.begin(), pins.end(),
                           [&](const Pin& pin) { return pin.name == name; });
    };
    while (taken(candidate)) {
        candidate = base + "_" + std::to_string(suffix++);
    }
    return candidate;
}

std::string FunctionNodeName(const Node& node, const char* property, const char* fallback) {
    const auto it = node.properties.find(property);
    if (it != node.properties.end() && IsStringProperty(it->second.kind) &&
        !it->second.text.empty()) {
        return it->second.text;
    }
    return fallback;
}

PinType FunctionNodePinType(const Node& node) {
    const auto it = node.properties.find("pinType");
    if (it == node.properties.end() || !IsStringProperty(it->second.kind)) {
        return PinType::Float;
    }
    bool ok = false;
    const PinType type = PinTypeFromString(it->second.text, &ok);
    return ok && IsNumericPin(type) ? type : PinType::Float;
}

ImVec4 LinkColorFor(const Pin& outputPin, TypeResolver& resolver) {
    return Ui::PinTypeColor(resolver.GetEffectiveType(outputPin), true);
}

void ShowPinTooltip(const Pin& pin, PinType effectiveType, bool connected) {
    const ImVec2 anchor = ImGui::GetItemRectMin();
    ImGui::SetNextWindowPos(ImVec2(anchor.x + Ui::S(12.0f),
                                   ImGui::GetItemRectMax().y + Ui::S(4.0f)),
                            ImGuiCond_Always);
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(pin.name.c_str());
    ImGui::TextColored(Ui::PinTypeColor(effectiveType, true), "%s  ·  %s",
                       pin.direction == PinDirection::Input ? Tr("Input") : Tr("Output"),
                       Ui::PinTypeShortName(effectiveType));
    ImGui::TextDisabled("%s", connected ? Tr("Connected") : Tr("Not connected"));
    if (!pin.tooltip.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", Tr(pin.tooltip.c_str()));
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Tr("Pin preview"));
    const float previewWidth = 46.0f;
    const float previewHeight = 26.0f;
    ImGui::Dummy(ImVec2(previewWidth, previewHeight));
    Ui::DrawPinThumbnail(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(),
                         previewWidth, previewHeight, effectiveType, pin.defaultValue);
    ImGui::EndTooltip();
}

} // namespace

NodeGraphView::NodeGraphView(MaterialGraph& graph)
    : m_Graph(graph) {
    // The editor keeps a pointer to this config for its lifetime, so it must
    // live in the view and must not be a local in CreateEditorContext.
    m_EditorConfig.SettingsFile = nullptr;
    m_EditorConfig.EnableSmoothZoom = true;
}

NodeGraphView::~NodeGraphView() {
    Shutdown();
}

void NodeGraphView::Initialize() {
    CreateEditorContext();
    OnGraphReplaced();
}

void NodeGraphView::Shutdown() {
    DestroyEditorContext();
}

void NodeGraphView::DestroyEditorContext() {
    if (m_Context) {
        if (ed::GetCurrentEditor() == m_Context) {
            ed::SetCurrentEditor(nullptr);
        }
        ed::DestroyEditor(m_Context);
        m_Context = nullptr;
    }
}

void NodeGraphView::CreateEditorContext() {
    DestroyEditorContext();
    m_Context = ed::CreateEditor(&m_EditorConfig);
    if (!m_Context) {
        return;
    }

    ed::SetCurrentEditor(m_Context);
    ed::Style& style = ed::GetStyle();
    style.NodePadding = ImVec4(Ui::S(12.0f), Ui::S(10.0f), Ui::S(12.0f), Ui::S(10.0f));
    style.NodeRounding = Ui::S(9.0f);
    style.NodeBorderWidth = Ui::S(1.2f);
    style.HoveredNodeBorderWidth = Ui::S(2.4f);
    style.SelectedNodeBorderWidth = Ui::S(2.4f);
    const ImVec4 windowBg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const ImVec4 childBg = ImGui::GetStyleColorVec4(ImGuiCol_ChildBg);
    style.Colors[ed::StyleColor_Bg] =
        ImVec4(windowBg.x * 0.92f, windowBg.y * 0.92f, windowBg.z * 0.92f, 1.0f);
    style.Colors[ed::StyleColor_Grid] = ImVec4(0.52f, 0.56f, 0.60f, 0.13f);
    style.Colors[ed::StyleColor_NodeBg] =
        ImVec4(childBg.x * 1.12f, childBg.y * 1.12f, childBg.z * 1.12f, 1.0f);
    style.Colors[ed::StyleColor_NodeBorder] = ImVec4(0.32f, 0.35f, 0.38f, 1.0f);
    style.Colors[ed::StyleColor_HovNodeBorder] = ImVec4(0.25f, 0.82f, 0.75f, 1.0f);
    style.Colors[ed::StyleColor_SelNodeBorder] = ImVec4(0.98f, 0.72f, 0.31f, 1.0f);
    style.Colors[ed::StyleColor_NodeSelRect] = ImVec4(0.20f, 0.75f, 0.68f, 0.10f);
    style.Colors[ed::StyleColor_NodeSelRectBorder] = ImVec4(0.25f, 0.82f, 0.75f, 0.55f);
    style.Colors[ed::StyleColor_HovLinkBorder] = ImVec4(0.39f, 0.88f, 0.81f, 1.0f);
    style.Colors[ed::StyleColor_SelLinkBorder] = ImVec4(0.98f, 0.72f, 0.31f, 1.0f);
    style.Colors[ed::StyleColor_HighlightLinkBorder] = ImVec4(0.98f, 0.80f, 0.50f, 1.0f);
    style.Colors[ed::StyleColor_LinkSelRect] = ImVec4(0.20f, 0.75f, 0.68f, 0.10f);
    style.Colors[ed::StyleColor_LinkSelRectBorder] = ImVec4(0.25f, 0.82f, 0.75f, 0.55f);
    style.Colors[ed::StyleColor_PinRect] = ImVec4(0.24f, 0.78f, 0.72f, 0.28f);
    style.Colors[ed::StyleColor_PinRectBorder] = ImVec4(0.24f, 0.78f, 0.72f, 0.50f);
    style.Colors[ed::StyleColor_Flow] = ImVec4(0.98f, 0.72f, 0.31f, 1.0f);
    style.Colors[ed::StyleColor_FlowMarker] = ImVec4(0.98f, 0.72f, 0.31f, 1.0f);
    ed::SetCurrentEditor(nullptr);
}

void NodeGraphView::OnGraphReplaced() {
    CreateEditorContext();
    m_PendingPositions.clear();
    for (const auto& node : m_Graph.Nodes()) {
        m_PendingPositions.insert(node->id);
    }
    // Prefer a parameter for the initial inspector view so the document opens
    // with an actionable property panel. Fall back to the material output.
    const Node* initialSelection = nullptr;
    if (m_FunctionMode) {
        for (const auto& node : m_Graph.Nodes()) {
            if (node->type == "FunctionInput") {
                initialSelection = node.get();
                break;
            }
        }
    }
    if (!initialSelection) {
        for (const auto& node : m_Graph.Nodes()) {
            if (node->type == "VectorParameter") {
                initialSelection = node.get();
                break;
            }
        }
    }
    if (!initialSelection) {
        initialSelection = m_Graph.FindMaterialOutput();
    }
    if (!initialSelection && !m_Graph.Nodes().empty()) {
        initialSelection = m_Graph.Nodes().front().get();
    }
    m_SelectedNodeId = initialSelection ? initialSelection->id : 0;
    m_PendingSelectNode = m_SelectedNodeId;
    m_PendingFrameAll = true;
    m_PendingFrameSelection = false;
    m_PendingCenterNode = 0;
    m_ContextNode = 0;
    m_ContextLink = 0;
    m_AddSearch[0] = '\0';
    m_ViewCenterCanvas = ImVec2(0.0f, 0.0f);
}

void NodeGraphView::OnGraphReplacedPreservingView() {
    const std::vector<NodeId> selected = m_SelectedNodes;
    const NodeId primary = m_SelectedNodeId;
    OnGraphReplaced();
    if (!selected.empty()) {
        m_PendingSelectNode = 0;
        m_PendingSelectNodes = selected;
    } else if (primary != 0 && m_Graph.FindNode(primary)) {
        m_PendingSelectNode = primary;
    }
    m_PendingFrameAll = false;
    m_PendingFrameSelection = false;
    m_PendingCenterNode = primary;
}

Node* NodeGraphView::AddNodeAtViewCenter(const std::string& type, std::string* error) {
    const NodeDefinition* definition = NodeRegistry::Get().Find(type);
    if (!definition) {
        if (error) *error = "Unknown node type: " + type;
        return nullptr;
    }
    const float scale = CanvasUiScale();
    Vec2 position(m_ViewCenterCanvas.x / scale - 160.0f,
                  m_ViewCenterCanvas.y / scale - 80.0f);
    const float offset = static_cast<float>(m_Graph.Nodes().size() % 5) * 26.0f;
    position.x += offset;
    position.y += offset;
    Node* node = m_Graph.AddNodeByType(type, position, error);
    if (!node) {
        return nullptr;
    }
    m_PendingPositions.insert(node->id);
    m_PendingSelectNode = node->id;
    m_SelectedNodeId = node->id;
    return node;
}

Node* NodeGraphView::CreateFunctionCallNodeAt(const std::string& functionPath,
                                              const ImVec2& canvasPosition,
                                              std::string* error) {
    if (functionPath.empty()) {
        if (error) *error = "The material function path is empty.";
        return nullptr;
    }
    MaterialGraph functionGraph;
    std::string loadError;
    if (!GraphSerializer::Load(functionPath, functionGraph, &loadError)) {
        if (error) *error = "Could not load material function: " + loadError;
        return nullptr;
    }
    const NodeDefinition* definition =
        NodeRegistry::Get().Find("MaterialFunctionCall");
    if (!definition) {
        if (error) *error = "MaterialFunctionCall node type is not registered.";
        return nullptr;
    }

    const float scale = CanvasUiScale();
    Node& node = m_Graph.AddNode(
        *definition, Vec2(canvasPosition.x / scale, canvasPosition.y / scale));
    const std::filesystem::path path(functionPath);
    const std::string functionName = path.stem().string();
    node.properties["functionPath"] = PropertyValue::StringValue(functionPath);
    node.properties["functionName"] = PropertyValue::StringValue(functionName);

    auto addPin = [&](const std::string& baseName, PinDirection direction,
                      PinType type = PinType::Float) {
        Pin pin;
        pin.id = m_Graph.NextPinId();
        pin.nodeId = node.id;
        pin.name = direction == PinDirection::Input
                       ? UniquePinName(node.inputs, baseName)
                       : UniquePinName(node.outputs, baseName);
        pin.direction = direction;
        pin.type = type;
        switch (type) {
        case PinType::Vector2: pin.defaultValue = PropertyValue::Vec2Value(0.0f, 0.0f); break;
        case PinType::Vector3: pin.defaultValue = PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f); break;
        case PinType::Vector4:
            pin.defaultValue = PropertyValue::Vec4Value(0.0f, 0.0f, 0.0f, 0.0f);
            break;
        default: pin.defaultValue = PropertyValue::Float(0.0f); break;
        }
        if (direction == PinDirection::Input) {
            node.inputs.push_back(std::move(pin));
        } else {
            node.outputs.push_back(std::move(pin));
        }
    };

    for (const auto& functionNode : functionGraph.Nodes()) {
        if (functionNode->type == "FunctionInput") {
            addPin(FunctionNodeName(*functionNode, "inputName", "Input"),
                   PinDirection::Input, FunctionNodePinType(*functionNode));
        }
    }
    for (const auto& functionNode : functionGraph.Nodes()) {
        if (functionNode->type == "FunctionOutput") {
            addPin(FunctionNodeName(*functionNode, "outputName", "Result"),
                   PinDirection::Output, FunctionNodePinType(*functionNode));
        }
    }
    if (node.inputs.empty()) {
        addPin("Input", PinDirection::Input);
    }
    if (node.outputs.empty()) {
        addPin("Result", PinDirection::Output);
    }

    m_Graph.MarkContentChanged();
    m_PendingPositions.insert(node.id);
    m_PendingSelectNode = node.id;
    m_SelectedNodeId = node.id;
    return &node;
}

Node* NodeGraphView::AddFunctionCallAtViewCenter(const std::string& functionPath,
                                                 std::string* error) {
    const float scale = CanvasUiScale();
    const float offset = static_cast<float>(m_Graph.Nodes().size() % 5) * 26.0f;
    const ImVec2 position(m_ViewCenterCanvas.x + offset * scale,
                          m_ViewCenterCanvas.y + offset * scale);
    return CreateFunctionCallNodeAt(functionPath, position, error);
}

std::string NodeGraphView::TakeLastError() {
    std::string error = std::move(m_LastError);
    m_LastError.clear();
    return error;
}

std::string NodeGraphView::TakeLastMessage() {
    std::string message = std::move(m_LastMessage);
    m_LastMessage.clear();
    return message;
}

std::string NodeGraphView::TakeFunctionOpenRequest() {
    std::string request = std::move(m_FunctionOpenRequest);
    m_FunctionOpenRequest.clear();
    return request;
}

bool NodeGraphView::TakeTexturePickerRequest(NodeId* nodeId, PinId* pinId) {
    if (m_TexturePickerNode == 0 || m_TexturePickerPin == 0) return false;
    if (nodeId) *nodeId = m_TexturePickerNode;
    if (pinId) *pinId = m_TexturePickerPin;
    m_TexturePickerNode = 0;
    m_TexturePickerPin = 0;
    return true;
}

bool NodeGraphView::TakeNodePreviewRequest(NodeId* nodeId, PinId* pinId) {
    if (m_NodePreviewRequestNode == 0) return false;
    if (nodeId) *nodeId = m_NodePreviewRequestNode;
    if (pinId) *pinId = m_NodePreviewRequestPin;
    m_NodePreviewRequestNode = 0;
    m_NodePreviewRequestPin = 0;
    return true;
}

bool NodeGraphView::ConvertConstantNodeToParameter(NodeId nodeId, std::string* error) {
    if (error) error->clear();
    Node* old = m_Graph.FindNode(nodeId);
    if (!old) {
        if (error) *error = "The selected node no longer exists.";
        return false;
    }
    std::string targetType;
    if (old->type == "Constant") {
        targetType = "ScalarParameter";
    } else if (old->type == "Constant2Vector" || old->type == "Constant3Vector" ||
               old->type == "Constant4Vector") {
        targetType = "VectorParameter";
    } else {
        if (error) *error = "Only constant nodes can be converted to parameters.";
        return false;
    }
    const NodeDefinition* definition = NodeRegistry::Get().Find(targetType);
    const Pin* oldOutput = old->FindOutput("Value");
    if (!definition || !oldOutput) {
        if (error) *error = "The parameter node definition is unavailable.";
        return false;
    }

    std::vector<PinId> downstreamInputs;
    for (const Link& link : m_Graph.Links()) {
        if (link.fromPin == oldOutput->id) downstreamInputs.push_back(link.toPin);
    }

    std::unordered_set<std::string> usedNames;
    for (const auto& node : m_Graph.Nodes()) {
        const auto it = node->properties.find("parameterName");
        if (it != node->properties.end() && IsStringProperty(it->second.kind)) {
            usedNames.insert(it->second.text);
        }
    }
    const std::string baseName = targetType == "ScalarParameter" ? "Param" : "VectorParam";
    std::string parameterName = baseName;
    for (int suffix = 1; usedNames.count(parameterName) != 0; ++suffix) {
        parameterName = baseName + std::to_string(suffix);
    }

    const PropertyValue oldValue =
        old->properties.count("value") ? old->properties.at("value") : PropertyValue{};
    const Vec2 position = old->position;
    const Vec2 size = old->size;
    Node& created = m_Graph.AddNode(*definition, position);
    created.size = size;
    created.properties["parameterName"] = PropertyValue::StringValue(parameterName);
    created.properties["parameterGroup"] = PropertyValue::StringValue("Default");
    created.properties["sortPriority"] = PropertyValue::Float(0.0f);
    created.properties["description"] =
        PropertyValue::StringValue("Converted from a constant node.");
    if (targetType == "ScalarParameter") {
        const float value =
            oldValue.kind == PropertyKind::Float ? oldValue.number : 0.5f;
        created.properties["value"] = PropertyValue::Float(value);
        created.properties["sliderMin"] = PropertyValue::Float(0.0f);
        created.properties["sliderMax"] =
            PropertyValue::Float(std::max(1.0f, std::abs(value) * 2.0f));
    } else {
        PropertyValue value = PropertyValue::Color4Value(1.0f, 1.0f, 1.0f, 1.0f);
        if (oldValue.kind == PropertyKind::Vector2) {
            value = PropertyValue::Color4Value(oldValue.vec2.x, oldValue.vec2.y, 0.0f, 1.0f);
        } else if (oldValue.kind == PropertyKind::Vector3 ||
                   oldValue.kind == PropertyKind::Color3) {
            value = PropertyValue::Color4Value(oldValue.vec3.x, oldValue.vec3.y,
                                               oldValue.vec3.z, 1.0f);
        } else if (oldValue.kind == PropertyKind::Vector4 ||
                   oldValue.kind == PropertyKind::Color4) {
            value = PropertyValue::Color4Value(oldValue.vec4.x, oldValue.vec4.y,
                                               oldValue.vec4.z, oldValue.vec4.w);
        }
        created.properties["value"] = value;
    }
    const NodeId createdId = created.id;
    Pin* createdOutput = created.FindOutput("Value");
    if (!createdOutput) {
        m_Graph.RemoveNode(createdId);
        if (error) *error = "The parameter node has no output pin.";
        return false;
    }
    const PinId createdOutputId = createdOutput->id;
    m_Graph.RemoveNode(nodeId);
    for (PinId inputId : downstreamInputs) {
        std::string connectError;
        if (!m_Graph.Connect(createdOutputId, inputId, &connectError) && error &&
            error->empty()) {
            *error = connectError;
        }
    }
    m_Graph.MarkContentChanged();
    m_PendingPositions.insert(createdId);
    m_PendingSelectNode = createdId;
    m_SelectedNodeId = createdId;
    m_LastMessage = std::string(Tr("Converted to parameter: ")) + parameterName;
    return true;
}

void NodeGraphView::RequestTexturePicker(NodeId nodeId, PinId pinId) {
    m_TexturePickerNode = nodeId;
    m_TexturePickerPin = pinId;
}

bool NodeGraphView::AttachTextureToPin(Node& node, Pin& pin, const std::string& texturePath,
                                       std::string* error) {
    if (pin.direction != PinDirection::Input ||
        (pin.type != PinType::Texture && pin.type != PinType::Wildcard)) {
        if (error) *error = "This input does not accept a texture.";
        return false;
    }
    if (texturePath.empty()) {
        if (error) *error = "The texture path is empty.";
        return false;
    }
    const auto property = node.properties.find("texture");
    if (property != node.properties.end()) {
        // TextureSampleParameter2D samples its own texture property when the
        // Texture input is unconnected.
        property->second = PropertyValue::AssetPathValue(texturePath);
        if (m_Graph.IsPinConnected(pin.id)) {
            m_Graph.DisconnectInput(pin.id);
        }
        m_Graph.MarkContentChanged();
        if (error) error->clear();
        return true;
    }

    const NodeDefinition* definition = NodeRegistry::Get().Find("TextureObject");
    if (!definition) {
        if (error) *error = "TextureObject node is not registered.";
        return false;
    }
    Node& textureObject = m_Graph.AddNode(
        *definition, Vec2(node.position.x - 300.0f, node.position.y + 24.0f));
    auto texture = textureObject.properties.find("texture");
    if (texture == textureObject.properties.end()) {
        textureObject.properties.emplace("texture", PropertyValue::AssetPathValue(texturePath));
    } else {
        texture->second = PropertyValue::AssetPathValue(texturePath);
    }
    Pin* source = textureObject.FindOutput("Texture");
    if (!source || !m_Graph.Connect(source->id, pin.id, error)) {
        if (error && error->empty()) *error = "Could not connect the TextureObject.";
        return false;
    }
    m_PendingPositions.insert(textureObject.id);
    m_Graph.MarkContentChanged();
    if (error) error->clear();
    return true;
}

bool NodeGraphView::ApplyTextureToInput(NodeId nodeId, PinId pinId,
                                        const std::string& texturePath,
                                        std::string* error) {
    Node* node = m_Graph.FindNode(nodeId);
    Pin* pin = node ? node->FindPin(pinId) : nullptr;
    if (!node || !pin) {
        if (error) *error = "The texture input no longer exists.";
        return false;
    }
    return AttachTextureToPin(*node, *pin, texturePath, error);
}

void NodeGraphView::FrameAll() {
    m_PendingFrameAll = true;
}

void NodeGraphView::FrameSelection() {
    m_PendingFrameSelection = true;
}

void NodeGraphView::SelectNode(NodeId id) {
    if (!m_Graph.FindNode(id)) return;
    m_PendingSelectNode = id;
    m_SelectedNodeId = id;
    m_PendingFrameSelection = true;
}

void NodeGraphView::SelectAllNodes() {
    if (!m_Context) return;
    ed::EditorContext* previous = ed::GetCurrentEditor();
    if (previous != m_Context) {
        ed::SetCurrentEditor(m_Context);
    }
    ed::ClearSelection();
    m_SelectedNodes.clear();
    for (const auto& node : m_Graph.Nodes()) {
        ed::SelectNode(ToEditorNode(node->id), true);
        m_SelectedNodes.push_back(node->id);
    }
    m_SelectedNodeId = m_SelectedNodes.empty() ? 0 : m_SelectedNodes.front();
    m_PendingFrameSelection = false;
    if (previous != m_Context) {
        ed::SetCurrentEditor(previous);
    }
}

std::vector<NodeId> NodeGraphView::SelectedNodeIds() const {
    std::vector<NodeId> result;
    for (NodeId id : m_SelectedNodes) {
        if (m_Graph.FindNode(id)) result.push_back(id);
    }
    if (result.empty() && m_SelectedNodeId != 0 && m_Graph.FindNode(m_SelectedNodeId)) {
        result.push_back(m_SelectedNodeId);
    }
    return result;
}

int NodeGraphView::SelectedNodeCount() const {
    return static_cast<int>(SelectedNodeIds().size());
}

void NodeGraphView::AlignSelected(AlignMode mode) {
    const std::vector<NodeId> ids = SelectedNodeIds();
    if (ids.size() < 2) return;

    struct Item {
        NodeId id = 0;
        ImVec2 position{};
        ImVec2 size{};
    };
    const float scale = CanvasUiScale();
    std::vector<Item> items;
    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
    for (NodeId id : ids) {
        const Node* node = m_Graph.FindNode(id);
        if (!node) continue;
        const ImVec2 position(node->position.x * scale, node->position.y * scale);
        const ImVec2 size(std::max(120.0f, node->size.x) * scale,
                          std::max(70.0f, node->size.y) * scale);
        items.push_back({id, position, size});
        minX = std::min(minX, position.x);
        minY = std::min(minY, position.y);
        maxX = std::max(maxX, position.x + size.x);
        maxY = std::max(maxY, position.y + size.y);
    }
    if (items.size() < 2) return;
    const float centerX = (minX + maxX) * 0.5f;
    const float centerY = (minY + maxY) * 0.5f;
    for (Item& item : items) {
        if (mode == AlignMode::Left) item.position.x = minX;
        else if (mode == AlignMode::CenterHorizontal) item.position.x = centerX - item.size.x * 0.5f;
        else if (mode == AlignMode::Right) item.position.x = maxX - item.size.x;
        else if (mode == AlignMode::Top) item.position.y = minY;
        else if (mode == AlignMode::CenterVertical) item.position.y = centerY - item.size.y * 0.5f;
        else if (mode == AlignMode::Bottom) item.position.y = maxY - item.size.y;
        m_Graph.SetNodePosition(item.id, Vec2(item.position.x / scale, item.position.y / scale));
        m_PendingPositions.insert(item.id);
    }
}

void NodeGraphView::DistributeSelected(bool horizontal) {
    const std::vector<NodeId> ids = SelectedNodeIds();
    if (ids.size() < 3) return;
    const float scale = CanvasUiScale();
    struct Item {
        NodeId id = 0;
        ImVec2 position{};
        ImVec2 size{};
    };
    std::vector<Item> items;
    for (NodeId id : ids) {
        const Node* node = m_Graph.FindNode(id);
        if (!node) continue;
        items.push_back({id,
                         ImVec2(node->position.x * scale, node->position.y * scale),
                         ImVec2(std::max(120.0f, node->size.x) * scale,
                                std::max(70.0f, node->size.y) * scale)});
    }
    if (items.size() < 3) return;
    std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        return horizontal ? a.position.x < b.position.x : a.position.y < b.position.y;
    });
    const float start = horizontal ? items.front().position.x : items.front().position.y;
    const float end = horizontal
                          ? items.back().position.x + items.back().size.x
                          : items.back().position.y + items.back().size.y;
    float totalSize = 0.0f;
    for (const Item& item : items) {
        totalSize += horizontal ? item.size.x : item.size.y;
    }
    const float gap = (end - start - totalSize) / static_cast<float>(items.size() - 1);
    float cursor = start;
    for (Item& item : items) {
        if (horizontal) {
            item.position.x = cursor;
            cursor += item.size.x + gap;
        } else {
            item.position.y = cursor;
            cursor += item.size.y + gap;
        }
        m_Graph.SetNodePosition(item.id, Vec2(item.position.x / scale, item.position.y / scale));
        m_PendingPositions.insert(item.id);
    }
}

void NodeGraphView::AddCommentAtViewCenter() {
    std::string error;
    Node* comment = AddNodeAtViewCenter("Comment", &error);
    if (!comment) return;
    comment->size = Vec2(300.0f, 180.0f);
    comment->properties["text"] = PropertyValue::StringValue("Comment");
    comment->properties["color"] = PropertyValue::Color3Value(0.18f, 0.34f, 0.42f);
    m_PendingPositions.insert(comment->id);
    m_Graph.MarkLayoutChanged();
}

void NodeGraphView::GroupSelectedNodes() {
    const std::vector<NodeId> ids = SelectedNodeIds();
    if (ids.empty()) {
        AddCommentAtViewCenter();
        return;
    }
    const float scale = CanvasUiScale();
    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
    for (NodeId id : ids) {
        const Node* node = m_Graph.FindNode(id);
        if (!node) continue;
        const float width = std::max(120.0f, node->size.x) * scale;
        const float height = std::max(70.0f, node->size.y) * scale;
        minX = std::min(minX, node->position.x * scale);
        minY = std::min(minY, node->position.y * scale);
        maxX = std::max(maxX, node->position.x * scale + width);
        maxY = std::max(maxY, node->position.y * scale + height);
    }
    if (minX == FLT_MAX) return;

    const float padding = 28.0f;
    const float headerPadding = 48.0f;
    const NodeDefinition* definition = NodeRegistry::Get().Find("Comment");
    if (!definition) return;
    Node& group = m_Graph.AddNode(
        *definition,
        Vec2((minX - padding) / scale, (minY - headerPadding) / scale));
    group.size = Vec2((maxX - minX + padding * 2.0f) / scale,
                      (maxY - minY + headerPadding + padding) / scale);
    group.properties["text"] = PropertyValue::StringValue("Group");
    group.properties["color"] = PropertyValue::Color3Value(0.22f, 0.38f, 0.46f);
    m_PendingPositions.insert(group.id);
    m_PendingSelectNode = group.id;
    m_SelectedNodeId = group.id;
    m_Graph.MarkLayoutChanged();
}

bool NodeGraphView::CollapseSelectedToFunction(const std::string& functionPath,
                                               std::string* error) {
    if (error) error->clear();
    if (functionPath.empty()) {
        if (error) *error = "The function asset path is empty.";
        return false;
    }
    const std::vector<NodeId> selectedIds = SelectedNodeIds();
    std::vector<Node*> selected;
    for (NodeId id : selectedIds) {
        Node* node = m_Graph.FindNode(id);
        if (!node) continue;
        const NodeDefinition* definition = NodeRegistry::Get().Find(node->type);
        if (node->IsMaterialOutput()) {
            if (error) *error = "Material Output cannot be collapsed into a function.";
            return false;
        }
        if (definition && (definition->isComment || definition->isFunctionOnly)) continue;
        selected.push_back(node);
    }
    if (selected.empty()) {
        if (error) *error = "Select one or more material expressions to collapse.";
        return false;
    }

    TypeResolver resolver(m_Graph);
    std::unordered_set<NodeId> selectedSet;
    for (const Node* node : selected) {
        selectedSet.insert(node->id);
    }

    struct BoundaryInput {
        PinId sourcePin = 0;
        PinId targetPin = 0;
        std::string name;
        PinType type = PinType::Float;
    };
    struct BoundaryOutput {
        PinId sourcePin = 0;
        std::string name;
        PinType type = PinType::Float;
        std::vector<PinId> targets;
    };
    std::vector<BoundaryInput> boundaryInputs;
    std::map<PinId, BoundaryOutput> boundaryOutputs;

    for (const Node* node : selected) {
        for (const Pin& input : node->inputs) {
            const Link* link = m_Graph.FindInputLink(input.id);
            if (!link) continue;
            const Pin* source = m_Graph.FindPin(link->fromPin);
            if (!source || selectedSet.count(source->nodeId) > 0) continue;
            BoundaryInput boundary;
            boundary.sourcePin = source->id;
            boundary.targetPin = input.id;
            boundary.name = input.name;
            boundary.type = resolver.GetEffectiveType(*source);
            boundaryInputs.push_back(std::move(boundary));
        }
        for (const Pin& output : node->outputs) {
            for (const Link& link : m_Graph.Links()) {
                if (link.fromPin != output.id) continue;
                const Pin* target = m_Graph.FindPin(link.toPin);
                if (!target || selectedSet.count(target->nodeId) > 0) continue;
                auto it = boundaryOutputs.find(output.id);
                if (it == boundaryOutputs.end()) {
                    BoundaryOutput boundary;
                    boundary.sourcePin = output.id;
                    boundary.name = output.name;
                    boundary.type = resolver.GetEffectiveType(output);
                    it = boundaryOutputs.emplace(output.id, std::move(boundary)).first;
                }
                it->second.targets.push_back(target->id);
            }
        }
    }
    if (boundaryOutputs.empty()) {
        if (error) *error = "The selected nodes have no outputs connected outside the selection.";
        return false;
    }

    auto uniqueName = [](const std::string& base, std::vector<std::string>& used) {
        std::string candidate = base.empty() ? "Value" : base;
        int suffix = 2;
        while (std::find(used.begin(), used.end(), candidate) != used.end()) {
            candidate = base + std::to_string(suffix++);
        }
        used.push_back(candidate);
        return candidate;
    };
    std::vector<std::string> usedInputNames;
    std::vector<std::string> usedOutputNames;
    for (BoundaryInput& boundary : boundaryInputs) {
        boundary.name = uniqueName(boundary.name, usedInputNames);
    }
    for (auto& [sourceId, boundary] : boundaryOutputs) {
        (void)sourceId;
        boundary.name = uniqueName(boundary.name, usedOutputNames);
    }

    MaterialGraph functionGraph;
    functionGraph.Settings().name = std::filesystem::path(functionPath).stem().string();
    std::map<NodeId, Node*> copiedNodes;
    float minX = selected.front()->position.x;
    float minY = selected.front()->position.y;
    float maxX = minX;
    for (const Node* node : selected) {
        minX = std::min(minX, node->position.x);
        minY = std::min(minY, node->position.y);
        maxX = std::max(maxX, node->position.x);
    }
    for (Node* node : selected) {
        const NodeDefinition* definition = NodeRegistry::Get().Find(node->type);
        if (!definition) continue;
        const Vec2 position(node->position.x - minX + 260.0f, node->position.y - minY + 80.0f);
        Node& copy = functionGraph.AddNode(*definition, position);
        copy.properties = node->properties;
        copy.size = node->size;
        copiedNodes[node->id] = &copy;

        auto copyPins = [&](const std::vector<Pin>& sourcePins,
                            std::vector<Pin>& destinationPins) {
            for (const Pin& sourcePin : sourcePins) {
                Pin* destination = nullptr;
                for (Pin& candidate : destinationPins) {
                    if (candidate.name == sourcePin.name) {
                        destination = &candidate;
                        break;
                    }
                }
                if (!destination) {
                    Pin pin;
                    pin.id = functionGraph.NextPinId();
                    pin.nodeId = copy.id;
                    pin.name = sourcePin.name;
                    pin.direction = sourcePin.direction;
                    destinationPins.push_back(std::move(pin));
                    destination = &destinationPins.back();
                }
                destination->type = sourcePin.type;
                destination->defaultValue = sourcePin.defaultValue;
                destination->hidden = sourcePin.hidden;
                destination->splitFrom = sourcePin.splitFrom;
                destination->splitIndex = sourcePin.splitIndex;
            }
        };
        copyPins(node->inputs, copy.inputs);
        copyPins(node->outputs, copy.outputs);
    }

    std::error_code ec;
    const std::filesystem::path targetPath(functionPath);
    if (targetPath.has_parent_path()) {
        std::filesystem::create_directories(targetPath.parent_path(), ec);
    }

    const std::vector<Link> originalLinks = m_Graph.Links();
    for (const Link& link : originalLinks) {
        const Pin* from = m_Graph.FindPin(link.fromPin);
        const Pin* to = m_Graph.FindPin(link.toPin);
        if (!from || !to) continue;
        if (selectedSet.count(from->nodeId) == 0 || selectedSet.count(to->nodeId) == 0) continue;
        Node* fromNode = copiedNodes[from->nodeId];
        Node* toNode = copiedNodes[to->nodeId];
        if (!fromNode || !toNode) continue;
        Pin* fromPin = fromNode->FindOutput(from->name);
        Pin* toPin = toNode->FindInput(to->name);
        if (fromPin && toPin) functionGraph.Connect(fromPin->id, toPin->id);
    }

    float inputY = 80.0f;
    for (const BoundaryInput& boundary : boundaryInputs) {
        std::string nodeError;
        Node* inputNode = functionGraph.AddNodeByType(
            "FunctionInput", Vec2(0.0f, inputY), &nodeError);
        if (!inputNode) {
            if (error) *error = "Could not create function input: " + nodeError;
            return false;
        }
        inputY += 120.0f;
        inputNode->properties["inputName"] = PropertyValue::StringValue(boundary.name);
        inputNode->properties["pinType"] =
            PropertyValue::StringValue(PinTypeToString(boundary.type));
        Pin* inputOutput = inputNode->FindOutput("Value");
        if (inputOutput) {
            inputOutput->type = boundary.type;
            inputOutput->defaultValue = PropertyValue::Float(0.0f);
        }
        Pin* targetPin = nullptr;
        const Pin* originalTarget = m_Graph.FindPin(boundary.targetPin);
        if (originalTarget) {
            auto it = copiedNodes.find(originalTarget->nodeId);
            if (it != copiedNodes.end()) {
                targetPin = it->second->FindInput(originalTarget->name);
            }
        }
        if (inputOutput && targetPin) {
            functionGraph.Connect(inputOutput->id, targetPin->id);
        }
    }

    float outputY = 80.0f;
    for (auto& [sourceId, boundary] : boundaryOutputs) {
        (void)sourceId;
        std::string nodeError;
        Node* outputNode = functionGraph.AddNodeByType(
            "FunctionOutput", Vec2(maxX - minX + 760.0f, outputY), &nodeError);
        if (!outputNode) {
            if (error) *error = "Could not create function output: " + nodeError;
            return false;
        }
        outputY += 120.0f;
        outputNode->properties["outputName"] = PropertyValue::StringValue(boundary.name);
        outputNode->properties["pinType"] =
            PropertyValue::StringValue(PinTypeToString(boundary.type));
        Pin* outputInput = outputNode->FindInput("Value");
        if (outputInput) {
            outputInput->type = boundary.type;
            outputInput->defaultValue = PropertyValue::Float(0.0f);
        }
        const Pin* originalSource = m_Graph.FindPin(boundary.sourcePin);
        if (!originalSource) continue;
        auto it = copiedNodes.find(originalSource->nodeId);
        if (it == copiedNodes.end()) continue;
        Pin* sourcePin = it->second->FindOutput(originalSource->name);
        if (outputInput && sourcePin) {
            functionGraph.Connect(sourcePin->id, outputInput->id);
        }
    }

    std::string saveError;
    if (!GraphSerializer::Save(functionGraph, functionPath, &saveError)) {
        if (error) *error = "Could not save the collapsed function: " + saveError;
        return false;
    }

    const Vec2 collapsePosition = selected.front()->position;
    for (Node* node : selected) {
        m_Graph.RemoveNode(node->id);
    }

    const NodeDefinition* callDefinition =
        NodeRegistry::Get().Find("MaterialFunctionCall");
    if (!callDefinition) {
        if (error) *error = "MaterialFunctionCall node type is not registered.";
        return false;
    }
    Node& call = m_Graph.AddNode(*callDefinition, collapsePosition);
    call.properties["functionPath"] = PropertyValue::StringValue(functionPath);
    call.properties["functionName"] =
        PropertyValue::StringValue(targetPath.stem().string());

    auto addCallPin = [&](const std::string& name, PinDirection direction, PinType type) {
        Pin pin;
        pin.id = m_Graph.NextPinId();
        pin.nodeId = call.id;
        pin.name = name;
        pin.direction = direction;
        pin.type = type;
        switch (type) {
        case PinType::Vector2: pin.defaultValue = PropertyValue::Vec2Value(0.0f, 0.0f); break;
        case PinType::Vector3: pin.defaultValue = PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f); break;
        case PinType::Vector4:
            pin.defaultValue = PropertyValue::Vec4Value(0.0f, 0.0f, 0.0f, 0.0f);
            break;
        default: pin.defaultValue = PropertyValue::Float(0.0f); break;
        }
        if (direction == PinDirection::Input) {
            call.inputs.push_back(std::move(pin));
        } else {
            call.outputs.push_back(std::move(pin));
        }
    };
    for (const BoundaryInput& boundary : boundaryInputs) {
        addCallPin(boundary.name, PinDirection::Input, boundary.type);
    }
    for (const auto& [sourceId, boundary] : boundaryOutputs) {
        (void)sourceId;
        addCallPin(boundary.name, PinDirection::Output, boundary.type);
    }

    for (const BoundaryInput& boundary : boundaryInputs) {
        Pin* callPin = call.FindInput(boundary.name);
        if (callPin) m_Graph.Connect(boundary.sourcePin, callPin->id);
    }
    for (const auto& [sourceId, boundary] : boundaryOutputs) {
        (void)sourceId;
        Pin* callPin = call.FindOutput(boundary.name);
        if (!callPin) continue;
        for (PinId target : boundary.targets) {
            m_Graph.Connect(callPin->id, target);
        }
    }

    m_PendingPositions.insert(call.id);
    m_PendingSelectNode = call.id;
    m_SelectedNodeId = call.id;
    m_Graph.MarkContentChanged();
    return true;
}

void NodeGraphView::AddBookmarkFromSelection() {
    const std::vector<NodeId> ids = SelectedNodeIds();
    if (ids.empty()) return;
    const Node* node = m_Graph.FindNode(ids.front());
    if (!node) return;
    const NodeDefinition* definition = NodeRegistry::Get().Find(node->type);
    std::string base = definition ? definition->displayName : node->type;
    std::string name = base + " #" + std::to_string(node->id);
    int suffix = 2;
    auto exists = [&](const std::string& candidate) {
        return std::any_of(m_Bookmarks.begin(), m_Bookmarks.end(),
                           [&](const Bookmark& bookmark) { return bookmark.name == candidate; });
    };
    const std::string original = name;
    while (exists(name)) {
        name = original + " (" + std::to_string(suffix++) + ")";
    }
    m_Bookmarks.push_back({name, node->id});
}

void NodeGraphView::GoToBookmark(std::size_t index) {
    if (index >= m_Bookmarks.size()) return;
    SelectNode(m_Bookmarks[index].nodeId);
}

void NodeGraphView::RemoveBookmark(std::size_t index) {
    if (index >= m_Bookmarks.size()) return;
    m_Bookmarks.erase(m_Bookmarks.begin() + static_cast<std::ptrdiff_t>(index));
}

void NodeGraphView::Draw(const ImVec2& size) {
    if (!m_Context) {
        CreateEditorContext();
    }
    if (!m_Context || size.x <= 1.0f || size.y <= 1.0f) {
        return;
    }

    ed::SetCurrentEditor(m_Context);
    m_EditorSize = size;
    m_EditorScreenPos = ImGui::GetCursorScreenPos();
    m_TexturePinDropHandled = false;
    ed::Begin("MaterialForgeNodeCanvas", size);

    TypeResolver resolver(m_Graph);
    auto drawNodes = [&](bool comments) {
        for (const auto& nodePtr : m_Graph.Nodes()) {
            Node& node = *nodePtr;
            const NodeDefinition* definition = NodeRegistry::Get().Find(node.type);
            const bool isComment = definition && definition->isComment;
            if (isComment != comments) continue;
            if (m_PendingPositions.erase(node.id) > 0) {
                const float scale = CanvasUiScale();
                ed::SetNodePosition(ToEditorNode(node.id),
                                    ImVec2(node.position.x * scale, node.position.y * scale));
            }
            DrawNode(node, resolver);
        }
    };
    drawNodes(true);
    drawNodes(false);

    for (const Link& link : m_Graph.Links()) {
        const Pin* from = m_Graph.FindPin(link.fromPin);
        const Pin* to = m_Graph.FindPin(link.toPin);
        if (!from || !to) continue;
        const ImVec4 color = LinkColorFor(*from, resolver);
        ed::Link(ToEditorLink(link.id), ToEditorPin(link.fromPin), ToEditorPin(link.toPin),
                 color, 2.0f);
    }

    DrawCreateAndDeleteInteractions();

    m_ViewCenterCanvas = ed::ScreenToCanvas(
        ImVec2(m_EditorScreenPos.x + m_EditorSize.x * 0.5f,
               m_EditorScreenPos.y + m_EditorSize.y * 0.5f));

    SyncPositions();
    UpdateSelection();
    ApplyPendingSelection();
    TryNodeShortcuts();
    TryClipboardShortcuts();

    const ImVec2 mouseScreen = ImGui::GetMousePos();
    m_ContextCanvasPos = mouseScreen;

    const bool backgroundDoubleClicked = ed::IsBackgroundDoubleClicked();

    ed::Suspend();
    bool openNodeContext = false;
    bool openPinContext = false;
    bool openLinkContext = false;
    bool openBackgroundContext = false;
    if (ed::ShowPinContextMenu(&m_ContextPin)) {
        openPinContext = true;
    } else if (ed::ShowNodeContextMenu(&m_ContextNode)) {
        openNodeContext = true;
    } else if (ed::ShowLinkContextMenu(&m_ContextLink)) {
        openLinkContext = true;
    } else if (ed::ShowBackgroundContextMenu()) {
        openBackgroundContext = true;
    }

    if (backgroundDoubleClicked && !openBackgroundContext) {
        openBackgroundContext = true;
    }

    const std::string nodeContextPopup = LocalizedPopupId("Node Context", "mf_node_context");
    const std::string pinContextPopup = LocalizedPopupId("Pin Context", "mf_pin_context");
    const std::string linkContextPopup = LocalizedPopupId("Link Context", "mf_link_context");
    const std::string createNodePopup = LocalizedPopupId("Create Node", "mf_create_node");

    if (openNodeContext) {
        ImGui::OpenPopup(nodeContextPopup.c_str());
    }
    if (openPinContext) {
        ImGui::OpenPopup(pinContextPopup.c_str());
    }
    if (openLinkContext) {
        ImGui::OpenPopup(linkContextPopup.c_str());
    }
    if (openBackgroundContext) {
        m_AddSearch[0] = '\0';
        m_FocusAddSearch = true;
        m_AddNodeCanvasPos = m_ContextCanvasPos;
        ImGui::OpenPopup(createNodePopup.c_str());
    }

    if (ImGui::BeginPopup(nodeContextPopup.c_str())) {
        Node* node = m_Graph.FindNode(static_cast<NodeId>(m_ContextNode.Get()));
        if (node) {
            const NodeDefinition* definition = NodeRegistry::Get().Find(node->type);
            ImGui::TextColored(Ui::CategoryColor(definition ? definition->category : ""), "%s",
                               definition ? definition->displayName.c_str() : node->type.c_str());
            ImGui::TextDisabled("%s", definition ? definition->ueClass.c_str() : "");
            ImGui::Separator();
            if (ImGui::MenuItem(Tr("Focus"))) {
                m_FocusNode = node->id;
                m_SelectedNodeId = node->id;
            }
            if (!m_FunctionMode && !node->IsMaterialOutput()) {
                TypeResolver resolver(m_Graph);
                const bool hasNumericOutput = std::any_of(
                    node->outputs.begin(), node->outputs.end(), [&](const Pin& candidate) {
                        const PinType type = resolver.GetEffectiveType(candidate);
                        return IsNumericPin(type) && type != PinType::Wildcard;
                    });
                if (hasNumericOutput && ImGui::MenuItem(Tr("Preview Node Result"))) {
                    m_NodePreviewRequestNode = node->id;
                    m_NodePreviewRequestPin = 0;
                    ImGui::CloseCurrentPopup();
                }
            }
            std::vector<std::string> splitBases;
            for (const Pin& pin : node->outputs) {
                if (!pin.splitFrom.empty() &&
                    std::find(splitBases.begin(), splitBases.end(), pin.splitFrom) ==
                        splitBases.end()) {
                    splitBases.push_back(pin.splitFrom);
                }
            }
            if (!splitBases.empty() && ImGui::MenuItem(Tr("Merge Split Pins"))) {
                for (const std::string& baseName : splitBases) {
                    if (Pin* base = node->FindOutput(baseName)) {
                        MergeOutputPin(*node, *base);
                    }
                }
            }
            if (ImGui::MenuItem(Tr("Delete Node"), nullptr, false, !node->IsMaterialOutput())) {
                const NodeId deletedId = node->id;
                m_Graph.RemoveNode(deletedId);
                if (m_SelectedNodeId == deletedId) m_SelectedNodeId = 0;
                node = nullptr;
            }
            if (node && node->IsMaterialOutput() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", Tr("The Material Output node is required and cannot be deleted."));
            }
            if (node && (node->type == "Constant" || node->type == "Constant2Vector" ||
                         node->type == "Constant3Vector" || node->type == "Constant4Vector")) {
                ImGui::Separator();
                if (ImGui::MenuItem(Tr("Convert to Parameter"))) {
                    std::string convertError;
                    if (!ConvertConstantNodeToParameter(node->id, &convertError) &&
                        !convertError.empty()) {
                        m_LastError = convertError;
                    }
                    ImGui::CloseCurrentPopup();
                }
            }
        } else {
            ImGui::TextDisabled("%s", Tr("Node no longer exists."));
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup(pinContextPopup.c_str())) {
        Pin* pin = m_Graph.FindPin(static_cast<PinId>(m_ContextPin.Get()));
        Node* node = pin ? m_Graph.FindNode(pin->nodeId) : nullptr;
        if (pin && node) {
            TypeResolver resolver(m_Graph);
            const PinType effective = resolver.GetEffectiveType(*pin);
            ImGui::TextUnformatted(pin->name.c_str());
            ImGui::TextDisabled("%s  ·  %s",
                                pin->direction == PinDirection::Input ? Tr("Input")
                                                                     : Tr("Output"),
                                Ui::PinTypeShortName(effective));
            ImGui::Separator();
            if (pin->direction == PinDirection::Output) {
                const std::string baseName =
                    pin->IsSplitComponent() ? pin->splitFrom : pin->name;
                const bool split = std::any_of(
                    node->outputs.begin(), node->outputs.end(),
                    [&](const Pin& candidate) { return candidate.splitFrom == baseName; });
                if (split) {
                    if (ImGui::MenuItem(Tr("Merge Pins"))) {
                        MergeOutputPin(*node, *pin);
                    }
                } else if (IsNumericPin(effective) && effective != PinType::Wildcard &&
                           PinComponentCount(effective) > 1) {
                    if (ImGui::MenuItem(Tr("Split Pin"))) {
                        SplitOutputPin(*node, *pin);
                    }
                } else {
                    ImGui::TextDisabled("%s", Tr("This pin cannot be split."));
                }
                if (!m_FunctionMode && IsNumericPin(effective) &&
                    effective != PinType::Wildcard) {
                    ImGui::Separator();
                    if (ImGui::MenuItem(Tr("Preview This Output"))) {
                        m_NodePreviewRequestNode = node->id;
                        m_NodePreviewRequestPin = pin->id;
                        ImGui::CloseCurrentPopup();
                    }
                }
            } else {
                ImGui::TextDisabled("%s", Tr("Input pins cannot be split."));
            }
            if (pin->direction == PinDirection::Input && pin->type == PinType::Texture) {
                ImGui::Separator();
                if (ImGui::MenuItem(Tr("Select Texture..."))) {
                    RequestTexturePicker(node->id, pin->id);
                    ImGui::CloseCurrentPopup();
                }
            }
        } else {
            ImGui::TextDisabled("%s", Tr("Pin no longer exists."));
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup(linkContextPopup.c_str())) {
        Link* link = m_Graph.FindLink(static_cast<LinkId>(m_ContextLink.Get()));
        if (link) {
            const Pin* from = m_Graph.FindPin(link->fromPin);
            const Pin* to = m_Graph.FindPin(link->toPin);
            if (from && to) {
                ImGui::Text("%s -> %s", from->name.c_str(), to->name.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem(Tr("Insert Reroute"))) {
                const Node* fromNode = m_Graph.FindNode(from->nodeId);
                const Node* toNode = m_Graph.FindNode(to->nodeId);
                const NodeDefinition* definition = NodeRegistry::Get().Find("Reroute");
                if (fromNode && toNode && definition) {
                    const Vec2 position(
                        (fromNode->position.x + toNode->position.x) * 0.5f,
                        (fromNode->position.y + toNode->position.y) * 0.5f);
                    Node& reroute = m_Graph.AddNode(*definition, position);
                    m_Graph.RemoveLink(link->id);
                    Pin* rerouteInput = reroute.FindInput("Input");
                    Pin* rerouteOutput = reroute.FindOutput("Output");
                    const bool first = rerouteInput &&
                                       m_Graph.Connect(from->id, rerouteInput->id);
                    const bool second = rerouteOutput &&
                                        m_Graph.Connect(rerouteOutput->id, to->id);
                    if (!first || !second) {
                        m_Graph.RemoveNode(reroute.id);
                        m_Graph.Connect(from->id, to->id);
                    } else {
                        m_PendingPositions.insert(reroute.id);
                        m_PendingSelectNode = reroute.id;
                        m_SelectedNodeId = reroute.id;
                    }
                }
            }
            if (ImGui::MenuItem(Tr("Delete Link"))) {
                m_Graph.RemoveLink(link->id);
            }
        } else {
            ImGui::TextDisabled("%s", Tr("Link no longer exists."));
        }
        ImGui::EndPopup();
    }

    DrawAddNodePopup();
    ed::Resume();
    ed::End();
    DrawNodeDropTarget();

    if (m_FocusNode != 0) {
        if (m_Graph.FindNode(m_FocusNode)) {
            ed::SelectNode(ToEditorNode(m_FocusNode), false);
            ed::NavigateToSelection(true, 0.30f);
            m_SelectedNodeId = m_FocusNode;
            m_PendingSelectNode = 0;
        }
        m_FocusNode = 0;
    }
    if (m_PendingFrameAll) {
        ed::NavigateToContent(0.0f);
        m_PendingFrameAll = false;
    }
    if (m_PendingFrameSelection) {
        ed::NavigateToSelection(true, 0.30f);
        m_PendingFrameSelection = false;
    }
    if (m_PendingCenterNode != 0) {
        if (m_Graph.FindNode(m_PendingCenterNode)) {
            ed::CenterNodeOnScreen(ToEditorNode(m_PendingCenterNode));
        }
        m_PendingCenterNode = 0;
    }

    ed::SetCurrentEditor(nullptr);
}

Node* NodeGraphView::CreateNodeAt(const std::string& type, const ImVec2& canvasPosition,
                                  std::string* error) {
    const NodeDefinition* definition = NodeRegistry::Get().Find(type);
    if (!definition) {
        if (error) *error = "Unknown node type: " + type;
        return nullptr;
    }
    if (definition->isMaterialOutput && m_Graph.FindMaterialOutput()) {
        if (error) *error = "The graph already has a Material Output node.";
        return nullptr;
    }
    const float scale = CanvasUiScale();
    Node& node = m_Graph.AddNode(*definition,
                                 Vec2(canvasPosition.x / scale, canvasPosition.y / scale));
    m_PendingPositions.insert(node.id);
    m_PendingSelectNode = node.id;
    m_SelectedNodeId = node.id;
    return &node;
}

void NodeGraphView::CreateTextureNodesAt(const std::string& texturePath,
                                         const ImVec2& canvasPosition) {
    if (texturePath.empty()) return;
    const float scale = CanvasUiScale();
    const Vec2 base(canvasPosition.x / scale, canvasPosition.y / scale);
    std::string error;
    Node* textureObject = m_Graph.AddNodeByType("TextureObject", base, &error);
    Node* textureSample = m_Graph.AddNodeByType(
        "TextureSample", Vec2(base.x + 250.0f, base.y + 24.0f), &error);
    if (!textureObject || !textureSample) return;

    auto texture = textureObject->properties.find("texture");
    if (texture == textureObject->properties.end()) {
        textureObject->properties.emplace("texture", PropertyValue::AssetPathValue(texturePath));
    } else {
        texture->second = PropertyValue::AssetPathValue(texturePath);
    }

    const Pin* textureOutput = textureObject->FindOutput("Texture");
    const Pin* textureInput = textureSample->FindInput("Texture");
    if (textureOutput && textureInput) {
        m_Graph.Connect(textureOutput->id, textureInput->id);
    }

    m_PendingPositions.insert(textureObject->id);
    m_PendingPositions.insert(textureSample->id);
    m_PendingSelectNode = 0;
    m_PendingSelectNodes.clear();
    m_PendingSelectNodes.push_back(textureObject->id);
    m_PendingSelectNodes.push_back(textureSample->id);
    m_SelectedNodeId = textureSample->id;
}

void NodeGraphView::CopySelectedNodes() {
    std::vector<ed::NodeId> selected;
    const int objectCount = ed::GetSelectedObjectCount();
    if (objectCount > 0) {
        selected.resize(static_cast<std::size_t>(objectCount));
        const int count = ed::GetSelectedNodes(selected.data(), objectCount);
        selected.resize(static_cast<std::size_t>(std::max(0, count)));
    }
    if (selected.empty() && m_SelectedNodeId != 0) {
        selected.push_back(ToEditorNode(m_SelectedNodeId));
    }

    std::map<NodeId, std::size_t> selectedIndices;
    std::vector<const Node*> nodes;
    float minX = 0.0f;
    float minY = 0.0f;
    bool first = true;
    for (const ed::NodeId& editorId : selected) {
        Node* node = m_Graph.FindNode(static_cast<NodeId>(editorId.Get()));
        if (!node || node->IsMaterialOutput()) continue;
        if (first) {
            minX = node->position.x;
            minY = node->position.y;
            first = false;
        } else {
            minX = std::min(minX, node->position.x);
            minY = std::min(minY, node->position.y);
        }
        selectedIndices[node->id] = nodes.size();
        nodes.push_back(node);
    }
    if (nodes.empty()) return;

    m_ClipboardNodes.clear();
    m_ClipboardLinks.clear();
    m_ClipboardNodes.reserve(nodes.size());
    for (const Node* node : nodes) {
        ClipboardNode item;
        item.type = node->type;
        item.position = Vec2(node->position.x - minX, node->position.y - minY);
        item.size = node->size;
        item.properties = node->properties;
        item.inputs = node->inputs;
        item.outputs = node->outputs;
        m_ClipboardNodes.push_back(std::move(item));
    }

    for (const Link& link : m_Graph.Links()) {
        const auto fromIt = selectedIndices.find(m_Graph.FindPin(link.fromPin)
                                                     ? m_Graph.FindPin(link.fromPin)->nodeId
                                                     : 0);
        const auto toIt = selectedIndices.find(m_Graph.FindPin(link.toPin)
                                                   ? m_Graph.FindPin(link.toPin)->nodeId
                                                   : 0);
        if (fromIt == selectedIndices.end() || toIt == selectedIndices.end()) continue;
        const Pin* fromPin = m_Graph.FindPin(link.fromPin);
        const Pin* toPin = m_Graph.FindPin(link.toPin);
        if (!fromPin || !toPin) continue;
        ClipboardLink item;
        item.fromNode = fromIt->second;
        item.toNode = toIt->second;
        item.fromPin = fromPin->name;
        item.toPin = toPin->name;
        m_ClipboardLinks.push_back(std::move(item));
    }
}

void NodeGraphView::PasteNodesAtMouse() {
    if (m_ClipboardNodes.empty()) return;
    const ImVec2 mouseCanvas = ed::ScreenToCanvas(ImGui::GetMousePos());
    const float scale = CanvasUiScale();
    const ImVec2 mouse(mouseCanvas.x / scale, mouseCanvas.y / scale);

    // Clipboard positions are normalized to the top-left of the copied group.
    // Center the whole group on the cursor instead of putting its top-left
    // corner under the mouse; otherwise large selections feel shifted down/right.
    float clipboardWidth = 0.0f;
    float clipboardHeight = 0.0f;
    for (const ClipboardNode& item : m_ClipboardNodes) {
        const float width = item.size.x > 0.0f ? item.size.x : 280.0f;
        const float height = item.size.y > 0.0f ? item.size.y : 160.0f;
        clipboardWidth = std::max(clipboardWidth, item.position.x + width);
        clipboardHeight = std::max(clipboardHeight, item.position.y + height);
    }
    const Vec2 origin(mouse.x - clipboardWidth * 0.5f,
                      mouse.y - clipboardHeight * 0.5f);

    std::vector<NodeId> created;
    created.reserve(m_ClipboardNodes.size());
    for (const ClipboardNode& item : m_ClipboardNodes) {
        const NodeDefinition* definition = NodeRegistry::Get().Find(item.type);
        if (!definition) continue;
        if (definition->isMaterialOutput && m_Graph.FindMaterialOutput()) continue;
        Node& node = m_Graph.AddNode(
            *definition,
            Vec2(origin.x + item.position.x, origin.y + item.position.y));
        node.properties = item.properties;
        if (definition->isComment) {
            node.size = item.size;
        }
        if (definition->isFunctionCall) {
            for (const Pin& source : item.inputs) {
                Pin pin = source;
                pin.id = m_Graph.NextPinId();
                pin.nodeId = node.id;
                node.inputs.push_back(std::move(pin));
            }
            for (const Pin& source : item.outputs) {
                Pin pin = source;
                pin.id = m_Graph.NextPinId();
                pin.nodeId = node.id;
                node.outputs.push_back(std::move(pin));
            }
            m_Graph.MarkContentChanged();
        }
        for (const Pin& source : item.outputs) {
            if (!source.IsSplitComponent() || node.FindOutput(source.name)) continue;
            Pin pin = source;
            pin.id = m_Graph.NextPinId();
            pin.nodeId = node.id;
            node.outputs.push_back(std::move(pin));
        }
        for (const Pin& source : item.outputs) {
            if (!source.hidden) continue;
            if (Pin* base = node.FindOutput(source.name)) {
                base->hidden = true;
            }
        }
        m_PendingPositions.insert(node.id);
        created.push_back(node.id);
    }
    if (created.empty()) return;

    for (const ClipboardLink& item : m_ClipboardLinks) {
        if (item.fromNode >= created.size() || item.toNode >= created.size()) continue;
        Node* from = m_Graph.FindNode(created[item.fromNode]);
        Node* to = m_Graph.FindNode(created[item.toNode]);
        if (!from || !to) continue;
        Pin* fromPin = from->FindOutput(item.fromPin);
        Pin* toPin = to->FindInput(item.toPin);
        if (fromPin && toPin) {
            m_Graph.Connect(fromPin->id, toPin->id);
        }
    }
    m_PendingSelectNodes.insert(m_PendingSelectNodes.end(), created.begin(), created.end());
    m_PendingSelectNode = 0;
    m_SelectedNodeId = created.front();
}

void NodeGraphView::PasteUe5NodesAtCanvas(const std::string& text,
                                          const ImVec2& canvasPosition) {
    const float scale = CanvasUiScale();
    const Vec2 origin(canvasPosition.x / scale, canvasPosition.y / scale);

    Ue5ClipboardImportResult import;
    if (!ImportUe5MaterialClipboard(text, m_Graph, origin, &import)) {
        m_LastError = import.error.empty()
                          ? Tr("Could not paste the UE5 material clipboard.")
                          : import.error;
        return;
    }
    if (import.createdNodes.empty()) {
        m_LastError = Tr("The clipboard contains no supported UE5 material expressions.");
        return;
    }

    m_PendingSelectNodes = import.createdNodes;
    m_PendingSelectNode = 0;
    m_SelectedNodeId = import.createdNodes.front();
    for (NodeId id : import.createdNodes) {
        m_PendingPositions.insert(id);
    }

    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  Tr("Pasted %d UE5 material node(s) and %d link(s)."),
                  import.nodeCount, import.linkCount);
    std::string message = buffer;
    if (import.skippedNodeCount > 0) {
        std::string skipped;
        for (const std::string& className : import.skippedClasses) {
            if (!skipped.empty()) skipped += ", ";
            skipped += className;
        }
        std::snprintf(buffer, sizeof(buffer),
                      Tr(" Skipped %d unsupported node(s): %s."),
                      import.skippedNodeCount, skipped.c_str());
        message += buffer;
    }
    if (import.droppedMaterialOutputLinks > 0) {
        std::snprintf(buffer, sizeof(buffer),
                      Tr(" %d link(s) to the material output were not restored because UE did not copy the root node."),
                      import.droppedMaterialOutputLinks);
        message += buffer;
    }
    m_LastMessage = std::move(message);
}

void NodeGraphView::SplitOutputPin(Node& node, Pin& base) {
    if (base.direction != PinDirection::Output || base.IsSplitComponent()) return;
    TypeResolver resolver(m_Graph);
    const PinType type = resolver.GetEffectiveType(base);
    const int components = PinComponentCount(type);
    if (!IsNumericPin(type) || type == PinType::Wildcard || components <= 1) return;
    const bool alreadySplit = std::any_of(
        node.outputs.begin(), node.outputs.end(),
        [&](const Pin& pin) { return pin.splitFrom == base.name; });
    if (alreadySplit) return;

    std::vector<LinkId> baseLinks;
    for (const Link& link : m_Graph.Links()) {
        if (link.fromPin == base.id) baseLinks.push_back(link.id);
    }
    for (LinkId linkId : baseLinks) {
        m_Graph.RemoveLink(linkId);
    }
    base.hidden = true;

    static const char* componentNames[] = {"R", "G", "B", "A"};
    const int count = std::min(components, 4);
    for (int i = 0; i < count; ++i) {
        Pin pin;
        pin.id = m_Graph.NextPinId();
        pin.nodeId = node.id;
        pin.name = UniquePinName(node.outputs, componentNames[i]);
        pin.direction = PinDirection::Output;
        pin.type = PinType::Float;
        pin.defaultValue = PropertyValue::Float(0.0f);
        pin.tooltip = "Split component of " + base.name;
        pin.splitFrom = base.name;
        pin.splitIndex = i;
        node.outputs.push_back(std::move(pin));
    }
    m_PendingPositions.insert(node.id);
    m_Graph.MarkContentChanged();
}

void NodeGraphView::MergeOutputPin(Node& node, const Pin& base) {
    const std::string baseName = base.IsSplitComponent() ? base.splitFrom : base.name;
    std::vector<PinId> removedPins;
    for (const Pin& pin : node.outputs) {
        if (pin.splitFrom == baseName) {
            removedPins.push_back(pin.id);
        }
    }
    if (removedPins.empty()) return;

    for (PinId pinId : removedPins) {
        std::vector<LinkId> linksToRemove;
        for (const Link& link : m_Graph.Links()) {
            if (link.fromPin == pinId) linksToRemove.push_back(link.id);
        }
        for (LinkId linkId : linksToRemove) {
            m_Graph.RemoveLink(linkId);
        }
    }
    node.outputs.erase(
        std::remove_if(node.outputs.begin(), node.outputs.end(),
                       [&](const Pin& pin) { return pin.splitFrom == baseName; }),
        node.outputs.end());
    if (Pin* basePin = node.FindOutput(baseName)) {
        basePin->hidden = false;
    }
    m_PendingPositions.insert(node.id);
    m_Graph.MarkContentChanged();
}

void NodeGraphView::TryClipboardShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyAlt || io.KeyShift || !io.KeyCtrl) {
        return;
    }
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        CopySelectedNodes();
    } else if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        const char* clipboard = ImGui::GetClipboardText();
        if (clipboard && LooksLikeUe5MaterialClipboard(clipboard)) {
            PasteUe5NodesAtCanvas(clipboard, ed::ScreenToCanvas(ImGui::GetMousePos()));
        } else {
            PasteNodesAtMouse();
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        SelectAllNodes();
    }
}

void NodeGraphView::TryNodeShortcuts() {
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool insideCanvas =
        mouse.x >= m_EditorScreenPos.x && mouse.y >= m_EditorScreenPos.y &&
        mouse.x < m_EditorScreenPos.x + m_EditorSize.x &&
        mouse.y < m_EditorScreenPos.y + m_EditorSize.y;
    const bool backgroundClicked =
        ed::IsBackgroundClicked() &&
        ed::GetBackgroundClickButtonIndex() == ImGuiMouseButton_Left;
    const bool canvasClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && insideCanvas &&
                             !ed::GetHoveredNode() && !ed::GetHoveredPin() &&
                             !ed::GetHoveredLink();
    if (!backgroundClicked && !canvasClick) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeyShift) {
        return;
    }
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }

    struct Shortcut {
        ImGuiKey key;
        const char* type;
    };
    static const Shortcut shortcuts[] = {
        {ImGuiKey_1, "Constant"},
        {ImGuiKey_2, "Constant2Vector"},
        {ImGuiKey_3, "Constant3Vector"},
        {ImGuiKey_4, "Constant4Vector"},
        {ImGuiKey_T, "TextureSample"},
        {ImGuiKey_V, "VectorParameter"},
        {ImGuiKey_S, "ScalarParameter"},
        {ImGuiKey_A, "Add"},
        {ImGuiKey_M, "Multiply"},
        {ImGuiKey_L, "Lerp"},
        {ImGuiKey_P, "Panner"},
        {ImGuiKey_O, "OneMinus"},
    };
    for (const Shortcut& shortcut : shortcuts) {
        if (!ImGui::IsKeyDown(shortcut.key)) continue;
        const ImVec2 clickPosition = ImGui::GetMousePos();
        CreateNodeAt(shortcut.type, clickPosition);
        break;
    }
}

void NodeGraphView::DrawNodeDropTarget() {
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (!payload || (!payload->IsDataType("MF_NODE_TYPE") &&
                     !payload->IsDataType("MF_TEXTURE_ASSET") &&
                     !payload->IsDataType("MF_FUNCTION_ASSET"))) {
        return;
    }
    if (payload->IsDataType("MF_TEXTURE_ASSET") && m_TexturePinDropHandled) {
        return;
    }

    ImGui::SetCursorScreenPos(m_EditorScreenPos);
    ImGui::InvisibleButton("##node_drop_target", m_EditorSize);
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }

    if (payload->IsDataType("MF_TEXTURE_ASSET")) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 editorMax(m_EditorScreenPos.x + m_EditorSize.x,
                               m_EditorScreenPos.y + m_EditorSize.y);
        drawList->AddRect(m_EditorScreenPos, editorMax, IM_COL32(255, 171, 74, 220), 0.0f, 0,
                          Ui::S(2.0f));
        drawList->AddText(ImVec2(m_EditorScreenPos.x + Ui::S(12.0f),
                                 m_EditorScreenPos.y + Ui::S(12.0f)),
                          IM_COL32(255, 201, 128, 255),
                          Tr("Drop texture to create TextureSample."));
    } else if (payload->IsDataType("MF_FUNCTION_ASSET")) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 editorMax(m_EditorScreenPos.x + m_EditorSize.x,
                               m_EditorScreenPos.y + m_EditorSize.y);
        drawList->AddRect(m_EditorScreenPos, editorMax, IM_COL32(120, 170, 255, 220), 0.0f, 0,
                          Ui::S(2.0f));
        drawList->AddText(ImVec2(m_EditorScreenPos.x + Ui::S(12.0f),
                                 m_EditorScreenPos.y + Ui::S(12.0f)),
                          IM_COL32(170, 202, 255, 255),
                          Tr("Drop material function to create a call node."));
    }

    if (const ImGuiPayload* dropped = ImGui::AcceptDragDropPayload("MF_NODE_TYPE")) {
        if (dropped->Data && dropped->DataSize > 1) {
            const std::string type(static_cast<const char*>(dropped->Data));
            CreateNodeAt(type, ed::ScreenToCanvas(ImGui::GetMousePos()));
        }
    } else if (const ImGuiPayload* dropped =
                   ImGui::AcceptDragDropPayload("MF_TEXTURE_ASSET")) {
        if (dropped->Data && dropped->DataSize > 1) {
            const std::string path(static_cast<const char*>(dropped->Data));
            CreateTextureNodesAt(path, ed::ScreenToCanvas(ImGui::GetMousePos()));
        }
    } else if (const ImGuiPayload* dropped =
                   ImGui::AcceptDragDropPayload("MF_FUNCTION_ASSET")) {
        if (dropped->Data && dropped->DataSize > 1) {
            const std::string path(static_cast<const char*>(dropped->Data));
            std::string functionError;
            if (!CreateFunctionCallNodeAt(path, ed::ScreenToCanvas(ImGui::GetMousePos()),
                                          &functionError)) {
                m_LastError = functionError;
            }
        }
    }
    ImGui::EndDragDropTarget();
}

void NodeGraphView::DrawNode(Node& node, TypeResolver& resolver) {
    const NodeDefinition* definition = NodeRegistry::Get().Find(node.type);
    ed::BeginNode(ToEditorNode(node.id));
    ImGui::PushID(static_cast<int>(node.id));

    if (!definition) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", Tr("Unknown node"));
        ImGui::TextDisabled("%s", node.type.c_str());
        ImGui::PopID();
        ed::EndNode();
        return;
    }

    if (definition->isComment) {
        const auto textIt = node.properties.find("text");
        const std::string text =
            textIt != node.properties.end() && IsStringProperty(textIt->second.kind) &&
                    !textIt->second.text.empty()
                ? textIt->second.text
                : std::string("Comment");
        const auto colorIt = node.properties.find("color");
        ImVec4 color(0.18f, 0.34f, 0.42f, 1.0f);
        if (colorIt != node.properties.end() &&
            (colorIt->second.kind == PropertyKind::Color3 ||
             colorIt->second.kind == PropertyKind::Color4)) {
            color = ImVec4(colorIt->second.vec3.x, colorIt->second.vec3.y,
                           colorIt->second.vec3.z, 1.0f);
        }
        const float scale = CanvasUiScale();
        const float width = std::max(120.0f, node.size.x) * scale;
        const float height = std::max(70.0f, node.size.y) * scale;
        const float headerHeight = Ui::S(26.0f);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 end(start.x + width, start.y + height);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(
            start, end,
            IM_COL32(static_cast<int>(color.x * 60.0f), static_cast<int>(color.y * 60.0f),
                     static_cast<int>(color.z * 60.0f), 150),
            Ui::S(6.0f));
        drawList->AddRectFilled(start, ImVec2(end.x, start.y + headerHeight),
                                Ui::ToU32(color), Ui::S(5.0f));
        drawList->AddRect(start, end, IM_COL32(255, 255, 255, 35), Ui::S(6.0f));
        drawList->AddText(ImVec2(start.x + Ui::S(10.0f), start.y + Ui::S(5.0f)),
                          IM_COL32(238, 246, 248, 255), text.c_str());
        ImGui::Dummy(ImVec2(width, height));
        ImGui::PopID();
        ed::EndNode();
        return;
    }

    if (definition->isFunctionCall &&
        ed::GetHoveredNode() == ToEditorNode(node.id) &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const auto pathIt = node.properties.find("functionPath");
        if (pathIt != node.properties.end() && IsStringProperty(pathIt->second.kind) &&
            !pathIt->second.text.empty()) {
            m_FunctionOpenRequest = pathIt->second.text;
        }
    }

    const ImVec4 categoryColor = Ui::CategoryColor(definition->category);
    const float lineHeight = ImGui::GetTextLineHeight();
    std::string nodeTitle = definition->displayName;
    if (definition->isFunctionCall) {
        const auto nameIt = node.properties.find("functionName");
        if (nameIt != node.properties.end() && IsStringProperty(nameIt->second.kind) &&
            !nameIt->second.text.empty()) {
            nodeTitle = nameIt->second.text;
        }
    }
    const bool inlineMathDefaults = definition->category == "Math" && !node.IsMaterialOutput();
    float inputWidth = 0.0f;
    float outputWidth = 0.0f;
    for (const Pin& pin : node.inputs) {
        float pinWidth = lineHeight + Ui::S(6.0f) +
                         ImGui::CalcTextSize(pin.name.c_str()).x;
        if (pin.type == PinType::Texture) pinWidth += Ui::S(28.0f);
        if (inlineMathDefaults && pin.defaultValue.kind == PropertyKind::Float &&
            !m_Graph.IsPinConnected(pin.id)) {
            pinWidth += Ui::S(62.0f);
        }
        inputWidth = std::max(inputWidth, pinWidth);
    }
    for (const Pin& pin : node.outputs) {
        if (pin.hidden) continue;
        outputWidth = std::max(outputWidth, ImGui::CalcTextSize(pin.name.c_str()).x + Ui::S(6.0f) + lineHeight);
    }
    const float titleWidth = ImGui::CalcTextSize(nodeTitle.c_str()).x;
    float nodeWidth = std::max(Ui::S(212.0f), titleWidth + Ui::S(52.0f));
    if (!node.outputs.empty()) {
        nodeWidth = std::max(nodeWidth, inputWidth + outputWidth + Ui::S(30.0f));
    }

    const ImVec2 headerStart = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(nodeWidth, Ui::S(30.0f)));
    const ImVec2 headerEnd(headerStart.x + nodeWidth, headerStart.y + Ui::S(30.0f));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec4 headerColor(
        0.040f + categoryColor.x * 0.15f,
        0.055f + categoryColor.y * 0.15f,
        0.068f + categoryColor.z * 0.15f,
        1.0f);
    drawList->AddRectFilled(headerStart, headerEnd, Ui::ToU32(headerColor), 6.0f);
    drawList->AddRectFilled(headerStart, ImVec2(headerStart.x + 3.0f, headerEnd.y),
                            Ui::ToU32(categoryColor), 6.0f, ImDrawFlags_RoundCornersLeft);
    Ui::DrawNodeThumbnail(drawList,
                          ImVec2(headerStart.x + Ui::S(7.0f), headerStart.y + Ui::S(5.5f)),
                          Ui::S(19.0f), definition->type, definition->category);
    drawList->AddText(ImVec2(headerStart.x + Ui::S(32.0f), headerStart.y + (Ui::S(30.0f) - lineHeight) * 0.5f),
                      IM_COL32(220, 235, 236, 255), nodeTitle.c_str());

    const auto valueIt = node.properties.find("value");
    if (valueIt != node.properties.end() &&
        (valueIt->second.kind == PropertyKind::Color3 || valueIt->second.kind == PropertyKind::Color4)) {
        const Vec4 color = valueIt->second.kind == PropertyKind::Color3
                               ? Vec4(valueIt->second.vec3, 1.0f)
                               : valueIt->second.vec4;
        const ImVec2 swatchCenter(headerEnd.x - 16.0f, headerStart.y + 15.0f);
        drawList->AddCircleFilled(swatchCenter, 7.0f,
                                  IM_COL32(static_cast<int>(Clamp(color.x, 0.0f, 1.0f) * 255.0f),
                                           static_cast<int>(Clamp(color.y, 0.0f, 1.0f) * 255.0f),
                                           static_cast<int>(Clamp(color.z, 0.0f, 1.0f) * 255.0f), 255),
                                  16);
        drawList->AddCircle(swatchCenter, 7.0f, IM_COL32(255, 255, 255, 70), 16, 1.0f);
    } else {
        const std::string parameterName = [&]() {
            const auto it = node.properties.find("parameterName");
            return it != node.properties.end() ? it->second.text : std::string();
        }();
        if (!parameterName.empty()) {
            const ImVec2 textSize = ImGui::CalcTextSize(parameterName.c_str());
            if (12.0f + titleWidth + 12.0f + textSize.x + 12.0f < nodeWidth) {
                drawList->AddText(ImVec2(headerEnd.x - textSize.x - 12.0f,
                                         headerStart.y + (Ui::S(30.0f) - textSize.y) * 0.5f),
                                  IM_COL32(142, 174, 180, 230), parameterName.c_str());
            }
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetNextWindowPos(ImVec2(headerStart.x + Ui::S(8.0f),
                                       headerEnd.y + Ui::S(4.0f)),
                                ImGuiCond_Always);
        ImGui::BeginTooltip();
        ImGui::TextColored(categoryColor, "%s", definition->ueClass.c_str());
        ImGui::TextWrapped("%s", Tr(definition->description.c_str()));
        ImGui::EndTooltip();
    }

    ImGui::Dummy(ImVec2(0.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 5.0f));

    if (!node.inputs.empty()) {
        ImGui::BeginGroup();
        for (Pin& pin : node.inputs) {
            const bool connected = m_Graph.IsPinConnected(pin.id);
            const PinType effective = resolver.GetEffectiveType(pin);
            ed::BeginPin(ToEditorPin(pin.id), ed::PinKind::Input);
            ed::PinPivotAlignment(ImVec2(0.0f, 0.5f));
            ed::PinPivotSize(ImVec2(0.0f, 0.0f));
            Ui::DrawPinDot(effective, connected);
            ImGui::SameLine(0.0f, 6.0f);
            std::string pinLabel = pin.name;
            if (pin.type == PinType::Texture) {
                const Link* inputLink = m_Graph.FindInputLink(pin.id);
                const Node* textureSource =
                    inputLink ? m_Graph.FindSourceNode(pin.id) : nullptr;
                std::string texturePath;
                if (textureSource) {
                    const auto textureIt = textureSource->properties.find("texture");
                    if (textureIt != textureSource->properties.end() &&
                        IsStringProperty(textureIt->second.kind)) {
                        texturePath = textureIt->second.text;
                    }
                }
                if (texturePath.empty()) {
                    const auto ownTexture = node.properties.find("texture");
                    if (ownTexture != node.properties.end() &&
                        IsStringProperty(ownTexture->second.kind)) {
                        texturePath = ownTexture->second.text;
                    }
                }
                pinLabel = texturePath.empty()
                               ? std::string("TextureObject")
                               : std::filesystem::path(texturePath).filename().u8string();
            }
            ImGui::TextUnformatted(pinLabel.c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ShowPinTooltip(pin, effective, connected);
            }
            if (pin.type == PinType::Texture) {
                if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    ImGui::SetTooltip("%s",
                                      Tr("Click to choose a texture, or drag a texture asset here."));
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    RequestTexturePicker(node.id, pin.id);
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload("MF_TEXTURE_ASSET")) {
                        if (payload->Data && payload->DataSize > 1) {
                            const std::string texturePath(static_cast<const char*>(payload->Data));
                            std::string attachError;
                            if (AttachTextureToPin(node, pin, texturePath, &attachError)) {
                                m_TexturePinDropHandled = true;
                            } else {
                                m_LastError = attachError;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine(0.0f, 4.0f);
                if (ImGui::SmallButton("...")) {
                    RequestTexturePicker(node.id, pin.id);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", Tr("Select Texture..."));
                }
            }
            if (inlineMathDefaults && !connected &&
                pin.defaultValue.kind == PropertyKind::Float) {
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::SetNextItemWidth(Ui::S(56.0f));
                ImGui::PushID(static_cast<int>(pin.id));
                if (ImGui::DragFloat("##inline_constant", &pin.defaultValue.number, 0.01f,
                                     0.0f, 0.0f, "%.3f")) {
                    m_Graph.MarkContentChanged();
                }
                ImGui::PopID();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                    ImGui::SetNextWindowPos(
                        ImVec2(ImGui::GetItemRectMin().x,
                               ImGui::GetItemRectMax().y + Ui::S(4.0f)),
                        ImGuiCond_Always);
                    ImGui::SetTooltip("%s\n%s", Tr("Input default"),
                                      Tr("Used when this input is not connected."));
                }
            }
            ed::EndPin();
        }
        ImGui::EndGroup();
    }

    if (!node.outputs.empty()) {
        if (!node.inputs.empty()) {
            const float spacing = std::max(Ui::S(24.0f), nodeWidth - inputWidth - outputWidth);
            ImGui::SameLine(0.0f, spacing);
        } else {
            const float spacer = std::max(0.0f, nodeWidth - outputWidth);
            if (spacer > 0.0f) {
                ImGui::Dummy(ImVec2(spacer, 0.0f));
                ImGui::SameLine(0.0f, 0.0f);
            }
        }
        ImGui::BeginGroup();
        for (Pin& pin : node.outputs) {
            if (pin.hidden) continue;
            const bool connected = m_Graph.IsPinConnected(pin.id);
            const PinType effective = resolver.GetEffectiveType(pin);
            ed::BeginPin(ToEditorPin(pin.id), ed::PinKind::Output);
            ed::PinPivotAlignment(ImVec2(1.0f, 0.5f));
            ed::PinPivotSize(ImVec2(0.0f, 0.0f));
            ImGui::TextUnformatted(pin.name.c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ShowPinTooltip(pin, effective, connected);
            }
            ImGui::SameLine(0.0f, 6.0f);
            Ui::DrawPinDot(effective, connected);
            ed::EndPin();
        }
        ImGui::EndGroup();
    }

    ImGui::PopStyleVar();
    ImGui::PopID();
    ed::EndNode();
}

void NodeGraphView::DrawCreateAndDeleteInteractions() {
    if (ed::BeginCreate(ImVec4(0.30f, 0.85f, 0.78f, 1.0f), 2.0f)) {
        ed::PinId startId;
        ed::PinId endId;
        if (ed::QueryNewLink(&startId, &endId, ImVec4(0.30f, 0.85f, 0.78f, 1.0f), 2.0f) &&
            startId && endId) {
            Pin* startPin = m_Graph.FindPin(static_cast<PinId>(startId.Get()));
            Pin* endPin = m_Graph.FindPin(static_cast<PinId>(endId.Get()));
            Pin* outputPin = nullptr;
            Pin* inputPin = nullptr;
            if (startPin && endPin) {
                if (startPin->direction == PinDirection::Output &&
                    endPin->direction == PinDirection::Input) {
                    outputPin = startPin;
                    inputPin = endPin;
                } else if (startPin->direction == PinDirection::Input &&
                           endPin->direction == PinDirection::Output) {
                    outputPin = endPin;
                    inputPin = startPin;
                }
            }

            std::string reason;
            if (!outputPin || !inputPin ||
                !m_Graph.CanConnect(outputPin->id, inputPin->id, &reason)) {
                ed::RejectNewItem(ImVec4(1.0f, 0.32f, 0.30f, 1.0f), 2.0f);
            } else if (ed::AcceptNewItem(Ui::PinTypeColor(outputPin->type), 2.0f)) {
                if (Link* link = m_Graph.Connect(outputPin->id, inputPin->id, &reason)) {
                    ed::Link(ToEditorLink(link->id), ToEditorPin(link->fromPin),
                             ToEditorPin(link->toPin), Ui::PinTypeColor(outputPin->type), 2.0f);
                }
            }
        }
    }
    ed::EndCreate();

    std::vector<NodeId> nodesToDelete;
    std::vector<LinkId> linksToDelete;
    if (ed::BeginDelete()) {
        ed::NodeId nodeId;
        while (ed::QueryDeletedNode(&nodeId)) {
            Node* node = m_Graph.FindNode(static_cast<NodeId>(nodeId.Get()));
            if (!node || node->IsMaterialOutput()) {
                ed::RejectDeletedItem();
            } else if (ed::AcceptDeletedItem()) {
                nodesToDelete.push_back(node->id);
            }
        }

        ed::LinkId linkId;
        while (ed::QueryDeletedLink(&linkId)) {
            if (ed::AcceptDeletedItem()) {
                linksToDelete.push_back(static_cast<LinkId>(linkId.Get()));
            }
        }
        ed::EndDelete();
    }

    for (const NodeId id : nodesToDelete) {
        m_Graph.RemoveNode(id);
        if (m_SelectedNodeId == id) m_SelectedNodeId = 0;
    }
    for (const LinkId id : linksToDelete) {
        m_Graph.RemoveLink(id);
    }
    if (!nodesToDelete.empty() || !linksToDelete.empty()) {
        m_PendingFrameAll = false;
    }
}

void NodeGraphView::SyncPositions() {
    for (const auto& node : m_Graph.Nodes()) {
        const ImVec2 position = ed::GetNodePosition(ToEditorNode(node->id));
        const float scale = CanvasUiScale();
        m_Graph.SetNodePosition(node->id, Vec2(position.x / scale, position.y / scale));
        if (node->type != "Comment") {
            const ImVec2 size = ed::GetNodeSize(ToEditorNode(node->id));
            if (size.x > 1.0f && size.y > 1.0f) {
                node->size = Vec2(size.x / scale, size.y / scale);
            }
        }
    }
}

void NodeGraphView::UpdateSelection() {
    m_SelectedNodes.clear();
    const int objectCount = ed::GetSelectedObjectCount();
    if (objectCount <= 0) {
        m_SelectedNodeId = 0;
        return;
    }
    std::vector<ed::NodeId> selected(static_cast<std::size_t>(objectCount));
    const int nodeCount = ed::GetSelectedNodes(selected.data(), static_cast<int>(selected.size()));
    if (nodeCount <= 0) {
        m_SelectedNodeId = 0;
        return;
    }
    for (int i = 0; i < nodeCount; ++i) {
        const Node* node = m_Graph.FindNode(static_cast<NodeId>(selected[static_cast<std::size_t>(i)].Get()));
        if (node) {
            m_SelectedNodes.push_back(node->id);
            m_SelectedNodeId = node->id;
        }
    }
    if (m_SelectedNodes.empty()) {
        m_SelectedNodeId = 0;
    }
}

void NodeGraphView::ApplyPendingSelection() {
    if (m_PendingSelectNode != 0) {
        if (m_Graph.FindNode(m_PendingSelectNode)) {
            ed::SelectNode(ToEditorNode(m_PendingSelectNode), false);
            m_SelectedNodeId = m_PendingSelectNode;
        }
        m_PendingSelectNode = 0;
    }
    if (!m_PendingSelectNodes.empty()) {
        ed::ClearSelection();
        for (NodeId id : m_PendingSelectNodes) {
            if (m_Graph.FindNode(id)) {
                ed::SelectNode(ToEditorNode(id), true);
                m_SelectedNodeId = id;
            }
        }
        m_PendingSelectNodes.clear();
    }
}

void NodeGraphView::DrawAddNodePopup() {
    ImGui::SetNextWindowSize(ImVec2(Ui::S(390.0f), Ui::S(500.0f)), ImGuiCond_Always);
    const std::string popupId = LocalizedPopupId("Create Node", "mf_create_node");
    if (!ImGui::BeginPopup(popupId.c_str())) {
        return;
    }

    ImGui::TextUnformatted(Tr("Add Material Expression"));
    ImGui::TextDisabled("%s", Tr("Click a node to place it on the canvas."));
    const char* clipboard = ImGui::GetClipboardText();
    if (clipboard && LooksLikeUe5MaterialClipboard(clipboard)) {
        ImGui::Spacing();
        if (ImGui::Selectable(Tr("Paste UE5 Material Nodes"))) {
            PasteUe5NodesAtCanvas(clipboard, m_AddNodeCanvasPos);
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        ImGui::Separator();
    }
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.0f);
    if (m_FocusAddSearch) {
        ImGui::SetKeyboardFocusHere();
        m_FocusAddSearch = false;
    }
    ImGui::InputTextWithHint("##search", Tr("Search nodes..."), m_AddSearch, sizeof(m_AddSearch));

    const std::string query = Trim(m_AddSearch);
    const bool searching = !query.empty();
    bool hasMaterialOutput = m_Graph.FindMaterialOutput() != nullptr;
    bool anyResult = false;

    const float availHeight = ImGui::GetContentRegionAvail().y - Ui::S(8.0f);
    ImGui::BeginChild("##add_node_scroll", ImVec2(0.0f, availHeight), ImGuiChildFlags_Borders);
    const NodeRegistry& registry = NodeRegistry::Get();
    std::vector<std::string> categories = registry.Categories();
    if (m_FunctionMode) {
        const auto functionCategory = std::find(categories.begin(), categories.end(), "Function");
        if (functionCategory != categories.end()) {
            std::rotate(categories.begin(), functionCategory, functionCategory + 1);
        }
    }
    for (const std::string& category : categories) {
        std::vector<const NodeDefinition*> matching;
        for (const NodeDefinition& definition : registry.All()) {
            if (definition.category != category) continue;
            if (!m_FunctionMode && definition.isFunctionOnly) continue;
            if (m_FunctionMode && definition.isMaterialOutput) continue;
            if (definition.isMaterialOutput && hasMaterialOutput) continue;
            if (searching && !NodeMatchesSearch(definition, query)) continue;
            matching.push_back(&definition);
        }
        if (matching.empty()) continue;

        anyResult = true;
        ImGui::PushStyleColor(ImGuiCol_Text, Ui::CategoryColor(category));
        ImGui::TextUnformatted(Tr(category.c_str()));
        ImGui::PopStyleColor();

        for (const NodeDefinition* definition : matching) {
            ImGui::PushID(definition->type.c_str());
            if (ImGui::Selectable(definition->displayName.c_str())) {
                CreateNodeAt(definition->type, m_AddNodeCanvasPos);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::BeginTooltip();
                ImGui::TextColored(Ui::CategoryColor(category), "%s", definition->ueClass.c_str());
                ImGui::TextWrapped("%s", Tr(definition->description.c_str()));
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
    }
    if (!anyResult) {
        ImGui::TextDisabled("%s", Tr("No matching nodes."));
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

} // namespace mf
