#include "codegen/GlslGenerator.h"

#include "core/NodeRegistry.h"
#include "core/TypeResolver.h"
#include "io/GraphSerializer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mf {
namespace {

class CodegenError : public std::runtime_error {
public:
    explicit CodegenError(const std::string& message) : std::runtime_error(message) {}
};

struct Expr {
    std::string code;
    PinType type = PinType::Float;
};

struct NodeEvaluation {
    std::unordered_map<std::string, Expr> outputs;
};

std::string GlslType(PinType type) {
    switch (type) {
    case PinType::Float: return "float";
    case PinType::Vector2: return "vec2";
    case PinType::Vector3: return "vec3";
    case PinType::Vector4: return "vec4";
    default: break;
    }
    throw CodegenError(std::string("Type '") + PinTypeToString(type) + "' cannot be represented as a GLSL numeric value.");
}

std::string FloatLiteral(float value) {
    if (!std::isfinite(value)) {
        return "0.0";
    }
    std::ostringstream out;
    out << std::setprecision(9) << value;
    std::string text = out.str();
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find('E') == std::string::npos) {
        text += ".0";
    }
    return text;
}

std::string SanitizeIdentifier(const std::string& input, const std::string& fallback = "Value") {
    // GLSL reserves every identifier containing "__" (and those beginning with
    // "gl_"). This helper must therefore never emit adjacent underscores, even
    // when a material parameter name is Chinese text that becomes a run of
    // placeholder characters.
    std::string out;
    out.reserve(input.size());
    bool previousUnderscore = false;
    for (unsigned char ch : input) {
        if (ch < 128 && std::isalnum(ch)) {
            out.push_back(static_cast<char>(ch));
            previousUnderscore = false;
        } else if (ch == '_') {
            if (!out.empty() && !previousUnderscore) {
                out.push_back('_');
                previousUnderscore = true;
            }
        } else {
            // Non-ASCII bytes (for example the UTF-8 bytes of a Chinese
            // parameter name) and punctuation collapse into one separator.
            if (!out.empty() && !previousUnderscore) {
                out.push_back('_');
                previousUnderscore = true;
            }
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.size() > 64) {
        out.resize(64);
        while (!out.empty() && out.back() == '_') out.pop_back();
    }
    if (out.empty()) {
        out = fallback.empty() ? "Value" : fallback;
    }
    if (std::isdigit(static_cast<unsigned char>(out.front()))) {
        // A leading digit is not a valid identifier start. Use a letter rather
        // than an underscore so callers can still safely join names with '_'.
        out.insert(out.begin(), 'n');
    }
    return out;
}

std::string SwizzleForCount(int count) {
    switch (count) {
    case 1: return ".x";
    case 2: return ".xy";
    case 3: return ".xyz";
    case 4: return "";
    default: return "";
    }
}

std::vector<float> PropertyComponents(const PropertyValue& value) {
    switch (value.kind) {
    case PropertyKind::Float:
    case PropertyKind::Bool:
        return {value.number};
    case PropertyKind::Vector2:
        return {value.vec2.x, value.vec2.y};
    case PropertyKind::Vector3:
    case PropertyKind::Color3:
        return {value.vec3.x, value.vec3.y, value.vec3.z};
    case PropertyKind::Vector4:
    case PropertyKind::Color4:
        return {value.vec4.x, value.vec4.y, value.vec4.z, value.vec4.w};
    default:
        return {};
    }
}

std::string MakePropertyLiteral(const PropertyValue& value, PinType target) {
    const int targetCount = PinComponentCount(target);
    if (!IsNumericPin(target) || target == PinType::Wildcard || targetCount <= 0) {
        throw CodegenError("Cannot convert a property value to " + std::string(PinTypeToString(target)) + ".");
    }

    std::vector<float> components = PropertyComponents(value);
    if (components.empty()) {
        components.push_back(0.0f);
    }

    if (target == PinType::Float) {
        return FloatLiteral(components.front());
    }

    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(targetCount));
    if (components.size() == 1) {
        for (int i = 0; i < targetCount; ++i) {
            out.push_back(components.front());
        }
    } else {
        for (int i = 0; i < targetCount && i < static_cast<int>(components.size()); ++i) {
            out.push_back(components[static_cast<std::size_t>(i)]);
        }
        while (static_cast<int>(out.size()) < targetCount) {
            const bool alphaChannel = targetCount == 4 && static_cast<int>(out.size()) == 3;
            out.push_back(alphaChannel ? 1.0f : 0.0f);
        }
    }

    std::ostringstream literal;
    literal << GlslType(target) << "(";
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i > 0) literal << ", ";
        literal << FloatLiteral(out[i]);
    }
    literal << ")";
    return literal.str();
}

Expr Coerce(const Expr& expr, PinType target, const std::string& context) {
    if (expr.type == target) {
        return expr;
    }
    if (!IsNumericPin(expr.type) || !IsNumericPin(target) ||
        expr.type == PinType::Wildcard || target == PinType::Wildcard) {
        throw CodegenError("Cannot convert " + std::string(PinTypeToString(expr.type)) + " to " +
                           PinTypeToString(target) + " at " + context + ".");
    }

    const int sourceCount = PinComponentCount(expr.type);
    const int targetCount = PinComponentCount(target);
    if (targetCount <= 0 || sourceCount <= 0) {
        throw CodegenError("Invalid numeric conversion at " + context + ".");
    }

    if (target == PinType::Float) {
        return {"(" + expr.code + ").x", PinType::Float};
    }

    if (sourceCount == 1) {
        return {GlslType(target) + "(" + expr.code + ")", target};
    }

    if (sourceCount > targetCount) {
        return {"(" + expr.code + ")" + SwizzleForCount(targetCount), target};
    }

    if (targetCount == 4 && sourceCount == 3) {
        return {GlslType(target) + "(" + expr.code + ", 1.0)", target};
    }
    if (targetCount == 4 && sourceCount == 2) {
        return {GlslType(target) + "(" + expr.code + ", 0.0, 1.0)", target};
    }
    if (targetCount == 3 && sourceCount == 2) {
        return {GlslType(target) + "(" + expr.code + ", 0.0)", target};
    }

    throw CodegenError("Unsupported numeric conversion at " + context + ".");
}

PropertyValue ReadProperty(const Node& node, const std::string& key) {
    const auto it = node.properties.find(key);
    return it == node.properties.end() ? PropertyValue{} : it->second;
}

float ReadFloatProperty(const Node& node, const std::string& key, float fallback = 0.0f) {
    const PropertyValue value = ReadProperty(node, key);
    return IsNumericPin(value.NumericPinType()) ? value.number : fallback;
}

Vec2 ReadVec2Property(const Node& node, const std::string& key, Vec2 fallback = Vec2(0.0f)) {
    const PropertyValue value = ReadProperty(node, key);
    if (value.kind == PropertyKind::Vector2) return value.vec2;
    return fallback;
}

std::string ReadStringProperty(const Node& node, const std::string& key, const std::string& fallback = {}) {
    const PropertyValue value = ReadProperty(node, key);
    if (IsStringProperty(value.kind)) return value.text;
    return fallback;
}

PinType FunctionNodePinType(const Node& node) {
    bool ok = false;
    const PinType type = PinTypeFromString(ReadStringProperty(node, "pinType", "Float"), &ok);
    return ok && IsNumericPin(type) ? type : PinType::Float;
}

Expr ZeroExpr(PinType type) {
    switch (type) {
    case PinType::Vector2: return Expr{"vec2(0.0)", PinType::Vector2};
    case PinType::Vector3: return Expr{"vec3(0.0)", PinType::Vector3};
    case PinType::Vector4: return Expr{"vec4(0.0)", PinType::Vector4};
    default: return Expr{"0.0", PinType::Float};
    }
}

std::string EscapeSingleLine(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '\n' || ch == '\r') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

const char* kPbrHelpers = R"GLSL(
// ---------------------------------------------------------------------------
// Compact metallic/roughness studio lighting used by the preview viewport.
// This is a MaterialForge preview approximation, not UE's exact shading model.
// ---------------------------------------------------------------------------
float mf_D_GGX(float NoH, float a)
{
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1e-6);
}

float mf_Vis_SmithGGX(float NoV, float NoL, float a)
{
    float k = a * 0.5;
    float gv = NoV / (NoV * (1.0 - k) + k);
    float gl = NoL / (NoL * (1.0 - k) + k);
    return gv * gl;
}

vec3 mf_EvalLight(vec3 N, vec3 V, vec3 L, vec3 baseColor, vec3 F0, float roughness, float metallic)
{
    vec3 H = normalize(V + L);
    float NoL = max(dot(N, L), 0.0);
    float NoV = max(dot(N, V), 1e-4);
    float NoH = max(dot(N, H), 0.0);
    float VoH = max(dot(V, H), 0.0);
    float a = max(roughness * roughness, 1e-3);

    float D = mf_D_GGX(NoH, a);
    float Vis = mf_Vis_SmithGGX(NoV, NoL, a);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VoH, 5.0);

    vec3 specular = (D * Vis * F) / max(4.0 * NoV * NoL, 1e-4);
    vec3 diffuse = (1.0 - F) * (1.0 - metallic) * baseColor / 3.14159265;
    return (diffuse + specular) * NoL;
}

float mf_hash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float mf_valueNoise(vec3 p)
{
    vec3 cell = floor(p);
    vec3 local = fract(p);
    local = local * local * (3.0 - 2.0 * local);
    float n000 = mf_hash13(cell + vec3(0.0, 0.0, 0.0));
    float n100 = mf_hash13(cell + vec3(1.0, 0.0, 0.0));
    float n010 = mf_hash13(cell + vec3(0.0, 1.0, 0.0));
    float n110 = mf_hash13(cell + vec3(1.0, 1.0, 0.0));
    float n001 = mf_hash13(cell + vec3(0.0, 0.0, 1.0));
    float n101 = mf_hash13(cell + vec3(1.0, 0.0, 1.0));
    float n011 = mf_hash13(cell + vec3(0.0, 1.0, 1.0));
    float n111 = mf_hash13(cell + vec3(1.0, 1.0, 1.0));
    float nx00 = mix(n000, n100, local.x);
    float nx10 = mix(n010, n110, local.x);
    float nx01 = mix(n001, n101, local.x);
    float nx11 = mix(n011, n111, local.x);
    return mix(mix(nx00, nx10, local.y), mix(nx01, nx11, local.y), local.z);
}
)GLSL";

class Compiler {
public:
    Compiler(const MaterialGraph& graph, int glslVersion,
             std::filesystem::path baseDirectory = {},
             std::shared_ptr<std::vector<std::string>> functionStack = {},
             std::string uniformPrefix = {},
             PreviewBuffer previewBuffer = PreviewBuffer::Lit,
             PreviewLightingSettings lighting = {})
        : m_Graph(graph),
          m_Resolver(graph),
          m_GlslVersion(glslVersion),
          m_BaseDirectory(std::move(baseDirectory)),
          m_FunctionStack(functionStack ? std::move(functionStack)
                                        : std::make_shared<std::vector<std::string>>()),
          m_UniformPrefix(std::move(uniformPrefix)),
          m_PreviewBuffer(previewBuffer),
          m_Lighting(lighting) {}

    void CompileMaterial(const Node& outputNode) {
        m_Body << "    vec3 mf_N = normalize(vNormal);\n";
        m_Body << "    vec3 mf_V = normalize(uCameraPos - vWorldPos);\n";

        auto requireInput = [&](const std::string& name) -> const Pin* {
            const Pin* pin = outputNode.FindInput(name);
            if (!pin) {
                throw CodegenError("Material Output is missing input '" + name + "'.");
            }
            return pin;
        };

        const bool normalConnected = [&]() {
            const Pin* pin = requireInput("Normal");
            return m_Graph.FindInputLink(pin->id) != nullptr;
        }();

        const Expr baseColor = Coerce(ReadInput(outputNode, "Base Color", std::nullopt),
                                      PinType::Vector3, "MaterialOutput.BaseColor");
        const Expr metallic = Coerce(ReadInput(outputNode, "Metallic", std::nullopt),
                                     PinType::Float, "MaterialOutput.Metallic");
        const Expr specular = Coerce(ReadInput(outputNode, "Specular", std::nullopt),
                                     PinType::Float, "MaterialOutput.Specular");
        const Expr roughness = Coerce(ReadInput(outputNode, "Roughness", std::nullopt),
                                      PinType::Float, "MaterialOutput.Roughness");
        const Expr emissive = Coerce(ReadInput(outputNode, "Emissive Color", std::nullopt),
                                     PinType::Vector3, "MaterialOutput.Emissive");
        const Expr normal = Coerce(ReadInput(outputNode, "Normal", std::nullopt),
                                   PinType::Vector3, "MaterialOutput.Normal");
        const Expr ao = Coerce(ReadInput(outputNode, "Ambient Occlusion", std::nullopt),
                               PinType::Float, "MaterialOutput.AO");
        const Expr opacity = Coerce(ReadInput(outputNode, "Opacity", std::nullopt),
                                    PinType::Float, "MaterialOutput.Opacity");
        const Expr opacityMask = Coerce(ReadInput(outputNode, "Opacity Mask", std::nullopt),
                                        PinType::Float, "MaterialOutput.OpacityMask");
        const Expr worldPositionOffset =
            Coerce(ReadInput(outputNode, "World Position Offset", std::nullopt),
                   PinType::Vector3, "MaterialOutput.WorldPositionOffset");
        const Expr pixelDepthOffset =
            Coerce(ReadInput(outputNode, "Pixel Depth Offset", std::nullopt),
                   PinType::Float, "MaterialOutput.PixelDepthOffset");
        const Expr refraction = Coerce(ReadInput(outputNode, "Refraction", std::nullopt),
                                       PinType::Float, "MaterialOutput.Refraction");
        const Expr clearCoat = Coerce(ReadInput(outputNode, "Clear Coat", std::nullopt),
                                      PinType::Float, "MaterialOutput.ClearCoat");
        const Expr clearCoatRoughness =
            Coerce(ReadInput(outputNode, "Clear Coat Roughness", std::nullopt),
                   PinType::Float, "MaterialOutput.ClearCoatRoughness");
        const Expr subsurfaceColor =
            Coerce(ReadInput(outputNode, "Subsurface Color", std::nullopt),
                   PinType::Vector3, "MaterialOutput.SubsurfaceColor");
        const Expr anisotropy = Coerce(ReadInput(outputNode, "Anisotropy", std::nullopt),
                                       PinType::Float, "MaterialOutput.Anisotropy");

        const MaterialSettings& settings = m_Graph.Settings();
        const std::string& blendMode = settings.blendMode;
        const std::string& shadingModel = settings.shadingModel;

        m_Body << "\n    // ---- Material Output ----\n";
        m_Body << "    vec3 mf_worldPos = vWorldPos + " << worldPositionOffset.code << ";\n";
        m_Body << "    mf_V = normalize(uCameraPos - mf_worldPos);\n";
        m_Body << "    vec3 mf_baseColor = " << baseColor.code << ";\n";
        m_Body << "    float mf_metallic = clamp(" << metallic.code << ", 0.0, 1.0);\n";
        m_Body << "    float mf_specular = clamp(" << specular.code << ", 0.0, 1.0);\n";
        m_Body << "    float mf_roughness = clamp(" << roughness.code << ", 0.04, 1.0);\n";
        m_Body << "    vec3 mf_emissive = " << emissive.code << ";\n";
        m_Body << "    float mf_ao = clamp(" << ao.code << ", 0.0, 1.0);\n";
        m_Body << "    float mf_opacity = clamp(" << opacity.code << ", 0.0, 1.0);\n";
        m_Body << "    float mf_opacityMask = clamp(" << opacityMask.code << ", 0.0, 1.0);\n";
        m_Body << "    float mf_pixelDepthOffset = " << pixelDepthOffset.code << ";\n";
        m_Body << "    float mf_refraction = clamp(" << refraction.code << ", 0.0, 2.0);\n";
        m_Body << "    float mf_clearCoat = clamp(" << clearCoat.code << ", 0.0, 1.0);\n";
        m_Body << "    float mf_clearCoatRoughness = clamp(" << clearCoatRoughness.code
               << ", 0.0, 1.0);\n";
        m_Body << "    vec3 mf_subsurfaceColor = " << subsurfaceColor.code << ";\n";
        m_Body << "    float mf_anisotropy = clamp(" << anisotropy.code << ", -1.0, 1.0);\n";

        if (blendMode == "Opaque") {
            m_Body << "    mf_opacity = 1.0;\n";
        } else if (blendMode == "Masked") {
            m_Body << "    // Masked: alpha is binary and composited against the preview background.\n";
            m_Body << "    mf_opacity = mf_opacityMask >= " << settings.opacityMaskClip
                   << " ? 1.0 : 0.0;\n";
        } else {
            m_Body << "    // " << blendMode
                   << ": opacity is used by the preview blend/composite stage.\n";
        }
        if (shadingModel == "Subsurface" || shadingModel == "SubsurfaceProfile" ||
            shadingModel == "TwoSidedFoliage") {
            m_Body << "    mf_baseColor = mix(mf_baseColor, mf_subsurfaceColor, 0.35);\n";
        }

        const int previewMode = static_cast<int>(m_PreviewBuffer);
        if (previewMode != static_cast<int>(PreviewBuffer::Lit)) {
            m_Body << "    // Buffer visualization: " << PreviewBufferName(m_PreviewBuffer)
                   << "\n";
            m_Body << "    if (" << previewMode
                   << " == 1) { FragColor = vec4(mf_baseColor, 1.0); return; }\n";
            m_Body << "    if (" << previewMode
                   << " == 2) { FragColor = vec4(mf_metallic, mf_metallic, mf_metallic, 1.0); return; }\n";
            m_Body << "    if (" << previewMode
                   << " == 3) { FragColor = vec4(mf_specular, mf_specular, mf_specular, 1.0); return; }\n";
            m_Body << "    if (" << previewMode
                   << " == 4) { FragColor = vec4(mf_roughness, mf_roughness, mf_roughness, 1.0); return; }\n";
            m_Body << "    if (" << previewMode
                   << " == 5) { FragColor = vec4(mf_emissive, 1.0); return; }\n";
            m_Body << "    if (" << previewMode
                   << " == 6) { FragColor = vec4(mf_N * 0.5 + 0.5, 1.0); return; }\n";
            m_Body << "    if (" << previewMode
                   << " == 7) { FragColor = vec4(mf_ao, mf_ao, mf_ao, 1.0); return; }\n";
            m_Body << "    if (" << previewMode
                   << " == 8) { FragColor = vec4(mf_opacity, mf_opacity, mf_opacity, 1.0); return; }\n";
            m_Body << "    if (" << previewMode
                   << " == 9) { FragColor = vec4(mf_N * 0.5 + 0.5, 1.0); return; }\n";
            m_Body << "    if (" << previewMode
                   << " == 10) { float mf_depth = clamp(distance(uCameraPos, mf_worldPos) * 0.1, 0.0, 1.0); FragColor = vec4(mf_depth, mf_depth, mf_depth, 1.0); return; }\n";
        }

        m_Body << "\n    // Perturbed tangent basis for the Normal input.\n";
        m_Body << "    vec3 mf_tangent = normalize(vTangent - mf_N * dot(mf_N, vTangent));\n";
        m_Body << "    vec3 mf_bitangent = normalize(cross(mf_N, mf_tangent));\n";
        if (normalConnected) {
            m_Body << "    vec3 mf_normalTS = " << normal.code << ";\n";
            m_Body << "    mf_N = normalize(mf_tangent * mf_normalTS.x + mf_bitangent * mf_normalTS.y + mf_N * mf_normalTS.z);\n";
        } else {
            m_Body << "    // Normal is unconnected: the geometric normal is used.\n";
        }
        m_Body << "    mf_V = normalize(mf_V + mf_N * (mf_refraction - 1.0) * 0.18);\n";

        if (shadingModel == "Unlit") {
            m_Body << "\n    // ---- Unlit shading model ----\n";
            m_Body << "    vec3 mf_color = mf_baseColor + mf_emissive;\n";
        } else {
            m_Body << "\n    // ---- Studio lighting (Default Lit family) ----\n";
            m_Body << "    vec3 mf_F0 = mix(vec3(0.08 * mf_specular), mf_baseColor, mf_metallic);\n";
            m_Body << "    vec3 mf_color = mf_emissive;\n";
            m_Body << "    vec3 mf_L1 = normalize(vec3(-0.45, 0.75, 0.55));\n";
            m_Body << "    vec3 mf_L2 = normalize(vec3(0.65, 0.25, 0.45));\n";
            m_Body << "    vec3 mf_L3 = normalize(vec3(0.05, -0.35, -0.90));\n";
            m_Body << "    mf_color += mf_EvalLight(mf_N, mf_V, mf_L1, mf_baseColor, mf_F0, mf_roughness, mf_metallic) * vec3(3.3, 3.15, 2.9) * "
                   << m_Lighting.keyIntensity << ";\n";
            m_Body << "    mf_color += mf_EvalLight(mf_N, mf_V, mf_L2, mf_baseColor, mf_F0, mf_roughness, mf_metallic) * vec3(0.9, 1.15, 1.7) * "
                   << m_Lighting.fillIntensity << ";\n";
            m_Body << "    mf_color += mf_EvalLight(mf_N, mf_V, mf_L3, mf_baseColor, mf_F0, mf_roughness, mf_metallic) * vec3(0.55, 0.9, 0.8) * "
                   << m_Lighting.rimIntensity << ";\n";
            m_Body << "    float mf_NoV = clamp(dot(mf_N, mf_V), 0.0, 1.0);\n";
            m_Body << "    vec3 mf_sky = mix(vec3(0.012, 0.018, 0.030), vec3(0.055, 0.10, 0.15), mf_N.y * 0.5 + 0.5);\n";
            m_Body << "    vec3 mf_ambient = mf_baseColor * mf_sky * mf_ao * (1.0 - mf_metallic * 0.7);\n";
            m_Body << "    vec3 mf_envSpec = mf_F0 * mf_sky * (0.25 + 0.75 * pow(1.0 - mf_NoV, 2.0)) * (1.0 - mf_roughness * 0.6);\n";
            m_Body << "    float mf_rim = pow(1.0 - mf_NoV, 4.0);\n";
            m_Body << "    mf_color += (mf_ambient + mf_envSpec) * "
                   << m_Lighting.ambientIntensity << ";\n";
            m_Body << "    mf_color += mf_sky * mf_rim * 0.45 * "
                   << m_Lighting.rimIntensity << ";\n";
            m_Body << "    float mf_coatPower = mix(2.0, 8.0, 1.0 - mf_clearCoatRoughness);\n";
            m_Body << "    mf_color += mf_clearCoat * pow(1.0 - mf_NoV, mf_coatPower) * vec3(0.20, 0.22, 0.24);\n";
        }

        // A SingleLayerWaterMaterialOutput is an additional output expression
        // rather than a replacement for MaterialOutput. Approximate the visible
        // single-layer water response here: the regular base color acts as the
        // scene behind the surface, scatter/absorption tint the volume, and a
        // small procedural term breaks up the water with suspended impurities.
        const Node* waterOutput = nullptr;
        for (const auto& graphNode : m_Graph.Nodes()) {
            if (graphNode->type == "SingleLayerWaterMaterialOutput") {
                waterOutput = graphNode.get();
                break;
            }
        }
        if (waterOutput) {
            const Expr waterScatter =
                Coerce(ReadInput(*waterOutput, "Scattering Coefficients"), PinType::Vector3,
                       "SingleLayerWater.Scattering");
            const Expr waterAbsorb =
                Coerce(ReadInput(*waterOutput, "Absorption Coefficients"), PinType::Vector3,
                       "SingleLayerWater.Absorption");
            const Expr waterPhase =
                Coerce(ReadInput(*waterOutput, "PhaseG"), PinType::Float,
                       "SingleLayerWater.PhaseG");
            const Expr waterBehind =
                Coerce(ReadInput(*waterOutput, "Color Scale Behind Water"), PinType::Vector3,
                       "SingleLayerWater.ColorScale");
            m_Body << "\n    // ---- Single Layer Water approximation ----\n";
            m_Body << "    vec3 mf_waterScatter = max(" << waterScatter.code << ", vec3(0.0));\n";
            m_Body << "    vec3 mf_waterAbsorb = max(" << waterAbsorb.code << ", vec3(0.001));\n";
            m_Body << "    float mf_waterPhase = mix(0.5, 2.5, clamp(" << waterPhase.code
                   << " * 0.5 + 0.5, 0.0, 1.0));\n";
            m_Body << "    vec3 mf_waterBehind = mf_baseColor * max(" << waterBehind.code
                   << ", vec3(0.0));\n";
            m_Body << "    float mf_waterDepth = clamp(0.35 + 0.65 * (1.0 - clamp(dot(mf_N, mf_V), 0.0, 1.0)), 0.0, 1.0);\n";
            m_Body << "    vec3 mf_waterTransmittance = exp(-mf_waterAbsorb * mf_waterDepth * 80.0);\n";
            m_Body << "    vec3 mf_waterInscatter = mf_waterScatter * (1.0 - mf_waterTransmittance) * mf_waterPhase;\n";
            m_Body << "    float mf_waterImpurity = mf_valueNoise(vWorldPos * 9.0 + vec3(3.7, 1.9, 5.3));\n";
            m_Body << "    mf_waterInscatter += mf_waterScatter * (mf_waterImpurity - 0.5) * 0.16;\n";
            m_Body << "    float mf_waterNoV = clamp(dot(mf_N, mf_V), 0.0, 1.0);\n";
            m_Body << "    float mf_waterSpec = pow(1.0 - mf_waterNoV, 3.0) * (0.25 + mf_specular * 0.75);\n";
            m_Body << "    mf_color = mf_waterBehind * mf_waterTransmittance + mf_waterInscatter";
            m_Body << " + vec3(mf_waterSpec) * vec3(0.55, 0.68, 0.78) + mf_emissive;\n";
        }

        m_Body << "\n    // Filmic shoulder + gamma. The target framebuffer is a plain RGBA8 texture,\n"
                  "    // so the tone mapping is applied here rather than by the OpenGL pipeline.\n";
        m_Body << "    mf_color *= 1.18 * " << m_Lighting.exposure << ";\n";
        m_Body << "    mf_color = mf_color / (mf_color + vec3(1.0));\n";
        m_Body << "    mf_color = pow(max(mf_color, vec3(0.0)), vec3(1.0 / 2.2));\n";
        m_Body << "    FragColor = vec4(mf_color, mf_opacity);\n";
    }

    std::string FinishSource(const MaterialGraph& graph) const {
        std::ostringstream src;
        src << "#version " << m_GlslVersion << " core\n";
        src << "// ===========================================================================\n";
        src << "// MaterialForge generated GLSL " << m_GlslVersion
            << " fragment shader. Do not edit by hand.\n";
        src << "// Material: " << EscapeSingleLine(graph.Settings().name) << "\n";
        src << "// Evaluated expression nodes: " << m_EvaluatedNodeCount << "\n";
        src << "// ===========================================================================\n\n";
        src << "in vec3 vWorldPos;\n";
        src << "in vec3 vNormal;\n";
        src << "in vec3 vTangent;\n";
        src << "in vec2 vUV;\n";
        src << "out vec4 FragColor;\n\n";
        src << "uniform vec3 uCameraPos;\n";
        src << "uniform float uTime;\n";
        src << "uniform vec2 uViewSize;\n";
        src << "uniform sampler2D uDefaultTexture;\n";

        if (!m_Textures.empty()) {
            src << "\n// Texture bindings\n";
            for (const ShaderTextureBinding& texture : m_Textures) {
                src << "uniform sampler2D " << texture.uniformName << "; // "
                    << EscapeSingleLine(texture.assetPath) << "\n";
            }
        }
        if (!m_Parameters.empty()) {
            src << "\n// Material parameter bindings\n";
            for (const ShaderParameterBinding& parameter : m_Parameters) {
                src << "uniform " << GlslType(parameter.type) << " " << parameter.uniformName
                    << "; // " << EscapeSingleLine(parameter.parameterName) << "\n";
            }
        }

        src << kPbrHelpers;
        src << "\nvoid main()\n{\n";
        src << m_Body.str();
        src << "}\n";
        return src.str();
    }

    const std::vector<ShaderParameterBinding>& Parameters() const { return m_Parameters; }
    const std::vector<ShaderTextureBinding>& Textures() const { return m_Textures; }
    const std::vector<std::string>& Warnings() const { return m_Warnings; }
    int EvaluatedNodeCount() const { return m_EvaluatedNodeCount; }

    Expr EvaluateFunctionOutput(const std::string& outputName) {
        for (const auto& node : m_Graph.Nodes()) {
            if (node->type != "FunctionOutput") continue;
            const std::string name = ReadStringProperty(*node, "outputName", "Output");
            if (name != outputName) continue;
            return ReadInput(*node, "Value");
        }
        throw CodegenError("Material function is missing output '" + outputName + "'.");
    }

private:
    std::string VariableName(const Node& node, const std::string& suffix = {}) const {
        return "mf_" + m_VariablePrefix + SanitizeIdentifier(node.type, "Node") + "_" +
               std::to_string(node.id) + suffix;
    }

    Expr EmitAssignment(const Node& node, PinType type, const std::string& expression,
                        const std::string& suffix = {}) {
        const std::string name = VariableName(node, suffix);
        m_Body << "    // " << node.type << " #" << node.id << "\n";
        m_Body << "    " << GlslType(type) << " " << name << " = " << expression << ";\n";
        return {name, type};
    }

    Expr DefaultInputValue(const Pin& pin) const {
        if (pin.type == PinType::Texture || pin.type == PinType::Sampler) {
            return {"uDefaultTexture", pin.type};
        }
        PinType type = pin.type;
        if (type == PinType::Wildcard) {
            type = m_Resolver.GetInputType(pin);
        }
        if (type == PinType::Wildcard) {
            type = PinType::Float;
        }
        return {MakePropertyLiteral(pin.defaultValue, type), type};
    }

    Expr ReadInput(const Node& node, const std::string& pinName,
                   std::optional<Expr> fallback = std::nullopt) {
        const Pin* pin = node.FindInput(pinName);
        if (!pin) {
            throw CodegenError(node.type + " is missing input pin '" + pinName + "'.");
        }
        if (const Link* link = m_Graph.FindInputLink(pin->id)) {
            const Pin* source = m_Graph.FindPin(link->fromPin);
            if (!source) {
                throw CodegenError("Link " + std::to_string(link->id) + " has a missing source pin.");
            }
            return CompileOutput(*source);
        }
        if (fallback) {
            return *fallback;
        }
        return DefaultInputValue(*pin);
    }

    const NodeEvaluation& EvaluateOutput(const Pin& outputPin) {
        const Node* node = m_Graph.FindNode(outputPin.nodeId);
        if (!node) {
            throw CodegenError("Output pin belongs to a missing node.");
        }
        const auto cached = m_NodeCache.find(node->id);
        if (cached == m_NodeCache.end()) {
            if (!m_Compiling.insert(node->id).second) {
                throw CodegenError("Cycle detected while evaluating node " + node->type +
                                   " #" + std::to_string(node->id) + ".");
            }
            NodeEvaluation evaluation = CompileNode(*node);
            m_Compiling.erase(node->id);
            ++m_EvaluatedNodeCount;
            m_NodeCache.emplace(node->id, std::move(evaluation));
        }
        const NodeEvaluation& eval = m_NodeCache.at(node->id);
        if (eval.outputs.find(outputPin.name) == eval.outputs.end()) {
            throw CodegenError(node->type + " did not produce output pin '" + outputPin.name + "'.");
        }
        return eval;
    }

    Expr CompileOutput(const Pin& outputPin) {
        if (outputPin.IsSplitComponent()) {
            const Node* node = m_Graph.FindNode(outputPin.nodeId);
            const Pin* base = node ? node->FindOutput(outputPin.splitFrom) : nullptr;
            if (!node || !base) {
                throw CodegenError("Split output '" + outputPin.name +
                                   "' does not reference a base output pin.");
            }
            Expr baseExpr = CompileOutput(*base);
            if (!IsNumericPin(baseExpr.type) || baseExpr.type == PinType::Wildcard ||
                PinComponentCount(baseExpr.type) <= outputPin.splitIndex) {
                throw CodegenError("Cannot split output '" + outputPin.splitFrom + "'.");
            }
            static const char components[] = {'x', 'y', 'z', 'w'};
            return {"(" + baseExpr.code + ")." +
                        std::string(1, components[outputPin.splitIndex]),
                    PinType::Float};
        }
        const NodeEvaluation& eval = EvaluateOutput(outputPin);
        return eval.outputs.at(outputPin.name);
    }

    NodeEvaluation SingleOutput(const Node& node, const Expr& expr) const {
        if (node.outputs.empty()) {
            throw CodegenError(node.type + " has no outputs.");
        }
        NodeEvaluation eval;
        eval.outputs[node.outputs.front().name] = expr;
        return eval;
    }

    NodeEvaluation CompileFunctionCall(const Node& node) {
        const std::string path = ReadStringProperty(node, "functionPath");
        if (path.empty()) {
            throw CodegenError("MaterialFunctionCall #" + std::to_string(node.id) +
                               " has no function asset assigned.");
        }
        std::error_code ec;
        std::filesystem::path resolved = std::filesystem::u8path(path);
        if (!resolved.is_absolute()) {
            if (!m_BaseDirectory.empty()) {
                const std::filesystem::path candidate = m_BaseDirectory / resolved;
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    resolved = candidate;
                }
            }
            if (!std::filesystem::is_regular_file(resolved, ec)) {
                const std::filesystem::path candidate =
                    std::filesystem::current_path(ec) / resolved;
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    resolved = candidate;
                }
            }
        }
        if (!std::filesystem::is_regular_file(resolved, ec)) {
            throw CodegenError("Material function asset is missing: " + path);
        }
        const std::string key = resolved.u8string();
        if (std::find(m_FunctionStack->begin(), m_FunctionStack->end(), key) !=
            m_FunctionStack->end()) {
            throw CodegenError("Cyclic material function call detected at: " + path);
        }
        m_FunctionStack->push_back(key);

        MaterialGraph functionGraph;
        std::string loadError;
        if (!GraphSerializer::Load(key, functionGraph, &loadError)) {
            m_FunctionStack->pop_back();
            throw CodegenError("Could not load material function '" + path + "': " + loadError);
        }

        Compiler functionCompiler(functionGraph, m_GlslVersion, resolved.parent_path(),
                                  m_FunctionStack);
        functionCompiler.m_UniformPrefix = m_UniformPrefix + "fn" +
                                           std::to_string(node.id) + "_";
        functionCompiler.m_VariablePrefix =
            "fn" + std::to_string(node.id) + "_" +
            std::to_string(m_FunctionStack->size()) + "_";

        std::vector<const Node*> functionInputs;
        std::vector<const Node*> functionOutputs;
        for (const auto& functionNode : functionGraph.Nodes()) {
            if (functionNode->type == "FunctionInput") {
                functionInputs.push_back(functionNode.get());
            } else if (functionNode->type == "FunctionOutput") {
                functionOutputs.push_back(functionNode.get());
            }
        }

        for (std::size_t i = 0; i < functionInputs.size(); ++i) {
            const std::string inputName =
                ReadStringProperty(*functionInputs[i], "inputName", "Input");
            const PinType inputType = FunctionNodePinType(*functionInputs[i]);
            Expr value = ZeroExpr(inputType);
            const Pin* callPin = node.FindInput(inputName);
            if (!callPin && i < node.inputs.size()) {
                callPin = &node.inputs[i];
            }
            if (callPin) {
                value = Coerce(ReadInput(node, callPin->name), inputType,
                               "function input " + inputName);
            } else {
                m_Warnings.push_back("Function call #" + std::to_string(node.id) +
                                     " is missing input '" + inputName + "'; using 0.");
            }
            functionCompiler.m_FunctionInputs[inputName] = value;
        }

        NodeEvaluation evaluation;
        for (std::size_t i = 0; i < node.outputs.size(); ++i) {
            const Pin& outputPin = node.outputs[i];
            const Node* outputNode = nullptr;
            for (const Node* candidate : functionOutputs) {
                if (ReadStringProperty(*candidate, "outputName", "Output") == outputPin.name) {
                    outputNode = candidate;
                    break;
                }
            }
            if (!outputNode && i < functionOutputs.size()) {
                outputNode = functionOutputs[i];
            }
            if (!outputNode) {
                throw CodegenError("Material function has no output '" + outputPin.name + "'.");
            }
            const std::string outputName =
                ReadStringProperty(*outputNode, "outputName", outputPin.name);
            Expr value = functionCompiler.EvaluateFunctionOutput(outputName);
            value = Coerce(value, FunctionNodePinType(*outputNode),
                           "function output " + outputName);
            evaluation.outputs[outputPin.name] = value;
        }

        for (const ShaderParameterBinding& parameter : functionCompiler.m_Parameters) {
            const auto existing = std::find_if(
                m_Parameters.begin(), m_Parameters.end(),
                [&](const ShaderParameterBinding& binding) {
                    return binding.uniformName == parameter.uniformName;
                });
            if (existing == m_Parameters.end()) {
                m_Parameters.push_back(parameter);
            }
        }
        for (const ShaderTextureBinding& texture : functionCompiler.m_Textures) {
            const auto existing = std::find_if(
                m_Textures.begin(), m_Textures.end(),
                [&](const ShaderTextureBinding& binding) {
                    return binding.uniformName == texture.uniformName;
                });
            if (existing == m_Textures.end()) {
                m_Textures.push_back(texture);
            }
        }
        for (const std::string& warning : functionCompiler.m_Warnings) {
            m_Warnings.push_back(warning);
        }
        m_EvaluatedNodeCount += functionCompiler.m_EvaluatedNodeCount;
        m_Body << functionCompiler.m_Body.str();
        m_FunctionStack->pop_back();
        return evaluation;
    }

    NodeEvaluation CompileNode(const Node& node) {
        const std::string& type = node.type;

        if (type == "MaterialFunctionCall") {
            return CompileFunctionCall(node);
        }
        if (type == "Reroute") {
            m_Body << "    // Reroute #" << node.id << "\n";
            return SingleOutput(node, ReadInput(node, "Input"));
        }

        if (type == "FunctionInput") {
            const std::string name = ReadStringProperty(node, "inputName", "Input");
            const auto value = m_FunctionInputs.find(name);
            return SingleOutput(node, value != m_FunctionInputs.end()
                                          ? value->second
                                          : ZeroExpr(FunctionNodePinType(node)));
        }
        if (type == "FunctionOutput") {
            throw CodegenError("FunctionOutput can only be evaluated through a function call.");
        }

        // ---- Constants -------------------------------------------------
        if (type == "Constant" || type == "Constant2Vector" ||
            type == "Constant3Vector" || type == "Constant4Vector") {
            if (node.outputs.empty()) throw CodegenError(type + " has no output pin.");
            const PinType outputType = node.outputs.front().type;
            const PropertyValue value = ReadProperty(node, "value");
            const Expr expr = EmitAssignment(node, outputType, MakePropertyLiteral(value, outputType));
            return SingleOutput(node, expr);
        }

        // ---- Parameters ------------------------------------------------
        if (type == "ScalarParameter" || type == "VectorParameter") {
            if (node.outputs.empty()) throw CodegenError(type + " has no output pin.");
            const std::string parameterName = ReadStringProperty(node, "parameterName", "Parameter");
            const std::string safeName = SanitizeIdentifier(parameterName, "Parameter");
            const PinType uniformType = (type == "ScalarParameter") ? PinType::Float : PinType::Vector4;
            const std::string uniformName =
                "uParam_" + m_UniformPrefix + safeName + "_" + std::to_string(node.id);
            if (!m_ParameterNames.insert(parameterName).second) {
                m_Warnings.push_back("Duplicate parameter name '" + parameterName +
                                     "'. UE5 requires unique material parameter names.");
            }
            m_Parameters.push_back({uniformName, node.id, parameterName, uniformType,
                                    ReadProperty(node, "value")});
            return SingleOutput(node, {uniformName, uniformType});
        }

        // ---- Binary math ----------------------------------------------
        if (type == "Add" || type == "Subtract" || type == "Multiply" ||
            type == "Divide" || type == "Power" || type == "Min" ||
            type == "Max" || type == "Fmod") {
            Expr a = ReadInput(node, "A");
            Expr b = ReadInput(node, "B");
            const PinType common = PromotePinTypes(a.type, b.type);
            if (!IsNumericPin(common) || common == PinType::Wildcard) {
                throw CodegenError(type + " requires numeric inputs.");
            }
            a = Coerce(a, common, type + ".A");
            b = Coerce(b, common, type + ".B");

            std::string expression;
            if (type == "Add") expression = "(" + a.code + " + " + b.code + ")";
            else if (type == "Subtract") expression = "(" + a.code + " - " + b.code + ")";
            else if (type == "Multiply") expression = "(" + a.code + " * " + b.code + ")";
            else if (type == "Divide") expression = "(" + a.code + " / " + b.code + ")";
            else if (type == "Power") expression = "pow(" + a.code + ", " + b.code + ")";
            else if (type == "Min") expression = "min(" + a.code + ", " + b.code + ")";
            else if (type == "Max") expression = "max(" + a.code + ", " + b.code + ")";
            else expression = "mod(" + a.code + ", " + b.code + ")";

            return SingleOutput(node, EmitAssignment(node, common, expression));
        }

        // ---- Unary math ------------------------------------------------
        if (type == "Abs" || type == "Ceil" || type == "Floor" || type == "Frac" ||
            type == "Round" || type == "Sqrt" || type == "OneMinus" ||
            type == "Sine" || type == "Cosine" || type == "Tangent" ||
            type == "Saturate" || type == "Sign" || type == "Truncate" ||
            type == "Logarithm2" || type == "Arcsine" || type == "Arccosine" ||
            type == "Arctangent") {
            Expr input = ReadInput(node, "Input");
            if (!IsNumericPin(input.type) || input.type == PinType::Wildcard) {
                throw CodegenError(type + " requires a numeric input.");
            }
            const PinType resultType = input.type;
            std::string expression;
            if (type == "Abs") expression = "abs(" + input.code + ")";
            else if (type == "Ceil") expression = "ceil(" + input.code + ")";
            else if (type == "Floor") expression = "floor(" + input.code + ")";
            else if (type == "Frac") expression = "fract(" + input.code + ")";
            else if (type == "Round") expression = "round(" + input.code + ")";
            else if (type == "Sqrt") expression = "sqrt(max(" + input.code + ", " +
                                                  GlslType(resultType) + "(0.0)))";
            else if (type == "OneMinus") expression = "(" + GlslType(resultType) + "(1.0) - " + input.code + ")";
            else if (type == "Sine") expression = "sin(" + input.code + ")";
            else if (type == "Cosine") expression = "cos(" + input.code + ")";
            else if (type == "Tangent") expression = "tan(" + input.code + ")";
            else if (type == "Sign") expression = "sign(" + input.code + ")";
            else if (type == "Truncate") expression = "trunc(" + input.code + ")";
            else if (type == "Logarithm2") expression = "log2(max(" + input.code + ", " +
                                                        GlslType(resultType) + "(1e-6)))";
            else if (type == "Arcsine") expression = "asin(clamp(" + input.code + ", " +
                                                      GlslType(resultType) + "(-1.0), " +
                                                      GlslType(resultType) + "(1.0)))";
            else if (type == "Arccosine") expression = "acos(clamp(" + input.code + ", " +
                                                        GlslType(resultType) + "(-1.0), " +
                                                        GlslType(resultType) + "(1.0)))";
            else if (type == "Arctangent") expression = "atan(" + input.code + ")";
            else expression = "clamp(" + input.code + ", " + GlslType(resultType) + "(0.0), " +
                              GlslType(resultType) + "(1.0))";
            return SingleOutput(node, EmitAssignment(node, resultType, expression));
        }

        if (type == "Lerp") {
            Expr a = ReadInput(node, "A");
            Expr b = ReadInput(node, "B");
            Expr alpha = Coerce(ReadInput(node, "Alpha"), PinType::Float, "Lerp.Alpha");
            const PinType common = PromotePinTypes(a.type, b.type);
            a = Coerce(a, common, "Lerp.A");
            b = Coerce(b, common, "Lerp.B");
            return SingleOutput(node, EmitAssignment(
                node, common, "mix(" + a.code + ", " + b.code + ", " + alpha.code + ")"));
        }

        if (type == "Clamp") {
            Expr value = ReadInput(node, "Value");
            Expr minValue = ReadInput(node, "Min");
            Expr maxValue = ReadInput(node, "Max");
            PinType common = PromotePinTypes(value.type, minValue.type);
            common = PromotePinTypes(common, maxValue.type);
            if (!IsNumericPin(common) || common == PinType::Wildcard) {
                throw CodegenError("Clamp requires numeric inputs.");
            }
            value = Coerce(value, common, "Clamp.Value");
            minValue = Coerce(minValue, common, "Clamp.Min");
            maxValue = Coerce(maxValue, common, "Clamp.Max");
            return SingleOutput(node, EmitAssignment(
                node, common, "clamp(" + value.code + ", " + minValue.code + ", " + maxValue.code + ")"));
        }

        if (type == "Greater" || type == "GreaterEqual" || type == "Less" ||
            type == "LessEqual" || type == "Equal" || type == "NotEqual") {
            Expr a = Coerce(ReadInput(node, "A"), PinType::Float, type + ".A");
            Expr b = Coerce(ReadInput(node, "B"), PinType::Float, type + ".B");
            std::string condition;
            if (type == "Greater") condition = "(" + a.code + " > " + b.code + ")";
            else if (type == "GreaterEqual") condition = "(" + a.code + " >= " + b.code + ")";
            else if (type == "Less") condition = "(" + a.code + " < " + b.code + ")";
            else if (type == "LessEqual") condition = "(" + a.code + " <= " + b.code + ")";
            else if (type == "Equal") condition = "(abs(" + a.code + " - " + b.code + ") <= 0.0001)";
            else condition = "(abs(" + a.code + " - " + b.code + ") > 0.0001)";
            return SingleOutput(node, EmitAssignment(
                node, PinType::Float, "(" + condition + " ? 1.0 : 0.0)"));
        }

        if (type == "If") {
            Expr a = Coerce(ReadInput(node, "A"), PinType::Float, "If.A");
            Expr b = Coerce(ReadInput(node, "B"), PinType::Float, "If.B");
            Expr greater = ReadInput(node, "A > B");
            Expr equal = ReadInput(node, "A == B");
            Expr less = ReadInput(node, "A < B");
            PinType result = PromotePinTypes(greater.type, equal.type);
            result = PromotePinTypes(result, less.type);
            if (!IsNumericPin(result) || result == PinType::Wildcard) result = PinType::Float;
            greater = Coerce(greater, result, "If.A>B");
            equal = Coerce(equal, result, "If.A==B");
            less = Coerce(less, result, "If.A<B");
            const std::string expression =
                "(" + a.code + " > " + b.code + " ? " + greater.code + " : (" + a.code +
                " == " + b.code + " ? " + equal.code + " : " + less.code + "))";
            return SingleOutput(node, EmitAssignment(node, result, expression));
        }

        if (type == "Switch") {
            Expr pick = Coerce(ReadInput(node, "Pick"), PinType::Float, "Switch.Pick");
            Expr a = ReadInput(node, "A");
            Expr b = ReadInput(node, "B");
            const PinType common = PromotePinTypes(a.type, b.type);
            if (!IsNumericPin(common) || common == PinType::Wildcard) {
                throw CodegenError("Switch requires numeric inputs.");
            }
            a = Coerce(a, common, "Switch.A");
            b = Coerce(b, common, "Switch.B");
            return SingleOutput(node, EmitAssignment(
                node, common, "(" + pick.code + " > 0.5 ? " + a.code + " : " + b.code + ")"));
        }

        if (type == "StaticSwitchParameter") {
            const bool enabled = ReadProperty(node, "value").boolean;
            Expr result = ReadInput(node, enabled ? "True" : "False");
            const PinType outputType = m_Resolver.GetOutputType(node.outputs.front());
            if (IsNumericPin(outputType) && outputType != PinType::Wildcard) {
                result = Coerce(result, outputType, "StaticSwitchParameter");
            }
            m_Body << "    // StaticSwitchParameter #" << node.id << " selected "
                   << (enabled ? "True" : "False") << "\n";
            return SingleOutput(node, result);
        }

        if (type == "RotateAboutAxis") {
            Expr position = Coerce(ReadInput(node, "Position"), PinType::Vector3,
                                   "RotateAboutAxis.Position");
            Expr axis = Coerce(ReadInput(node, "Axis"), PinType::Vector3,
                               "RotateAboutAxis.Axis");
            Expr angle = Coerce(ReadInput(node, "Angle"), PinType::Float,
                                "RotateAboutAxis.Angle");
            const std::string axisCode = "normalize(" + axis.code + ")";
            const std::string cosine = "cos(" + angle.code + ")";
            const std::string sine = "sin(" + angle.code + ")";
            const std::string expression =
                "((" + position.code + ") * " + cosine + " + cross(" + axisCode + ", " +
                position.code + ") * " + sine + " + " + axisCode + " * dot(" + axisCode +
                ", " + position.code + ") * (1.0 - " + cosine + "))";
            return SingleOutput(node, EmitAssignment(node, PinType::Vector3, expression));
        }

        if (type == "ComponentMask") {
            auto enabled = [&](const char* key) {
                const auto it = node.properties.find(key);
                return it != node.properties.end() && it->second.kind == PropertyKind::Bool &&
                       it->second.boolean;
            };
            std::string swizzle;
            if (enabled("r")) swizzle += "x";
            if (enabled("g")) swizzle += "y";
            if (enabled("b")) swizzle += "z";
            if (enabled("a")) swizzle += "w";
            if (swizzle.empty()) {
                return SingleOutput(node, EmitAssignment(node, PinType::Float, "0.0"));
            }
            const PinType result = m_Resolver.GetOutputType(node.outputs.front());
            Expr input = Coerce(ReadInput(node, "Input"), PinType::Vector4,
                                "ComponentMask.Input");
            return SingleOutput(node, EmitAssignment(
                node, result, "(" + input.code + ")." + swizzle));
        }

        if (type == "AppendVector") {
            Expr a = ReadInput(node, "A");
            Expr b = ReadInput(node, "B");
            const int components = PinComponentCount(a.type) + PinComponentCount(b.type);
            if (components <= 0 || components > 4) {
                throw CodegenError("AppendVector requires 1 to 4 combined components.");
            }
            const PinType result = NumericPinType(components);
            return SingleOutput(node, EmitAssignment(
                node, result, GlslType(result) + "(" + a.code + ", " + b.code + ")"));
        }

        if (type == "BreakVector2" || type == "BreakVector3" || type == "BreakVector4") {
            const int count = type == "BreakVector2" ? 2 : type == "BreakVector3" ? 3 : 4;
            const PinType inputType = NumericPinType(count);
            Expr input = Coerce(ReadInput(node, "Input"), inputType, type + ".Input");
            NodeEvaluation eval;
            static const char* components[] = {"R", "G", "B", "A"};
            static const char componentSuffix[] = {'x', 'y', 'z', 'w'};
            for (int i = 0; i < count; ++i) {
                const std::string suffix = std::string(".") + componentSuffix[i];
                eval.outputs[components[i]] =
                    EmitAssignment(node, PinType::Float, "(" + input.code + ")" + suffix,
                                   "_" + std::string(1, componentSuffix[i]));
            }
            return eval;
        }

        if (type == "Length" || type == "Distance") {
            if (type == "Length") {
                Expr input = ReadInput(node, "Input");
                const std::string expression =
                    input.type == PinType::Float ? "abs(" + input.code + ")"
                                                 : "length(" + input.code + ")";
                return SingleOutput(node, EmitAssignment(node, PinType::Float, expression));
            }
            Expr a = ReadInput(node, "A");
            Expr b = ReadInput(node, "B");
            PinType common = PromotePinTypes(a.type, b.type);
            a = Coerce(a, common, "Distance.A");
            b = Coerce(b, common, "Distance.B");
            const std::string expression =
                common == PinType::Float ? "abs(" + a.code + " - " + b.code + ")"
                                         : "distance(" + a.code + ", " + b.code + ")";
            return SingleOutput(node, EmitAssignment(node, PinType::Float, expression));
        }

        if (type == "SmoothStep" || type == "Step") {
            PinType common;
            Expr first;
            Expr second;
            Expr third;
            if (type == "SmoothStep") {
                first = ReadInput(node, "Value");
                second = ReadInput(node, "Min");
                third = ReadInput(node, "Max");
                common = PromotePinTypes(first.type, second.type);
                common = PromotePinTypes(common, third.type);
                first = Coerce(first, common, "SmoothStep.Value");
                second = Coerce(second, common, "SmoothStep.Min");
                third = Coerce(third, common, "SmoothStep.Max");
                return SingleOutput(node, EmitAssignment(
                    node, common,
                    "smoothstep(" + second.code + ", " + third.code + ", " + first.code + ")"));
            }
            first = ReadInput(node, "Value");
            second = ReadInput(node, "Edge");
            common = PromotePinTypes(first.type, second.type);
            first = Coerce(first, common, "Step.Value");
            second = Coerce(second, common, "Step.Edge");
            return SingleOutput(node, EmitAssignment(
                node, common, "step(" + second.code + ", " + first.code + ")"));
        }

        if (type == "ConstantBiasScale") {
            Expr input = ReadInput(node, "Input");
            Expr bias = ReadInput(node, "Bias");
            Expr scale = ReadInput(node, "Scale");
            PinType common = PromotePinTypes(input.type, bias.type);
            common = PromotePinTypes(common, scale.type);
            input = Coerce(input, common, "ConstantBiasScale.Input");
            bias = Coerce(bias, common, "ConstantBiasScale.Bias");
            scale = Coerce(scale, common, "ConstantBiasScale.Scale");
            return SingleOutput(node, EmitAssignment(
                node, common,
                "(" + input.code + " * " + scale.code + " + " + bias.code + ")"));
        }

        if (type == "SphereMask") {
            Expr a = Coerce(ReadInput(node, "A"), PinType::Vector3, "SphereMask.A");
            Expr b = Coerce(ReadInput(node, "B"), PinType::Vector3, "SphereMask.B");
            Expr radius = Coerce(ReadInput(node, "Radius"), PinType::Float, "SphereMask.Radius");
            Expr hardness = Coerce(ReadInput(node, "Hardness"), PinType::Float, "SphereMask.Hardness");
            const std::string distanceExpr = "distance(" + a.code + ", " + b.code + ")";
            const std::string falloff = "max(1.0 - (" + hardness.code + "), 0.0001)";
            return SingleOutput(node, EmitAssignment(
                node, PinType::Float,
                "(1.0 - clamp((" + distanceExpr + " - " + radius.code + ") / " + falloff +
                    ", 0.0, 1.0))"));
        }

        if (type == "Noise") {
            Expr position = Coerce(ReadInput(node, "Position"), PinType::Vector3, "Noise.Position");
            Expr filterWidth = Coerce(ReadInput(node, "Filter Width", Expr{"0.0", PinType::Float}),
                                      PinType::Float, "Noise.FilterWidth");
            const float scale = ReadFloatProperty(node, "scale", 1.0f);
            return SingleOutput(node, EmitAssignment(
                node, PinType::Float,
                "mf_valueNoise(" + position.code + " * " + FloatLiteral(scale) +
                    " + vec3(" + filterWidth.code + " * 0.01))"));
        }

        if (type == "DotProduct") {
            Expr a = ReadInput(node, "A");
            Expr b = ReadInput(node, "B");
            const PinType common = PromotePinTypes(a.type, b.type);
            a = Coerce(a, common, "DotProduct.A");
            b = Coerce(b, common, "DotProduct.B");
            const std::string expression =
                (common == PinType::Float) ? "(" + a.code + " * " + b.code + ")" :
                                             "dot(" + a.code + ", " + b.code + ")";
            return SingleOutput(node, EmitAssignment(node, PinType::Float, expression));
        }

        if (type == "CrossProduct") {
            Expr a = Coerce(ReadInput(node, "A"), PinType::Vector3, "CrossProduct.A");
            Expr b = Coerce(ReadInput(node, "B"), PinType::Vector3, "CrossProduct.B");
            return SingleOutput(node, EmitAssignment(
                node, PinType::Vector3, "cross(" + a.code + ", " + b.code + ")"));
        }

        if (type == "Normalize") {
            Expr input = ReadInput(node, "Input");
            if (!IsNumericPin(input.type) || input.type == PinType::Wildcard) {
                throw CodegenError("Normalize requires a numeric input.");
            }
            if (input.type == PinType::Float) {
                return SingleOutput(node, EmitAssignment(
                    node, PinType::Float,
                    "((" + input.code + ") >= 0.0 ? 1.0 : -1.0)"));
            }
            return SingleOutput(node, EmitAssignment(
                node, input.type, "normalize(" + input.code + ")"));
        }

        if (type == "Desaturation") {
            Expr color = Coerce(ReadInput(node, "Color"), PinType::Vector3, "Desaturation.Color");
            Expr fraction = Coerce(ReadInput(node, "Fraction"), PinType::Float, "Desaturation.Fraction");
            const std::string luminance = "vec3(dot(" + color.code + ", vec3(0.3, 0.59, 0.11)))";
            return SingleOutput(node, EmitAssignment(
                node, PinType::Vector3,
                "mix(" + color.code + ", " + luminance + ", clamp(" + fraction.code + ", 0.0, 1.0))"));
        }

        if (type == "Fresnel") {
            Expr normal = Coerce(ReadInput(node, "Normal", Expr{"mf_N", PinType::Vector3}),
                                 PinType::Vector3, "Fresnel.Normal");
            const float exponentProperty = ReadFloatProperty(node, "exponent", 5.0f);
            const float baseReflectProperty =
                ReadFloatProperty(node, "baseReflectFraction", 0.04f);
            Expr exponent = Coerce(
                ReadInput(node, "Exponent",
                          Expr{FloatLiteral(exponentProperty), PinType::Float}),
                PinType::Float, "Fresnel.Exponent");
            Expr baseReflect = Coerce(
                ReadInput(node, "Base Reflect Fraction",
                          Expr{FloatLiteral(baseReflectProperty), PinType::Float}),
                PinType::Float, "Fresnel.BaseReflectFraction");
            const std::string expression =
                "(" + baseReflect.code + " + (1.0 - " + baseReflect.code +
                ") * pow(1.0 - clamp(dot(" + normal.code + ", mf_V), 0.0, 1.0), max(" +
                exponent.code + ", 0.0001)))";
            return SingleOutput(node, EmitAssignment(node, PinType::Float, expression));
        }

        // ---- Coordinates ----------------------------------------------
        if (type == "TextureCoordinate") {
            const float uTiling = ReadFloatProperty(node, "uTiling", 1.0f);
            const float vTiling = ReadFloatProperty(node, "vTiling", 1.0f);
            return SingleOutput(node, EmitAssignment(
                node, PinType::Vector2,
                "vUV * vec2(" + FloatLiteral(uTiling) + ", " + FloatLiteral(vTiling) + ")"));
        }

        if (type == "Panner") {
            Expr coordinate = Coerce(ReadInput(node, "Coordinate", Expr{"vUV", PinType::Vector2}),
                                     PinType::Vector2, "Panner.Coordinate");
            Expr time = Coerce(ReadInput(node, "Time", Expr{"uTime", PinType::Float}),
                               PinType::Float, "Panner.Time");
            const Vec2 propertySpeed = ReadVec2Property(node, "speed", Vec2(0.05f, 0.02f));
            const std::string propertySpeedCode =
                "vec2(" + FloatLiteral(propertySpeed.x) + ", " +
                FloatLiteral(propertySpeed.y) + ")";
            Expr speed = Coerce(ReadInput(node, "Speed",
                                          Expr{propertySpeedCode, PinType::Vector2}),
                                PinType::Vector2, "Panner.Speed");
            Expr seed = Coerce(ReadInput(node, "Seed", Expr{"0.0", PinType::Float}),
                               PinType::Float, "Panner.Seed");
            const std::string expression =
                "(" + coordinate.code + " + (" + time.code + " + " + seed.code +
                " * 3.0) * " + speed.code + " + " + seed.code +
                " * vec2(13.17, 7.91))";
            return SingleOutput(node, EmitAssignment(node, PinType::Vector2, expression));
        }

        if (type == "Rotator") {
            Expr coordinate = Coerce(ReadInput(node, "Coordinate", Expr{"vUV", PinType::Vector2}),
                                     PinType::Vector2, "Rotator.Coordinate");
            Expr time = Coerce(ReadInput(node, "Time", Expr{"uTime", PinType::Float}),
                               PinType::Float, "Rotator.Time");
            const Vec2 propertyCenter = ReadVec2Property(node, "center", Vec2(0.5f, 0.5f));
            const float propertySpeed = ReadFloatProperty(node, "speed", 0.1f);
            const std::string centerCode =
                "vec2(" + FloatLiteral(propertyCenter.x) + ", " +
                FloatLiteral(propertyCenter.y) + ")";
            Expr center = Coerce(ReadInput(node, "Center",
                                           Expr{centerCode, PinType::Vector2}),
                                 PinType::Vector2, "Rotator.Center");
            Expr speed = Coerce(ReadInput(node, "Speed",
                                          Expr{FloatLiteral(propertySpeed), PinType::Float}),
                                PinType::Float, "Rotator.Speed");
            const std::string centerVar = VariableName(node, "_center");
            const std::string deltaVar = VariableName(node, "_delta");
            const std::string angleVar = VariableName(node, "_angle");
            m_Body << "    // " << type << " #" << node.id << "\n";
            m_Body << "    vec2 " << centerVar << " = " << center.code << ";\n";
            m_Body << "    vec2 " << deltaVar << " = " << coordinate.code << " - " << centerVar << ";\n";
            m_Body << "    float " << angleVar << " = " << time.code << " * " << speed.code << ";\n";
            const std::string expression =
                "vec2(cos(" + angleVar + ") * " + deltaVar + ".x - sin(" + angleVar + ") * " + deltaVar +
                ".y, sin(" + angleVar + ") * " + deltaVar + ".x + cos(" + angleVar + ") * " + deltaVar +
                ".y) + " + centerVar;
            return SingleOutput(node, EmitAssignment(node, PinType::Vector2, expression));
        }

        if (type == "Time") {
            return SingleOutput(node, EmitAssignment(node, PinType::Float, "uTime"));
        }
        if (type == "WorldPosition") {
            return SingleOutput(node, EmitAssignment(node, PinType::Vector3, "vWorldPos"));
        }
        if (type == "VertexNormal") {
            return SingleOutput(node, EmitAssignment(node, PinType::Vector3, "normalize(vNormal)"));
        }
        if (type == "CameraVector") {
            return SingleOutput(node, EmitAssignment(node, PinType::Vector3, "mf_V"));
        }
        if (type == "CameraPosition") {
            return SingleOutput(node, EmitAssignment(node, PinType::Vector3, "uCameraPos"));
        }
        if (type == "PixelDepth" || type == "SceneDepth") {
            return SingleOutput(node, EmitAssignment(
                node, PinType::Float, "distance(uCameraPos, vWorldPos)"));
        }
        if (type == "ObjectPosition" || type == "ActorPosition") {
            return SingleOutput(node, EmitAssignment(node, PinType::Vector3, "vec3(0.0)"));
        }
        if (type == "LightVector") {
            return SingleOutput(node, EmitAssignment(
                node, PinType::Vector3, "normalize(vec3(-0.45, 0.75, 0.55))"));
        }
        if (type == "ReflectionVector") {
            return SingleOutput(node, EmitAssignment(
                node, PinType::Vector3, "reflect(-mf_V, mf_N)"));
        }
        if (type == "ScreenPosition") {
            return SingleOutput(node, EmitAssignment(node, PinType::Vector2, "vUV"));
        }
        if (type == "ViewSize") {
            return SingleOutput(node, EmitAssignment(node, PinType::Vector2, "uViewSize"));
        }
        if (type == "VertexColor") {
            return SingleOutput(node, EmitAssignment(node, PinType::Vector4, "vec4(1.0)"));
        }
        if (type == "TwoSidedSign") {
            return SingleOutput(node, EmitAssignment(node, PinType::Float, "1.0"));
        }

        // ---- Textures --------------------------------------------------
        if (type == "BumpOffset") {
            Expr coordinate = ReadInput(node, "Coordinate", Expr{"vUV", PinType::Vector2});
            coordinate = Coerce(coordinate, PinType::Vector2, "BumpOffset.Coordinate");
            Expr height = Coerce(ReadInput(node, "Height"), PinType::Float, "BumpOffset.Height");
            Expr ratio = Coerce(ReadInput(node, "HeightRatio"), PinType::Float,
                                "BumpOffset.HeightRatio");
            Expr reference = Coerce(ReadInput(node, "ReferencePlane"), PinType::Float,
                                    "BumpOffset.ReferencePlane");
            const std::string offset =
                "vec2((" + height.code + " - " + reference.code + ") * " + ratio.code + ")";
            return SingleOutput(node, EmitAssignment(
                node, PinType::Vector2, "(" + coordinate.code + " + " + offset + ")"));
        }

        if (type == "SceneTexture") {
            Expr uv = ReadInput(node, "UVs", Expr{"vUV", PinType::Vector2});
            uv = Coerce(uv, PinType::Vector2, "SceneTexture.UVs");
            NodeEvaluation evaluation;
            evaluation.outputs["Color"] = EmitAssignment(
                node, PinType::Vector4, "vec4(" + uv.code + ", 0.5, 1.0)", "_color");
            evaluation.outputs["Size"] =
                EmitAssignment(node, PinType::Vector2, "uViewSize", "_size");
            evaluation.outputs["InvSize"] = EmitAssignment(
                node, PinType::Vector2,
                "(vec2(1.0) / max(uViewSize, vec2(1.0)))", "_invsize");
            return evaluation;
        }

        if (type == "Sampler") {
            m_Body << "    // Sampler #" << node.id << " settings are applied by the preview backend.\n";
            return SingleOutput(node, {"", PinType::Sampler});
        }

        if (type == "TextureObject" || type == "TextureObjectParameter") {
            const std::string path = ReadStringProperty(node, "texture");
            if (path.empty()) {
                m_Warnings.push_back(type + " #" + std::to_string(node.id) +
                                     " has no texture; using the fallback texture.");
                return SingleOutput(node, {"uDefaultTexture", PinType::Texture});
            }
            const std::string uniformName =
                "uTexture_" + m_UniformPrefix + std::to_string(node.id);
            m_Textures.push_back(
                {uniformName, node.id, path, ReadSamplerSettings(node)});
            return SingleOutput(node, {uniformName, PinType::Texture});
        }

        if (type == "TextureSample" || type == "TextureSampleParameter2D") {
            Expr uvs = Coerce(ReadInput(node, "UVs", Expr{"vUV", PinType::Vector2}),
                              PinType::Vector2, "TextureSample.UVs");
            Expr texture = ReadInput(node, "Texture", Expr{"uDefaultTexture", PinType::Texture});
            SamplerSettings samplerSettings = ReadSamplerSettings(node);
            const Pin* texturePin = node.FindInput("Texture");
            if (texturePin && !m_Graph.IsPinConnected(texturePin->id)) {
                const std::string texturePath = ReadStringProperty(node, "texture");
                if (!texturePath.empty()) {
                    const std::string uniformName =
                        "uTexture_" + m_UniformPrefix + "node" + std::to_string(node.id);
                    m_Textures.push_back(
                        {uniformName, node.id, texturePath, samplerSettings});
                    texture = {uniformName, PinType::Texture};
                }
            }

            const Pin* samplerPin = node.FindInput("Sampler");
            if (samplerPin) {
                if (const Link* samplerLink = m_Graph.FindInputLink(samplerPin->id)) {
                    if (const Pin* samplerSource = m_Graph.FindPin(samplerLink->fromPin)) {
                        if (const Node* samplerNode = m_Graph.FindNode(samplerSource->nodeId)) {
                            if (samplerNode->type == "Sampler") {
                                samplerSettings = ReadSamplerSettings(*samplerNode);
                                m_Body << "    // Sampler #" << samplerNode->id
                                       << " settings are applied by the preview backend.\n";
                            }
                        }
                    }
                }
            }
            for (ShaderTextureBinding& binding : m_Textures) {
                if (binding.uniformName == texture.code) {
                    binding.sampler = samplerSettings;
                }
            }

            if (texture.type != PinType::Texture) {
                throw CodegenError("TextureSample.Texture expected a Texture pin.");
            }
            const std::string var = VariableName(node);
            m_Body << "    // TextureSample #" << node.id << "\n";
            m_Body << "    vec4 " << var << " = texture(" << texture.code << ", " << uvs.code << ");\n";

            NodeEvaluation eval;
            eval.outputs["RGBA"] = {var, PinType::Vector4};
            eval.outputs["RGB"] = {var + ".rgb", PinType::Vector3};
            eval.outputs["R"] = {var + ".r", PinType::Float};
            eval.outputs["G"] = {var + ".g", PinType::Float};
            eval.outputs["B"] = {var + ".b", PinType::Float};
            eval.outputs["A"] = {var + ".a", PinType::Float};
            return eval;
        }

        throw CodegenError("No GLSL implementation for node type '" + type + "'.");
    }

    const MaterialGraph& m_Graph;
    TypeResolver m_Resolver;
    std::ostringstream m_Body;
    std::unordered_map<NodeId, NodeEvaluation> m_NodeCache;
    std::unordered_set<NodeId> m_Compiling;
    std::vector<ShaderParameterBinding> m_Parameters;
    std::vector<ShaderTextureBinding> m_Textures;
    std::vector<std::string> m_Warnings;
    std::set<std::string> m_ParameterNames;
    int m_EvaluatedNodeCount = 0;
    int m_GlslVersion = 330;
    std::filesystem::path m_BaseDirectory;
    std::unordered_map<std::string, Expr> m_FunctionInputs;
    std::shared_ptr<std::vector<std::string>> m_FunctionStack;
    std::string m_UniformPrefix;
    std::string m_VariablePrefix;
    PreviewBuffer m_PreviewBuffer = PreviewBuffer::Lit;
    PreviewLightingSettings m_Lighting;
};

} // namespace

GeneratedShader GlslGenerator::Generate(const MaterialGraph& graph, int glslVersion,
                                        const std::filesystem::path& baseDirectory,
                                        PreviewBuffer previewBuffer,
                                        const PreviewLightingSettings& lighting) const {
    GeneratedShader result;
    try {
        const Node* outputNode = nullptr;
        int outputCount = 0;
        for (const auto& node : graph.Nodes()) {
            if (node->IsMaterialOutput()) {
                outputNode = node.get();
                ++outputCount;
            }
        }
        if (outputCount == 0) {
            throw CodegenError("The graph has no Material Output node.");
        }
        if (outputCount > 1) {
            throw CodegenError("The graph has multiple Material Output nodes; exactly one is allowed.");
        }

        Compiler compiler(graph, glslVersion, baseDirectory, {}, {}, previewBuffer,
                          lighting);
        compiler.CompileMaterial(*outputNode);
        result.fragmentSource = compiler.FinishSource(graph);
        result.parameters = compiler.Parameters();
        result.textures = compiler.Textures();
        result.warnings = compiler.Warnings();
        result.evaluatedNodeCount = compiler.EvaluatedNodeCount();
        result.blendMode = graph.Settings().blendMode;
        result.shadingModel = graph.Settings().shadingModel;
        result.success = true;

        std::ostringstream diagnostic;
        diagnostic << "GLSL " << glslVersion << " generated successfully ("
                   << result.evaluatedNodeCount << " expressions, "
                   << result.parameters.size() << " parameters, " << result.textures.size()
                   << " textures).";
        result.diagnostic = diagnostic.str();
    } catch (const std::exception& error) {
        result.success = false;
        result.diagnostic = std::string("GLSL generation failed: ") + error.what();
        result.fragmentSource.clear();
        result.parameters.clear();
        result.textures.clear();
    }
    return result;
}

const char* GlslGenerator::PreviewVertexShaderSource() {
    return R"GLSL(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aTangent;
layout(location = 3) in vec2 aUV;

uniform mat4 uViewProj;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vTangent;
out vec2 vUV;

void main()
{
    vWorldPos = aPosition;
    vNormal = aNormal;
    vTangent = aTangent;
    vUV = aUV;
    gl_Position = uViewProj * vec4(aPosition, 1.0);
}
)GLSL";
}

const char* GlslGenerator::PreviewShaderInterfaceSummary() {
    return "Outputs: Base Color, Metallic, Specular, Roughness, Emissive Color, "
           "Normal (tangent space), Ambient Occlusion, Opacity, Opacity Mask, "
           "World Position Offset, Pixel Depth Offset, Refraction, Clear Coat, "
           "Clear Coat Roughness, Subsurface Color, Anisotropy.";
}

} // namespace mf
