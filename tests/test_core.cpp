#include "codegen/GlslGenerator.h"
#include "core/DefaultGraph.h"
#include "core/MaterialInstance.h"
#include "core/MaterialParameters.h"
#include "core/NodeRegistry.h"
#include "core/TypeResolver.h"
#include "io/GraphSerializer.h"
#include "project/Project.h"
#include "project/UeSync.h"
#include "render/RenderApi.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace mf;

namespace {

int g_Failures = 0;

void Check(bool condition, const std::string& message, const char* file, int line) {
    if (!condition) {
        ++g_Failures;
        std::cerr << "FAIL " << file << ":" << line << " - " << message << "\n";
    }
}

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)
#define CHECK_MSG(condition, message) Check((condition), (message), __FILE__, __LINE__)

const Pin* FirstNumericOutput(const Node& node) {
    for (const Pin& pin : node.outputs) {
        if (IsNumericPin(pin.type)) return &pin;
    }
    return nullptr;
}

namespace {
std::filesystem::path TestScratchDirectory() {
    const std::filesystem::path directory =
        std::filesystem::current_path() / "build" / "test-scratch";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    return directory;
}
} // namespace

void TestRegistry() {
    const NodeRegistry& registry = NodeRegistry::Get();
    CHECK(registry.Find("MaterialOutput") != nullptr);
    CHECK(registry.Find("Constant3Vector") != nullptr);
    CHECK(registry.Find("TextureSample") != nullptr);
    CHECK(registry.Find("Panner") != nullptr);
    CHECK(registry.Find("Sine") != nullptr);
    CHECK(registry.Find("Add") != nullptr);
    CHECK(registry.Find("ScalarParameter") != nullptr);
    CHECK(registry.Find("VectorParameter") != nullptr);
    CHECK(registry.Find("NoSuchNode") == nullptr);

    CHECK(registry.Find("TextureSample")->ueClass == "MaterialExpressionTextureSample");
    CHECK(registry.Find("Panner")->ueClass == "MaterialExpressionPanner");

    bool ok = false;
    CHECK(PinTypeFromString("Vector3", &ok) == PinType::Vector3 && ok);
    CHECK(PinTypeFromString("Texture", &ok) == PinType::Texture && ok);
    CHECK(PinTypeFromString("Sampler", &ok) == PinType::Sampler && ok);
    CHECK(PinTypeFromString("Bogus", &ok) == PinType::Float && !ok);

    CHECK(ArePinTypesLinkCompatible(PinType::Float, PinType::Vector4));
    CHECK(ArePinTypesLinkCompatible(PinType::Vector2, PinType::Float));
    CHECK(!ArePinTypesLinkCompatible(PinType::Texture, PinType::Vector4));
    CHECK(ArePinTypesLinkCompatible(PinType::Texture, PinType::Texture));
}

void TestDefaultGraphAndCodegen() {
    MaterialGraph graph;
    std::string error;
    CHECK_MSG(BuildDefaultGraph(graph, &error), "BuildDefaultGraph failed: " + error);

    const Node* outputNode = graph.FindMaterialOutput();
    CHECK(outputNode != nullptr);
    CHECK(outputNode && outputNode->FindInput("Base Color") != nullptr);
    CHECK(outputNode && outputNode->FindInput("Roughness") != nullptr);
    CHECK(graph.Nodes().size() == 14u);
    CHECK(graph.Links().size() == 14u);

    const Node* addNode = nullptr;
    for (const auto& node : graph.Nodes()) {
        if (node->type == "Add") addNode = node.get();
    }
    CHECK(addNode != nullptr);
    if (addNode) {
        const Pin* result = addNode->FindOutput("Result");
        CHECK(result != nullptr);
        TypeResolver resolver(graph);
        CHECK(resolver.GetEffectiveType(*result) == PinType::Vector3);
    }

    GlslGenerator generator;
    const GeneratedShader shader = generator.Generate(graph);
    CHECK_MSG(shader.success, shader.diagnostic);
    CHECK(!shader.fragmentSource.empty());
    CHECK(shader.fragmentSource.find("#version 330 core") != std::string::npos);
    CHECK(shader.fragmentSource.find("mf_Add_") != std::string::npos);
    CHECK(shader.fragmentSource.find("uParam_Roughness") != std::string::npos);
    CHECK(shader.fragmentSource.find("uParam_Metallic") != std::string::npos);
    CHECK(shader.fragmentSource.find("uTexture_") != std::string::npos);
    CHECK(shader.fragmentSource.find("texture(") != std::string::npos);
    CHECK(shader.fragmentSource.find("FragColor") != std::string::npos);
    CHECK(shader.evaluatedNodeCount > 10);
    CHECK(shader.parameters.size() == 3u);
    CHECK(shader.textures.size() == 1u);
}

void TestSerializationRoundTrip() {
    MaterialGraph graph;
    std::string error;
    CHECK_MSG(BuildDefaultGraph(graph, &error), error);

    const nlohmann::json document = GraphSerializer::ToJson(graph);
    CHECK(document["schema"] == GraphSerializer::SchemaId());
    CHECK(document["version"] == 1);
    CHECK(document["nodes"].is_array());
    CHECK(document["links"].is_array());
    CHECK(document["nodes"].size() == graph.Nodes().size());
    CHECK(document["links"].size() == graph.Links().size());
    CHECK(document["material"]["name"] == "M_HoloCircuit");

    MaterialGraph loaded;
    CHECK_MSG(GraphSerializer::FromJson(document, loaded, &error), error);
    CHECK(loaded.Nodes().size() == graph.Nodes().size());
    CHECK(loaded.Links().size() == graph.Links().size());
    CHECK(loaded.Settings().name == graph.Settings().name);
    CHECK(loaded.FindMaterialOutput() != nullptr);
    CHECK(!loaded.IsDirty());

    GlslGenerator generator;
    const GeneratedShader shader = generator.Generate(loaded);
    CHECK_MSG(shader.success, shader.diagnostic);
    CHECK(shader.parameters.size() == 3u);

    // Corrupt documents must fail without modifying the target graph.
    nlohmann::json broken = document;
    broken["schema"] = "something.else";
    const std::size_t before = loaded.Nodes().size();
    CHECK(!GraphSerializer::FromJson(broken, loaded, &error));
    CHECK(loaded.Nodes().size() == before);
    CHECK(!error.empty());

    broken = document;
    broken["links"][0]["toPin"] = 999999999;
    CHECK(!GraphSerializer::FromJson(broken, loaded, &error));

    broken = document;
    broken["nodes"][0]["type"] = "NotARealNode";
    CHECK(!GraphSerializer::FromJson(broken, loaded, &error));
}

void TestConnectionRulesAndCycles() {
    MaterialGraph graph;
    std::string error;
    Node* add = graph.AddNodeByType("Add", Vec2(0, 0), &error);
    Node* multiply = graph.AddNodeByType("Multiply", Vec2(200, 0), &error);
    Node* texture = graph.AddNodeByType("TextureObject", Vec2(0, 200), &error);
    Node* constantA = graph.AddNodeByType("Constant", Vec2(0, 300), &error);
    Node* constantB = graph.AddNodeByType("Constant", Vec2(0, 400), &error);
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(400, 0), &error);
    CHECK(add && multiply && texture && constantA && constantB && output);

    const Pin* addOut = add->FindOutput("Result");
    const Pin* mulA = multiply->FindInput("A");
    const Pin* mulOut = multiply->FindOutput("Result");
    const Pin* addA = add->FindInput("A");
    const Pin* texOut = texture->FindOutput("Texture");
    const Pin* mulB = multiply->FindInput("B");

    CHECK(graph.Connect(addOut->id, mulA->id, &error) != nullptr);
    CHECK(!graph.CanConnect(mulOut->id, addA->id, &error));
    CHECK(error.find("cycle") != std::string::npos);
    CHECK(!graph.CanConnect(texOut->id, mulB->id, &error));
    CHECK(error.find("Incompatible") != std::string::npos);

    // Replacing an input link is allowed and removes the previous one.
    const Pin* addB = add->FindInput("B");
    CHECK(graph.Connect(constantA->FindOutput("Value")->id, addB->id, &error) != nullptr);
    CHECK(graph.Connect(constantB->FindOutput("Value")->id, addB->id, &error) != nullptr);
    CHECK(graph.FindInputLink(addB->id) != nullptr);
    const Pin* mulA2 = multiply->FindInput("A");
    CHECK(graph.FindInputLink(mulA2->id) != nullptr);
    CHECK(graph.Links().size() == 2u);
}

void TestAllNodeTypesCompile() {
    const NodeRegistry& registry = NodeRegistry::Get();
    GlslGenerator generator;
    for (const NodeDefinition& definition : registry.All()) {
        if (definition.isMaterialOutput || definition.isFunctionOnly ||
            definition.hiddenInLibrary || definition.isComment ||
            definition.outputs.empty()) {
            continue;
        }
        MaterialGraph graph;
        std::string error;
        Node* node = &graph.AddNode(definition, Vec2(0, 0));
        Node* output = graph.AddNodeByType("MaterialOutput", Vec2(400, 0), &error);
        CHECK(output != nullptr);
        if (!node || !output) continue;

        if (definition.type == "TextureObject" || definition.type == "TextureObjectParameter") {
            Node* sample = graph.AddNodeByType("TextureSample", Vec2(200, 0), &error);
            CHECK(sample != nullptr);
            const Pin* textureOut = node->FindOutput("Texture");
            const Pin* textureIn = sample->FindInput("Texture");
            const Pin* rgbs = sample->FindOutput("RGB");
            const Pin* base = output->FindInput("Base Color");
            CHECK(graph.Connect(textureOut->id, textureIn->id, &error) != nullptr);
            CHECK(graph.Connect(rgbs->id, base->id, &error) != nullptr);
        } else if (definition.type == "Sampler") {
            Node* texture = graph.AddNodeByType("TextureObject", Vec2(100, 120), &error);
            Node* sample = graph.AddNodeByType("TextureSample", Vec2(250, 0), &error);
            CHECK(texture != nullptr);
            CHECK(sample != nullptr);
            if (texture && sample) {
                CHECK(graph.Connect(texture->FindOutput("Texture")->id,
                                    sample->FindInput("Texture")->id, &error) != nullptr);
                CHECK(graph.Connect(node->FindOutput("Sampler")->id,
                                    sample->FindInput("Sampler")->id, &error) != nullptr);
                CHECK(graph.Connect(sample->FindOutput("RGB")->id,
                                    output->FindInput("Base Color")->id, &error) != nullptr);
            }
        } else {
            const Pin* firstOutput = nullptr;
            for (const Pin& pin : node->outputs) {
                if (IsNumericPin(pin.type)) {
                    firstOutput = &pin;
                    break;
                }
            }
            CHECK_MSG(firstOutput != nullptr, "No numeric output for " + definition.type);
            if (firstOutput) {
                const Pin* base = output->FindInput("Base Color");
                CHECK_MSG(graph.Connect(firstOutput->id, base->id, &error),
                          definition.type + " -> Base Color: " + error);
            }
        }

        const GeneratedShader shader = generator.Generate(graph);
        CHECK_MSG(shader.success, definition.type + ": " + shader.diagnostic);
        const bool emittedDirectly = shader.fragmentSource.find(definition.type) != std::string::npos;
        const bool emittedAsUniform = definition.type == "ScalarParameter" ||
                                      definition.type == "VectorParameter" ||
                                      definition.type == "TextureObject" ||
                                      definition.type == "TextureObjectParameter";
        CHECK_MSG(emittedDirectly || emittedAsUniform,
                  "Generated shader does not reference node type " + definition.type);
    }
}

void TestMissingOutputFailure() {
    MaterialGraph graph;
    std::string error;
    Node* constant = graph.AddNodeByType("Constant", Vec2(0, 0), &error);
    CHECK(constant != nullptr);
    const GeneratedShader shader = GlslGenerator().Generate(graph);
    CHECK(!shader.success);
    CHECK(shader.diagnostic.find("Material Output") != std::string::npos);
}

void TestMaterialOutputDefaults() {
    MaterialGraph graph;
    std::string error;
    CHECK(graph.AddNodeByType("MaterialOutput", Vec2(0, 0), &error) != nullptr);
    const GeneratedShader shader = GlslGenerator().Generate(graph);
    CHECK_MSG(shader.success, shader.diagnostic);
    CHECK(shader.fragmentSource.find("mf_baseColor") != std::string::npos);
    CHECK(shader.fragmentSource.find("mf_metallic") != std::string::npos);
}

void TestProjectFolderOperations() {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = TestScratchDirectory() /
                          ("materialforge_folder_test_" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    std::string error;
    auto created = MaterialProject::Create(base, "Folder Test", &error);
    CHECK_MSG(created.has_value(), "Folder project create failed: " + error);
    if (!created) return;
    MaterialProject project = std::move(*created);
    CHECK(project.CreateFolder("", "FolderA", &error));
    CHECK(project.CreateFolder("FolderA", "Nested", &error));

    MaterialGraph initialGraph;
    CHECK(initialGraph.AddNodeByType("MaterialOutput", Vec2(0, 0), &error) != nullptr);
    CHECK_MSG(GraphSerializer::Save(initialGraph, project.MainMaterialAbsolutePath().u8string(),
                                    &error),
              "Folder project material save failed: " + error);

    const fs::path external = base / "source.txt";
    {
        std::ofstream stream(external, std::ios::binary);
        stream << "folder operations\n";
    }
    std::string imported;
    CHECK(project.ImportFile(external, "FolderA", &imported, &error));
    CHECK(imported == "FolderA/source.txt");
    CHECK(project.SetFolderColor("FolderA", "#5AA9E6", &error));
    CHECK(project.FolderColor("FolderA") == "#5AA9E6");

    CHECK_MSG(project.RenameEntry("FolderA", "Renamed", &error),
              "Rename folder failed: " + error);
    CHECK(fs::is_directory(project.RootPath() / "Renamed", ec));
    CHECK(!fs::exists(project.RootPath() / "FolderA", ec));
    CHECK(project.FolderColor("Renamed") == "#5AA9E6");
    CHECK(project.Resources().size() == 1u);
    CHECK(project.Resources().front().path == "Renamed/source.txt");

    CHECK_MSG(project.MoveEntry("Renamed/source.txt", "Renamed/Nested", &error),
              "Move file failed: " + error);
    CHECK(fs::is_regular_file(project.RootPath() / "Renamed" / "Nested" / "source.txt", ec));
    CHECK(project.Resources().front().path == "Renamed/Nested/source.txt");

    std::string copied;
    CHECK_MSG(project.CopyProjectFile("Renamed/Nested/source.txt", "", &copied, &error),
              "Copy file failed: " + error);
    CHECK(copied == "source.txt");
    CHECK(fs::is_regular_file(project.RootPath() / "source.txt", ec));
    CHECK(project.Resources().size() == 2u);
    CHECK(!project.MoveEntry("Renamed", "Renamed/Nested", &error));

    CHECK_MSG(project.Save(&error), "Folder project save failed: " + error);
    auto reloaded = MaterialProject::Load(project.ProjectFilePath(), &error);
    CHECK_MSG(reloaded.has_value(), "Folder project reload failed: " + error);
    if (reloaded) {
        CHECK(reloaded->FolderColor("Renamed") == "#5AA9E6");
        CHECK(!reloaded->IsBroken());
        const auto entries = reloaded->ListDirectory("", &error);
        CHECK(std::any_of(entries.begin(), entries.end(), [](const ProjectEntry& entry) {
            return entry.directory && entry.name == "Renamed";
        }));
    }

    std::string projectRelative = project.RelativePathOf(project.ProjectFilePath());
    CHECK(!projectRelative.empty());
    CHECK(!project.DeleteEntry(projectRelative, false, &error));
    CHECK(project.DeleteEntry("source.txt", false, &error));
    CHECK(!fs::exists(project.RootPath() / "source.txt", ec));
    CHECK(!project.DeleteEntry("Renamed", false, &error));
    CHECK(project.DeleteEntry("Renamed", true, &error));
    CHECK(!fs::exists(project.RootPath() / "Renamed", ec));
    CHECK(project.FolderColor("Renamed").empty());
    CHECK(project.Resources().empty());

    fs::remove_all(base, ec);
}

void TestProjectLifecycle() {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = TestScratchDirectory() / ("materialforge_project_test_" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    std::string error;
    auto created = MaterialProject::Create(base, "Test Project", &error);
    CHECK_MSG(created.has_value(), "Project create failed: " + error);
    if (!created) {
        fs::remove_all(base, ec);
        return;
    }
    MaterialProject project = std::move(*created);
    CHECK(project.IsOpen());
    CHECK(project.Name() == "Test Project");
    CHECK(fs::is_regular_file(project.ProjectFilePath(), ec));
    CHECK(fs::is_directory(project.RootPath() / "materials", ec));
    CHECK(fs::is_directory(project.RootPath() / "assets" / "textures", ec));
    CHECK(project.MainMaterialPath() == "materials/M_Test Project.json");

    auto duplicate = MaterialProject::Create(base, "Test Project", &error);
    CHECK(!duplicate.has_value());
    CHECK(!project.SetMainMaterial("../outside.json", &error));
    CHECK(project.ResolveRelative("../outside.json").empty());
    CHECK(!project.CreateFolder("", "../escape", &error));

    // A newly created project is not complete until the initial material is
    // written. The app does this immediately; the test mirrors that behavior.
    MaterialGraph initialGraph;
    CHECK(initialGraph.AddNodeByType("MaterialOutput", Vec2(0, 0), &error) != nullptr);
    CHECK_MSG(GraphSerializer::Save(initialGraph, project.MainMaterialAbsolutePath().u8string(), &error),
              "Initial material save failed: " + error);
    CHECK_MSG(project.RefreshIntegrity(&error), "Fresh project should be healthy: " + error);

    CHECK_MSG(project.CreateFolder("", "references", &error), "CreateFolder failed: " + error);
    CHECK(fs::is_directory(project.RootPath() / "references", ec));
    CHECK(!project.CreateFolder("", "references", &error));

    const fs::path external = base / "external_source.txt";
    {
        std::ofstream stream(external, std::ios::binary);
        stream << "hello from outside the project\n";
    }
    std::string importedRelative;
    CHECK_MSG(project.ImportFile(external, "assets/textures", &importedRelative, &error),
              "ImportFile failed: " + error);
    CHECK(importedRelative == "assets/textures/external_source.txt");
    CHECK(fs::is_regular_file(project.RootPath() / fs::u8path(importedRelative), ec));
    CHECK(project.Resources().size() == 1u);
    CHECK(project.Resources().front().required);
    CHECK(project.Resources().front().kind == "text");

    const fs::path externalModel = base / "PreviewCube.obj";
    {
        std::ofstream stream(externalModel, std::ios::binary);
        stream << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }
    std::string importedModel;
    CHECK_MSG(project.ImportFile(externalModel, "assets/models", &importedModel, &error),
              "Model import failed: " + error);
    CHECK(importedModel == "assets/models/PreviewCube.obj");
    CHECK(project.Resources().size() == 2u);
    CHECK(project.Resources().back().kind == "model");

    CHECK_MSG(project.Save(&error), "Project save failed: " + error);
    auto reloaded = MaterialProject::Load(project.ProjectFilePath(), &error);
    CHECK_MSG(reloaded.has_value(), "Project load failed: " + error);
    if (reloaded) {
        CHECK(reloaded->Name() == "Test Project");
        CHECK(reloaded->Resources().size() == 2u);
        CHECK(reloaded->MainMaterialPath() == "materials/M_Test Project.json");
        CHECK(!reloaded->IsBroken());
        const auto entries = reloaded->ListDirectory("", &error);
        CHECK(!entries.empty());
        CHECK(std::any_of(entries.begin(), entries.end(), [](const ProjectEntry& entry) {
            return entry.directory && entry.name == "materials";
        }));
        CHECK(std::any_of(reloaded->Resources().begin(), reloaded->Resources().end(),
                          [](const ProjectResource& resource) {
                              return resource.path == "assets/models/PreviewCube.obj" &&
                                     resource.kind == "model";
                          }));
    }

    // Missing referenced files mark the project broken without preventing the
    // project file or the remaining files from loading.
    fs::remove(project.RootPath() / fs::u8path(importedRelative), ec);
    auto broken = MaterialProject::Load(project.ProjectFilePath(), &error);
    CHECK_MSG(broken.has_value(), "Broken project should still load: " + error);
    if (broken) {
        CHECK(broken->IsBroken());
        CHECK(!broken->Issues().empty());
        bool hasImportedIssue = false;
        for (const ProjectIntegrityIssue& issue : broken->Issues()) {
            if (issue.path == importedRelative) hasImportedIssue = true;
        }
        CHECK(hasImportedIssue);
        CHECK(!broken->ListDirectory("materials").empty());
        CHECK(!broken->ListDirectory("assets/models").empty());
        CHECK(broken->RefreshIntegrity(&error) == false);
    }

    // Missing main material is also a broken state but the project stays open.
    fs::remove(project.MainMaterialAbsolutePath(), ec);
    auto brokenMain = MaterialProject::Load(project.ProjectFilePath(), &error);
    CHECK_MSG(brokenMain.has_value(), "Project with missing main material should load: " + error);
    if (brokenMain) {
        CHECK(brokenMain->IsBroken());
    }

    RecentProjects recent(base / "recent_projects.json");
    recent.Touch(project.ProjectFilePath(), project.Name());
    recent.Touch(base / "Other.mfproject", "Other Project");
    CHECK(recent.Entries().size() == 2u);
    CHECK(recent.Entries().front().name == "Other Project");
    recent.Touch(project.ProjectFilePath(), project.Name());
    CHECK(recent.Entries().size() == 2u);
    CHECK(recent.Entries().front().name == "Test Project");
    CHECK_MSG(recent.Save(&error), "Recent save failed: " + error);
    RecentProjects loadedRecent(base / "recent_projects.json");
    CHECK_MSG(loadedRecent.Load(&error), "Recent load failed: " + error);
    CHECK(loadedRecent.Entries().size() == 2u);
    loadedRecent.Remove(project.ProjectFilePath());
    CHECK(loadedRecent.Entries().size() == 1u);
    loadedRecent.Clear();
    CHECK(loadedRecent.Entries().empty());

    fs::remove_all(base, ec);
}

} // namespace

void TestFunctionGraphCopyAndSerialization() {
    MaterialGraph graph;
    std::string error;
    Node* input = graph.AddNodeByType("FunctionInput", Vec2(-200, 0), &error);
    Node* output = graph.AddNodeByType("FunctionOutput", Vec2(200, 0), &error);
    CHECK(input != nullptr);
    CHECK(output != nullptr);
    if (input && output) {
        CHECK(graph.Connect(input->FindOutput("Value")->id,
                            output->FindInput("Value")->id, &error) != nullptr);
    }

    MaterialGraph copy = graph;
    CHECK(copy.Nodes().size() == graph.Nodes().size());
    CHECK(copy.Links().size() == graph.Links().size());
    CHECK(copy.FindNode(input->id) != nullptr);
    CHECK(copy.FindNode(input->id) != graph.FindNode(input->id));

    const std::filesystem::path file =
        TestScratchDirectory() / "materialforge_function_test.mffunction";
    CHECK_MSG(GraphSerializer::Save(graph, file.u8string(), &error),
              "function save: " + error);
    MaterialGraph loaded;
    CHECK_MSG(GraphSerializer::Load(file.u8string(), loaded, &error),
              "function load: " + error);
    CHECK(loaded.Nodes().size() == 2u);
    CHECK(loaded.FindMaterialOutput() == nullptr);
    bool hasInput = false;
    bool hasOutput = false;
    for (const auto& node : loaded.Nodes()) {
        hasInput = hasInput || node->type == "FunctionInput";
        hasOutput = hasOutput || node->type == "FunctionOutput";
    }
    CHECK(hasInput);
    CHECK(hasOutput);
    std::filesystem::remove(file);
}

void TestCommentAndReroute() {
    MaterialGraph graph;
    std::string error;
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(500.0f, 0.0f), &error);
    Node* reroute = graph.AddNodeByType("Reroute", Vec2(0.0f, 0.0f), &error);
    Node* constant = graph.AddNodeByType("Constant", Vec2(-220.0f, 0.0f), &error);
    Node* comment = graph.AddNodeByType("Comment", Vec2(-400.0f, -200.0f), &error);
    CHECK(output && reroute && constant && comment);
    if (!output || !reroute || !constant || !comment) return;

    CHECK(graph.Connect(constant->FindOutput("Value")->id,
                        reroute->FindInput("Input")->id, &error) != nullptr);
    CHECK(graph.Connect(reroute->FindOutput("Output")->id,
                        output->FindInput("Base Color")->id, &error) != nullptr);
    comment->size = Vec2(420.0f, 260.0f);
    comment->properties["text"] = PropertyValue::StringValue("Lighting notes");

    const GeneratedShader shader = GlslGenerator().Generate(graph, 460);
    CHECK_MSG(shader.success, "Comment/Reroute: " + shader.diagnostic);
    CHECK(shader.fragmentSource.find("Reroute") != std::string::npos);

    const std::filesystem::path file =
        TestScratchDirectory() / "materialforge_comment_test.json";
    CHECK_MSG(GraphSerializer::Save(graph, file.u8string(), &error),
              "comment save: " + error);
    MaterialGraph loaded;
    CHECK_MSG(GraphSerializer::Load(file.u8string(), loaded, &error),
              "comment load: " + error);
    bool foundComment = false;
    for (const auto& node : loaded.Nodes()) {
        if (node->type != "Comment") continue;
        foundComment = std::abs(node->size.x - 420.0f) < 1e-3f &&
                       std::abs(node->size.y - 260.0f) < 1e-3f;
    }
    CHECK(foundComment);
}

void TestMaterialSettings() {
    MaterialGraph graph;
    std::string error;
    CHECK_MSG(BuildDefaultGraph(graph, &error), "BuildDefaultGraph failed: " + error);
    const Node* output = graph.FindMaterialOutput();
    CHECK(output != nullptr);
    if (!output) return;
    CHECK(output->FindInput("World Position Offset") != nullptr);
    CHECK(output->FindInput("Opacity Mask") != nullptr);
    CHECK(output->FindInput("Clear Coat") != nullptr);
    CHECK(output->FindInput("Subsurface Color") != nullptr);

    MaterialSettings& settings = graph.Settings();
    settings.blendMode = "Masked";
    settings.shadingModel = "Unlit";
    settings.twoSided = true;
    settings.opacityMaskClip = 0.25f;
    settings.ditheredLodTransition = true;
    settings.castDynamicShadowAsMasked = true;
    settings.useEmissiveForStaticLighting = true;
    settings.translucencyLightingMode = "VolumetricDirectional";

    const GeneratedShader shader = GlslGenerator().Generate(graph, 460);
    CHECK_MSG(shader.success, "Masked/Unlit shader: " + shader.diagnostic);
    CHECK(shader.fragmentSource.find("mf_opacityMask >= 0.25") != std::string::npos);
    CHECK(shader.fragmentSource.find("Unlit shading model") != std::string::npos);
    CHECK(shader.fragmentSource.find("mf_worldPos") != std::string::npos);
    CHECK(shader.blendMode == "Masked");
    CHECK(shader.shadingModel == "Unlit");

    const std::filesystem::path file =
        TestScratchDirectory() / "materialforge_settings_test.json";
    CHECK_MSG(GraphSerializer::Save(graph, file.u8string(), &error),
              "settings save: " + error);
    MaterialGraph loaded;
    CHECK_MSG(GraphSerializer::Load(file.u8string(), loaded, &error),
              "settings load: " + error);
    const MaterialSettings& loadedSettings = loaded.Settings();
    CHECK(loadedSettings.blendMode == "Masked");
    CHECK(loadedSettings.shadingModel == "Unlit");
    CHECK(loadedSettings.twoSided);
    CHECK(std::abs(loadedSettings.opacityMaskClip - 0.25f) < 1e-4f);
    CHECK(loadedSettings.ditheredLodTransition);
    CHECK(loadedSettings.castDynamicShadowAsMasked);
    CHECK(loadedSettings.useEmissiveForStaticLighting);

    const std::filesystem::path legacyPath =
        std::filesystem::current_path() / "examples" / "projects" / "hologram_demo" /
        "materials" / "M_HoloCircuit.json";
    if (std::filesystem::is_regular_file(legacyPath)) {
        MaterialGraph legacy;
        CHECK_MSG(GraphSerializer::Load(legacyPath.u8string(), legacy, &error),
                  "legacy material load failed: " + error);
        const Node* legacyOutput = legacy.FindMaterialOutput();
        CHECK(legacyOutput && legacyOutput->FindInput("Opacity Mask") != nullptr);
        CHECK(legacyOutput && legacyOutput->FindInput("World Position Offset") != nullptr);
    }
}

void TestSplitOutputPins() {
    namespace fs = std::filesystem;
    MaterialGraph graph;
    std::string error;
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(500.0f, 0.0f), &error);
    Node* vector = graph.AddNodeByType("Constant3Vector", Vec2(0.0f, 0.0f), &error);
    CHECK(output != nullptr);
    CHECK(vector != nullptr);
    if (!output || !vector) return;

    Pin split;
    split.id = graph.NextPinId();
    split.nodeId = vector->id;
    split.name = "R";
    split.direction = PinDirection::Output;
    split.type = PinType::Float;
    split.defaultValue = PropertyValue::Float(0.0f);
    split.splitFrom = "Value";
    split.splitIndex = 0;
    vector->outputs.push_back(split);
    CHECK(graph.Connect(vector->outputs.back().id,
                        output->FindInput("Base Color")->id, &error) != nullptr);

    const GeneratedShader shader = GlslGenerator().Generate(graph, 460);
    CHECK_MSG(shader.success, "Split output: " + shader.diagnostic);
    CHECK(shader.fragmentSource.find(").x") != std::string::npos);

    const fs::path file = TestScratchDirectory() / "materialforge_split_pin_test.json";
    CHECK_MSG(GraphSerializer::Save(graph, file.u8string(), &error),
              "split save: " + error);
    MaterialGraph loaded;
    CHECK_MSG(GraphSerializer::Load(file.u8string(), loaded, &error),
              "split load: " + error);
    bool foundSplit = false;
    for (const auto& node : loaded.Nodes()) {
        if (node->type != "Constant3Vector") continue;
        for (const Pin& pin : node->outputs) {
            if (pin.name == "R" && pin.splitFrom == "Value" && pin.splitIndex == 0) {
                foundSplit = true;
            }
        }
    }
    CHECK(foundSplit);
}

void TestComponentMaskTypes() {
    MaterialGraph graph;
    std::string error;
    Node* mask = graph.AddNodeByType("ComponentMask", Vec2(0.0f, 0.0f), &error);
    CHECK(mask != nullptr);
    if (!mask) return;

    auto setMask = [&](bool r, bool g, bool b, bool a) {
        mask->properties["r"].boolean = r;
        mask->properties["g"].boolean = g;
        mask->properties["b"].boolean = b;
        mask->properties["a"].boolean = a;
    };
    const Pin* output = mask->FindOutput("Result");
    CHECK(output != nullptr);
    if (!output) return;

    TypeResolver allMask(graph);
    CHECK(allMask.GetOutputType(*output) == PinType::Vector4);
    setMask(true, false, false, false);
    TypeResolver redOnly(graph);
    CHECK(redOnly.GetOutputType(*output) == PinType::Float);
    setMask(true, true, false, false);
    TypeResolver redGreen(graph);
    CHECK(redGreen.GetOutputType(*output) == PinType::Vector2);
    setMask(true, true, true, false);
    TypeResolver rgb(graph);
    CHECK(rgb.GetOutputType(*output) == PinType::Vector3);

    Node* materialOutput = graph.AddNodeByType("MaterialOutput", Vec2(500.0f, 0.0f), &error);
    CHECK(materialOutput != nullptr);
    if (materialOutput) {
        CHECK(graph.Connect(output->id, materialOutput->FindInput("Base Color")->id, &error) !=
              nullptr);
    }
    const GeneratedShader shader = GlslGenerator().Generate(graph, 460);
    CHECK_MSG(shader.success, "ComponentMask: " + shader.diagnostic);
    CHECK(shader.fragmentSource.find(".xyz") != std::string::npos);
}

void TestTextureSamplerSettings() {
    MaterialGraph graph;
    std::string error;
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(600.0f, 0.0f), &error);
    Node* texture = graph.AddNodeByType("TextureObject", Vec2(0.0f, 0.0f), &error);
    Node* sample = graph.AddNodeByType("TextureSample", Vec2(250.0f, 0.0f), &error);
    Node* sampler = graph.AddNodeByType("Sampler", Vec2(0.0f, 180.0f), &error);
    CHECK(output && texture && sample && sampler);
    if (!output || !texture || !sample || !sampler) return;
    texture->properties["texture"] =
        PropertyValue::AssetPathValue("assets/textures/T_HoloGrid.png");
    sampler->properties["filter"] = PropertyValue::StringValue("Anisotropic");
    sampler->properties["addressU"] = PropertyValue::StringValue("Clamp");
    sampler->properties["addressV"] = PropertyValue::StringValue("Mirror");
    sampler->properties["mipBias"] = PropertyValue::Float(0.5f);
    sampler->properties["maxAnisotropy"] = PropertyValue::Float(12.0f);
    CHECK(graph.Connect(texture->FindOutput("Texture")->id,
                        sample->FindInput("Texture")->id, &error) != nullptr);
    CHECK(graph.Connect(sampler->FindOutput("Sampler")->id,
                        sample->FindInput("Sampler")->id, &error) != nullptr);
    CHECK(graph.Connect(sample->FindOutput("RGB")->id,
                        output->FindInput("Base Color")->id, &error) != nullptr);

    const GeneratedShader shader = GlslGenerator().Generate(graph, 460);
    CHECK_MSG(shader.success, "texture sampler shader: " + shader.diagnostic);
    CHECK(shader.textures.size() == 1u);
    if (shader.textures.size() == 1u) {
        CHECK(shader.textures[0].sampler.filter == "Anisotropic");
        CHECK(shader.textures[0].sampler.addressU == "Clamp");
        CHECK(shader.textures[0].sampler.addressV == "Mirror");
        CHECK(std::abs(shader.textures[0].sampler.mipBias - 0.5f) < 1e-4f);
        CHECK(std::abs(shader.textures[0].sampler.maxAnisotropy - 12.0f) < 1e-4f);
    }
}

void TestUeSyncStore() {
    const std::filesystem::path root = TestScratchDirectory() / "ue_sync_project";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    UeSyncStore store;
    CHECK(store.Load(root));
    store.Set("materials/M_Test.json", "/Game/MaterialForge/Materials/M_Test", "pushed");
    CHECK(store.Save());
    UeSyncStore reloaded;
    CHECK(reloaded.Load(root));
    const UeSyncEntry* entry = reloaded.Find("materials/M_Test.json");
    CHECK(entry != nullptr);
    if (entry) {
        CHECK(entry->uePath == "/Game/MaterialForge/Materials/M_Test");
        CHECK(entry->state == "pushed");
        CHECK(!entry->updatedAt.empty());
    }
    CHECK(reloaded.Find("materials/missing.json") == nullptr);

    CHECK(reloaded.RemapPrefix("materials", "library"));
    CHECK(reloaded.Find("materials/M_Test.json") == nullptr);
    const UeSyncEntry* moved = reloaded.Find("library/M_Test.json");
    CHECK(moved != nullptr);
    if (moved) {
        CHECK(moved->uePath == "/Game/MaterialForge/Materials/M_Test");
        CHECK(moved->state == "pushed");
    }
    CHECK(reloaded.Save());
    UeSyncStore afterMove;
    CHECK(afterMove.Load(root));
    CHECK(afterMove.Find("library/M_Test.json") != nullptr);

    CHECK(afterMove.RemovePrefix("library"));
    CHECK(afterMove.Find("library/M_Test.json") == nullptr);
    CHECK(afterMove.Save());
    UeSyncStore afterRemove;
    CHECK(afterRemove.Load(root));
    CHECK(afterRemove.Find("library/M_Test.json") == nullptr);

    std::filesystem::remove_all(root, ec);
}

void TestMaterialInstance() {
    MaterialGraph graph;
    std::string error;
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(600.0f, 0.0f), &error);
    Node* scalar = graph.AddNodeByType("ScalarParameter", Vec2(0.0f, 0.0f), &error);
    Node* vector = graph.AddNodeByType("VectorParameter", Vec2(0.0f, 160.0f), &error);
    CHECK(output && scalar && vector);
    if (!output || !scalar || !vector) return;
    scalar->properties["parameterName"] = PropertyValue::StringValue("RoughnessAmount");
    scalar->properties["value"] = PropertyValue::Float(0.4f);
    scalar->properties["parameterGroup"] = PropertyValue::StringValue("Surface");
    scalar->properties["sortPriority"] = PropertyValue::Float(2.0f);
    scalar->properties["description"] = PropertyValue::StringValue("Surface roughness");
    scalar->properties["sliderMin"] = PropertyValue::Float(0.05f);
    scalar->properties["sliderMax"] = PropertyValue::Float(0.95f);
    vector->properties["parameterName"] = PropertyValue::StringValue("TintColor");
    vector->properties["value"] =
        PropertyValue::Color4Value(0.1f, 0.2f, 0.3f, 1.0f);
    vector->properties["parameterGroup"] = PropertyValue::StringValue("Surface");
    vector->properties["sortPriority"] = PropertyValue::Float(1.0f);
    CHECK(graph.Connect(vector->FindOutput("Value")->id,
                        output->FindInput("Base Color")->id, &error) != nullptr);
    CHECK(graph.Connect(scalar->FindOutput("Value")->id,
                        output->FindInput("Roughness")->id, &error) != nullptr);

    const std::vector<MaterialParameterInfo> parameters =
        CollectMaterialParameters(graph);
    CHECK(parameters.size() == 2u);
    if (parameters.size() == 2u) {
        CHECK(parameters[0].name == "TintColor");
        CHECK(parameters[0].group == "Surface");
        CHECK(parameters[0].sortPriority == 1);
        CHECK(parameters[1].name == "RoughnessAmount");
        CHECK(std::abs(parameters[1].sliderMin - 0.05f) < 1e-4f);
        CHECK(std::abs(parameters[1].sliderMax - 0.95f) < 1e-4f);
    }

    const std::filesystem::path graphPath =
        TestScratchDirectory() / "materialforge_parameters_test.json";
    CHECK_MSG(GraphSerializer::Save(graph, graphPath.u8string(), &error),
              "parameter metadata save failed: " + error);
    MaterialGraph metadataReload;
    CHECK_MSG(GraphSerializer::Load(graphPath.u8string(), metadataReload, &error),
              "parameter metadata load failed: " + error);
    const std::vector<MaterialParameterInfo> reloadedParameters =
        CollectMaterialParameters(metadataReload);
    CHECK(reloadedParameters.size() == 2u);
    if (reloadedParameters.size() == 2u) {
        CHECK(reloadedParameters[1].group == "Surface");
        CHECK(reloadedParameters[1].sortPriority == 2);
        CHECK(reloadedParameters[1].description == "Surface roughness");
        CHECK(std::abs(reloadedParameters[1].sliderMax - 0.95f) < 1e-4f);
    }

    MaterialInstance instance;
    instance.SetName("MI_Test");
    instance.SetParentMaterial("materials/M_Test.json");
    instance.SetOverride("RoughnessAmount", PinType::Float,
                         PropertyValue::Float(0.75f));
    instance.SetOverride("TintColor", PinType::Vector4,
                         PropertyValue::Color4Value(0.9f, 0.3f, 0.2f, 1.0f));
    instance.SetOverride("MissingParameter", PinType::Float,
                         PropertyValue::Float(1.0f));

    const std::filesystem::path path =
        TestScratchDirectory() / "materialforge_instance_test.mfinstance";
    CHECK_MSG(instance.Save(path, &error), "instance save failed: " + error);
    MaterialInstance loaded;
    CHECK_MSG(loaded.Load(path, &error), "instance load failed: " + error);
    CHECK(loaded.Name() == "MI_Test");
    CHECK(loaded.ParentMaterial() == "materials/M_Test.json");
    CHECK(loaded.Overrides().size() == 3u);
    CHECK(loaded.IsOverridden("RoughnessAmount"));
    CHECK(!loaded.IsOverridden("NotThere"));

    std::vector<std::string> warnings;
    CHECK(loaded.ApplyToGraph(graph, &warnings));
    CHECK(loaded.FindOverride("RoughnessAmount") != nullptr);
    CHECK(std::abs(scalar->properties["value"].number - 0.75f) < 1e-4f);
    CHECK(std::abs(vector->properties["value"].vec4.x - 0.9f) < 1e-4f);
    CHECK(!warnings.empty());
    CHECK(loaded.ClearOverride("MissingParameter"));
    CHECK(loaded.Overrides().size() == 2u);
}

void TestTypedMaterialFunction() {
    namespace fs = std::filesystem;
    MaterialGraph function;
    std::string error;
    Node* input = function.AddNodeByType("FunctionInput", Vec2(0.0f, 0.0f), &error);
    Node* output = function.AddNodeByType("FunctionOutput", Vec2(300.0f, 0.0f), &error);
    CHECK(input && output);
    if (!input || !output) return;
    input->properties["inputName"] = PropertyValue::StringValue("Color");
    input->properties["pinType"] = PropertyValue::StringValue("Vector3");
    input->FindOutput("Value")->type = PinType::Vector3;
    output->properties["outputName"] = PropertyValue::StringValue("Color");
    output->properties["pinType"] = PropertyValue::StringValue("Vector3");
    output->FindInput("Value")->type = PinType::Vector3;
    CHECK(function.Connect(input->FindOutput("Value")->id,
                           output->FindInput("Value")->id, &error) != nullptr);
    TypeResolver functionResolver(function);
    CHECK(functionResolver.GetEffectiveType(*input->FindOutput("Value")) ==
          PinType::Vector3);
    CHECK(functionResolver.GetEffectiveType(*output->FindInput("Value")) ==
          PinType::Vector3);

    const fs::path functionPath =
        TestScratchDirectory() / "materialforge_typed_function.mffunction";
    CHECK_MSG(GraphSerializer::Save(function, functionPath.u8string(), &error),
              "typed function save failed: " + error);

    MaterialGraph material;
    Node* materialOutput =
        material.AddNodeByType("MaterialOutput", Vec2(600.0f, 0.0f), &error);
    Node* call = material.AddNodeByType("MaterialFunctionCall", Vec2(100.0f, 0.0f),
                                        &error);
    CHECK(materialOutput && call);
    if (!materialOutput || !call) return;
    call->properties["functionPath"] = PropertyValue::StringValue(functionPath.u8string());
    call->properties["functionName"] = PropertyValue::StringValue("TypedFunction");
    Pin callInput;
    callInput.id = material.NextPinId();
    callInput.nodeId = call->id;
    callInput.name = "Color";
    callInput.direction = PinDirection::Input;
    callInput.type = PinType::Vector3;
    callInput.defaultValue = PropertyValue::Vec3Value(0.2f, 0.5f, 0.8f);
    call->inputs.push_back(callInput);
    Pin callOutput;
    callOutput.id = material.NextPinId();
    callOutput.nodeId = call->id;
    callOutput.name = "Color";
    callOutput.direction = PinDirection::Output;
    callOutput.type = PinType::Vector3;
    callOutput.defaultValue = PropertyValue::Vec3Value(0.0f, 0.0f, 0.0f);
    call->outputs.push_back(callOutput);
    CHECK(material.Connect(call->FindOutput("Color")->id,
                           materialOutput->FindInput("Base Color")->id, &error) != nullptr);

    const GeneratedShader shader =
        GlslGenerator().Generate(material, 460, TestScratchDirectory());
    CHECK_MSG(shader.success, "typed function shader failed: " + shader.diagnostic);
    CHECK(shader.fragmentSource.find("vec3") != std::string::npos);
}

void TestMaterialFunctionCall() {
    namespace fs = std::filesystem;
    const fs::path functionPath =
        fs::current_path() / "assets" / "functions" / "MF_CheapContrast.mffunction";
    if (!fs::is_regular_file(functionPath)) {
        CHECK_MSG(false, "Built-in material function preset is missing: " + functionPath.string());
        return;
    }

    for (const auto& entry : fs::directory_iterator(functionPath.parent_path())) {
        if (entry.path().extension() != ".mffunction") continue;
        MaterialGraph preset;
        std::string presetError;
        if (!GraphSerializer::Load(entry.path().u8string(), preset, &presetError)) {
            CHECK_MSG(false, "Could not load preset " + entry.path().filename().string() +
                                 ": " + presetError);
            return;
        }
        if (preset.Nodes().empty() || preset.FindMaterialOutput() != nullptr) {
            CHECK_MSG(false, "Preset is not a function graph: " +
                                 entry.path().filename().string());
            return;
        }
    }

    MaterialGraph graph;
    std::string error;
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(600.0f, 0.0f), &error);
    Node* call = graph.AddNodeByType("MaterialFunctionCall", Vec2(100.0f, 0.0f), &error);
    if (!output || !call) {
        CHECK_MSG(false, "Could not create function call graph: " + error);
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
        CHECK_MSG(false, "Could not connect function call output: " + error);
        return;
    }

    const GeneratedShader shader = GlslGenerator().Generate(graph, 460, fs::current_path());
    if (!shader.success) {
        CHECK_MSG(false, "Function call GLSL generation failed: " + shader.diagnostic);
        return;
    }
    if (shader.fragmentSource.find("Clamp_") == std::string::npos) {
        CHECK_MSG(false, "Function call shader does not contain the inlined function graph.");
    }
    const std::size_t clampPos = shader.fragmentSource.find("Clamp_");
    if (clampPos == std::string::npos ||
        shader.fragmentSource.find(" = ", clampPos) == std::string::npos) {
        CHECK_MSG(false, "Function call shader does not emit the inlined function body.");
    }

    const fs::path materialPath =
        TestScratchDirectory() / "materialforge_function_call_test.json";
    if (!GraphSerializer::Save(graph, materialPath.u8string(), &error)) {
        CHECK_MSG(false, "Could not save function call graph: " + error);
        return;
    }
    MaterialGraph loaded;
    if (!GraphSerializer::Load(materialPath.u8string(), loaded, &error)) {
        CHECK_MSG(false, "Could not load function call graph: " + error);
        fs::remove(materialPath);
        return;
    }
    const Node* loadedCall = nullptr;
    for (const auto& node : loaded.Nodes()) {
        if (node->type == "MaterialFunctionCall") loadedCall = node.get();
    }
    if (!loadedCall || loadedCall->inputs.size() != 2 || loadedCall->outputs.size() != 1) {
        CHECK_MSG(false, "Function call pins were not restored from JSON.");
    } else if (!loadedCall->FindInput("Value") ||
               std::abs(loadedCall->FindInput("Value")->defaultValue.number - 0.5f) > 1e-4f) {
        CHECK_MSG(false, "Function call input defaults were not restored from JSON.");
    }
    fs::remove(materialPath);
}

void TestNonAsciiParameterIdentifiers() {
    MaterialGraph graph;
    std::string error;
    Node* output = graph.AddNodeByType("MaterialOutput", Vec2(600.0f, 0.0f), &error);
    Node* scalarA = graph.AddNodeByType("ScalarParameter", Vec2(0.0f, 0.0f), &error);
    Node* scalarB = graph.AddNodeByType("ScalarParameter", Vec2(0.0f, 120.0f), &error);
    Node* vector = graph.AddNodeByType("VectorParameter", Vec2(0.0f, 240.0f), &error);
    CHECK(output && scalarA && scalarB && vector);
    if (!output || !scalarA || !scalarB || !vector) return;

    scalarA->properties["parameterName"] = PropertyValue::StringValue("强度");
    scalarA->properties["value"] = PropertyValue::Float(0.4f);
    scalarB->properties["parameterName"] = PropertyValue::StringValue("强度");
    scalarB->properties["value"] = PropertyValue::Float(0.6f);
    vector->properties["parameterName"] = PropertyValue::StringValue("水颜色A");
    vector->properties["value"] = PropertyValue::Color4Value(0.1f, 0.2f, 0.3f, 1.0f);
    CHECK(graph.Connect(vector->FindOutput("Value")->id,
                        output->FindInput("Base Color")->id, &error) != nullptr);
    CHECK(graph.Connect(scalarA->FindOutput("Value")->id,
                        output->FindInput("Roughness")->id, &error) != nullptr);
    CHECK(graph.Connect(scalarB->FindOutput("Value")->id,
                        output->FindInput("Metallic")->id, &error) != nullptr);

    const GeneratedShader shader = GlslGenerator().Generate(graph, 460);
    CHECK_MSG(shader.success, "Chinese parameter names must still generate a shader.");
    CHECK(shader.parameters.size() == 3u);
    for (const ShaderParameterBinding& parameter : shader.parameters) {
        CHECK_MSG(parameter.uniformName.find("__") == std::string::npos,
                  "GLSL identifiers must not contain '__': " + parameter.uniformName);
        CHECK_MSG(!parameter.uniformName.empty() &&
                      !std::isdigit(static_cast<unsigned char>(parameter.uniformName.front())),
                  "GLSL identifiers must not start with a digit: " + parameter.uniformName);
    }
    CHECK(shader.fragmentSource.find("uParam__") == std::string::npos);
    // Two parameters intentionally share a name; the generator reports the
    // UE constraint but still produces distinct uniforms.
    CHECK(!shader.warnings.empty());
}

void TestRenderApiParsing() {
    CHECK_MSG(ParseRenderApi("vulkan") == RenderApi::Vulkan,
              "ParseRenderApi failed for vulkan.");
    CHECK_MSG(ParseRenderApi("dx12") == RenderApi::DirectX12,
              "ParseRenderApi failed for dx12.");
    CHECK_MSG(ParseRenderApi("opengl") == RenderApi::OpenGL,
              "ParseRenderApi failed for opengl.");
    CHECK_MSG(ParseRenderApi("unknown") == RenderApi::OpenGL,
              "ParseRenderApi did not fall back to OpenGL.");
}

void TestSpecialMaterialOutputs() {
    const NodeRegistry& registry = NodeRegistry::Get();
    const NodeDefinition* water = registry.Find("SingleLayerWaterMaterialOutput");
    const NodeDefinition* thin = registry.Find("ThinTranslucentMaterialOutput");
    const NodeDefinition* clearCoat = registry.Find("ClearCoatNormalCustomOutput");
    CHECK_MSG(water != nullptr, "SingleLayerWaterMaterialOutput is missing.");
    CHECK_MSG(thin != nullptr, "ThinTranslucentMaterialOutput is missing.");
    CHECK_MSG(clearCoat != nullptr, "ClearCoatNormalCustomOutput is missing.");
    if (water) {
        CHECK(water->inputs.size() == 4u);
        CHECK(water->ueClass == "MaterialExpressionSingleLayerWaterMaterialOutput");
        CHECK(water->inputs[2].name == "PhaseG");
    }
    if (thin) {
        CHECK(thin->inputs.size() == 2u);
        CHECK(thin->ueClass == "MaterialExpressionThinTranslucentMaterialOutput");
    }
    if (clearCoat) {
        CHECK(clearCoat->inputs.size() == 1u);
        CHECK(clearCoat->inputs[0].name == "Clear Coat Normal");
    }
}

int main() {
    TestRegistry();
    TestDefaultGraphAndCodegen();
    TestSerializationRoundTrip();
    TestConnectionRulesAndCycles();
    TestAllNodeTypesCompile();
    TestSpecialMaterialOutputs();
    TestMissingOutputFailure();
    TestFunctionGraphCopyAndSerialization();
    TestComponentMaskTypes();
    TestSplitOutputPins();
    TestMaterialSettings();
    TestCommentAndReroute();
    TestMaterialFunctionCall();
    TestTypedMaterialFunction();
    TestMaterialInstance();
    TestUeSyncStore();
    TestTextureSamplerSettings();
    TestMaterialOutputDefaults();
    TestNonAsciiParameterIdentifiers();
    TestProjectLifecycle();
    TestProjectFolderOperations();
    TestRenderApiParsing();

    if (g_Failures == 0) {
        std::cout << "All core tests passed.\n";
        return 0;
    }
    std::cerr << g_Failures << " core test(s) failed.\n";
    return 1;
}
