#include "codegen/GlslGenerator.h"
#include "core/DefaultGraph.h"
#include "core/NodeRegistry.h"
#include "io/Ue5Clipboard.h"
#include "render/HlslPreviewShader.h"
#include "render/Mesh.h"
#include "render/OpenGLPreviewRenderer.h"
#include "render/ShaderProgram.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#if defined(_WIN32)
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace mf;

namespace {

bool g_Failed = false;

void Fail(const std::string& message) {
    g_Failed = true;
    std::cerr << "FAIL: " << message << "\n";
}

GLFWwindow* CreateHiddenWindow() {
    glfwSetErrorCallback([](int code, const char* description) {
        std::cerr << "GLFW error " << code << ": " << description << "\n";
    });
    if (!glfwInit()) {
        return nullptr;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(64, 64, "MaterialForgeShaderTest", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return nullptr;
    }
    glfwMakeContextCurrent(window);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return nullptr;
    }
    return window;
}

bool CompileHlslPreview(const std::string& source, std::string* error) {
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> messages;
    const HRESULT result = D3DCompile(
        source.c_str(), source.size(), "MFPreviewCS", nullptr, nullptr, "main", "cs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &bytecode, &messages);
    if (FAILED(result)) {
        if (error) {
            *error = "D3DCompile failed";
            if (messages) {
                *error += ": ";
                *error += static_cast<const char*>(messages->GetBufferPointer());
            }
        }
        return false;
    }
    return true;
#else
    (void)source;
    (void)error;
    return true;
#endif
}

void BuildSingleNodeGraph(const NodeDefinition& definition, MaterialGraph& graph) {
    std::string error;
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(500.0f, 0.0f), &error);
    Node* node = &graph.AddNode(definition, Vec2(0.0f, 0.0f));
    if (!output || !node) {
        Fail("Could not build graph for " + definition.type + ": " + error);
        return;
    }

    if (definition.type == "TextureObject" || definition.type == "TextureObjectParameter") {
        Node* sample = graph.AddNodeByType("TextureSample", Vec2(250.0f, 0.0f), &error);
        if (!sample) {
            Fail("Could not add TextureSample.");
            return;
        }
        graph.Connect(node->FindOutput("Texture")->id, sample->FindInput("Texture")->id, &error);
        graph.Connect(sample->FindOutput("RGB")->id, output->FindInput("Base Color")->id, &error);
    } else if (definition.type == "Sampler") {
        Node* texture = graph.AddNodeByType("TextureObject", Vec2(100.0f, 120.0f), &error);
        Node* sample = graph.AddNodeByType("TextureSample", Vec2(300.0f, 0.0f), &error);
        if (!texture || !sample) {
            Fail("Could not build Sampler graph.");
            return;
        }
        graph.Connect(texture->FindOutput("Texture")->id,
                      sample->FindInput("Texture")->id, &error);
        graph.Connect(node->FindOutput("Sampler")->id,
                      sample->FindInput("Sampler")->id, &error);
        graph.Connect(sample->FindOutput("RGB")->id,
                      output->FindInput("Base Color")->id, &error);
    } else {
        const Pin* source = nullptr;
        for (const Pin& pin : node->outputs) {
            if (IsNumericPin(pin.type)) {
                source = &pin;
                break;
            }
        }
        if (!source) {
            Fail("No numeric output for " + definition.type);
            return;
        }
        graph.Connect(source->id, output->FindInput("Base Color")->id, &error);
    }
}

void CompileAllExpressionShaders() {
    GlslGenerator generator;
    const NodeRegistry& registry = NodeRegistry::Get();
    int compiled = 0;
    for (const NodeDefinition& definition : registry.All()) {
        if (definition.isMaterialOutput || definition.isFunctionOnly ||
            definition.hiddenInLibrary || definition.isComment || definition.isFunctionCall) {
            continue;
        }

        MaterialGraph graph;
        BuildSingleNodeGraph(definition, graph);
        if (g_Failed) return;
        const GeneratedShader shader = generator.Generate(graph);
        if (!shader.success) {
            Fail(definition.type + " GLSL generation failed: " + shader.diagnostic);
            continue;
        }
        const HlslPreviewShader hlsl = HlslPreviewShaderGenerator::Generate(shader);
        if (!hlsl.success) {
            Fail(definition.type + " HLSL generation failed: " + hlsl.diagnostic);
            continue;
        }
        std::string hlslError;
        if (!CompileHlslPreview(hlsl.source, &hlslError)) {
            Fail(definition.type + " HLSL compile failed: " + hlslError);
            continue;
        }

        ShaderProgram program;
        std::string error;
        if (!program.Compile(GlslGenerator::PreviewVertexShaderSource(),
                             shader.fragmentSource.c_str(), &error)) {
            Fail(definition.type + " shader compile failed:\n" + error);
            continue;
        }
        ++compiled;
    }
    std::cout << "Compiled " << compiled << " per-expression preview shaders.\n";
}

void TestRender() {
    MaterialGraph graph;
    std::string error;
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(500.0f, 0.0f), &error);
    Node* constant = graph.AddNodeByType("Constant3Vector", Vec2(0.0f, 0.0f), &error);
    if (!output || !constant) {
        Fail("Could not build solid-color render graph.");
        return;
    }
    constant->properties["value"] = PropertyValue::Color3Value(1.0f, 0.05f, 0.02f);
    graph.Connect(constant->FindOutput("Value")->id, output->FindInput("Base Color")->id, &error);
    graph.Connect(constant->FindOutput("Value")->id, output->FindInput("Emissive Color")->id, &error);

    const GeneratedShader shader = GlslGenerator().Generate(graph);
    if (!shader.success) {
        Fail("Solid color GLSL generation failed: " + shader.diagnostic);
        return;
    }

    OpenGLPreviewRenderer renderer;
    if (!renderer.Initialize(&error)) {
        Fail("PreviewRenderer::Initialize failed: " + error);
        return;
    }
    if (!renderer.SetMaterial(shader, &error)) {
        Fail("PreviewRenderer::SetMaterial failed: " + error);
        return;
    }

    renderer.Render(graph, 0.0f, 256, 256);
    const GLenum renderError = glGetError();
    if (renderError != GL_NO_ERROR) {
        Fail("OpenGL error after preview render: " + std::to_string(renderError));
    }

    const GLuint texture = renderer.ColorTexture();
    if (texture == 0) {
        Fail("Preview framebuffer color texture is missing.");
        return;
    }
    std::vector<unsigned char> pixels(256u * 256u * 4u);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    const std::size_t center = (static_cast<std::size_t>(128) * 256u + 128u) * 4u;
    const unsigned char red = pixels[center + 0];
    const unsigned char green = pixels[center + 1];
    const unsigned char blue = pixels[center + 2];
    std::cout << "Center pixel (solid red material): " << static_cast<int>(red) << ", "
              << static_cast<int>(green) << ", " << static_cast<int>(blue) << "\n";
    if (red < 90 || red <= green + 25 || red <= blue + 25) {
        Fail("The preview mesh did not render the expected red material at the image center.");
    }

    renderer.Shutdown();
}

void TestNonAsciiParameterCompile() {
    MaterialGraph graph;
    std::string error;
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(500.0f, 0.0f), &error);
    Node* scalarA = graph.AddNodeByType("ScalarParameter", Vec2(0.0f, 0.0f), &error);
    Node* scalarB = graph.AddNodeByType("ScalarParameter", Vec2(0.0f, 120.0f), &error);
    Node* vector = graph.AddNodeByType("VectorParameter", Vec2(0.0f, 240.0f), &error);
    if (!output || !scalarA || !scalarB || !vector) {
        Fail("Could not build non-ASCII parameter graph: " + error);
        return;
    }
    scalarA->properties["parameterName"] = PropertyValue::StringValue("强度");
    scalarA->properties["value"] = PropertyValue::Float(0.4f);
    scalarB->properties["parameterName"] = PropertyValue::StringValue("强度");
    scalarB->properties["value"] = PropertyValue::Float(0.6f);
    vector->properties["parameterName"] = PropertyValue::StringValue("水颜色A");
    vector->properties["value"] = PropertyValue::Color4Value(0.1f, 0.2f, 0.3f, 1.0f);
    graph.Connect(vector->FindOutput("Value")->id, output->FindInput("Base Color")->id, &error);
    graph.Connect(scalarA->FindOutput("Value")->id, output->FindInput("Roughness")->id, &error);
    graph.Connect(scalarB->FindOutput("Value")->id, output->FindInput("Metallic")->id, &error);

    const GeneratedShader shader = GlslGenerator().Generate(graph, 460);
    if (!shader.success) {
        Fail("Non-ASCII parameter GLSL generation failed: " + shader.diagnostic);
        return;
    }
    if (shader.fragmentSource.find("uParam__") != std::string::npos) {
        Fail("Generated GLSL contains a reserved double-underscore identifier.");
        return;
    }
    ShaderProgram program;
    if (!program.Compile(GlslGenerator::PreviewVertexShaderSource(),
                         shader.fragmentSource.c_str(), &error)) {
        Fail("Non-ASCII parameter GLSL compile failed:\n" + error);
    }
}

void TestAnimeWaterClipboardSample() {
    namespace fs = std::filesystem;
    const fs::path samplePath =
        fs::current_path() / "dist" / fs::u8path("动漫水材质.txt");
    if (!fs::is_regular_file(samplePath)) {
        std::cout << "Skipping UE anime-water sample: " << samplePath.u8string() << "\n";
        return;
    }
    std::ifstream stream(samplePath, std::ios::binary);
    if (!stream) {
        Fail("Could not open the UE anime-water sample.");
        return;
    }
    const std::string clipboardText((std::istreambuf_iterator<char>(stream)),
                                    std::istreambuf_iterator<char>());

    MaterialGraph graph;
    std::string error;
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(1600.0f, 400.0f), &error);
    if (!output) {
        Fail("Could not create MaterialOutput for the UE sample: " + error);
        return;
    }
    Ue5ClipboardImportResult result;
    if (!ImportUe5MaterialClipboard(clipboardText, graph, Vec2(0.0f, 0.0f), &result)) {
        Fail("UE anime-water clipboard import failed: " + result.error);
        return;
    }
    std::cout << "UE anime-water import: " << result.nodeCount << " nodes, "
              << result.linkCount << " links, " << result.skippedNodeCount
              << " skipped, " << result.droppedMaterialOutputLinks
              << " dropped material-output links.\n";
    for (const std::string& skipped : result.skippedClasses) {
        std::cout << "  skipped class: " << skipped << "\n";
    }
    if (result.skippedNodeCount != 0) {
        Fail("The anime-water sample contains unsupported UE node classes.");
        return;
    }
    if (result.nodeCount < 60) {
        Fail("The anime-water sample imported too few nodes.");
        return;
    }
    if (result.droppedMaterialOutputLinks != 3) {
        Fail("The anime-water sample should report exactly three root links.");
        return;
    }

    // The UE clipboard was taken without the MaterialGraphNode_Root object, so
    // its three root links were dropped. Reconnect the intentional exports:
    // Lerp -> Base Color, the last Add -> Emissive, and the Reroute knot ->
    // World Position Offset.
    std::set<NodeId> nodesWithOutgoingLinks;
    for (const Link& link : graph.Links()) {
        if (const Pin* from = graph.FindPin(link.fromPin)) {
            nodesWithOutgoingLinks.insert(from->nodeId);
        }
    }
    bool baseColorConnected = false;
    bool emissiveConnected = false;
    bool wpoConnected = false;
    for (const auto& nodePtr : graph.Nodes()) {
        Node& node = *nodePtr;
        if (node.IsMaterialOutput() || node.type == "Comment") continue;
        if (nodesWithOutgoingLinks.count(node.id) != 0 || node.outputs.empty()) continue;
        Pin* source = nullptr;
        for (Pin& pin : node.outputs) {
            if (IsNumericPin(pin.type)) {
                source = &pin;
                break;
            }
        }
        if (!source) source = &node.outputs.front();
        if (node.type == "Lerp" && !baseColorConnected) {
            if (Pin* target = output->FindInput("Base Color")) {
                graph.Connect(source->id, target->id, &error);
                baseColorConnected = true;
            }
        } else if (node.type == "Add" && !emissiveConnected) {
            if (Pin* target = output->FindInput("Emissive Color")) {
                graph.Connect(source->id, target->id, &error);
                emissiveConnected = true;
            }
        } else if (node.type == "Reroute" && !wpoConnected) {
            if (Pin* target = output->FindInput("World Position Offset")) {
                graph.Connect(source->id, target->id, &error);
                wpoConnected = true;
            }
        }
    }
    if (!baseColorConnected || !emissiveConnected || !wpoConnected) {
        Fail("Could not reconnect the anime-water terminal nodes to MaterialOutput.");
        return;
    }

    const GeneratedShader shader = GlslGenerator().Generate(graph, 460);
    if (!shader.success) {
        Fail("Anime-water GLSL generation failed: " + shader.diagnostic);
        return;
    }
    for (const ShaderParameterBinding& parameter : shader.parameters) {
        if (parameter.uniformName.find("__") != std::string::npos) {
            Fail("Anime-water shader contains a reserved '__' identifier: " +
                 parameter.uniformName);
            return;
        }
    }
    ShaderProgram program;
    if (!program.Compile(GlslGenerator::PreviewVertexShaderSource(),
                         shader.fragmentSource.c_str(), &error)) {
        Fail("Anime-water GLSL compile failed:\n" + error);
        return;
    }
    const HlslPreviewShader hlsl = HlslPreviewShaderGenerator::Generate(shader);
    if (!hlsl.success) {
        Fail("Anime-water HLSL generation failed: " + hlsl.diagnostic);
        return;
    }
    std::string hlslError;
    if (!CompileHlslPreview(hlsl.source, &hlslError)) {
        Fail("Anime-water HLSL compile failed: " + hlslError);
        return;
    }
    std::cout << "Anime-water sample compiled successfully: " << graph.Nodes().size()
              << " graph nodes, " << shader.parameters.size() << " parameters, "
              << shader.warnings.size() << " warnings.\n";
}

void TestHlslGeneration() {
    MaterialGraph graph;
    std::string error;
    if (!BuildDefaultGraph(graph, &error)) {
        Fail("BuildDefaultGraph failed for HLSL generation: " + error);
        return;
    }
    const GeneratedShader glsl = GlslGenerator().Generate(graph, 460);
    if (!glsl.success) {
        Fail("Default graph GLSL 460 generation failed: " + glsl.diagnostic);
        return;
    }
    if (glsl.fragmentSource.rfind("#version 460", 0) != 0) {
        Fail("GLSL generator did not emit the requested #version 460 header.");
    }
    const HlslPreviewShader hlsl = HlslPreviewShaderGenerator::Generate(glsl);
    if (!hlsl.success) {
        Fail("HLSL preview generation failed: " + hlsl.diagnostic);
        return;
    }
    if (hlsl.source.find("RWTexture2D<float4> mfOutput") == std::string::npos ||
        hlsl.source.find("[numthreads(8, 8, 1)]") == std::string::npos ||
        hlsl.source.find("SampleLevel") == std::string::npos) {
        Fail("The generated HLSL preview shader is missing expected GPU bindings or entry points.");
    }
    std::cout << "Generated HLSL preview shader (" << hlsl.source.size() << " bytes).\n";
}

void TestDefaultGraphRender() {
    MaterialGraph graph;
    std::string error;
    if (!BuildDefaultGraph(graph, &error)) {
        Fail("BuildDefaultGraph failed: " + error);
        return;
    }
    const GeneratedShader shader = GlslGenerator().Generate(graph);
    if (!shader.success) {
        Fail("Default graph GLSL generation failed: " + shader.diagnostic);
        return;
    }
    OpenGLPreviewRenderer renderer;
    if (!renderer.Initialize(&error)) {
        Fail("PreviewRenderer init failed for default graph: " + error);
        return;
    }
    if (!renderer.SetMaterial(shader, &error)) {
        Fail("Default graph shader compile failed: " + error);
        return;
    }
    renderer.Render(graph, 0.5f, 256, 256);
    if (glGetError() != GL_NO_ERROR) {
        Fail("OpenGL error after rendering the default material.");
    }
    renderer.Shutdown();
}

void TestFunctionCallShaders() {
    namespace fs = std::filesystem;
    const fs::path functionPath =
        fs::current_path() / "assets" / "functions" / "MF_CheapContrast.mffunction";
    if (!fs::is_regular_file(functionPath)) {
        Fail("Built-in function preset is missing.");
        return;
    }

    MaterialGraph graph;
    std::string error;
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(500.0f, 0.0f), &error);
    Node* call = graph.AddNodeByType("MaterialFunctionCall", Vec2(0.0f, 0.0f), &error);
    if (!output || !call) {
        Fail("Could not build function call graph: " + error);
        return;
    }
    call->properties["functionPath"] = PropertyValue::StringValue(functionPath.string());
    call->properties["functionName"] = PropertyValue::StringValue("MF_CheapContrast");
    auto addPin = [&](const char* name, PinDirection direction) {
        Pin pin;
        pin.id = graph.NextPinId();
        pin.nodeId = call->id;
        pin.name = name;
        pin.direction = direction;
        pin.type = PinType::Float;
        pin.defaultValue = PropertyValue::Float(0.0f);
        if (direction == PinDirection::Input) {
            call->inputs.push_back(std::move(pin));
        } else {
            call->outputs.push_back(std::move(pin));
        }
    };
    addPin("Value", PinDirection::Input);
    addPin("Contrast", PinDirection::Input);
    addPin("Result", PinDirection::Output);
    call->FindInput("Value")->defaultValue = PropertyValue::Float(0.5f);
    call->FindInput("Contrast")->defaultValue = PropertyValue::Float(2.0f);
    if (!graph.Connect(call->FindOutput("Result")->id,
                       output->FindInput("Base Color")->id, &error)) {
        Fail("Could not connect function call output: " + error);
        return;
    }

    const GeneratedShader shader = GlslGenerator().Generate(graph, 460, fs::current_path());
    if (!shader.success) {
        Fail("Function call GLSL generation failed: " + shader.diagnostic);
        return;
    }
    if (shader.fragmentSource.find("Clamp_") == std::string::npos) {
        Fail("Function call shader does not contain the inlined graph.");
        return;
    }
    ShaderProgram program;
    if (!program.Compile(GlslGenerator::PreviewVertexShaderSource(),
                         shader.fragmentSource.c_str(), &error)) {
        Fail("Function call GLSL compile failed:\n" + error);
        return;
    }

    const HlslPreviewShader hlsl = HlslPreviewShaderGenerator::Generate(shader);
    if (!hlsl.success) {
        Fail("Function call HLSL generation failed: " + hlsl.diagnostic);
        return;
    }
    std::string hlslError;
    if (!CompileHlslPreview(hlsl.source, &hlslError)) {
        Fail("Function call HLSL compile failed: " + hlslError);
    }
}

void TestBufferVisualizationShaders() {
    MaterialGraph graph;
    std::string error;
    if (!BuildDefaultGraph(graph, &error)) {
        Fail("Could not build the default graph for buffer visualization: " + error);
        return;
    }
    for (int i = 1; i <= static_cast<int>(PreviewBuffer::Depth); ++i) {
        const PreviewBuffer buffer = static_cast<PreviewBuffer>(i);
        const GeneratedShader shader = GlslGenerator().Generate(
            graph, 460, std::filesystem::path{}, buffer);
        if (!shader.success) {
            Fail(std::string(PreviewBufferName(buffer)) +
                 " buffer GLSL generation failed: " + shader.diagnostic);
            continue;
        }
        if (shader.fragmentSource.find("Buffer visualization") == std::string::npos) {
            Fail(std::string(PreviewBufferName(buffer)) +
                 " buffer shader has no visualization branch.");
            continue;
        }
        ShaderProgram program;
        if (!program.Compile(GlslGenerator::PreviewVertexShaderSource(),
                             shader.fragmentSource.c_str(), &error)) {
            Fail(std::string(PreviewBufferName(buffer)) + " GLSL compile failed:\n" + error);
            continue;
        }
        const HlslPreviewShader hlsl = HlslPreviewShaderGenerator::Generate(shader);
        if (!hlsl.success) {
            Fail(std::string(PreviewBufferName(buffer)) + " HLSL generation failed: " +
                 hlsl.diagnostic);
            continue;
        }
        std::string hlslError;
        if (!CompileHlslPreview(hlsl.source, &hlslError)) {
            Fail(std::string(PreviewBufferName(buffer)) + " HLSL compile failed: " +
                 hlslError);
        }
    }
}

} // namespace

void TestObjMeshLoader() {
    namespace fs = std::filesystem;
    const fs::path directory = fs::current_path() / "build" / "test-scratch";
    fs::create_directories(directory);
    const fs::path path = directory / "preview_triangle.obj";
    {
        std::ofstream stream(path);
        stream << "# MaterialForge preview mesh test\n"
               << "v -1 0 -1\n"
               << "v 1 0 -1\n"
               << "v 0 2 0\n"
               << "vt 0 0\n"
               << "vt 1 0\n"
               << "vt 0.5 1\n"
               << "vn 0 0 1\n"
               << "f 1/1/1 2/2/1 3/3/1\n";
    }
    std::string error;
    const Mesh mesh = Mesh::CreateFromObj(path.u8string(), 1.05f, &error);
    if (!mesh.Valid()) {
        Fail("OBJ mesh loader failed: " + error);
    }
    fs::remove(path);
}

int main() {
    GLFWwindow* window = CreateHiddenWindow();
    if (!window) {
        std::cerr << "Could not create an OpenGL 3.3 test context.\n";
        return 1;
    }

    std::cout << "Shader test context: " << reinterpret_cast<const char*>(glGetString(GL_VERSION))
              << " / " << reinterpret_cast<const char*>(glGetString(GL_RENDERER)) << "\n";

    CompileAllExpressionShaders();
    TestFunctionCallShaders();
    TestBufferVisualizationShaders();
    TestNonAsciiParameterCompile();
    TestAnimeWaterClipboardSample();
    TestHlslGeneration();
    TestDefaultGraphRender();
    TestRender();
    TestObjMeshLoader();

    glfwDestroyWindow(window);
    glfwTerminate();

    if (g_Failed) {
        std::cerr << "Shader smoke test failed.\n";
        return 1;
    }
    std::cout << "Shader smoke test passed.\n";
    return 0;
}




