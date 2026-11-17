#pragma once

#include <glad/gl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mf {

struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

bool LoadImageRGBA(const std::string& path, ImageData& image, std::string* error = nullptr);

class Texture2D {
public:
    Texture2D() = default;
    ~Texture2D();

    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;
    Texture2D(Texture2D&& other) noexcept;
    Texture2D& operator=(Texture2D&& other) noexcept;

    static Texture2D FromFile(const std::string& path, bool srgb = true, std::string* error = nullptr);
    static Texture2D FromHdr(const std::string& path, std::string* error = nullptr);
    static Texture2D FromCheckerboard(int size = 64);

    void Destroy();
    bool Valid() const { return m_Texture != 0; }
    GLuint Id() const { return m_Texture; }
    int Width() const { return m_Width; }
    int Height() const { return m_Height; }

private:
    void Upload(const ImageData& image, bool srgb);

    GLuint m_Texture = 0;
    int m_Width = 0;
    int m_Height = 0;
};

} // namespace mf

