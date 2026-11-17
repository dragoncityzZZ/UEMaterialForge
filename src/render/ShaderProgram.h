#pragma once

#include <glad/gl.h>

#include <string>
#include <unordered_map>

namespace mf {

class ShaderProgram {
public:
    ShaderProgram() = default;
    ~ShaderProgram();

    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
    ShaderProgram(ShaderProgram&& other) noexcept;
    ShaderProgram& operator=(ShaderProgram&& other) noexcept;

    bool Compile(const char* vertexSource, const char* fragmentSource, std::string* error = nullptr);
    void Destroy();
    void Use() const;
    bool Valid() const { return m_Program != 0; }
    GLuint Id() const { return m_Program; }

    GLint Uniform(const char* name);
    void SetFloat(const char* name, float value);
    void SetInt(const char* name, int value);
    void SetVec2(const char* name, float x, float y);
    void SetVec3(const char* name, float x, float y, float z);
    void SetVec4(const char* name, float x, float y, float z, float w);
    void SetMat4(const char* name, const float* values);

    const std::string& LastLog() const { return m_Log; }

private:
    GLuint m_Program = 0;
    std::unordered_map<std::string, GLint> m_Uniforms;
    std::string m_Log;
};

} // namespace mf

