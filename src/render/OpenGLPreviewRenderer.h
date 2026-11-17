#pragma once

#include "codegen/GlslGenerator.h"
#include "core/Graph.h"
#include "io/TextureLoader.h"
#include "render/Mesh.h"
#include "render/PreviewBackend.h"
#include "render/ShaderProgram.h"

#include <glad/gl.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mf {

// Reference preview backend. Renders the material on a sphere or plane with a
// generated GLSL fragment shader, MSAA and studio lighting.
class OpenGLPreviewRenderer final : public PreviewBackend {
public:
    OpenGLPreviewRenderer() = default;
    ~OpenGLPreviewRenderer() override;

    OpenGLPreviewRenderer(const OpenGLPreviewRenderer&) = delete;
    OpenGLPreviewRenderer& operator=(const OpenGLPreviewRenderer&) = delete;

    bool Initialize(std::string* error = nullptr) override;
    void Shutdown() override;

    // Updates the material program and texture bindings. The program is only
    // recompiled when the generated GLSL source actually changes.
    bool SetMaterial(const GeneratedShader& material, std::string* error = nullptr) override;

    void Render(const MaterialGraph& graph, float timeSeconds, int width, int height) override;

    void SetMesh(PreviewMesh mesh) override { m_Mesh = mesh; }
    PreviewMesh GetMesh() const override { return m_Mesh; }
    bool LoadCustomMesh(const std::string& path, std::string* error = nullptr) override;
    bool SetEnvironment(const PreviewEnvironmentSettings& settings,
                        std::string* error = nullptr) override;

    void Orbit(float deltaX, float deltaY) override;
    void Zoom(float wheelDelta) override;
    PreviewCamera& Camera() override { return m_Camera; }
    const PreviewCamera& Camera() const override { return m_Camera; }

    unsigned int ColorTexture() const override { return m_ResolveTexture; }
    int RenderWidth() const override { return m_Width; }
    int RenderHeight() const override { return m_Height; }
    bool ReadPixelTopLeft(int x, int y, float rgba[4]) const override;
    bool ReadPixelsTopLeft(std::vector<unsigned char>& rgba, int& width, int& height) const;

    bool HasMaterial() const override { return m_MaterialValid; }
    bool HasFramebuffer() const override { return m_ResolveFbo != 0; }
    const std::string& ShaderLog() const override { return m_ShaderLog; }
    const std::string& TextureDiagnostics() const override { return m_TextureDiagnostics; }
    const char* MeshName() const override { return PreviewMeshName(m_Mesh); }

    const char* BackendName() const override { return "OpenGL"; }
    std::string BackendDetails() const override;
    const std::string& LastError() const override { return m_LastError; }

private:
    bool InitializeGeometry(std::string* error);
    bool InitializeFramebuffers(std::string* error);
    bool EnsureSize(int width, int height);
    void DestroyFramebuffers();
    const Texture2D& GetOrLoadTexture(const std::string& path, bool srgb);
    void RenderBackground();
    void RenderGround();
    void RenderMesh(const MaterialGraph& graph, float timeSeconds);
    void ApplyTextureUniforms();

    GLuint m_MsaaFbo = 0;
    GLuint m_MsaaColor = 0;
    GLuint m_MsaaDepth = 0;
    GLuint m_ResolveFbo = 0;
    GLuint m_ResolveTexture = 0;
    int m_Width = 0;
    int m_Height = 0;
    int m_Samples = 4;

    GLuint m_FullscreenVao = 0;
    ShaderProgram m_BackgroundProgram;
    ShaderProgram m_PreviewProgram;
    ShaderProgram m_GroundProgram;

    Mesh m_Sphere;
    Mesh m_Plane;
    Mesh m_CustomMesh;
    Mesh m_Ground;
    PreviewMesh m_Mesh = PreviewMesh::Sphere;
    PreviewCamera m_Camera;

    PreviewEnvironmentSettings m_EnvironmentSettings;
    Texture2D m_EnvironmentTexture;
    std::string m_EnvironmentPath;
    bool m_HasEnvironment = false;
    float m_GroundHeight = -1.05f;

    std::string m_LastFragmentSource;
    bool m_MaterialValid = false;
    std::string m_ShaderLog;
    std::string m_TextureDiagnostics;
    std::string m_LastError;

    Texture2D m_FallbackTexture;
    std::unordered_map<std::string, Texture2D> m_TextureCache;
    std::unordered_set<std::string> m_FailedTextures;
    std::vector<ShaderTextureBinding> m_TextureBindings;
    std::vector<ShaderParameterBinding> m_ParameterBindings;
};

} // namespace mf

