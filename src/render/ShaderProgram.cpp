#include "ShaderProgram.hpp"

#include <spdlog/spdlog.h>

#include <vector>

namespace render {

ShaderProgram::~ShaderProgram() {
    destroy();
}

void ShaderProgram::destroy() {
    if (programId_ != 0) {
        glDeleteProgram(programId_);
        programId_ = 0;
    }
}

GLuint ShaderProgram::compile(GLenum type, const std::string& source) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        spdlog::error("ShaderProgram: glCreateShader failed");
        return 0;
    }

    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(static_cast<size_t>(logLength + 1), 0);
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        spdlog::error("ShaderProgram: shader compile error: {}", log.data());
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool ShaderProgram::buildFromSource(const std::string& vertexSrc, const std::string& fragmentSrc) {
    destroy();

    GLuint vs = compile(GL_VERTEX_SHADER, vertexSrc);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragmentSrc);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        return false;
    }

    programId_ = glCreateProgram();
    glAttachShader(programId_, vs);
    glAttachShader(programId_, fs);
    glLinkProgram(programId_);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(programId_, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint logLength = 0;
        glGetProgramiv(programId_, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(static_cast<size_t>(logLength + 1), 0);
        glGetProgramInfoLog(programId_, logLength, nullptr, log.data());
        spdlog::error("ShaderProgram: program link error: {}", log.data());
        destroy();
        return false;
    }

    return true;
}

bool ShaderProgram::buildComputeFromSource(const std::string& computeSrc) {
    destroy();

    GLuint cs = compile(GL_COMPUTE_SHADER, computeSrc);
    if (cs == 0) {
        return false;
    }

    programId_ = glCreateProgram();
    glAttachShader(programId_, cs);
    glLinkProgram(programId_);

    glDeleteShader(cs);

    GLint linked = 0;
    glGetProgramiv(programId_, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint logLength = 0;
        glGetProgramiv(programId_, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(static_cast<size_t>(logLength + 1), 0);
        glGetProgramInfoLog(programId_, logLength, nullptr, log.data());
        spdlog::error("ShaderProgram: compute program link error: {}", log.data());
        destroy();
        return false;
    }

    return true;
}

void ShaderProgram::use() const {
    glUseProgram(programId_);
}

GLint ShaderProgram::uniformLocation(const char* name) const {
    return glGetUniformLocation(programId_, name);
}

}  // namespace render
