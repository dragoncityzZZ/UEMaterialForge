#pragma once

#include "codegen/GlslGenerator.h"
#include "render/OpenGLPreviewRenderer.h"

#include <string>

namespace mf {

// Renders material spheres into standalone OpenGL textures for the Content
// Browser. It owns a private OpenGLPreviewRenderer so thumbnail rendering does
// not disturb the interactive preview backend.
class MaterialThumbnailRenderer {
public:
    ~MaterialThumbnailRenderer();

    bool Initialize(std::string* error = nullptr);
    void Shutdown();
    unsigned int Render(const MaterialGraph& graph, const GeneratedShader& shader,
                        int width, int height, int* outWidth = nullptr,
                        int* outHeight = nullptr, std::string* error = nullptr);

private:
    OpenGLPreviewRenderer m_Renderer;
    bool m_Initialized = false;
};

} // namespace mf
