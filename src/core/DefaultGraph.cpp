#include "core/DefaultGraph.h"

#include "core/NodeRegistry.h"

namespace mf {
namespace {

Node* AddNode(MaterialGraph& graph, const char* type, Vec2 position, std::string* error) {
    return graph.AddNodeByType(type, position, error);
}

bool Connect(MaterialGraph& graph, const Node* fromNode, const char* outputName,
             const Node* toNode, const char* inputName, std::string* error) {
    if (!fromNode || !toNode) {
        if (error) *error = "Internal demo graph error: missing node.";
        return false;
    }
    const Pin* output = fromNode->FindOutput(outputName);
    const Pin* input = toNode->FindInput(inputName);
    if (!output || !input) {
        if (error) {
            *error = "Internal demo graph error: missing pin '" +
                     std::string(output ? inputName : outputName) + "'.";
        }
        return false;
    }
    return graph.Connect(output->id, input->id, error) != nullptr;
}

void SetProperty(Node* node, const std::string& key, const PropertyValue& value) {
    if (!node) return;
    node->properties[key] = value;
}

} // namespace

bool BuildDefaultGraph(MaterialGraph& graph, std::string* error) {
    graph.Clear();
    graph.Settings().name = "M_HoloCircuit";

    Node* output = AddNode(graph, "MaterialOutput", Vec2(330.0f, -260.0f), error);
    Node* time = AddNode(graph, "Time", Vec2(-450.0f, -340.0f), error);
    Node* texCoord = AddNode(graph, "TextureCoordinate", Vec2(-450.0f, -180.0f), error);
    Node* textureObject = AddNode(graph, "TextureObject", Vec2(-450.0f, 20.0f), error);
    Node* panner = AddNode(graph, "Panner", Vec2(-200.0f, -340.0f), error);
    Node* textureSample = AddNode(graph, "TextureSample", Vec2(-200.0f, -150.0f), error);
    Node* baseTint = AddNode(graph, "VectorParameter", Vec2(-200.0f, 110.0f), error);
    Node* baseMultiply = AddNode(graph, "Multiply", Vec2(70.0f, -300.0f), error);
    Node* roughness = AddNode(graph, "ScalarParameter", Vec2(70.0f, -120.0f), error);
    Node* metallic = AddNode(graph, "ScalarParameter", Vec2(70.0f, 20.0f), error);
    Node* fresnel = AddNode(graph, "Fresnel", Vec2(70.0f, 180.0f), error);
    Node* glowTint = AddNode(graph, "Constant3Vector", Vec2(70.0f, 320.0f), error);
    Node* fresnelGlow = AddNode(graph, "Multiply", Vec2(330.0f, 180.0f), error);
    Node* emissiveAdd = AddNode(graph, "Add", Vec2(330.0f, 380.0f), error);

    if (!output || !time || !texCoord || !textureObject || !panner || !textureSample ||
        !baseTint || !baseMultiply || !roughness || !metallic || !fresnel ||
        !glowTint || !fresnelGlow || !emissiveAdd) {
        return false;
    }

    SetProperty(texCoord, "uTiling", PropertyValue::Float(2.0f));
    SetProperty(texCoord, "vTiling", PropertyValue::Float(2.0f));
    SetProperty(panner, "speed", PropertyValue::Vec2Value(0.015f, 0.006f));
    SetProperty(textureObject, "texture",
                PropertyValue::AssetPathValue("assets/textures/T_HoloGrid.png"));
    SetProperty(baseTint, "parameterName", PropertyValue::StringValue("BaseTint"));
    SetProperty(baseTint, "value", PropertyValue::Color4Value(0.18f, 0.82f, 0.9f, 1.0f));
    SetProperty(roughness, "parameterName", PropertyValue::StringValue("Roughness"));
    SetProperty(roughness, "value", PropertyValue::Float(0.22f));
    SetProperty(metallic, "parameterName", PropertyValue::StringValue("Metallic"));
    SetProperty(metallic, "value", PropertyValue::Float(0.9f));
    SetProperty(glowTint, "value", PropertyValue::Color3Value(0.0f, 0.55f, 0.7f));
    SetProperty(fresnel, "baseReflectFraction", PropertyValue::Float(0.05f));

    if (!Connect(graph, time, "Time", panner, "Time", error)) return false;
    if (!Connect(graph, texCoord, "UV", panner, "Coordinate", error)) return false;
    if (!Connect(graph, panner, "UV", textureSample, "UVs", error)) return false;
    if (!Connect(graph, textureObject, "Texture", textureSample, "Texture", error)) return false;
    if (!Connect(graph, textureSample, "RGB", baseMultiply, "A", error)) return false;
    if (!Connect(graph, baseTint, "Value", baseMultiply, "B", error)) return false;
    if (!Connect(graph, baseMultiply, "Result", output, "Base Color", error)) return false;
    if (!Connect(graph, roughness, "Value", output, "Roughness", error)) return false;
    if (!Connect(graph, metallic, "Value", output, "Metallic", error)) return false;
    if (!Connect(graph, fresnel, "Result", fresnelGlow, "A", error)) return false;
    if (!Connect(graph, glowTint, "Value", fresnelGlow, "B", error)) return false;
    if (!Connect(graph, textureSample, "R", emissiveAdd, "A", error)) return false;
    if (!Connect(graph, fresnelGlow, "Result", emissiveAdd, "B", error)) return false;
    if (!Connect(graph, emissiveAdd, "Result", output, "Emissive Color", error)) return false;

    return true;
}

} // namespace mf
