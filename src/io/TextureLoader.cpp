#include "io/TextureLoader.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace mf {

bool LoadImageRGBA(const std::string& path, ImageData& image, std::string* error) {
    // stb_image's file APIs use narrow paths and therefore cannot open files
    // in Unicode project directories on Windows. Read the bytes through
    // std::filesystem (which uses the native wide API) and decode from memory.
    std::ifstream stream(std::filesystem::u8path(path), std::ios::binary);
    if (!stream) {
        if (error) *error = "Could not open image file '" + path + "'.";
        return false;
    }
    std::vector<unsigned char> encoded((std::istreambuf_iterator<char>(stream)),
                                       std::istreambuf_iterator<char>());
    if (encoded.empty()) {
        if (error) *error = "Image file is empty: '" + path + "'.";
        return false;
    }
    if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        if (error) *error = "Image file is too large: '" + path + "'.";
        return false;
    }

    stbi_set_flip_vertically_on_load(1);
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                                            &width, &height, &channels, 4);
    if (!pixels) {
        if (error) {
            *error = "Could not decode image '" + path + "': " + stbi_failure_reason();
        }
        return false;
    }
    image.width = width;
    image.height = height;
    image.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    if (error) error->clear();
    return true;
}

Texture2D::~Texture2D() {
    Destroy();
}

Texture2D::Texture2D(Texture2D&& other) noexcept
    : m_Texture(other.m_Texture), m_Width(other.m_Width), m_Height(other.m_Height) {
    other.m_Texture = 0;
    other.m_Width = 0;
    other.m_Height = 0;
}

Texture2D& Texture2D::operator=(Texture2D&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_Texture = other.m_Texture;
        m_Width = other.m_Width;
        m_Height = other.m_Height;
        other.m_Texture = 0;
        other.m_Width = 0;
        other.m_Height = 0;
    }
    return *this;
}

void Texture2D::Destroy() {
    if (m_Texture != 0) {
        glDeleteTextures(1, &m_Texture);
        m_Texture = 0;
    }
    m_Width = 0;
    m_Height = 0;
}

Texture2D Texture2D::FromFile(const std::string& path, bool srgb, std::string* error) {
    Texture2D texture;
    ImageData image;
    if (!LoadImageRGBA(path, image, error)) {
        return texture;
    }
    texture.Upload(image, srgb);
    return texture;
}

Texture2D Texture2D::FromHdr(const std::string& path, std::string* error) {
    Texture2D texture;
    std::ifstream stream(std::filesystem::u8path(path), std::ios::binary);
    if (!stream) {
        if (error) *error = "Could not open HDR environment: " + path;
        return texture;
    }
    std::vector<unsigned char> encoded((std::istreambuf_iterator<char>(stream)),
                                       std::istreambuf_iterator<char>());
    if (encoded.empty()) {
        if (error) *error = "HDR environment is empty: " + path;
        return texture;
    }
    stbi_set_flip_vertically_on_load(1);
    int width = 0;
    int height = 0;
    int channels = 0;
    float* pixels = stbi_loadf_from_memory(encoded.data(),
                                           static_cast<int>(encoded.size()), &width,
                                           &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        if (error) {
            *error = "Could not decode HDR environment '" + path + "': " +
                     stbi_failure_reason();
        }
        stbi_image_free(pixels);
        return texture;
    }
    glGenTextures(1, &texture.m_Texture);
    glBindTexture(GL_TEXTURE_2D, texture.m_Texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA,
                 GL_FLOAT, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);
    texture.m_Width = width;
    texture.m_Height = height;
    if (error) error->clear();
    return texture;
}

Texture2D Texture2D::FromCheckerboard(int size) {
    ImageData image;
    size = std::max(2, size);
    image.width = size;
    image.height = size;
    image.rgba.resize(static_cast<std::size_t>(size) * size * 4);
    const int cell = std::max(1, size / 8);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool dark = ((x / cell) + (y / cell)) % 2 == 0;
            const std::uint8_t a = dark ? 36 : 210;
            const std::uint8_t b = dark ? 44 : 50;
            const std::size_t index = (static_cast<std::size_t>(y) * size + x) * 4;
            image.rgba[index + 0] = dark ? a : 235;
            image.rgba[index + 1] = dark ? b : 40;
            image.rgba[index + 2] = dark ? 58 : 160;
            image.rgba[index + 3] = 255;
        }
    }
    Texture2D texture;
    texture.Upload(image, false);
    return texture;
}

void Texture2D::Upload(const ImageData& image, bool srgb) {
    Destroy();
    if (image.width <= 0 || image.height <= 0 || image.rgba.empty()) {
        return;
    }

    glGenTextures(1, &m_Texture);
    glBindTexture(GL_TEXTURE_2D, m_Texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0,
                 srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                 image.width, image.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_Width = image.width;
    m_Height = image.height;
}

} // namespace mf
