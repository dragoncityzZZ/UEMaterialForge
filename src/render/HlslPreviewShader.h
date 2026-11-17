#pragma once

#include "codegen/GlslGenerator.h"

#include <string>
#include <vector>

namespace mf {

// HLSL compute shader generated from the GLSL material shader. The shader
// ray-casts the preview sphere/plane in a fullscreen compute dispatch, evaluates
// the same material expression graph and writes an R8G8B8A8_UNORM storage
// texture. The same source is consumed by DirectX 12 (D3DCompile) and Vulkan
// (glslangValidator -> SPIR-V).
struct HlslPreviewShader {
    bool success = false;
    std::string source;
    std::string diagnostic;
    std::vector<ShaderParameterBinding> parameters;
    std::vector<ShaderTextureBinding> textures;
    int textureCount = 0;  // default texture + material textures.
};

class HlslPreviewShaderGenerator {
public:
    static HlslPreviewShader Generate(const GeneratedShader& material);
};

} // namespace mf
