#include "render/ShaderProgram.h"

#include <sstream>
#include <utility>
#include <vector>

namespace mf {
namespace {

std::string DescribeShaderLog(GLuint shader, const char* stage, const char* source) {
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(length > 1 ? length - 1 : 0), '\0');
    if (length > 1) {
        glGetShaderInfoLog(shader, length, nullptr, log.data());
    }

    std::ostringstream out;
    out << stage << " shader compilation failed:\n" << log << "\n--- Source ---\n";
    const std::string sourceText = source ? source : "";
    std::istringstream lines(sourceText);
    std::string line;
    int index = 1;
    while (std::getline(lines, line)) {
        out << index++ << ": " << line << '\n';
    }
    return out.str();
}

GLuint CompileOne(GLenum stage, const char* source, std::string* error, const char* label) {
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        if (error) {
            *error = DescribeShaderLog(shader, label, source);
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

ShaderProgram::~ShaderProgram() {
    Destroy();
}

ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept
    : m_Program(other.m_Program),
      m_Uniforms(std::move(other.m_Uniforms)),
      m_Log(std::move(other.m_Log)) {
    other.m_Program = 0;
}

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_Program = other.m_Program;
        m_Uniforms = std::move(other.m_Uniforms);
        m_Log = std::move(other.m_Log);
        other.m_Program = 0;
    }
    return *this;
}

void ShaderProgram::Destroy() {
    if (m_Program != 0) {
        glDeleteProgram(m_Program);
        m_Program = 0;
    }
    m_Uniforms.clear();
}

bool ShaderProgram::Compile(const char* vertexSource, const char* fragmentSource, std::string* error) {
    Destroy();
    m_Log.clear();

    std::string compileError;
    GLuint vertex = CompileOne(GL_VERTEX_SHADER, vertexSource, &compileError, "Vertex");
    if (vertex == 0) {
        m_Log = compileError;
        if (error) *error = m_Log;
        return false;
    }

    GLuint fragment = CompileOne(GL_FRAGMENT_SHADER, fragmentSource, &compileError, "Fragment");
    if (fragment == 0) {
        glDeleteShader(vertex);
        m_Log = compileError;
        if (error) *error = m_Log;
        return false;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(length > 1 ? length - 1 : 0), '\0');
        if (length > 1) {
            glGetProgramInfoLog(program, length, nullptr, log.data());
        }
        glDeleteProgram(program);
        m_Log = "Shader program link failed:\n" + log;
        if (error) *error = m_Log;
        return false;
    }

    GLint logLength = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength > 1) {
        std::string log(static_cast<std::size_t>(logLength - 1), '\0');
        glGetProgramInfoLog(program, logLength, nullptr, log.data());
        m_Log = log;
    }

    m_Program = program;
    if (error) error->clear();
    return true;
}

void ShaderProgram::Use() const {
    glUseProgram(m_Program);
}

GLint ShaderProgram::Uniform(const char* name) {
    if (!name || m_Program == 0) {
        return -1;
    }
    const std::string key(name);
    const auto it = m_Uniforms.find(key);
    if (it != m_Uniforms.end()) {
        return it->second;
    }
    const GLint location = glGetUniformLocation(m_Program, name);
    m_Uniforms.emplace(key, location);
    return location;
}

void ShaderProgram::SetFloat(const char* name, float value) {
    const GLint location = Uniform(name);
    if (location >= 0) glUniform1f(location, value);
}

void ShaderProgram::SetInt(const char* name, int value) {
    const GLint location = Uniform(name);
    if (location >= 0) glUniform1i(location, value);
}

void ShaderProgram::SetVec2(const char* name, float x, float y) {
    const GLint location = Uniform(name);
    if (location >= 0) glUniform2f(location, x, y);
}

void ShaderProgram::SetVec3(const char* name, float x, float y, float z) {
    const GLint location = Uniform(name);
    if (location >= 0) glUniform3f(location, x, y, z);
}

void ShaderProgram::SetVec4(const char* name, float x, float y, float z, float w) {
    const GLint location = Uniform(name);
    if (location >= 0) glUniform4f(location, x, y, z, w);
}

void ShaderProgram::SetMat4(const char* name, const float* values) {
    const GLint location = Uniform(name);
    if (location >= 0) glUniformMatrix4fv(location, 1, GL_FALSE, values);
}

} // namespace mf
