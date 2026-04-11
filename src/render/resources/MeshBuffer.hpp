#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL_opengl.h>

#include <cstddef>

#include "Geometry.hpp"
namespace render {

class MeshBuffer {
public:
    /**
     * Creates an empty mesh buffer wrapper without allocating GL objects.
     */
    MeshBuffer() = default;
    /**
     * Releases VAO/VBO/EBO resources if created.
     */
    ~MeshBuffer();

    /**
     * Non-copyable to avoid double-deleting GL buffers.
     */
    MeshBuffer(const MeshBuffer&) = delete;
    /**
     * Non-copyable assignment to avoid double-deleting GL buffers.
     */
    MeshBuffer& operator=(const MeshBuffer&) = delete;

    /**
     * Uploads vertex and index data and configures vertex attributes.
     * Expects positions, normals, tangents, colors, and up to two UV sets.
     * @param mesh Mesh data to upload.
     * @return true on success, false if input is empty or upload fails.
     */
    bool upload(const Mesh& mesh);
    /**
     * Draws the indexed mesh if the buffer is valid.
     */
    void draw() const;
    /**
     * Binds the mesh VAO for custom attribute setup or external draw orchestration.
     */
    void bind() const;
    /**
     * Unbinds the current mesh VAO.
     */
    static void unbind();
    /**
     * Draws the indexed mesh with instancing if valid and count > 0.
     * @param instanceCount Number of instances to render.
     */
    void drawInstanced(GLsizei instanceCount) const;
    /**
     * Checks whether GPU buffers and index data are available.
     * @return true if VAO/VBO/EBO are created and index count is non-zero.
     */
    bool valid() const { return vao_ != 0 && vbo_ != 0 && ebo_ != 0 && indexCount_ > 0; }
    [[nodiscard]] std::size_t vertexBufferBytes() const { return vertexBufferBytes_; }
    [[nodiscard]] std::size_t indexBufferBytes() const { return indexBufferBytes_; }
    [[nodiscard]] std::size_t gpuBytes() const { return vertexBufferBytes_ + indexBufferBytes_; }

private:
    /**
     * Deletes GL buffers and resets internal state.
     */
    void destroy();

    GLuint vao_{0};
    GLuint vbo_{0};
    GLuint ebo_{0};
    GLsizei indexCount_{0};
    std::size_t vertexBufferBytes_{0u};
    std::size_t indexBufferBytes_{0u};
};

}  // namespace render
