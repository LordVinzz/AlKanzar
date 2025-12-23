#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL_opengl.h>

#include <string>

namespace render {

class ShaderProgram {
public:
    ShaderProgram() = default;
    ~ShaderProgram();

    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;

    bool buildFromSource(const std::string& vertexSrc, const std::string& fragmentSrc);
    void use() const;
    GLint uniformLocation(const char* name) const;
    GLuint id() const { return programId_; }

private:
    GLuint compile(GLenum type, const std::string& source);
    void destroy();

    GLuint programId_{0};
};

}  // namespace render
