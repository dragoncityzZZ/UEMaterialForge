#pragma once

// Small dependency-free math layer used by the node graph data model and the
// preview renderer. Matrices are column-major, matching OpenGL's expectations.

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace mf {

constexpr float kPi = 3.14159265358979323846f;

inline float Radians(float degrees) { return degrees * (kPi / 180.0f); }
inline float Clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
    explicit Vec2(float s) : x(s), y(s) {}

    float* data() { return &x; }
    const float* data() const { return &x; }
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit Vec3(float s) : x(s), y(s), z(s) {}

    float* data() { return &x; }
    const float* data() const { return &x; }
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    explicit Vec4(float s) : x(s), y(s), z(s), w(s) {}
    explicit Vec4(const Vec3& v) : x(v.x), y(v.y), z(v.z), w(1.0f) {}

    Vec3 xyz() const { return {x, y, z}; }
    float* data() { return &x; }
    const float* data() const { return &x; }
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};

inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
inline Vec2 operator*(float s, Vec2 a) { return a * s; }
inline Vec2 operator*(Vec2 a, Vec2 b) { return {a.x * b.x, a.y * b.y}; }
inline Vec2 operator/(Vec2 a, float s) { return {a.x / s, a.y / s}; }

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return a * s; }
inline Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Vec3 operator/(Vec3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }

inline Vec4 operator+(Vec4 a, Vec4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline Vec4 operator-(Vec4 a, Vec4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline Vec4 operator*(Vec4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline Vec4 operator*(float s, Vec4 a) { return a * s; }

inline float Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Dot(Vec4 a, Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

inline Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float Length(Vec2 v) { return std::sqrt(Dot(v, v)); }
inline float Length(Vec3 v) { return std::sqrt(Dot(v, v)); }

inline Vec2 Normalize(Vec2 v) {
    const float len = Length(v);
    return len > 1e-8f ? v / len : Vec2(0.0f);
}
inline Vec3 Normalize(Vec3 v) {
    const float len = Length(v);
    return len > 1e-8f ? v * (1.0f / len) : Vec3(0.0f);
}

inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

// Column-major 4x4 matrix. Element [column][row] is stored as m[column * 4 + row].
struct Mat4 {
    float m[16] = {1, 0, 0, 0,
                   0, 1, 0, 0,
                   0, 0, 1, 0,
                   0, 0, 0, 1};

    static Mat4 Identity() { return {}; }

    static Mat4 Perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
        Mat4 out{};
        const float f = 1.0f / std::tan(fovYRadians * 0.5f);
        out.m[0] = f / aspect;
        out.m[5] = f;
        out.m[10] = (farZ + nearZ) / (nearZ - farZ);
        out.m[11] = -1.0f;
        out.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
        out.m[15] = 0.0f;
        return out;
    }

    static Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        const Vec3 f = Normalize(target - eye);
        const Vec3 s = Normalize(Cross(f, up));
        const Vec3 u = Cross(s, f);

        Mat4 out = Identity();
        out.m[0] = s.x;
        out.m[4] = s.y;
        out.m[8] = s.z;
        out.m[12] = -Dot(s, eye);

        out.m[1] = u.x;
        out.m[5] = u.y;
        out.m[9] = u.z;
        out.m[13] = -Dot(u, eye);

        out.m[2] = -f.x;
        out.m[6] = -f.y;
        out.m[10] = -f.z;
        out.m[14] = Dot(f, eye);
        return out;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + r] * b.m[c * 4 + k];
            }
            out.m[c * 4 + r] = sum;
        }
    }
    return out;
}

} // namespace mf
