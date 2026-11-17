#include "core/Graph.h"
#include "core/NodeRegistry.h"
#include "io/GraphSerializer.h"
#include "util/PathUtil.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace mf;

Node* AddNode(MaterialGraph& graph, const char* type, Vec2 position) {
    std::string error;
    Node* node = graph.AddNodeByType(type, position, &error);
    if (!node) {
        std::cerr << "Could not add " << type << ": " << error << "\n";
    }
    return node;
}

bool Connect(MaterialGraph& graph, Node* from, const char* output,
             Node* to, const char* input) {
    if (!from || !to) return false;
    Pin* outputPin = from->FindOutput(output);
    Pin* inputPin = to->FindInput(input);
    if (!outputPin || !inputPin) {
        std::cerr << "Missing pin " << output << " -> " << input << "\n";
        return false;
    }
    std::string error;
    if (!graph.Connect(outputPin->id, inputPin->id, &error)) {
        std::cerr << "Could not connect: " << error << "\n";
        return false;
    }
    return true;
}

void SetString(Node* node, const char* key, const std::string& value) {
    if (node) node->properties[key] = PropertyValue::StringValue(value);
}

void SetFloat(Node* node, const char* key, float value) {
    if (node) node->properties[key] = PropertyValue::Float(value);
}

MaterialGraph BuildCheapContrast() {
    MaterialGraph graph;
    graph.Settings().name = "MF_CheapContrast";
    Node* value = AddNode(graph, "FunctionInput", Vec2(-720, -80));
    Node* contrast = AddNode(graph, "FunctionInput", Vec2(-720, 100));
    Node* half = AddNode(graph, "Constant", Vec2(-720, 260));
    Node* subtract = AddNode(graph, "Subtract", Vec2(-420, -80));
    Node* multiply = AddNode(graph, "Multiply", Vec2(-180, 20));
    Node* add = AddNode(graph, "Add", Vec2(60, 100));
    Node* clamp = AddNode(graph, "Clamp", Vec2(300, 100));
    Node* output = AddNode(graph, "FunctionOutput", Vec2(560, 100));
    SetString(value, "inputName", "Value");
    SetString(contrast, "inputName", "Contrast");
    SetFloat(half, "value", 0.5f);
    SetString(output, "outputName", "Result");
    Connect(graph, value, "Value", subtract, "A");
    Connect(graph, half, "Value", subtract, "B");
    Connect(graph, subtract, "Result", multiply, "A");
    Connect(graph, contrast, "Value", multiply, "B");
    Connect(graph, multiply, "Result", add, "A");
    Connect(graph, half, "Value", add, "B");
    Connect(graph, add, "Result", clamp, "Value");
    Connect(graph, clamp, "Result", output, "Value");
    return graph;
}

MaterialGraph BuildSineWave() {
    MaterialGraph graph;
    graph.Settings().name = "MF_SineWave";
    Node* time = AddNode(graph, "FunctionInput", Vec2(-600, 0));
    Node* frequency = AddNode(graph, "FunctionInput", Vec2(-600, 160));
    Node* multiply = AddNode(graph, "Multiply", Vec2(-300, 60));
    Node* sine = AddNode(graph, "Sine", Vec2(-40, 60));
    Node* output = AddNode(graph, "FunctionOutput", Vec2(220, 60));
    SetString(time, "inputName", "Time");
    SetString(frequency, "inputName", "Frequency");
    SetString(output, "outputName", "Result");
    Connect(graph, time, "Value", multiply, "A");
    Connect(graph, frequency, "Value", multiply, "B");
    Connect(graph, multiply, "Result", sine, "Input");
    Connect(graph, sine, "Result", output, "Value");
    return graph;
}

MaterialGraph BuildRoundToInt() {
    MaterialGraph graph;
    graph.Settings().name = "MF_RoundToInt";
    Node* value = AddNode(graph, "FunctionInput", Vec2(-400, 0));
    Node* round = AddNode(graph, "Round", Vec2(-120, 0));
    Node* output = AddNode(graph, "FunctionOutput", Vec2(160, 0));
    SetString(value, "inputName", "Value");
    SetString(output, "outputName", "Result");
    Connect(graph, value, "Value", round, "Input");
    Connect(graph, round, "Result", output, "Value");
    return graph;
}

MaterialGraph BuildPulse() {
    MaterialGraph graph;
    graph.Settings().name = "MF_Pulse";
    Node* time = AddNode(graph, "FunctionInput", Vec2(-640, 0));
    Node* frequency = AddNode(graph, "FunctionInput", Vec2(-640, 160));
    Node* multiply = AddNode(graph, "Multiply", Vec2(-340, 60));
    Node* sine = AddNode(graph, "Sine", Vec2(-80, 60));
    Node* absolute = AddNode(graph, "Abs", Vec2(160, 60));
    Node* output = AddNode(graph, "FunctionOutput", Vec2(400, 60));
    SetString(time, "inputName", "Time");
    SetString(frequency, "inputName", "Frequency");
    SetString(output, "outputName", "Result");
    Connect(graph, time, "Value", multiply, "A");
    Connect(graph, frequency, "Value", multiply, "B");
    Connect(graph, multiply, "Result", sine, "Input");
    Connect(graph, sine, "Result", absolute, "Input");
    Connect(graph, absolute, "Result", output, "Value");
    return graph;
}

MaterialGraph BuildRemap01() {
    MaterialGraph graph;
    graph.Settings().name = "MF_Remap01";
    Node* value = AddNode(graph, "FunctionInput", Vec2(-800, -100));
    Node* inMin = AddNode(graph, "FunctionInput", Vec2(-800, 60));
    Node* inMax = AddNode(graph, "FunctionInput", Vec2(-800, 220));
    Node* subtract = AddNode(graph, "Subtract", Vec2(-500, -100));
    Node* range = AddNode(graph, "Subtract", Vec2(-500, 160));
    Node* divide = AddNode(graph, "Divide", Vec2(-240, -40));
    Node* clamp = AddNode(graph, "Clamp", Vec2(20, -40));
    Node* output = AddNode(graph, "FunctionOutput", Vec2(280, -40));
    SetString(value, "inputName", "Value");
    SetString(inMin, "inputName", "In Min");
    SetString(inMax, "inputName", "In Max");
    SetString(output, "outputName", "Result");
    Connect(graph, value, "Value", subtract, "A");
    Connect(graph, inMin, "Value", subtract, "B");
    Connect(graph, inMax, "Value", range, "A");
    Connect(graph, inMin, "Value", range, "B");
    Connect(graph, subtract, "Result", divide, "A");
    Connect(graph, range, "Result", divide, "B");
    Connect(graph, divide, "Result", clamp, "Value");
    Connect(graph, clamp, "Result", output, "Value");
    return graph;
}

} // namespace

int main() {
    const std::filesystem::path root = PathUtil::FindProjectRoot();
    const std::filesystem::path directory =
        (root.empty() ? std::filesystem::current_path() : root) / "assets" / "functions";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        std::cerr << "Could not create " << directory << ": " << ec.message() << "\n";
        return 1;
    }

    struct Preset {
        const char* name;
        MaterialGraph (*build)();
    };
    const Preset presets[] = {
        {"MF_CheapContrast.mffunction", &BuildCheapContrast},
        {"MF_SineWave.mffunction", &BuildSineWave},
        {"MF_RoundToInt.mffunction", &BuildRoundToInt},
        {"MF_Pulse.mffunction", &BuildPulse},
        {"MF_Remap01.mffunction", &BuildRemap01},
    };
    for (const Preset& preset : presets) {
        MaterialGraph graph = preset.build();
        const std::filesystem::path path = directory / preset.name;
        std::string error;
        if (!GraphSerializer::Save(graph, PathUtil::ToUtf8(path), &error)) {
            std::cerr << "Could not save " << path << ": " << error << "\n";
            return 1;
        }
        std::cout << "Wrote " << path << " (" << graph.Nodes().size() << " nodes)\n";
    }
    return 0;
}
