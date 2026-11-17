#include "render/Mesh.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace mf {

const char* PreviewMeshName(PreviewMesh mesh) {
    if (mesh == PreviewMesh::Plane) return "Plane";
    if (mesh == PreviewMesh::Custom) return "Custom";
    return "Sphere";
}

Mesh::~Mesh() {
    Destroy();
}

Mesh::Mesh(Mesh&& other) noexcept
    : m_VAO(other.m_VAO), m_VBO(other.m_VBO), m_EBO(other.m_EBO), m_IndexCount(other.m_IndexCount) {
    other.m_VAO = 0;
    other.m_VBO = 0;
    other.m_EBO = 0;
    other.m_IndexCount = 0;
}

Mesh& Mesh::operator=(Mesh&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_VAO = other.m_VAO;
        m_VBO = other.m_VBO;
        m_EBO = other.m_EBO;
        m_IndexCount = other.m_IndexCount;
        other.m_VAO = 0;
        other.m_VBO = 0;
        other.m_EBO = 0;
        other.m_IndexCount = 0;
    }
    return *this;
}

void Mesh::Destroy() {
    if (m_EBO) glDeleteBuffers(1, &m_EBO);
    if (m_VBO) glDeleteBuffers(1, &m_VBO);
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
    m_VAO = m_VBO = m_EBO = 0;
    m_IndexCount = 0;
}

void Mesh::Upload(const std::vector<PreviewVertex>& vertices, const std::vector<std::uint32_t>& indices) {
    Destroy();
    m_IndexCount = static_cast<GLsizei>(indices.size());

    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glGenBuffers(1, &m_EBO);

    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(PreviewVertex)),
                 vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)),
                 indices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(PreviewVertex),
                          reinterpret_cast<void*>(offsetof(PreviewVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(PreviewVertex),
                          reinterpret_cast<void*>(offsetof(PreviewVertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(PreviewVertex),
                          reinterpret_cast<void*>(offsetof(PreviewVertex, tangent)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(PreviewVertex),
                          reinterpret_cast<void*>(offsetof(PreviewVertex, uv)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

Mesh Mesh::CreateSphere(float radius, int segments, int rings) {
    Mesh mesh;
    segments = (std::max)(8, segments);
    rings = (std::max)(4, rings);

    std::vector<PreviewVertex> vertices;
    vertices.reserve(static_cast<std::size_t>((segments + 1) * (rings + 1)));
    for (int ring = 0; ring <= rings; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(rings);
        const float theta = v * kPi;
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);
        for (int segment = 0; segment <= segments; ++segment) {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float phi = u * 2.0f * kPi;
            const Vec3 normal(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
            const Vec3 tangent(-std::sin(phi), 0.0f, std::cos(phi));

            PreviewVertex vertex;
            vertex.position = normal * radius;
            vertex.normal = normal;
            vertex.tangent = tangent;
            vertex.uv = Vec2(u, 1.0f - v);
            vertices.push_back(vertex);
        }
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(segments * rings * 6));
    for (int ring = 0; ring < rings; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            const std::uint32_t a = static_cast<std::uint32_t>(ring * (segments + 1) + segment);
            const std::uint32_t b = a + 1;
            const std::uint32_t c = static_cast<std::uint32_t>((ring + 1) * (segments + 1) + segment);
            const std::uint32_t d = c + 1;
            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(b);
            indices.push_back(d);
            indices.push_back(c);
        }
    }

    mesh.Upload(vertices, indices);
    return mesh;
}

Mesh Mesh::CreatePlane(float halfExtent) {
    Mesh mesh;
    const float h = halfExtent;
    std::vector<PreviewVertex> vertices = {
        {{-h, -h, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{h, -h, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{h, h, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{-h, h, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}}
    };
    std::vector<std::uint32_t> indices = {0, 1, 2, 0, 2, 3};
    mesh.Upload(vertices, indices);
    return mesh;
}

Mesh Mesh::CreateGround(float halfExtent, float height) {
    Mesh mesh;
    const float h = halfExtent;
    std::vector<PreviewVertex> vertices = {
        {{-h, height, -h}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{h, height, -h}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{h, height, h}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{-h, height, h}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}}
    };
    std::vector<std::uint32_t> indices = {0, 2, 1, 0, 3, 2};
    mesh.Upload(vertices, indices);
    return mesh;
}

Mesh Mesh::CreateFromObj(const std::string& path, float targetRadius, std::string* error) {
    Mesh mesh;
    std::ifstream stream(std::filesystem::u8path(path), std::ios::binary);
    if (!stream) {
        if (error) *error = "Could not open OBJ file: " + path;
        return mesh;
    }

    std::vector<Vec3> positions;
    std::vector<Vec2> texcoords;
    std::vector<Vec3> normals;
    std::vector<PreviewVertex> vertices;
    std::vector<std::uint32_t> indices;
    struct Corner {
        int position = -1;
        int texcoord = -1;
        int normal = -1;
    };
    const auto resolveIndex = [](int value, std::size_t count) -> int {
        if (value > 0) return value - 1;
        if (value < 0) return static_cast<int>(count) + value;
        return -1;
    };
    const auto parseCorner = [&](const std::string& token) {
        Corner corner;
        int values[3] = {0, 0, 0};
        int count = 0;
        std::size_t start = 0;
        while (start <= token.size() && count < 3) {
            const std::size_t slash = token.find('/', start);
            const std::string part =
                token.substr(start, slash == std::string::npos ? std::string::npos
                                                               : slash - start);
            if (!part.empty()) values[count] = std::atoi(part.c_str());
            ++count;
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        corner.position = resolveIndex(values[0], positions.size());
        corner.texcoord = resolveIndex(values[1], texcoords.size());
        corner.normal = resolveIndex(values[2], normals.size());
        return corner;
    };

    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream lineStream(line);
        std::string tag;
        lineStream >> tag;
        if (tag == "v") {
            Vec3 position;
            lineStream >> position.x >> position.y >> position.z;
            positions.push_back(position);
        } else if (tag == "vt") {
            Vec2 uv;
            lineStream >> uv.x >> uv.y;
            texcoords.push_back(uv);
        } else if (tag == "vn") {
            Vec3 normal;
            lineStream >> normal.x >> normal.y >> normal.z;
            normals.push_back(normal);
        } else if (tag == "f") {
            std::vector<Corner> face;
            std::string token;
            while (lineStream >> token) {
                Corner corner = parseCorner(token);
                if (corner.position >= 0) face.push_back(corner);
            }
            if (face.size() < 3) continue;
            for (std::size_t i = 1; i + 1 < face.size(); ++i) {
                const Corner corners[3] = {face[0], face[i], face[i + 1]};
                const std::uint32_t base =
                    static_cast<std::uint32_t>(vertices.size());
                for (const Corner& corner : corners) {
                    PreviewVertex vertex;
                    if (corner.position >= 0 &&
                        static_cast<std::size_t>(corner.position) < positions.size()) {
                        vertex.position = positions[static_cast<std::size_t>(corner.position)];
                    }
                    if (corner.texcoord >= 0 &&
                        static_cast<std::size_t>(corner.texcoord) < texcoords.size()) {
                        vertex.uv = texcoords[static_cast<std::size_t>(corner.texcoord)];
                    }
                    if (corner.normal >= 0 &&
                        static_cast<std::size_t>(corner.normal) < normals.size()) {
                        vertex.normal = normals[static_cast<std::size_t>(corner.normal)];
                    }
                    vertex.tangent = Vec3(1.0f, 0.0f, 0.0f);
                    vertices.push_back(vertex);
                }
                indices.push_back(base);
                indices.push_back(base + 1);
                indices.push_back(base + 2);
            }
        }
    }
    if (vertices.empty() || indices.empty()) {
        if (error) *error = "OBJ file contains no triangle geometry: " + path;
        return mesh;
    }

    Vec3 minimum = vertices.front().position;
    Vec3 maximum = vertices.front().position;
    for (const PreviewVertex& vertex : vertices) {
        minimum.x = std::min(minimum.x, vertex.position.x);
        minimum.y = std::min(minimum.y, vertex.position.y);
        minimum.z = std::min(minimum.z, vertex.position.z);
        maximum.x = std::max(maximum.x, vertex.position.x);
        maximum.y = std::max(maximum.y, vertex.position.y);
        maximum.z = std::max(maximum.z, vertex.position.z);
    }
    const Vec3 center(
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f);
    const float extent = std::max({maximum.x - minimum.x, maximum.y - minimum.y,
                                   maximum.z - minimum.z, 1e-4f});
    const float scale = (targetRadius * 2.0f) / extent;

    for (PreviewVertex& vertex : vertices) {
        vertex.position = (vertex.position - center) * scale;
    }
    for (std::size_t i = 0; i < indices.size(); i += 3) {
        PreviewVertex& a = vertices[indices[i]];
        PreviewVertex& b = vertices[indices[i + 1]];
        PreviewVertex& c = vertices[indices[i + 2]];
        const Vec3 edge1 = b.position - a.position;
        const Vec3 edge2 = c.position - a.position;
        Vec3 faceNormal = Cross(edge1, edge2);
        const float normalLength = Length(faceNormal);
        if (normalLength > 1e-6f) faceNormal = faceNormal / normalLength;
        if (Length(a.normal) < 1e-5f) a.normal = faceNormal;
        if (Length(b.normal) < 1e-5f) b.normal = faceNormal;
        if (Length(c.normal) < 1e-5f) c.normal = faceNormal;
        const Vec2 duv1 = b.uv - a.uv;
        const Vec2 duv2 = c.uv - a.uv;
        const float det = duv1.x * duv2.y - duv1.y * duv2.x;
        Vec3 tangent = std::abs(det) > 1e-6f
                           ? (edge1 * duv2.y - edge2 * duv1.y) / det
                           : Vec3(1.0f, 0.0f, 0.0f);
        tangent = Normalize(tangent - faceNormal * Dot(faceNormal, tangent));
        if (Length(tangent) < 1e-5f) tangent = Vec3(1.0f, 0.0f, 0.0f);
        a.tangent = b.tangent = c.tangent = tangent;
    }
    for (PreviewVertex& vertex : vertices) {
        vertex.normal = Normalize(vertex.normal);
    }

    mesh.Upload(vertices, indices);
    if (error) error->clear();
    return mesh;
}

void Mesh::Draw() const {
    if (m_VAO == 0 || m_IndexCount == 0) {
        return;
    }
    glBindVertexArray(m_VAO);
    glDrawElements(GL_TRIANGLES, m_IndexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

} // namespace mf
