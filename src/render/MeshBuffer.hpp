#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL_opengl.h>

#include <vector>

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
     * Expects 9 floats per vertex: position (3), normal (3), color (3).
     * @param vertices Interleaved vertex data.
     * @param indices Triangle indices.
     * @return true on success, false if input is empty or upload fails.
     */
    bool upload(const std::vector<float>& vertices, const std::vector<unsigned int>& indices);
    /**
     * Draws the indexed mesh if the buffer is valid.
     */
    void draw() const;
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

private:
    /**
     * Deletes GL buffers and resets internal state.
     */
    void destroy();

    GLuint vao_{0};
    GLuint vbo_{0};
    GLuint ebo_{0};
    GLsizei indexCount_{0};
};

}  // namespace render
