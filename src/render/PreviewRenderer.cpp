#include "render/PreviewRenderer.h"

#include "render/OpenGLPreviewRenderer.h"

#if defined(_WIN32)
#include "render/D3D12PreviewRenderer.h"
#include "render/VulkanPreviewRenderer.h"
#endif

#include <algorithm>
#include <utility>

namespace mf {

PreviewRenderer::PreviewRenderer() = default;

PreviewRenderer::~PreviewRenderer() {
    Shutdown();
}

std::unique_ptr<PreviewBackend> PreviewRenderer::CreateBackend(RenderApi api) const {
    switch (api) {
    case RenderApi::OpenGL:
        return std::make_unique<OpenGLPreviewRenderer>();
#if defined(_WIN32)
    case RenderApi::DirectX12:
        return std::make_unique<D3D12PreviewRenderer>();
    case RenderApi::Vulkan:
        return std::make_unique<VulkanPreviewRenderer>();
#else
    case RenderApi::DirectX12:
    case RenderApi::Vulkan:
        return nullptr;
#endif
    default:
        return nullptr;
    }
}

bool PreviewRenderer::ProbeBackend(RenderApi api, std::string* details, std::string* error) {
    if (details) details->clear();
    if (error) error->clear();
    if (api == RenderApi::OpenGL) {
        if (details) {
            const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            *details = version && version[0] ? version : "OpenGL context active";
        }
        return true;
    }
    if (api == RenderApi::Vulkan) {
#if defined(_WIN32)
        std::string deviceName;
        std::string probeError;
        if (VulkanPreviewRenderer::Probe(&deviceName, &probeError)) {
            if (details) *details = deviceName;
            return true;
        }
        if (error) *error = probeError;
        return false;
#else
        if (error) *error = "Vulkan is only available on Windows in this build.";
        return false;
#endif
    }
    if (api == RenderApi::DirectX12) {
#if defined(_WIN32)
        std::string adapter;
        std::string probeError;
        if (D3D12PreviewRenderer::Probe(&adapter, &probeError)) {
            if (details) *details = adapter;
            return true;
        }
        if (error) *error = probeError;
        return false;
#else
        if (error) *error = "DirectX 12 is only available on Windows.";
        return false;
#endif
    }
    return false;
}

bool PreviewRenderer::Activate(RenderApi api, std::string* error) {
    std::unique_ptr<PreviewBackend> backend = CreateBackend(api);
    if (!backend) {
        const std::string message =
            std::string(RenderApiDisplayName(api)) + " backend is not available in this build.";
        if (error) *error = message;
        m_LastError = message;
        return false;
    }

    std::string initializeError;
    if (!backend->Initialize(&initializeError)) {
        if (error) {
            *error = std::string(RenderApiDisplayName(api)) +
                     " initialization failed: " + initializeError;
        }
        m_LastError = initializeError.empty() ? "initialization failed" : initializeError;
        return false;
    }

    // Re-apply the current material before publishing the new backend so a
    // failed shader compilation cannot leave the UI with a blank preview.
    if (m_HasMaterial) {
        std::string materialError;
        if (!backend->SetMaterial(m_Material, &materialError)) {
            m_LastError = materialError;
            // Keep the backend: it can still show its background while the user
            // fixes the graph. The caller sees the error through ShaderLog().
        }
    }

    m_Backend = std::move(backend);
    m_Active = api;
    if (error) error->clear();
    return true;
}

bool PreviewRenderer::Initialize(std::string* error) {
    m_LastError.clear();
    m_FellBack = false;
    if (m_Backend) {
        return true;
    }

    if (m_Preferred != RenderApi::OpenGL) {
        std::string preferredError;
        if (Activate(m_Preferred, &preferredError)) {
            if (error) error->clear();
            return true;
        }
        m_FellBack = true;
        m_LastError = preferredError;
        if (!Activate(RenderApi::OpenGL, error)) {
            return false;
        }
        // Keep the requested API so the preference is not silently rewritten;
        // the UI reports the fallback through FellBackToOpenGL().
        return true;
    }

    return Activate(RenderApi::OpenGL, error);
}

bool PreviewRenderer::SetBackend(RenderApi api, std::string* error) {
    if (m_Backend && api == m_Active) {
        m_Preferred = api;
        if (error) error->clear();
        return true;
    }
    if (!Activate(api, error)) {
        return false;
    }
    m_Preferred = api;
    m_FellBack = false;
    m_LastError.clear();
    return true;
}

void PreviewRenderer::Shutdown() {
    if (m_Backend) {
        m_Backend->Shutdown();
        m_Backend.reset();
    }
    m_HasMaterial = false;
    m_Material = GeneratedShader{};
}

bool PreviewRenderer::SetMaterial(const GeneratedShader& material, std::string* error) {
    m_Material = material;
    m_HasMaterial = true;
    if (!m_Backend) {
        if (error) *error = "The preview backend is not initialized.";
        return false;
    }
    const bool ok = m_Backend->SetMaterial(material, error);
    if (!ok) {
        m_LastError = m_Backend->ShaderLog();
    }
    return ok;
}

void PreviewRenderer::Render(const MaterialGraph& graph, float timeSeconds, int width, int height) {
    if (m_Backend) {
        m_Backend->Render(graph, timeSeconds, width, height);
    }
}

void PreviewRenderer::SetMesh(PreviewMesh mesh) {
    m_FallbackMesh = mesh;
    if (m_Backend) m_Backend->SetMesh(mesh);
}

PreviewMesh PreviewRenderer::GetMesh() const {
    return m_Backend ? m_Backend->GetMesh() : m_FallbackMesh;
}

bool PreviewRenderer::LoadCustomMesh(const std::string& path, std::string* error) {
    if (!m_Backend) {
        if (error) *error = "No preview backend is active.";
        return false;
    }
    return m_Backend->LoadCustomMesh(path, error);
}

bool PreviewRenderer::SetEnvironment(const PreviewEnvironmentSettings& settings,
                                     std::string* error) {
    if (!m_Backend) {
        if (error) *error = "No preview backend is active.";
        return false;
    }
    return m_Backend->SetEnvironment(settings, error);
}

void PreviewRenderer::Orbit(float deltaX, float deltaY) {
    if (m_Backend) m_Backend->Orbit(deltaX, deltaY);
}

void PreviewRenderer::Zoom(float wheelDelta) {
    if (m_Backend) m_Backend->Zoom(wheelDelta);
}

PreviewCamera& PreviewRenderer::Camera() {
    return m_Backend ? m_Backend->Camera() : m_FallbackCamera;
}

const PreviewCamera& PreviewRenderer::Camera() const {
    return m_Backend ? m_Backend->Camera() : m_FallbackCamera;
}

unsigned int PreviewRenderer::ColorTexture() const {
    return m_Backend ? m_Backend->ColorTexture() : 0;
}

int PreviewRenderer::RenderWidth() const {
    return m_Backend ? m_Backend->RenderWidth() : 0;
}

int PreviewRenderer::RenderHeight() const {
    return m_Backend ? m_Backend->RenderHeight() : 0;
}

bool PreviewRenderer::ProbePixel(float u, float v, float rgba[4]) const {
    if (!rgba || !m_Backend) return false;
    const int width = m_Backend->RenderWidth();
    const int height = m_Backend->RenderHeight();
    if (width <= 0 || height <= 0) return false;

    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const int x = std::clamp(static_cast<int>(u * width), 0, width - 1);
    const int y = std::clamp(static_cast<int>(v * height), 0, height - 1);
    return m_Backend->ReadPixelTopLeft(x, y, rgba);
}

bool PreviewRenderer::HasMaterial() const {
    return m_Backend ? m_Backend->HasMaterial() : false;
}

bool PreviewRenderer::HasFramebuffer() const {
    return m_Backend ? m_Backend->HasFramebuffer() : false;
}

const std::string& PreviewRenderer::ShaderLog() const {
    static const std::string kEmpty;
    return m_Backend ? m_Backend->ShaderLog() : kEmpty;
}

const std::string& PreviewRenderer::TextureDiagnostics() const {
    static const std::string kEmpty;
    return m_Backend ? m_Backend->TextureDiagnostics() : kEmpty;
}

const char* PreviewRenderer::MeshName() const {
    return m_Backend ? m_Backend->MeshName() : PreviewMeshName(m_FallbackMesh);
}

const char* PreviewRenderer::BackendName() const {
    return m_Backend ? m_Backend->BackendName() : "None";
}

std::string PreviewRenderer::BackendDetails() const {
    return m_Backend ? m_Backend->BackendDetails() : std::string();
}

} // namespace mf

