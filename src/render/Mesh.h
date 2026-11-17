#pragma once

#include "math/Math.h"
#include "render/PreviewTypes.h"

#include <glad/gl.h>

#include <cstdint>
#include <vector>

namespace mf {

struct PreviewVertex {
    Vec3 position{};
    Vec3 normal{};
    Vec3 tangent{};
    Vec2 uv{};
};

class Mesh {
public:
    Mesh() = default;
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    static Mesh CreateSphere(float radius = 1.05f, int segments = 96, int rings = 64);
    static Mesh CreatePlane(float halfExtent = 1.25f);
    static Mesh CreateGround(float halfExtent = 8.0f, float height = -1.05f);
    static Mesh CreateFromObj(const std::string& path, float targetRadius = 1.05f,
                              std::string* error = nullptr);

    void Draw() const;
    bool Valid() const { return m_VAO != 0; }
    GLsizei IndexCount() const { return m_IndexCount; }

private:
    void Upload(const std::vector<PreviewVertex>& vertices, const std::vector<std::uint32_t>& indices);
    void Destroy();

    GLuint m_VAO = 0;
    GLuint m_VBO = 0;
    GLuint m_EBO = 0;
    GLsizei m_IndexCount = 0;
};

} // namespace mf

