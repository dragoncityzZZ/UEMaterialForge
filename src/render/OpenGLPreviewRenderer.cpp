#include "render/OpenGLPreviewRenderer.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace mf {
namespace {

const char* kBackgroundVertexShader = R"GLSL(#version 330 core
const vec2 kPositions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);
void main()
{
    gl_Position = vec4(kPositions[gl_VertexID], 0.0, 1.0);
}
)GLSL";

const char* kBackgroundFragmentShader = R"GLSL(#version 330 core
out vec4 FragColor;
uniform vec2 uResolution;
uniform sampler2D uEnvironment;
uniform int uHasEnvironment;
uniform float uExposure;
uniform float uEnvironmentRotation;
uniform vec3 uCameraForward;
uniform vec3 uCameraRight;
uniform vec3 uCameraUp;
uniform float uTanHalfFov;
uniform float uAspect;

void main()
{
    vec2 uv = gl_FragCoord.xy / uResolution;
    vec2 aspect = vec2(uResolution.x / max(uResolution.y, 1.0), 1.0);
    vec2 centered = (uv - vec2(0.5, 0.53)) * aspect;

    vec3 color;
    if (uHasEnvironment != 0) {
        vec2 ndc = uv * 2.0 - 1.0;
        vec3 direction = normalize(uCameraForward +
                                   uCameraRight * ndc.x * uAspect * uTanHalfFov +
                                   uCameraUp * ndc.y * uTanHalfFov);
        float longitude = atan(direction.z, direction.x) / 6.2831853 + 0.5 +
                          uEnvironmentRotation;
        float latitude = acos(clamp(direction.y, -1.0, 1.0)) / 3.14159265;
        color = texture(uEnvironment, vec2(fract(longitude), latitude)).rgb *
                uExposure;
    } else {
        vec3 bottom = vec3(0.004, 0.006, 0.010);
        vec3 top = vec3(0.028, 0.048, 0.065);
        color = mix(bottom, top, smoothstep(0.0, 1.0, uv.y));

        float halo = exp(-dot(centered, centered) * 3.0);
        color += vec3(0.020, 0.075, 0.082) * halo;
        color += vec3(0.015, 0.022, 0.027) *
                 exp(-dot(centered * vec2(0.6, 2.2), centered * vec2(0.6, 2.2)) * 5.0);

        float diagonal = smoothstep(0.55, 0.0, abs((uv.x * 0.7 + uv.y * 0.3) - 0.56));
        color += vec3(0.010, 0.016, 0.019) * diagonal;

        float vignette = smoothstep(1.35, 0.25, length(centered));
        color *= mix(0.62, 1.0, vignette);
    }

    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    color += (dither - 0.5) / 255.0;

    FragColor = vec4(color, 1.0);
}
)GLSL";

const char* kGroundVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPosition;
uniform mat4 uViewProj;
out vec3 vWorldPos;
void main()
{
    vWorldPos = aPosition;
    gl_Position = uViewProj * vec4(aPosition, 1.0);
}
)GLSL";

const char* kGroundFragmentShader = R"GLSL(#version 330 core
out vec4 FragColor;
in vec3 vWorldPos;
uniform vec3 uCameraPos;
uniform vec3 uGroundColor;
void main()
{
    vec2 delta = vWorldPos.xz - uCameraPos.xz;
    float distanceToCamera = length(delta);
    float fade = 1.0 - smoothstep(5.0, 11.0, distanceToCamera);
    vec2 grid = abs(fract(vWorldPos.xz * 0.5) - 0.5);
    float line = smoothstep(0.42, 0.5, max(grid.x, grid.y));
    float radial = smoothstep(0.0, 5.0, distanceToCamera);
    vec3 color = mix(uGroundColor * 0.65, uGroundColor, radial);
    color = mix(color, vec3(0.62, 0.78, 0.76), line * 0.45);
    FragColor = vec4(color, fade * 0.90);
}
)GLSL";

} // namespace

OpenGLPreviewRenderer::~OpenGLPreviewRenderer() {
    Shutdown();
}

bool OpenGLPreviewRenderer::Initialize(std::string* error) {
    if (m_FullscreenVao != 0) {
        return true;
    }

    glGenVertexArrays(1, &m_FullscreenVao);
    if (m_FullscreenVao == 0) {
        if (error) *error = "Could not create the preview fullscreen VAO.";
        return false;
    }

    m_FallbackTexture = Texture2D::FromCheckerboard(64);
    m_Sphere = Mesh::CreateSphere(1.05f, 96, 64);
    m_Plane = Mesh::CreatePlane(1.10f);
    if (!m_Sphere.Valid() || !m_Plane.Valid()) {
        if (error) *error = "Could not create preview meshes.";
        return false;
    }

    std::string shaderError;
    if (!m_BackgroundProgram.Compile(kBackgroundVertexShader, kBackgroundFragmentShader, &shaderError)) {
        if (error) *error = "Background shader failed: " + shaderError;
        return false;
    }
    if (!m_GroundProgram.Compile(kGroundVertexShader, kGroundFragmentShader,
                                 &shaderError)) {
        if (error) *error = "Ground shader failed: " + shaderError;
        return false;
    }

    GLint maxSamples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    m_Samples = std::max(1, std::min(4, maxSamples));

    if (error) error->clear();
    return true;
}

void OpenGLPreviewRenderer::Shutdown() {
    m_PreviewProgram.Destroy();
    m_BackgroundProgram.Destroy();
    m_GroundProgram.Destroy();
    if (m_FullscreenVao) {
        glDeleteVertexArrays(1, &m_FullscreenVao);
        m_FullscreenVao = 0;
    }
    m_Sphere = Mesh{};
    m_Plane = Mesh{};
    m_CustomMesh = Mesh{};
    m_Ground = Mesh{};
    m_EnvironmentTexture.Destroy();
    m_HasEnvironment = false;
    m_EnvironmentPath.clear();
    m_TextureCache.clear();
    m_FailedTextures.clear();
    m_FallbackTexture.Destroy();
    DestroyFramebuffers();
    m_MaterialValid = false;
    m_LastFragmentSource.clear();
}

bool OpenGLPreviewRenderer::ReadPixelsTopLeft(std::vector<unsigned char>& rgba,
                                              int& width, int& height) const {
    if (m_ResolveFbo == 0 || m_Width <= 0 || m_Height <= 0) return false;
    width = m_Width;
    height = m_Height;
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0);
    GLint previousReadFramebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ResolveFbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    std::vector<unsigned char> flipped(rgba.size(), 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4u;
    for (int y = 0; y < height; ++y) {
        std::copy_n(flipped.data() + static_cast<std::size_t>(y) * rowBytes, rowBytes,
                    rgba.data() + static_cast<std::size_t>(height - 1 - y) * rowBytes);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
    return true;
}

bool OpenGLPreviewRenderer::ReadPixelTopLeft(int x, int y, float rgba[4]) const {
    if (!rgba || m_ResolveFbo == 0 || m_Width <= 0 || m_Height <= 0) return false;
    if (x < 0 || x >= m_Width || y < 0 || y >= m_Height) return false;

    GLint previousReadFramebuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ResolveFbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    unsigned char pixel[4] = {0, 0, 0, 0};
    glReadPixels(x, m_Height - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    for (int i = 0; i < 4; ++i) {
        rgba[i] = static_cast<float>(pixel[i]) / 255.0f;
    }
    return true;
}

std::string OpenGLPreviewRenderer::BackendDetails() const {
    std::string details = "OpenGL";
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (version && version[0]) {
        details += " ";
        details += version;
    }
    if (renderer && renderer[0]) {
        details += " / ";
        details += renderer;
    }
    details += m_Samples > 1 ? " / 4x MSAA" : " / no MSAA";
    return details;
}

bool OpenGLPreviewRenderer::SetMaterial(const GeneratedShader& material, std::string* error) {
    m_TextureBindings = material.textures;
    m_ParameterBindings = material.parameters;
    m_TextureDiagnostics.clear();

    for (const ShaderTextureBinding& binding : m_TextureBindings) {
        GetOrLoadTexture(binding.assetPath, binding.sampler.srgb);
    }

    if (!material.success) {
        m_MaterialValid = false;
        m_ShaderLog = material.diagnostic;
        if (error) *error = m_ShaderLog;
        return false;
    }

    if (!m_PreviewProgram.Valid() || material.fragmentSource != m_LastFragmentSource) {
        std::string shaderError;
        if (!m_PreviewProgram.Compile(GlslGenerator::PreviewVertexShaderSource(),
                                      material.fragmentSource.c_str(), &shaderError)) {
            m_LastFragmentSource.clear();
            m_MaterialValid = false;
            m_ShaderLog = shaderError;
            if (error) *error = shaderError;
            return false;
        }
        m_LastFragmentSource = material.fragmentSource;
    }

    m_MaterialValid = true;
    m_ShaderLog = m_PreviewProgram.LastLog();
    if (error) error->clear();
    return true;
}

bool OpenGLPreviewRenderer::LoadCustomMesh(const std::string& path,
                                            std::string* error) {
    if (path.empty()) {
        if (error) *error = "The custom mesh path is empty.";
        return false;
    }
    std::string loadError;
    Mesh mesh = Mesh::CreateFromObj(path, 1.05f, &loadError);
    if (!mesh.Valid()) {
        if (error) *error = loadError;
        return false;
    }
    m_CustomMesh = std::move(mesh);
    m_Mesh = PreviewMesh::Custom;
    if (error) error->clear();
    return true;
}

bool OpenGLPreviewRenderer::SetEnvironment(const PreviewEnvironmentSettings& settings,
                                           std::string* error) {
    m_EnvironmentSettings = settings;
    m_GroundHeight = settings.groundHeight;
    if (!settings.hdriPath.empty()) {
        if (m_EnvironmentPath != settings.hdriPath || !m_EnvironmentTexture.Valid()) {
            std::string loadError;
            Texture2D texture = Texture2D::FromHdr(settings.hdriPath, &loadError);
            if (!texture.Valid()) {
                m_EnvironmentTexture.Destroy();
                m_EnvironmentPath.clear();
                m_HasEnvironment = false;
                if (error) *error = loadError;
                return false;
            }
            m_EnvironmentTexture = std::move(texture);
            m_EnvironmentPath = settings.hdriPath;
        }
        m_HasEnvironment = true;
    } else {
        m_EnvironmentTexture.Destroy();
        m_EnvironmentPath.clear();
        m_HasEnvironment = false;
    }
    if (settings.groundEnabled) {
        m_Ground = Mesh::CreateGround(8.0f, settings.groundHeight);
    } else {
        m_Ground = Mesh{};
    }
    if (error) error->clear();
    return true;
}

const Texture2D& OpenGLPreviewRenderer::GetOrLoadTexture(const std::string& path,
                                                          bool srgb) {
    if (path.empty()) {
        return m_FallbackTexture;
    }
    const std::string cacheKey = path + (srgb ? "#srgb" : "#linear");
    if (const auto cached = m_TextureCache.find(cacheKey); cached != m_TextureCache.end()) {
        return cached->second;
    }
    if (m_FailedTextures.find(cacheKey) != m_FailedTextures.end()) {
        return m_FallbackTexture;
    }

    std::string loadError;
    Texture2D texture = Texture2D::FromFile(path, srgb, &loadError);
    if (!texture.Valid()) {
        m_FailedTextures.insert(cacheKey);
        if (!m_TextureDiagnostics.empty()) m_TextureDiagnostics += "\n";
        m_TextureDiagnostics += loadError;
        return m_FallbackTexture;
    }
    const auto inserted = m_TextureCache.emplace(cacheKey, std::move(texture));
    return inserted.first->second;
}

void OpenGLPreviewRenderer::ApplyTextureUniforms() {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_FallbackTexture.Id());
    m_PreviewProgram.SetInt("uDefaultTexture", 0);

    const auto applySampler = [](const SamplerSettings& sampler) {
        const auto addressMode = [](const std::string& value) {
            if (value == "Clamp") return GL_CLAMP_TO_EDGE;
            if (value == "Mirror") return GL_MIRRORED_REPEAT;
            return GL_REPEAT;
        };
        GLint minFilter = GL_LINEAR_MIPMAP_LINEAR;
        GLint magFilter = GL_LINEAR;
        if (sampler.filter == "Point") {
            minFilter = GL_NEAREST;
            magFilter = GL_NEAREST;
        } else if (sampler.filter == "Linear") {
            minFilter = GL_LINEAR_MIPMAP_LINEAR;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, addressMode(sampler.addressU));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, addressMode(sampler.addressV));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, sampler.mipBias);
        if (sampler.filter == "Anisotropic") {
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                            std::clamp(sampler.maxAnisotropy, 1.0f, 16.0f));
        } else {
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 1.0f);
        }
    };

    GLint maxUnits = 16;
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxUnits);
    for (std::size_t i = 0; i < m_TextureBindings.size(); ++i) {
        const ShaderTextureBinding& binding = m_TextureBindings[i];
        const Texture2D& texture =
            GetOrLoadTexture(binding.assetPath, binding.sampler.srgb);
        const int unit = 1 + static_cast<int>(i);
        if (unit >= maxUnits) {
            m_PreviewProgram.SetInt(binding.uniformName.c_str(), 0);
            continue;
        }
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, texture.Valid() ? texture.Id() : m_FallbackTexture.Id());
        applySampler(binding.sampler);
        m_PreviewProgram.SetInt(binding.uniformName.c_str(), unit);
    }
    glActiveTexture(GL_TEXTURE0);
}

void OpenGLPreviewRenderer::Render(const MaterialGraph& graph, float timeSeconds, int width, int height) {
    if (width < 2 || height < 2 || m_FullscreenVao == 0) {
        return;
    }
    if (!EnsureSize(width, height)) {
        return;
    }

    GLint previousFbo = 0;
    GLint previousViewport[4] = {0, 0, 0, 0};
    GLint previousProgram = 0;
    GLint previousVao = 0;
    GLint previousActiveTexture = GL_TEXTURE0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    const GLboolean previousDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previousCull = glIsEnabled(GL_CULL_FACE);
    const GLboolean previousBlend = glIsEnabled(GL_BLEND);
    const GLboolean previousScissor = glIsEnabled(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, m_MsaaFbo);
    glViewport(0, 0, m_Width, m_Height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.004f, 0.006f, 0.010f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    RenderBackground();
    glClear(GL_DEPTH_BUFFER_BIT);
    RenderGround();

    if (m_MaterialValid && m_PreviewProgram.Valid()) {
        RenderMesh(graph, timeSeconds);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_MsaaFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ResolveFbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0, 0, m_Width, m_Height, 0, 0, m_Width, m_Height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    glUseProgram(static_cast<GLuint>(previousProgram));
    glBindVertexArray(static_cast<GLuint>(previousVao));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    if (previousDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (previousCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (previousBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (previousScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

void OpenGLPreviewRenderer::RenderBackground() {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    const float aspect =
        static_cast<float>(m_Width) / static_cast<float>(std::max(m_Height, 1));
    const Vec3 eye(
        m_Camera.distance * std::cos(m_Camera.pitch) * std::sin(m_Camera.yaw),
        m_Camera.distance * std::sin(m_Camera.pitch),
        m_Camera.distance * std::cos(m_Camera.pitch) * std::cos(m_Camera.yaw));
    const Vec3 forward = Normalize(-eye);
    const Vec3 right = Normalize(Cross(forward, Vec3(0.0f, 1.0f, 0.0f)));
    const Vec3 up = Cross(right, forward);

    m_BackgroundProgram.Use();
    m_BackgroundProgram.SetVec2("uResolution", static_cast<float>(m_Width),
                                static_cast<float>(m_Height));
    m_BackgroundProgram.SetVec3("uCameraForward", forward.x, forward.y, forward.z);
    m_BackgroundProgram.SetVec3("uCameraRight", right.x, right.y, right.z);
    m_BackgroundProgram.SetVec3("uCameraUp", up.x, up.y, up.z);
    m_BackgroundProgram.SetFloat("uTanHalfFov", std::tan(Radians(20.0f)));
    m_BackgroundProgram.SetFloat("uAspect", aspect);
    m_BackgroundProgram.SetFloat("uExposure", m_EnvironmentSettings.exposure);
    m_BackgroundProgram.SetFloat("uEnvironmentRotation",
                                 m_EnvironmentSettings.environmentRotation);
    m_BackgroundProgram.SetInt("uHasEnvironment", m_HasEnvironment ? 1 : 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_HasEnvironment && m_EnvironmentTexture.Valid()
                                    ? m_EnvironmentTexture.Id()
                                    : m_FallbackTexture.Id());
    m_BackgroundProgram.SetInt("uEnvironment", 2);
    glBindVertexArray(m_FullscreenVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void OpenGLPreviewRenderer::RenderGround() {
    if (!m_Ground.Valid() || !m_EnvironmentSettings.groundEnabled) return;

    const Vec3 eye(
        m_Camera.distance * std::cos(m_Camera.pitch) * std::sin(m_Camera.yaw),
        m_Camera.distance * std::sin(m_Camera.pitch),
        m_Camera.distance * std::cos(m_Camera.pitch) * std::cos(m_Camera.yaw));
    const Mat4 view = Mat4::LookAt(eye, Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
    const float aspect =
        static_cast<float>(m_Width) / static_cast<float>(std::max(m_Height, 1));
    const Mat4 projection = Mat4::Perspective(Radians(40.0f), aspect, 0.05f, 100.0f);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_GroundProgram.Use();
    m_GroundProgram.SetMat4("uViewProj", (projection * view).m);
    m_GroundProgram.SetVec3("uCameraPos", eye.x, eye.y, eye.z);
    m_GroundProgram.SetVec3("uGroundColor", 0.16f, 0.22f, 0.23f);
    m_Ground.Draw();

    glDepthMask(GL_TRUE);
}

void OpenGLPreviewRenderer::RenderMesh(const MaterialGraph& graph, float timeSeconds) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    const std::string& blendMode = graph.Settings().blendMode;
    if (blendMode == "Opaque") {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        if (blendMode == "Additive") {
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
        } else if (blendMode == "Modulate") {
            glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
        } else {
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                GL_ONE_MINUS_SRC_ALPHA);
        }
    }
    if (graph.Settings().twoSided) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
    }

    const float aspect = static_cast<float>(m_Width) / static_cast<float>(std::max(m_Height, 1));
    const Vec3 target(0.0f, 0.0f, 0.0f);
    const Vec3 eye(
        m_Camera.distance * std::cos(m_Camera.pitch) * std::sin(m_Camera.yaw),
        m_Camera.distance * std::sin(m_Camera.pitch),
        m_Camera.distance * std::cos(m_Camera.pitch) * std::cos(m_Camera.yaw));
    const Mat4 view = Mat4::LookAt(eye, target, Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 projection = Mat4::Perspective(Radians(40.0f), aspect, 0.05f, 100.0f);
    const Mat4 viewProjection = projection * view;

    m_PreviewProgram.Use();
    m_PreviewProgram.SetMat4("uViewProj", viewProjection.m);
    m_PreviewProgram.SetVec3("uCameraPos", eye.x, eye.y, eye.z);
    m_PreviewProgram.SetFloat("uTime", timeSeconds);
    m_PreviewProgram.SetVec2("uViewSize", static_cast<float>(m_Width),
                             static_cast<float>(m_Height));

    for (const ShaderParameterBinding& parameter : m_ParameterBindings) {
        PropertyValue value = parameter.defaultValue;
        const Node* node = graph.FindNode(parameter.nodeId);
        if (node) {
            const auto valueIt = node->properties.find("value");
            if (valueIt != node->properties.end()) {
                value = valueIt->second;
            }
        }
        if (parameter.type == PinType::Float) {
            m_PreviewProgram.SetFloat(parameter.uniformName.c_str(), value.number);
        } else if (parameter.type == PinType::Vector4) {
            m_PreviewProgram.SetVec4(parameter.uniformName.c_str(),
                                     value.vec4.x, value.vec4.y, value.vec4.z, value.vec4.w);
        }
    }

    ApplyTextureUniforms();

    if (m_Mesh == PreviewMesh::Plane) {
        m_Plane.Draw();
    } else if (m_Mesh == PreviewMesh::Custom && m_CustomMesh.Valid()) {
        m_CustomMesh.Draw();
    } else {
        m_Sphere.Draw();
    }
    glBindVertexArray(0);
}

void OpenGLPreviewRenderer::Orbit(float deltaX, float deltaY) {
    m_Camera.yaw -= deltaX * 0.008f;
    m_Camera.pitch = Clamp(m_Camera.pitch + deltaY * 0.008f, -1.35f, 1.35f);
}

void OpenGLPreviewRenderer::Zoom(float wheelDelta) {
    m_Camera.distance = Clamp(m_Camera.distance - wheelDelta * 0.25f, 1.8f, 7.5f);
}

bool OpenGLPreviewRenderer::InitializeFramebuffers(std::string* error) {
    DestroyFramebuffers();
    if (m_Width <= 0 || m_Height <= 0) {
        return false;
    }

    glGenFramebuffers(1, &m_MsaaFbo);
    glGenRenderbuffers(1, &m_MsaaColor);
    glGenRenderbuffers(1, &m_MsaaDepth);
    glBindFramebuffer(GL_FRAMEBUFFER, m_MsaaFbo);

    glBindRenderbuffer(GL_RENDERBUFFER, m_MsaaColor);
    if (m_Samples > 1) {
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_Samples, GL_RGBA8, m_Width, m_Height);
    } else {
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, m_Width, m_Height);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_MsaaColor);

    glBindRenderbuffer(GL_RENDERBUFFER, m_MsaaDepth);
    if (m_Samples > 1) {
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_Samples, GL_DEPTH_COMPONENT24, m_Width, m_Height);
    } else {
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_Width, m_Height);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_MsaaDepth);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        DestroyFramebuffers();
        if (error) *error = "Multisample preview framebuffer is incomplete.";
        return false;
    }

    glGenFramebuffers(1, &m_ResolveFbo);
    glGenTextures(1, &m_ResolveTexture);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ResolveFbo);
    glBindTexture(GL_TEXTURE_2D, m_ResolveTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_Width, m_Height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ResolveTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        DestroyFramebuffers();
        if (error) *error = "Preview resolve framebuffer is incomplete.";
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

bool OpenGLPreviewRenderer::EnsureSize(int width, int height) {
    if (width == m_Width && height == m_Height && m_MsaaFbo != 0) {
        return true;
    }
    m_Width = std::max(2, width);
    m_Height = std::max(2, height);
    return InitializeFramebuffers(nullptr);
}

void OpenGLPreviewRenderer::DestroyFramebuffers() {
    if (m_MsaaFbo) glDeleteFramebuffers(1, &m_MsaaFbo);
    if (m_MsaaColor) glDeleteRenderbuffers(1, &m_MsaaColor);
    if (m_MsaaDepth) glDeleteRenderbuffers(1, &m_MsaaDepth);
    if (m_ResolveFbo) glDeleteFramebuffers(1, &m_ResolveFbo);
    if (m_ResolveTexture) glDeleteTextures(1, &m_ResolveTexture);
    m_MsaaFbo = 0;
    m_MsaaColor = 0;
    m_MsaaDepth = 0;
    m_ResolveFbo = 0;
    m_ResolveTexture = 0;
}

} // namespace mf

