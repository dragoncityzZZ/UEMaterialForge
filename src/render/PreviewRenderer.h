#pragma once

#include "codegen/GlslGenerator.h"
#include "core/Graph.h"
#include "render/OpenGLPreviewRenderer.h"
#include "render/PreviewBackend.h"
#include "render/PreviewTypes.h"
#include "render/RenderApi.h"

#include <memory>
#include <string>

namespace mf {

// Selects and owns the GPU backend that shades the live material preview.
// The ImGui editor shell stays on OpenGL; this facade keeps a GL texture name
// for every backend so the UI code does not change when the pipeline changes.
class PreviewRenderer {
public:
    PreviewRenderer();
    ~PreviewRenderer();

    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;

    bool Initialize(std::string* error = nullptr);
    void Shutdown();

    void SetPreferredBackend(RenderApi api) { m_Preferred = api; }
    RenderApi PreferredBackend() const { return m_Preferred; }
    RenderApi Backend() const { return m_Active; }
    bool IsInitialized() const { return m_Backend != nullptr; }

    // Activates a backend without falling back. If the new backend cannot be
    // initialized the previously active one is kept.
    bool SetBackend(RenderApi api, std::string* error = nullptr);

    // Checks whether a backend can be created on this machine and returns a
    // short human-readable description plus an explanatory error.
    static bool ProbeBackend(RenderApi api, std::string* details, std::string* error);

    bool SetMaterial(const GeneratedShader& material, std::string* error = nullptr);
    void Render(const MaterialGraph& graph, float timeSeconds, int width, int height);

    void SetMesh(PreviewMesh mesh);
    PreviewMesh GetMesh() const;
    bool LoadCustomMesh(const std::string& path, std::string* error = nullptr);
    bool SetEnvironment(const PreviewEnvironmentSettings& settings,
                        std::string* error = nullptr);

    void Orbit(float deltaX, float deltaY);
    void Zoom(float wheelDelta);
    PreviewCamera& Camera();
    const PreviewCamera& Camera() const;

    unsigned int ColorTexture() const;
    int RenderWidth() const;
    int RenderHeight() const;
    bool ProbePixel(float u, float v, float rgba[4]) const;
    bool HasMaterial() const;
    bool HasFramebuffer() const;

    const std::string& ShaderLog() const;
    const std::string& TextureDiagnostics() const;
    const char* MeshName() const;

    const char* BackendName() const;
    std::string BackendDetails() const;
    const std::string& LastBackendError() const { return m_LastError; }
    bool FellBackToOpenGL() const { return m_FellBack; }

private:
    std::unique_ptr<PreviewBackend> CreateBackend(RenderApi api) const;
    bool Activate(RenderApi api, std::string* error);

    RenderApi m_Preferred = RenderApi::OpenGL;
    RenderApi m_Active = RenderApi::OpenGL;
    std::unique_ptr<PreviewBackend> m_Backend;
    PreviewCamera m_FallbackCamera;
    PreviewMesh m_FallbackMesh = PreviewMesh::Sphere;

    GeneratedShader m_Material;
    bool m_HasMaterial = false;

    std::string m_LastError;
    bool m_FellBack = false;
};

} // namespace mf
