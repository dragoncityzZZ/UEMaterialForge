#include "render/HlslPreviewShader.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace mf {
namespace {

constexpr int kMaxMaterialTextures = 12;

bool IsIdentifierChar(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

std::string Trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string ReplaceWord(const std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) return text;
    std::string result;
    result.reserve(text.size() + 8);
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t found = text.find(from, cursor);
        if (found == std::string::npos) {
            result.append(text, cursor, std::string::npos);
            break;
        }
        const bool leftOk = found == 0 || !IsIdentifierChar(text[found - 1]);
        const std::size_t end = found + from.size();
        const bool rightOk = end >= text.size() || !IsIdentifierChar(text[end]);
        result.append(text, cursor, found - cursor);
        if (leftOk && rightOk) {
            result.append(to);
            cursor = end;
        } else {
            result.append(text, found, from.size());
            cursor = end;
        }
    }
    return result;
}

std::string ReplaceTextureCalls(const std::string& line) {
    std::string result;
    result.reserve(line.size() + 16);
    std::size_t cursor = 0;
    while (cursor < line.size()) {
        const std::size_t found = line.find("texture(", cursor);
        if (found == std::string::npos) {
            result.append(line, cursor, std::string::npos);
            break;
        }
        const bool leftOk = found == 0 || !IsIdentifierChar(line[found - 1]);
        if (!leftOk) {
            result.append(line, cursor, found + 8 - cursor);
            cursor = found + 8;
            continue;
        }
        result.append(line, cursor, found - cursor);
        std::size_t open = found + 7;  // points at '('
        int depth = 0;
        std::size_t close = std::string::npos;
        for (std::size_t i = open; i < line.size(); ++i) {
            if (line[i] == '(') ++depth;
            else if (line[i] == ')') {
                --depth;
                if (depth == 0) {
                    close = i;
                    break;
                }
            }
        }
        if (close == std::string::npos) {
            result.append(line, found, std::string::npos);
            break;
        }
        const std::string arguments = line.substr(open + 1, close - open - 1);
        int nested = 0;
        std::size_t comma = std::string::npos;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (arguments[i] == '(') ++nested;
            else if (arguments[i] == ')') --nested;
            else if (arguments[i] == ',' && nested == 0) {
                comma = i;
                break;
            }
        }
        if (comma == std::string::npos) {
            result.append(line, found, close - found + 1);
        } else {
            const std::string sampler = Trim(arguments.substr(0, comma));
            const std::string uvs = Trim(arguments.substr(comma + 1));
            result += sampler + ".SampleLevel(mfSampler, " + uvs + ", 0.0)";
        }
        cursor = close + 1;
    }
    return result;
}

std::string FixSplatConstructors(const std::string& line) {
    // GLSL allows vec3(scalar); HLSL requires an explicit scalar-to-vector
    // cast. Rewrite single-argument floatN(...) constructors to (floatN)(...).
    std::string result;
    result.reserve(line.size() + 16);
    std::size_t cursor = 0;
    while (cursor < line.size()) {
        std::size_t found = std::string::npos;
        std::string type;
        for (const char* candidate : {"float2(", "float3(", "float4("}) {
            const std::size_t position = line.find(candidate, cursor);
            if (position != std::string::npos &&
                (found == std::string::npos || position < found)) {
                found = position;
                type = std::string(candidate).substr(0, 6);
            }
        }
        if (found == std::string::npos) {
            result.append(line, cursor, std::string::npos);
            break;
        }
        const bool leftOk = found == 0 || !IsIdentifierChar(line[found - 1]);
        if (!leftOk) {
            result.append(line, cursor, found + type.size() + 1 - cursor);
            cursor = found + type.size() + 1;
            continue;
        }
        result.append(line, cursor, found - cursor);
        const std::size_t open = found + type.size();  // '('
        int depth = 0;
        std::size_t close = std::string::npos;
        int topLevelCommas = 0;
        for (std::size_t i = open; i < line.size(); ++i) {
            if (line[i] == '(') ++depth;
            else if (line[i] == ')') {
                --depth;
                if (depth == 0) {
                    close = i;
                    break;
                }
            } else if (line[i] == ',' && depth == 1) {
                ++topLevelCommas;
            }
        }
        if (close == std::string::npos) {
            result.append(line, found, std::string::npos);
            break;
        }
        if (topLevelCommas == 0) {
            const std::string argument = line.substr(open + 1, close - open - 1);
            result += "((" + type + ")(" + argument + "))";
        } else {
            result.append(line, found, close - found + 1);
        }
        cursor = close + 1;
    }
    return result;
}

std::string TranslateLine(const std::string& line,
                          const std::string& blendMode = "Opaque") {
    if (StartsWith(Trim(line), "//")) {
        return line;
    }
    std::string translated = line;
    translated = ReplaceWord(translated, "vec2", "float2");
    translated = ReplaceWord(translated, "vec3", "float3");
    translated = ReplaceWord(translated, "vec4", "float4");
    translated = ReplaceWord(translated, "mat4", "float4x4");
    translated = ReplaceWord(translated, "mix", "lerp");
    translated = ReplaceWord(translated, "fract", "frac");
    translated = ReplaceWord(translated, "mod", "mf_mod");
    translated = ReplaceTextureCalls(translated);
    translated = ReplaceWord(translated, "FragColor", "mfOutput[mfId.xy]");
    translated = FixSplatConstructors(translated);
    const std::string surfaceAssignment =
        "mfOutput[mfId.xy] = float4(mf_color, mf_opacity);";
    if (translated.find(surfaceAssignment) != std::string::npos) {
        std::string composite =
            "float4 mf_surface = float4(mf_color, mf_opacity);\n"
            "    float3 mf_background = mf_Background(mfUv, float2(mfId.xy));\n";
        if (blendMode == "Additive") {
            composite +=
                "    mfOutput[mfId.xy] = float4(mf_background.rgb + "
                "mf_surface.rgb * mf_surface.a, 1.0);";
        } else if (blendMode == "Modulate") {
            composite +=
                "    mfOutput[mfId.xy] = float4(mf_background.rgb * "
                "max(mf_surface.rgb, float3(0.0, 0.0, 0.0)), 1.0);";
        } else {
            composite +=
                "    mfOutput[mfId.xy] = float4(lerp(mf_background.rgb, "
                "mf_surface.rgb, mf_surface.a), 1.0);";
        }
        translated = composite;
    }
    return translated;
}

std::string SwizzleForType(PinType type) {
    switch (type) {
    case PinType::Float: return ".x";
    case PinType::Vector2: return ".xy";
    case PinType::Vector3: return ".xyz";
    case PinType::Vector4:
    default: return "";
    }
}

const char* kSceneHelpers = R"HLSL(
#define MF_PI 3.14159265359

float mf_mod(float x, float y) { return x - y * floor(x / y); }
float2 mf_mod(float2 x, float2 y) { return x - y * floor(x / y); }
float3 mf_mod(float3 x, float3 y) { return x - y * floor(x / y); }
float4 mf_mod(float4 x, float4 y) { return x - y * floor(x / y); }
float2 mf_mod(float2 x, float y) { return x - y * floor(x / y); }
float3 mf_mod(float3 x, float y) { return x - y * floor(x / y); }
float4 mf_mod(float4 x, float y) { return x - y * floor(x / y); }

float3 mf_Background(float2 uv, float2 pixel)
{
    float2 resolution = max(mfResolution.xy, float2(1.0, 1.0));
    float2 aspect = float2(resolution.x / max(resolution.y, 1.0), 1.0);
    float2 centered = (uv - float2(0.5, 0.53)) * aspect;

    float3 bottom = float3(0.004, 0.006, 0.010);
    float3 top = float3(0.028, 0.048, 0.065);
    float3 color = lerp(bottom, top, smoothstep(0.0, 1.0, uv.y));

    float halo = exp(-dot(centered, centered) * 3.0);
    color += float3(0.020, 0.075, 0.082) * halo;
    color += float3(0.015, 0.022, 0.027) *
             exp(-dot(centered * float2(0.6, 2.2), centered * float2(0.6, 2.2)) * 5.0);

    float diagonal = 1.0 - smoothstep(0.0, 0.55, abs((uv.x * 0.7 + uv.y * 0.3) - 0.56));
    color += float3(0.010, 0.016, 0.019) * diagonal;

    float vignette = smoothstep(1.35, 0.25, length(centered));
    color *= lerp(0.62, 1.0, vignette);

    float dither = frac(sin(dot(pixel, float2(12.9898, 78.233))) * 43758.5453);
    color += (dither - 0.5) / 255.0;
    return color;
}
)HLSL";

} // namespace

HlslPreviewShader HlslPreviewShaderGenerator::Generate(const GeneratedShader& material) {
    HlslPreviewShader result;
    if (!material.success || material.fragmentSource.empty()) {
        result.diagnostic = "Cannot generate HLSL: the material shader is not valid.";
        return result;
    }
    if (material.textures.size() > static_cast<std::size_t>(kMaxMaterialTextures)) {
        result.diagnostic =
            "The material uses more than " + std::to_string(kMaxMaterialTextures) +
            " texture samplers; the GPU preview backend supports at most " +
            std::to_string(kMaxMaterialTextures) + ".";
        return result;
    }

    std::istringstream input(material.fragmentSource);
    std::vector<std::string> helperLines;
    std::vector<std::string> bodyLines;
    bool inMain = false;
    bool bodyStarted = false;
    bool bodyClosed = false;
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (StartsWith(trimmed, "#version") || StartsWith(trimmed, "uniform ") ||
            StartsWith(trimmed, "in ") || StartsWith(trimmed, "out ")) {
            continue;
        }
        if (!inMain) {
            if (trimmed == "void main()" || trimmed == "void main() {") {
                inMain = true;
                if (trimmed.back() == '{') bodyStarted = true;
                continue;
            }
            helperLines.push_back(line);
            continue;
        }
        if (!bodyStarted) {
            if (trimmed == "{") {
                bodyStarted = true;
            }
            continue;
        }
        if (trimmed == "}") {
            bodyClosed = true;
            break;
        }
        bodyLines.push_back(line);
    }
    if (!inMain || !bodyStarted || !bodyClosed) {
        result.diagnostic = "Could not locate the material fragment main() in generated GLSL.";
        return result;
    }

    std::ostringstream source;
    source << "// MaterialForge GPU preview compute shader (generated from GLSL).\n";
    source << "// Used by the DirectX 12 and Vulkan preview pipelines.\n\n";
    source << "cbuffer MFScene : register(b0)\n{\n";
    source << "    float4 mfCameraPos;\n";
    source << "    float4 mfCameraRight;\n";
    source << "    float4 mfCameraUp;\n";
    source << "    float4 mfCameraForward;\n";
    source << "    float4 mfResolution; // xy size, z tanHalfFov, w aspect\n";
    source << "    float mfTime;\n";
    source << "    float mfMeshType;\n";
    source << "    float mfTwoSided;\n";
    source << "    float mfPadding;\n";
    for (std::size_t i = 0; i < material.parameters.size(); ++i) {
        source << "    float4 mfParam_" << i << ";\n";
    }
    source << "};\n\n";

    source << "#define uCameraPos (mfCameraPos.xyz)\n";
    source << "#define uTime (mfTime)\n";
    source << "#define uViewSize (mfResolution.xy)\n";
    for (std::size_t i = 0; i < material.parameters.size(); ++i) {
        const ShaderParameterBinding& parameter = material.parameters[i];
        source << "#define " << parameter.uniformName << " (mfParam_" << i
               << SwizzleForType(parameter.type) << ")\n";
    }
    source << "\nTexture2D<float4> uDefaultTexture : register(t1);\n";
    for (std::size_t i = 0; i < material.textures.size(); ++i) {
        source << "Texture2D<float4> " << material.textures[i].uniformName << " : register(t"
               << (2 + i) << ");\n";
    }
    source << "SamplerState mfSampler : register(s17);\n";
    source << "RWTexture2D<float4> mfOutput : register(u15);\n\n";
    source << kSceneHelpers << "\n";

    for (const std::string& helper : helperLines) {
        source << TranslateLine(helper) << "\n";
    }

    source << R"HLSL(
[numthreads(8, 8, 1)]
void main(uint3 mfId : SV_DispatchThreadID)
{
    uint2 mfSize = (uint2)max(mfResolution.xy, float2(1.0, 1.0));
    if (mfId.x >= mfSize.x || mfId.y >= mfSize.y)
    {
        return;
    }

    float2 mfUv = (float2(mfId.xy) + 0.5) / max(mfResolution.xy, float2(1.0, 1.0));
    float2 mfNdc = float2(mfUv.x * 2.0 - 1.0, mfUv.y * 2.0 - 1.0);
    float3 mfRayOrigin = mfCameraPos.xyz;
    float3 mfRayDirection = normalize(
        mfCameraForward.xyz +
        mfCameraRight.xyz * (mfNdc.x * mfResolution.w * mfResolution.z) +
        mfCameraUp.xyz * (mfNdc.y * mfResolution.z));

    float3 mfHitPos = float3(0.0, 0.0, 0.0);
    float3 mfHitNormal = float3(0.0, 0.0, 1.0);
    float3 mfHitTangent = float3(1.0, 0.0, 0.0);
    float2 mfHitUv = float2(0.5, 0.5);
    bool mfHit = false;

    if (mfMeshType < 0.5)
    {
        const float mfRadius = 1.05;
        float mfB = dot(mfRayOrigin, mfRayDirection);
        float mfC = dot(mfRayOrigin, mfRayOrigin) - mfRadius * mfRadius;
        float mfH = mfB * mfB - mfC;
        if (mfH > 0.0)
        {
            float mfT = -mfB - sqrt(mfH);
            if (mfT < 0.01)
            {
                mfT = -mfB + sqrt(mfH);
            }
            if (mfT > 0.01)
            {
                mfHit = true;
                mfHitPos = mfRayOrigin + mfT * mfRayDirection;
                mfHitNormal = normalize(mfHitPos);
                float mfU = atan2(mfHitNormal.z, mfHitNormal.x) / (2.0 * MF_PI) + 0.5;
                float mfV = acos(clamp(mfHitNormal.y, -1.0, 1.0)) / MF_PI;
                mfHitUv = float2(mfU, mfV);
                float3 mfUp = abs(mfHitNormal.y) > 0.999
                                  ? float3(1.0, 0.0, 0.0)
                                  : float3(0.0, 1.0, 0.0);
                mfHitTangent = normalize(cross(mfUp, mfHitNormal));
            }
        }
    }
    else
    {
        if (abs(mfRayDirection.z) > 1e-6)
        {
            float mfT = -mfRayOrigin.z / mfRayDirection.z;
            if (mfT > 0.01)
            {
                float3 mfP = mfRayOrigin + mfT * mfRayDirection;
                if (abs(mfP.x) <= 1.1 && abs(mfP.y) <= 1.1)
                {
                    mfHit = true;
                    mfHitPos = mfP;
                    mfHitNormal = float3(0.0, 0.0, 1.0);
                    mfHitTangent = float3(1.0, 0.0, 0.0);
                    mfHitUv = float2(mfP.x / 2.2 + 0.5, mfP.y / 2.2 + 0.5);
                }
            }
        }
    }

    if (!mfHit)
    {
        mfOutput[mfId.xy] = float4(mf_Background(mfUv, float2(mfId.xy)), 1.0);
        return;
    }

    if (mfTwoSided > 0.5 && dot(mfHitNormal, -mfRayDirection) < 0.0)
    {
        mfHitNormal = -mfHitNormal;
    }

    float3 vWorldPos = mfHitPos;
    float3 vNormal = mfHitNormal;
    float3 vTangent = mfHitTangent;
    float2 vUV = mfHitUv;

)HLSL";

    for (const std::string& body : bodyLines) {
        source << TranslateLine(body, material.blendMode) << "\n";
    }
    source << "}\n";

    result.success = true;
    result.source = source.str();
    result.parameters = material.parameters;
    result.textures = material.textures;
    result.textureCount = 1 + static_cast<int>(material.textures.size());
    result.diagnostic = "HLSL preview shader generated (" +
                        std::to_string(material.evaluatedNodeCount) + " expressions, " +
                        std::to_string(material.parameters.size()) + " parameters, " +
                        std::to_string(material.textures.size()) + " textures).";
    return result;
}

} // namespace mf
