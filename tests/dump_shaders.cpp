#include "codegen/GlslGenerator.h"
#include "io/GraphSerializer.h"
#include "render/HlslPreviewShader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: shader_dump <graph.json> <output-base>\n";
        return 2;
    }
    mf::MaterialGraph graph;
    std::string error;
    if (!mf::GraphSerializer::Load(argv[1], graph, &error)) {
        std::cerr << "Load failed: " << error << "\n";
        return 1;
    }
    const mf::GeneratedShader glsl = mf::GlslGenerator().Generate(graph, 460);
    if (!glsl.success) {
        std::cerr << "GLSL generation failed: " << glsl.diagnostic << "\n";
        return 1;
    }
    const mf::HlslPreviewShader hlsl = mf::HlslPreviewShaderGenerator::Generate(glsl);
    if (!hlsl.success) {
        std::cerr << "HLSL generation failed: " << hlsl.diagnostic << "\n";
        return 1;
    }
    const std::filesystem::path base(argv[2]);
    std::ofstream((base.string()) + ".glsl", std::ios::binary) << glsl.fragmentSource;
    std::ofstream((base.string()) + ".hlsl", std::ios::binary) << hlsl.source;
    std::cout << glsl.diagnostic << "\n" << hlsl.diagnostic << "\n";
    std::cout << "Wrote " << base.string() << ".glsl/.hlsl\n";
    return 0;
}
