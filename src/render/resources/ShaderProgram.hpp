#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL_opengl.h>

#include <string>

namespace render {

class ShaderProgram {
public:
    /**
     * Creates an empty shader program wrapper without allocating GL objects.
     */
    ShaderProgram() = default;
    /**
     * Releases the GL program if one was created.
     */
    ~ShaderProgram();

    /**
     * Non-copyable to avoid double-deleting GL program handles.
     */
    ShaderProgram(const ShaderProgram&) = delete;
    /**
     * Non-copyable assignment to avoid double-deleting GL program handles.
     */
    ShaderProgram& operator=(const ShaderProgram&) = delete;

    /**
     * Compiles and links a vertex/fragment program from source strings.
     * @param vertexSrc GLSL vertex shader source.
     * @param fragmentSrc GLSL fragment shader source.
     * @return true on successful compile/link, false otherwise.
     */
    bool buildFromSource(const std::string& vertexSrc, const std::string& fragmentSrc);
    /**
     * Loads vertex/fragment shaders from files and builds the program.
     * @param vertexPath Path to the vertex shader file.
     * @param fragmentPath Path to the fragment shader file.
     * @return true on success, false on load/compile/link failure.
     */
    bool buildFromFiles(const std::string& vertexPath, const std::string& fragmentPath);
    /**
     * Compiles and links a compute program from a source string.
     * @param computeSrc GLSL compute shader source.
     * @return true on successful compile/link, false otherwise.
     */
    bool buildComputeFromSource(const std::string& computeSrc);
    /**
     * Loads a compute shader from file and builds the program.
     * @param computePath Path to the compute shader file.
     * @return true on success, false on load/compile/link failure.
     */
    bool buildComputeFromFile(const std::string& computePath);
    /**
     * Binds this program for subsequent draw/dispatch calls.
     */
    void use() const;
    /**
     * Queries a uniform location by name.
     * @param name Uniform name in the program.
     * @return Uniform location, or -1 if not found.
     */
    GLint uniformLocation(const char* name) const;
    /**
     * Returns the underlying OpenGL program id.
     * @return Program object id, or 0 if not built.
     */
    GLuint id() const { return programId_; }

private:
    /**
     * Compiles a shader stage from source.
     * @param type GL shader type (e.g., GL_VERTEX_SHADER).
     * @param source GLSL source.
     * @return Shader object id, or 0 on failure.
     */
    GLuint compile(GLenum type, const std::string& source);
    /**
     * Deletes the program object and resets the id.
     */
    void destroy();

    GLuint programId_{0};
};

}  // namespace render
