#pragma once

#include "core/Graph.h"
#include "core/PreviewBuffer.h"
#include "core/PreviewLighting.h"
#include "core/TextureSettings.h"

#include <filesystem>
#include <string>
#include <vector>

namespace mf {

struct ShaderParameterBinding {
    std::string uniformName;
    NodeId nodeId = 0;
    std::string parameterName;
    PinType type = PinType::Float;
    PropertyValue defaultValue{};
};

struct ShaderTextureBinding {
    std::string uniformName;
    NodeId nodeId = 0;
    std::string assetPath;
    SamplerSettings sampler{};
};

struct GeneratedShader {
    bool success = false;
    std::string fragmentSource;
    std::string diagnostic;
    std::string blendMode = "Opaque";
    std::string shadingModel = "DefaultLit";
    std::vector<ShaderParameterBinding> parameters;
    std::vector<ShaderTextureBinding> textures;
    std::vector<std::string> warnings;
    int evaluatedNodeCount = 0;
};

// Translates the material DAG into a self-contained GLSL 3.30 fragment shader.
// Starting at MaterialOutput, upstream nodes are compiled in dependency order
// and each intermediate value receives a typed local variable.
class GlslGenerator {
public:
    GeneratedShader Generate(const MaterialGraph& graph, int glslVersion = 330,
                             const std::filesystem::path& baseDirectory = {},
                             PreviewBuffer previewBuffer = PreviewBuffer::Lit,
                             const PreviewLightingSettings& lighting = {}) const;

    static const char* PreviewVertexShaderSource();
    static const char* PreviewShaderInterfaceSummary();
};

} // namespace mf
