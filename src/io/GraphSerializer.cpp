#include "io/GraphSerializer.h"

#include "core/NodeRegistry.h"
#include "core/TypeResolver.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace mf {
namespace {

class JsonError : public std::runtime_error {
public:
    explicit JsonError(const std::string& message) : std::runtime_error(message) {}
};

json PropertyToJson(const PropertyValue& value) {
    switch (value.kind) {
    case PropertyKind::Float:
        return static_cast<double>(value.number);
    case PropertyKind::Vector2:
        return json::array({value.vec2.x, value.vec2.y});
    case PropertyKind::Vector3:
    case PropertyKind::Color3:
        return json::array({value.vec3.x, value.vec3.y, value.vec3.z});
    case PropertyKind::Vector4:
    case PropertyKind::Color4:
        return json::array({value.vec4.x, value.vec4.y, value.vec4.z, value.vec4.w});
    case PropertyKind::Bool:
        return value.boolean;
    case PropertyKind::String:
    case PropertyKind::AssetPath:
        return value.text;
    }
    return nullptr;
}

float GetNumber(const json& value, const std::string& context) {
    if (!value.is_number()) {
        throw JsonError(context + " must be a number.");
    }
    return value.get<float>();
}

std::string GetString(const json& value, const std::string& context) {
    if (!value.is_string()) {
        throw JsonError(context + " must be a string.");
    }
    return value.get<std::string>();
}

std::uint64_t GetId(const json& value, const std::string& context) {
    if (value.is_number_unsigned()) {
        const auto id = value.get<std::uint64_t>();
        if (id == 0) throw JsonError(context + " must be greater than zero.");
        return id;
    }
    if (value.is_number_integer()) {
        const auto id = value.get<std::int64_t>();
        if (id <= 0) throw JsonError(context + " must be greater than zero.");
        return static_cast<std::uint64_t>(id);
    }
    throw JsonError(context + " must be an unsigned integer.");
}

Vec2 ReadVec2(const json& value, const std::string& context) {
    if (!value.is_array() || value.size() != 2) {
        throw JsonError(context + " must be an array of two numbers.");
    }
    return Vec2(GetNumber(value[0], context + "[0]"), GetNumber(value[1], context + "[1]"));
}

Vec3 ReadVec3(const json& value, const std::string& context) {
    if (!value.is_array() || value.size() != 3) {
        throw JsonError(context + " must be an array of three numbers.");
    }
    return Vec3(GetNumber(value[0], context + "[0]"), GetNumber(value[1], context + "[1]"),
                GetNumber(value[2], context + "[2]"));
}

Vec4 ReadVec4(const json& value, const std::string& context) {
    if (!value.is_array() || value.size() != 4) {
        throw JsonError(context + " must be an array of four numbers.");
    }
    return Vec4(GetNumber(value[0], context + "[0]"), GetNumber(value[1], context + "[1]"),
                GetNumber(value[2], context + "[2]"), GetNumber(value[3], context + "[3]"));
}

PropertyValue DefaultValueForPinType(PinType type) {
    switch (type) {
    case PinType::Vector2: return PropertyValue::Vec2Value(0.0f, 0.0f);
    case PinType::Vector3: return PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f);
    case PinType::Vector4: return PropertyValue::Vec4Value(0.0f, 0.0f, 0.0f, 0.0f);
    case PinType::Float:
    case PinType::Texture:
    case PinType::Sampler:
    case PinType::Wildcard:
    default:
        return PropertyValue::Float(0.0f);
    }
}

PropertyValue PropertyFromJson(const json& value, PropertyKind kind, const std::string& context) {
    switch (kind) {
    case PropertyKind::Float:
        return PropertyValue::Float(GetNumber(value, context));
    case PropertyKind::Vector2: {
        const Vec2 v = ReadVec2(value, context);
        return PropertyValue::Vec2Value(v.x, v.y);
    }
    case PropertyKind::Vector3: {
        const Vec3 v = ReadVec3(value, context);
        return PropertyValue::Vec3Value(v.x, v.y, v.z);
    }
    case PropertyKind::Vector4: {
        const Vec4 v = ReadVec4(value, context);
        return PropertyValue::Vec4Value(v.x, v.y, v.z, v.w);
    }
    case PropertyKind::Color3: {
        const Vec3 v = ReadVec3(value, context);
        return PropertyValue::Color3Value(v.x, v.y, v.z);
    }
    case PropertyKind::Color4: {
        const Vec4 v = ReadVec4(value, context);
        return PropertyValue::Color4Value(v.x, v.y, v.z, v.w);
    }
    case PropertyKind::Bool:
        if (!value.is_boolean()) {
            throw JsonError(context + " must be a boolean.");
        }
        return PropertyValue::BoolValue(value.get<bool>());
    case PropertyKind::String:
        return PropertyValue::StringValue(GetString(value, context));
    case PropertyKind::AssetPath:
        return PropertyValue::AssetPathValue(GetString(value, context));
    }
    throw JsonError(context + " has an unsupported property kind.");
}

std::string ReadFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw JsonError("Could not open file: " + path.string());
    }
    std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (contents.size() >= 3 &&
        static_cast<unsigned char>(contents[0]) == 0xEF &&
        static_cast<unsigned char>(contents[1]) == 0xBB &&
        static_cast<unsigned char>(contents[2]) == 0xBF) {
        contents.erase(0, 3);
    }
    return contents;
}

} // namespace

const char* GraphSerializer::SchemaId() {
    return kSchemaId;
}

int GraphSerializer::SchemaVersion() {
    return kSchemaVersion;
}

json GraphSerializer::ToJson(const MaterialGraph& graph) {
    json document;
    document["schema"] = SchemaId();
    document["version"] = SchemaVersion();
    document["generator"] = "MaterialForge 1.1.0";
    document["engine"] = "Unreal Engine 5";

    const MaterialSettings& settings = graph.Settings();
    document["material"] = {
        {"name", settings.name},
        {"domain", settings.domain},
        {"blendMode", settings.blendMode},
        {"shadingModel", settings.shadingModel},
        {"translucencyLightingMode", settings.translucencyLightingMode},
        {"twoSided", settings.twoSided},
        {"ditheredLodTransition", settings.ditheredLodTransition},
        {"castDynamicShadowAsMasked", settings.castDynamicShadowAsMasked},
        {"useEmissiveForStaticLighting", settings.useEmissiveForStaticLighting},
        {"opacityMaskClip", settings.opacityMaskClip}
    };

    TypeResolver resolver(graph);
    json nodes = json::array();
    for (const auto& nodePtr : graph.Nodes()) {
        const Node& node = *nodePtr;
        const NodeDefinition* definition = NodeRegistry::Get().Find(node.type);

        json nodeJson;
        nodeJson["id"] = node.id;
        nodeJson["type"] = node.type;
        nodeJson["ueClass"] = definition ? definition->ueClass : std::string();
        nodeJson["category"] = definition ? definition->category : std::string("Unknown");
        nodeJson["position"] = {
            {"x", node.position.x},
            {"y", node.position.y}
        };
        if (definition && definition->isComment) {
            nodeJson["size"] = {
                {"x", node.size.x},
                {"y", node.size.y}
            };
        }

        json properties = json::object();
        for (const auto& [key, value] : node.properties) {
            properties[key] = PropertyToJson(value);
        }
        nodeJson["properties"] = std::move(properties);

        json pins = json::array();
        auto writePin = [&](const Pin& pin) {
            json pinJson;
            pinJson["id"] = pin.id;
            pinJson["name"] = pin.name;
            pinJson["direction"] = PinDirectionToString(pin.direction);
            pinJson["type"] = PinTypeToString(pin.type);
            if (pin.IsSplitComponent()) {
                pinJson["splitFrom"] = pin.splitFrom;
                pinJson["splitIndex"] = pin.splitIndex;
            }
            if (pin.hidden) {
                pinJson["hidden"] = true;
            }
            const PinType resolved = resolver.GetEffectiveType(pin);
            if (resolved != pin.type) {
                pinJson["resolvedType"] = PinTypeToString(resolved);
            }
            if (pin.direction == PinDirection::Input &&
                pin.type != PinType::Texture && pin.type != PinType::Sampler) {
                pinJson["default"] = PropertyToJson(pin.defaultValue);
            }
            pins.push_back(std::move(pinJson));
        };
        for (const Pin& pin : node.inputs) writePin(pin);
        for (const Pin& pin : node.outputs) writePin(pin);
        nodeJson["pins"] = std::move(pins);
        nodes.push_back(std::move(nodeJson));
    }
    document["nodes"] = std::move(nodes);

    json links = json::array();
    for (const Link& link : graph.Links()) {
        const Pin* from = graph.FindPin(link.fromPin);
        const Pin* to = graph.FindPin(link.toPin);
        if (!from || !to) {
            continue;
        }
        links.push_back({
            {"id", link.id},
            {"fromNode", from->nodeId},
            {"fromPin", from->id},
            {"fromPinName", from->name},
            {"toNode", to->nodeId},
            {"toPin", to->id},
            {"toPinName", to->name}
        });
    }
    document["links"] = std::move(links);
    return document;
}

bool GraphSerializer::FromJson(const json& document, MaterialGraph& graph, std::string* error) {
    try {
        if (!document.is_object()) {
            throw JsonError("Root JSON value must be an object.");
        }
        const std::string schema = document.value("schema", std::string());
        if (schema != SchemaId()) {
            throw JsonError("Unsupported schema '" + schema + "'. Expected '" + SchemaId() + "'.");
        }
        const int version = document.value("version", 0);
        if (version != SchemaVersion()) {
            throw JsonError("Unsupported schema version " + std::to_string(version) + ".");
        }
        if (!document.contains("nodes") || !document["nodes"].is_array()) {
            throw JsonError("Document must contain a 'nodes' array.");
        }
        if (!document.contains("links") || !document["links"].is_array()) {
            throw JsonError("Document must contain a 'links' array.");
        }

        MaterialGraph loaded;
        loaded.Clear();

        if (document.contains("material") && document["material"].is_object()) {
            const json& material = document["material"];
            MaterialSettings settings;
            settings.name = material.value("name", settings.name);
            settings.domain = material.value("domain", settings.domain);
            settings.blendMode = material.value("blendMode", settings.blendMode);
            settings.shadingModel = material.value("shadingModel", settings.shadingModel);
            settings.translucencyLightingMode =
                material.value("translucencyLightingMode", settings.translucencyLightingMode);
            settings.twoSided = material.value("twoSided", settings.twoSided);
            settings.ditheredLodTransition =
                material.value("ditheredLodTransition", settings.ditheredLodTransition);
            settings.castDynamicShadowAsMasked = material.value(
                "castDynamicShadowAsMasked", settings.castDynamicShadowAsMasked);
            settings.useEmissiveForStaticLighting = material.value(
                "useEmissiveForStaticLighting", settings.useEmissiveForStaticLighting);
            settings.opacityMaskClip = std::clamp(
                static_cast<float>(material.value("opacityMaskClip", settings.opacityMaskClip)),
                0.0f, 1.0f);
            loaded.Settings() = settings;
        }

        std::unordered_map<NodeId, NodeId> nodeIdMap;
        std::unordered_map<PinId, NodeId> pinToNodeMap;
        std::unordered_map<PinId, PinDirection> pinDirectionMap;
        std::unordered_map<PinId, std::string> pinNameMap;
        std::unordered_set<NodeId> seenNodeIds;
        std::unordered_set<PinId> seenPinIds;
        int outputCount = 0;

        for (const json& nodeJson : document["nodes"]) {
            if (!nodeJson.is_object()) {
                throw JsonError("Each entry in 'nodes' must be an object.");
            }
            if (!nodeJson.contains("id")) {
                throw JsonError("Node must contain an 'id'.");
            }
            const NodeId jsonNodeId = GetId(nodeJson["id"], "Node id");
            if (!seenNodeIds.insert(jsonNodeId).second) {
                throw JsonError("Duplicate node id " + std::to_string(jsonNodeId) + ".");
            }
            const std::string type = GetString(nodeJson.at("type"), "Node type");
            const NodeDefinition* definition = NodeRegistry::Get().Find(type);
            if (!definition) {
                throw JsonError("Unknown node type '" + type + "'.");
            }
            if (definition->isMaterialOutput) {
                ++outputCount;
                if (outputCount > 1) {
                    throw JsonError("Graph documents may contain at most one Material Output node.");
                }
            }

            Vec2 position(0.0f, 0.0f);
            if (nodeJson.contains("position")) {
                position.x = GetNumber(nodeJson["position"].at("x"), "position.x");
                position.y = GetNumber(nodeJson["position"].at("y"), "position.y");
            }

            Node& node = loaded.AddNode(*definition, position);
            nodeIdMap[jsonNodeId] = node.id;
            if (definition->isComment && nodeJson.contains("size")) {
                node.size.x = std::max(80.0f, GetNumber(nodeJson["size"].at("x"), "size.x"));
                node.size.y = std::max(60.0f, GetNumber(nodeJson["size"].at("y"), "size.y"));
            }

            if (definition->isFunctionCall && nodeJson.contains("pins") &&
                nodeJson["pins"].is_array()) {
                std::unordered_set<std::string> inputNames;
                std::unordered_set<std::string> outputNames;
                for (const json& pinJson : nodeJson["pins"]) {
                    if (!pinJson.is_object()) {
                        throw JsonError("Function call pins must be objects.");
                    }
                    const std::string name = GetString(pinJson.at("name"), "Pin name");
                    const std::string directionText =
                        GetString(pinJson.at("direction"), "Pin direction");
                    Pin pin;
                    pin.id = loaded.NextPinId();
                    pin.nodeId = node.id;
                    pin.name = name;
                    pin.direction = directionText == "input" ? PinDirection::Input
                                                            : PinDirection::Output;
                    if (directionText != "input" && directionText != "output") {
                        throw JsonError("Pin direction must be 'input' or 'output'.");
                    }
                    bool typeOk = true;
                    pin.type = pinJson.contains("type")
                                   ? PinTypeFromString(
                                         GetString(pinJson["type"], "Pin type"), &typeOk)
                                   : PinType::Float;
                    if (!typeOk) {
                        throw JsonError("Unknown pin type on function call node.");
                    }
                    pin.defaultValue = DefaultValueForPinType(pin.type);
                    if (pin.direction == PinDirection::Input && pinJson.contains("default") &&
                        pin.type != PinType::Texture && pin.type != PinType::Sampler) {
                        pin.defaultValue = PropertyFromJson(
                            pinJson["default"], pin.defaultValue.kind,
                            "Function call pin '" + name + "' default");
                    }
                    if (pin.direction == PinDirection::Input) {
                        if (!inputNames.insert(name).second) {
                            throw JsonError("Duplicate function call input pin '" + name + "'.");
                        }
                        node.inputs.push_back(std::move(pin));
                    } else {
                        if (!outputNames.insert(name).second) {
                            throw JsonError("Duplicate function call output pin '" + name + "'.");
                        }
                        node.outputs.push_back(std::move(pin));
                    }
                }
            }

            if (nodeJson.contains("pins") && nodeJson["pins"].is_array()) {
                for (const json& pinJson : nodeJson["pins"]) {
                    if (!pinJson.is_object() ||
                        !pinJson.contains("splitFrom")) {
                        continue;
                    }
                    const std::string name = GetString(pinJson.at("name"), "Pin name");
                    const std::string direction =
                        GetString(pinJson.at("direction"), "Pin direction");
                    if (direction != "output" || node.FindOutput(name)) {
                        continue;
                    }
                    Pin pin;
                    pin.id = loaded.NextPinId();
                    pin.nodeId = node.id;
                    pin.name = name;
                    pin.direction = PinDirection::Output;
                    pin.type = PinType::Float;
                    pin.splitFrom = GetString(pinJson["splitFrom"], "splitFrom");
                    pin.splitIndex = pinJson.value("splitIndex", -1);
                    if (pin.splitFrom.empty() || pin.splitIndex < 0 ||
                        pin.splitIndex > 3) {
                        throw JsonError("Invalid split output pin '" + name + "'.");
                    }
                    node.outputs.push_back(std::move(pin));
                }
            }

            if (nodeJson.contains("properties")) {
                const json& properties = nodeJson["properties"];
                if (!properties.is_object()) {
                    throw JsonError("Node '" + type + "' properties must be an object.");
                }
                for (auto it = properties.begin(); it != properties.end(); ++it) {
                    const PropertySpec* spec = nullptr;
                    for (const PropertySpec& candidate : definition->properties) {
                        if (candidate.key == it.key()) {
                            spec = &candidate;
                            break;
                        }
                    }
                    if (!spec) {
                        throw JsonError("Unknown property '" + it.key() + "' on node type '" + type + "'.");
                    }
                    node.properties[it.key()] = PropertyFromJson(
                        it.value(), spec->defaultValue.kind,
                        "Node '" + type + "' property '" + it.key() + "'");
                }
            }

            if (!nodeJson.contains("pins") || !nodeJson["pins"].is_array()) {
                throw JsonError("Node '" + type + "' must contain a 'pins' array.");
            }
            for (const json& pinJson : nodeJson["pins"]) {
                if (!pinJson.is_object() || !pinJson.contains("id")) {
                    throw JsonError("Every pin must be an object with an 'id'.");
                }
                const PinId jsonPinId = GetId(pinJson["id"], "Pin id");
                if (!seenPinIds.insert(jsonPinId).second) {
                    throw JsonError("Duplicate pin id " + std::to_string(jsonPinId) + ".");
                }
                const std::string name = GetString(pinJson.at("name"), "Pin name");
                const std::string direction = GetString(pinJson.at("direction"), "Pin direction");
                Pin* pin = nullptr;
                if (direction == "input") {
                    pin = node.FindInput(name);
                } else if (direction == "output") {
                    pin = node.FindOutput(name);
                } else {
                    throw JsonError("Pin direction must be 'input' or 'output'.");
                }
                if (!pin) {
                    throw JsonError("Node '" + type + "' has no " + direction + " pin named '" + name + "'.");
                }
                pinToNodeMap[jsonPinId] = node.id;
                pinDirectionMap[jsonPinId] = pin->direction;
                pinNameMap[jsonPinId] = name;
                if (pinJson.value("hidden", false)) {
                    pin->hidden = true;
                }

                if (pin->direction == PinDirection::Input && pinJson.contains("default") &&
                    pin->type != PinType::Texture && pin->type != PinType::Sampler) {
                    pin->defaultValue = PropertyFromJson(
                        pinJson["default"], pin->defaultValue.kind,
                        "Node '" + type + "' pin '" + name + "' default");
                }
            }
        }

        std::unordered_set<PinId> connectedInputs;
        for (const json& linkJson : document["links"]) {
            if (!linkJson.is_object() ||
                !linkJson.contains("fromPin") || !linkJson.contains("toPin")) {
                throw JsonError("Each link must contain 'fromPin' and 'toPin' ids.");
            }
            const PinId fromJson = GetId(linkJson["fromPin"], "Link fromPin");
            const PinId toJson = GetId(linkJson["toPin"], "Link toPin");
            const auto fromIt = pinToNodeMap.find(fromJson);
            const auto toIt = pinToNodeMap.find(toJson);
            if (fromIt == pinToNodeMap.end() || toIt == pinToNodeMap.end()) {
                throw JsonError("Link references a pin id that does not exist.");
            }
            if (pinDirectionMap[fromJson] != PinDirection::Output ||
                pinDirectionMap[toJson] != PinDirection::Input) {
                throw JsonError("Link endpoints must be an output pin followed by an input pin.");
            }
            if (!connectedInputs.insert(toJson).second) {
                throw JsonError("Input pin '" + pinNameMap[toJson] + "' has more than one incoming link.");
            }

            Node* fromNode = loaded.FindNode(fromIt->second);
            Node* toNode = loaded.FindNode(toIt->second);
            if (!fromNode || !toNode) {
                throw JsonError("Link references a missing node.");
            }
            const Pin* from = fromNode->FindOutput(pinNameMap[fromJson]);
            const Pin* to = toNode->FindInput(pinNameMap[toJson]);
            if (!from || !to) {
                throw JsonError("Link references an impossible pin combination.");
            }
            std::string reason;
            if (!loaded.Connect(from->id, to->id, &reason)) {
                throw JsonError("Invalid link: " + reason);
            }
        }

        loaded.RebuildIdCounters();
        loaded.MarkSaved();
        graph = std::move(loaded);
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) {
            *error = exception.what();
        }
        return false;
    }
}

bool GraphSerializer::Save(const MaterialGraph& graph, const std::string& path, std::string* error) {
    try {
        const fs::path filePath = fs::u8path(path);
        if (filePath.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(filePath.parent_path(), ec);
            if (ec) {
                throw JsonError("Could not create directory '" + filePath.parent_path().string() +
                                "': " + ec.message());
            }
        }
        const json document = ToJson(graph);
        std::ofstream stream(filePath, std::ios::binary | std::ios::trunc);
        if (!stream) {
            throw JsonError("Could not open file for writing: " + filePath.string());
        }
        stream << document.dump(2) << '\n';
        if (!stream) {
            throw JsonError("Failed while writing file: " + filePath.string());
        }
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) {
            *error = exception.what();
        }
        return false;
    }
}

bool GraphSerializer::Load(const std::string& path, MaterialGraph& graph, std::string* error) {
    try {
        const fs::path filePath = fs::u8path(path);
        const std::string contents = ReadFile(filePath);
        const json document = json::parse(contents);
        return FromJson(document, graph, error);
    } catch (const json::parse_error& exception) {
        std::string message = "JSON parse error: " + std::string(exception.what());
        if (error) *error = message;
        return false;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

} // namespace mf
