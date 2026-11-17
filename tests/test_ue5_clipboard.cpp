#include "core/Graph.h"
#include "core/NodeRegistry.h"
#include "io/Ue5Clipboard.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace mf;

namespace {

int g_Failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_Failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

MaterialGraph GraphWithOutput() {
    MaterialGraph graph;
    const NodeDefinition* output = NodeRegistry::Get().Find("MaterialOutput");
    if (output) {
        graph.AddNode(*output, Vec2(400.0f, 100.0f));
    }
    return graph;
}

const Node* FindNodeOfType(const MaterialGraph& graph, const std::string& type) {
    for (const auto& node : graph.Nodes()) {
        if (node->type == type) return node.get();
    }
    return nullptr;
}

void TestLooksLikeClipboard() {
    Check(!LooksLikeUe5MaterialClipboard(""), "empty text is not a UE clipboard");
    Check(!LooksLikeUe5MaterialClipboard("{\"schema\":\"materialforge.ue5.material\"}"),
          "JSON is not a UE clipboard");
    Check(LooksLikeUe5MaterialClipboard(
              "Begin Object Class=/Script/UnrealEd.MaterialGraphNode Name=\"MaterialGraphNode_0\" End Object"),
          "graph node export is recognized");
}

void TestUe5CompactClipboard() {
    const std::string text =
        "Begin Object Class=/Script/UnrealEd.MaterialGraphNode_Root "
        "Name=\"MaterialGraphNode_Root_0\" ExportPath=\"/Script/UnrealEd."
        "MaterialGraphNode_Root'/Engine/Transient.M_Test:MaterialGraph_0."
        "MaterialGraphNode_Root_0'\" Material=\"/Script/UnrealEd.PreviewMaterial'/Engine/"
        "Transient.M_Test'\" NodePosX=512 NodeGuid=A84BEE874D6CDDE93587339C217C2255 "
        "CustomProperties Pin (PinId=5C5DEF9B410A47CBEE6F21B3BB023E5D,PinName=\"Base Color\","
        "PinType.PinCategory=\"materialinput\",PinType.PinSubCategory=\"rgba\","
        "DefaultValue=\"(R=0.500000,G=0.500000,B=0.500000,A=1.000000)\","
        "LinkedTo=(MaterialGraphNode_1 2B82E8214414524B1B2212B63FAFC061,),"
        "PersistentGuid=00000000000000000000000000000000,bHidden=False) "
        "CustomProperties Pin (PinId=87097F064E28883CC804C29B8188E54A,PinName=\"Roughness\","
        "PinType.PinCategory=\"materialinput\",DefaultValue=\"0.5\",bHidden=False) "
        "End Object "
        "Begin Object Class=/Script/UnrealEd.MaterialGraphNode Name=\"MaterialGraphNode_1\" "
        "Begin Object Class=/Script/Engine.MaterialExpressionConstant3Vector "
        "Name=\"MaterialExpressionConstant3Vector_0\" End Object "
        "Begin Object Name=\"MaterialExpressionConstant3Vector_0\" "
        "Constant=(R=0.100000,G=0.200000,B=0.300000,A=1.000000) "
        "MaterialExpressionEditorX=-224 MaterialExpressionEditorY=96 End Object "
        "MaterialExpression=\"/Script/Engine.MaterialExpressionConstant3Vector'"
        "MaterialExpressionConstant3Vector_0'\" NodePosX=-224 NodePosY=96 "
        "NodeGuid=107492C54A9A800B62DAE08482FD4235 "
        "CustomProperties Pin (PinId=2B82E8214414524B1B2212B63FAFC061,PinName=\"Output\","
        "Direction=\"EGPD_Output\",PinType.PinCategory=\"\","
        "LinkedTo=(MaterialGraphNode_Root_0 5C5DEF9B410A47CBEE6F21B3BB023E5D,),"
        "bHidden=False) End Object";

    MaterialGraph graph = GraphWithOutput();
    Ue5ClipboardImportResult result;
    Check(ImportUe5MaterialClipboard(text, graph, Vec2(100.0f, 50.0f), &result),
          "compact UE5 clipboard import succeeds");
    Check(result.success, "result reports success");
    Check(result.nodeCount == 1, "one expression imported");
    Check(result.linkCount == 1, "base color link imported");
    Check(graph.Links().size() == 1, "graph contains one link");

    const Node* constant = FindNodeOfType(graph, "Constant3Vector");
    Check(constant != nullptr, "constant3vector node exists");
    if (constant) {
        const auto value = constant->properties.find("value");
        Check(value != constant->properties.end(), "constant has value property");
        if (value != constant->properties.end()) {
            Check(value->second.kind == PropertyKind::Color3 ||
                      value->second.kind == PropertyKind::Vector3,
                  "constant value is a vector");
            Check(std::abs(value->second.vec3.x - 0.1f) < 0.001f, "R imported");
            Check(std::abs(value->second.vec3.y - 0.2f) < 0.001f, "G imported");
            Check(std::abs(value->second.vec3.z - 0.3f) < 0.001f, "B imported");
        }
        Check(std::abs(constant->position.x - 100.0f) < 0.01f, "layout placed at origin x");
        Check(std::abs(constant->position.y - 50.0f) < 0.01f, "layout placed at origin y");
    }

    const Node* output = graph.FindMaterialOutput();
    Check(output != nullptr, "material output exists");
    if (output) {
        const Pin* baseColor = output->FindInput("Base Color");
        Check(baseColor != nullptr, "base color input exists");
        if (baseColor) {
            Check(graph.IsPinConnected(baseColor->id), "base color is connected");
            const auto& defaultValue = baseColor->defaultValue;
            Check(defaultValue.kind == PropertyKind::Vector3 ||
                      defaultValue.kind == PropertyKind::Color3,
                  "root default value was applied");
            Check(std::abs(defaultValue.vec3.x - 0.5f) < 0.001f, "root R default applied");
        }
    }
}

void TestUe4MultilineClipboard() {
    const std::string text =
        "Begin Object Class=/Script/UnrealEd.MaterialGraphNode_Root "
        "Name=\"MaterialGraphNode_Root_0\"\n"
        "   NodePosX=512\n"
        "   CustomProperties Pin (PinId=R1,PinName=\"Roughness\","
        "PinType.PinCategory=\"materialinput\",DefaultValue=\"0.5\","
        "LinkedTo=(MaterialGraphNode_2 P2,),bHidden=False,)\n"
        "End Object\n"
        "Begin Object Class=/Script/UnrealEd.MaterialGraphNode Name=\"MaterialGraphNode_1\"\n"
        "   Begin Object Class=/Script/Engine.MaterialExpressionScalarParameter "
        "Name=\"MaterialExpressionScalarParameter_0\"\n"
        "   End Object\n"
        "   Begin Object Name=\"MaterialExpressionScalarParameter_0\"\n"
        "      ParameterName=\"Roughness\"\n"
        "      DefaultValue=0.25\n"
        "      Group=\"Surface\"\n"
        "      SortPriority=3\n"
        "      SliderMin=0.0\n"
        "      SliderMax=0.8\n"
        "      MaterialExpressionEditorX=-800\n"
        "      MaterialExpressionEditorY=272\n"
        "   End Object\n"
        "   MaterialExpression=MaterialExpressionScalarParameter'"
        "\"MaterialExpressionScalarParameter_0\"'\n"
        "   NodePosX=-800\n"
        "   NodePosY=272\n"
        "   CustomProperties Pin (PinId=P1,PinName=\"Output\","
        "Direction=\"EGPD_Output\",PinType.PinCategory=\"\","
        "LinkedTo=(MaterialGraphNode_2 Q2,),bHidden=False,)\n"
        "End Object\n"
        "Begin Object Class=/Script/UnrealEd.MaterialGraphNode Name=\"MaterialGraphNode_2\"\n"
        "   Begin Object Class=/Script/Engine.MaterialExpressionLinearInterpolate "
        "Name=\"MaterialExpressionLinearInterpolate_0\"\n"
        "   End Object\n"
        "   Begin Object Name=\"MaterialExpressionLinearInterpolate_0\"\n"
        "      B=(Expression=MaterialExpressionScalarParameter'"
        "\"MaterialGraphNode_1.MaterialExpressionScalarParameter_0\"')\n"
        "      Alpha=0.5\n"
        "      MaterialExpressionEditorX=-432\n"
        "      MaterialExpressionEditorY=96\n"
        "   End Object\n"
        "   NodePosX=-432\n"
        "   NodePosY=96\n"
        "   CustomProperties Pin (PinId=Q1,PinName=\"A\","
        "PinType.PinCategory=\"optional\",DefaultValue=\"0.0\",bHidden=False,)\n"
        "   CustomProperties Pin (PinId=Q2,PinName=\"B\","
        "PinType.PinCategory=\"optional\",DefaultValue=\"1.0\","
        "LinkedTo=(MaterialGraphNode_1 P1,),bHidden=False,)\n"
        "   CustomProperties Pin (PinId=P2,PinName=\"Output\","
        "Direction=\"EGPD_Output\",PinType.PinCategory=\"\","
        "LinkedTo=(MaterialGraphNode_Root_0 R1,),bHidden=False,)\n"
        "End Object\n"
        "Begin Object Class=/Script/UnrealEd.MaterialGraphNode Name=\"MaterialGraphNode_3\"\n"
        "   Begin Object Class=/Script/Engine.MaterialExpressionCustom "
        "Name=\"MaterialExpressionCustom_0\"\n"
        "   End Object\n"
        "   Begin Object Name=\"MaterialExpressionCustom_0\"\n"
        "      Code=\"return 1;\"\n"
        "      MaterialExpressionEditorX=0\n"
        "      MaterialExpressionEditorY=0\n"
        "   End Object\n"
        "End Object\n";

    MaterialGraph graph = GraphWithOutput();
    Ue5ClipboardImportResult result;
    Check(ImportUe5MaterialClipboard(text, graph, Vec2(0.0f, 0.0f), &result),
          "UE4 multiline clipboard import succeeds");
    Check(result.nodeCount == 2, "two supported expressions imported");
    Check(result.skippedNodeCount == 1, "custom expression was skipped");
    Check(result.linkCount == 2, "roughness output and parameter links imported");
    Check(graph.Links().size() == 2, "graph contains two links");

    const Node* parameter = FindNodeOfType(graph, "ScalarParameter");
    Check(parameter != nullptr, "scalar parameter node exists");
    if (parameter) {
        const auto name = parameter->properties.find("parameterName");
        Check(name != parameter->properties.end() && name->second.text == "Roughness",
              "parameter name imported");
        const auto value = parameter->properties.find("value");
        Check(value != parameter->properties.end() &&
                  std::abs(value->second.number - 0.25f) < 0.001f,
              "parameter default imported");
        const auto group = parameter->properties.find("parameterGroup");
        Check(group != parameter->properties.end() && group->second.text == "Surface",
              "parameter group imported");
    }

    const Node* lerp = FindNodeOfType(graph, "Lerp");
    Check(lerp != nullptr, "LinearInterpolate aliases to Lerp");
    if (lerp) {
        Check(lerp->FindInput("B") != nullptr, "lerp B input exists");
        Check(lerp->FindOutput("Result") != nullptr, "lerp result output exists");
    }

    const Node* output = graph.FindMaterialOutput();
    Check(output != nullptr, "material output exists");
    if (output) {
        const Pin* roughness = output->FindInput("Roughness");
        Check(roughness != nullptr, "roughness input exists");
        if (roughness) {
            Check(graph.IsPinConnected(roughness->id), "roughness is connected");
        }
    }
}

void TestTextureSampleClipboard() {
    const std::string text =
        "Begin Object Class=/Script/UnrealEd.MaterialGraphNode_Root "
        "Name=\"MaterialGraphNode_Root_0\" CustomProperties Pin (PinId=R1,"
        "PinName=\"Base Color\",DefaultValue=\"(R=0,G=0,B=0,A=1)\","
        "LinkedTo=(MaterialGraphNode_7 P7,),) End Object "
        "Begin Object Class=/Script/UnrealEd.MaterialGraphNode Name=\"MaterialGraphNode_7\" "
        "Begin Object Class=/Script/Engine.MaterialExpressionTextureSample "
        "Name=\"MaterialExpressionTextureSample_0\" End Object "
        "Begin Object Name=\"MaterialExpressionTextureSample_0\" "
        "Texture=Texture2D'/Game/MaterialForge/Textures/T_Test.T_Test' "
        "MaterialExpressionEditorX=-224 MaterialExpressionEditorY=96 End Object "
        "NodePosX=-224 NodePosY=96 "
        "CustomProperties Pin (PinId=P7,PinName=\"RGB\",Direction=\"EGPD_Output\","
        "LinkedTo=(MaterialGraphNode_Root_0 R1,),) End Object";

    MaterialGraph graph = GraphWithOutput();
    Ue5ClipboardImportResult result;
    Check(ImportUe5MaterialClipboard(text, graph, Vec2(0.0f, 0.0f), &result),
          "texture sample clipboard import succeeds");
    Check(result.nodeCount == 2, "texture sample gets an auto TextureObject");
    Check(result.linkCount == 2, "texture and base color links imported");

    const Node* sample = FindNodeOfType(graph, "TextureSample");
    const Node* textureObject = FindNodeOfType(graph, "TextureObject");
    Check(sample != nullptr, "texture sample node exists");
    Check(textureObject != nullptr, "texture object node exists");
    if (textureObject) {
        const auto texture = textureObject->properties.find("texture");
        Check(texture != textureObject->properties.end(), "texture path property exists");
        if (texture != textureObject->properties.end()) {
            Check(texture->second.text == "/Game/MaterialForge/Textures/T_Test",
                  "UE texture asset path was extracted");
        }
    }
    if (sample) {
        const Pin* textureInput = sample->FindInput("Texture");
        Check(textureInput != nullptr, "texture sample has Texture input");
        if (textureInput) {
            Check(graph.IsPinConnected(textureInput->id),
                  "auto TextureObject is connected to the sample");
        }
    }
}

} // namespace

int main() {
    TestLooksLikeClipboard();
    TestUe5CompactClipboard();
    TestUe4MultilineClipboard();
    TestTextureSampleClipboard();
    if (g_Failures != 0) {
        std::cerr << g_Failures << " UE5 clipboard test(s) failed.\n";
        return 1;
    }
    std::cout << "UE5 clipboard tests passed.\n";
    return 0;
}
