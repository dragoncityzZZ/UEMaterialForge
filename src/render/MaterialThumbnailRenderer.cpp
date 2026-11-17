#include "render/MaterialThumbnailRenderer.h"

#include <algorithm>
#include <vector>

namespace mf {

MaterialThumbnailRenderer::~MaterialThumbnailRenderer() {
    Shutdown();
}

bool MaterialThumbnailRenderer::Initialize(std::string* error) {
    if (m_Initialized) {
        if (error) error->clear();
        return true;
    }
    std::string initError;
    if (!m_Renderer.Initialize(&initError)) {
        if (error) *error = initError;
        return false;
    }
    m_Initialized = true;
    if (error) error->clear();
    return true;
}

void MaterialThumbnailRenderer::Shutdown() {
    if (m_Initialized) {
        m_Renderer.Shutdown();
        m_Initialized = false;
    }
}

unsigned int MaterialThumbnailRenderer::Render(const MaterialGraph& graph,
                                                const GeneratedShader& shader,
                                                int width, int height,
                                                int* outWidth, int* outHeight,
                                                std::string* error) {
    if (!m_Initialized && !Initialize(error)) {
        return 0;
    }
    width = std::clamp(width, 32, 512);
    height = std::clamp(height, 32, 512);
    m_Renderer.SetMesh(PreviewMesh::Sphere);
    std::string materialError;
    if (!m_Renderer.SetMaterial(shader, &materialError)) {
        if (error) *error = materialError;
        return 0;
    }
    m_Renderer.Render(graph, 0.0f, width, height);
    std::vector<unsigned char> pixels;
    int actualWidth = 0;
    int actualHeight = 0;
    if (!m_Renderer.ReadPixelsTopLeft(pixels, actualWidth, actualHeight) ||
        actualWidth <= 0 || actualHeight <= 0) {
        if (error) *error = "Could not read the thumbnail framebuffer.";
        return 0;
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, actualWidth, actualHeight, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (outWidth) *outWidth = actualWidth;
    if (outHeight) *outHeight = actualHeight;
    if (error) error->clear();
    return texture;
}

} // namespace mf
