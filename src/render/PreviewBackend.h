#pragma once

#include "codegen/GlslGenerator.h"
#include "core/Graph.h"
#include "render/PreviewTypes.h"

#include <string>

namespace mf {

// Common interface implemented by the OpenGL, DirectX 12 and Vulkan preview
// backends. Every backend renders into an OpenGL texture name so the ImGui
// editor shell can composite the result without caring which GPU API produced
// the pixels.
class PreviewBackend {
public:
    virtual ~PreviewBackend() = default;

    virtual bool Initialize(std::string* error = nullptr) = 0;
    virtual void Shutdown() = 0;

    virtual bool SetMaterial(const GeneratedShader& material, std::string* error = nullptr) = 0;
    virtual void Render(const MaterialGraph& graph, float timeSeconds, int width, int height) = 0;

    virtual void SetMesh(PreviewMesh mesh) = 0;
    virtual PreviewMesh GetMesh() const = 0;
    virtual bool LoadCustomMesh(const std::string& path, std::string* error = nullptr) {
        (void)path;
        if (error) *error = "Custom meshes are not supported by this preview backend.";
        return false;
    }
    virtual bool SetEnvironment(const PreviewEnvironmentSettings& settings,
                                std::string* error = nullptr) {
        (void)settings;
        if (error) *error = "Preview environments are not supported by this backend.";
        return false;
    }

    virtual void Orbit(float deltaX, float deltaY) = 0;
    virtual void Zoom(float wheelDelta) = 0;
    virtual PreviewCamera& Camera() = 0;
    virtual const PreviewCamera& Camera() const = 0;

    virtual unsigned int ColorTexture() const = 0;
    virtual int RenderWidth() const = 0;
    virtual int RenderHeight() const = 0;
    virtual bool ReadPixelTopLeft(int x, int y, float rgba[4]) const = 0;
    virtual bool HasMaterial() const = 0;
    virtual bool HasFramebuffer() const = 0;

    virtual const std::string& ShaderLog() const = 0;
    virtual const std::string& TextureDiagnostics() const = 0;
    virtual const char* MeshName() const = 0;

    virtual const char* BackendName() const = 0;
    virtual std::string BackendDetails() const = 0;
    virtual const std::string& LastError() const = 0;
};

} // namespace mf
