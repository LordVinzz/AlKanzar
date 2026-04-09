#include "ShaderProgram.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>
#include <vector>

namespace {

bool readFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) {
        spdlog::error("ShaderProgram: unable to open shader file: {}", path);
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    if (out.empty()) {
        spdlog::warn("ShaderProgram: shader file is empty: {}", path);
    }

    return true;
}

}  // namespace

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

bool ShaderProgram::buildFromFiles(const std::string& vertexPath, const std::string& fragmentPath) {
    std::string vertexSrc;
    std::string fragmentSrc;
    if (!readFile(vertexPath, vertexSrc) || !readFile(fragmentPath, fragmentSrc)) {
        destroy();
        return false;
    }
    if (!buildFromSource(vertexSrc, fragmentSrc)) {
        spdlog::error("ShaderProgram: failed to build program from {} and {}", vertexPath, fragmentPath);
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

bool ShaderProgram::buildComputeFromFile(const std::string& computePath) {
    std::string computeSrc;
    if (!readFile(computePath, computeSrc)) {
        destroy();
        return false;
    }
    if (!buildComputeFromSource(computeSrc)) {
        spdlog::error("ShaderProgram: failed to build compute program from {}", computePath);
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
