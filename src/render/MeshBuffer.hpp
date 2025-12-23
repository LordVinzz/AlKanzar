#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL_opengl.h>

#include <vector>

namespace render {

class MeshBuffer {
public:
    MeshBuffer() = default;
    ~MeshBuffer();

    MeshBuffer(const MeshBuffer&) = delete;
    MeshBuffer& operator=(const MeshBuffer&) = delete;

    bool upload(const std::vector<float>& vertices, const std::vector<unsigned int>& indices);
    void draw() const;
    bool valid() const { return vao_ != 0 && vbo_ != 0 && ebo_ != 0 && indexCount_ > 0; }

private:
    void destroy();

    GLuint vao_{0};
    GLuint vbo_{0};
    GLuint ebo_{0};
    GLsizei indexCount_{0};
};

}  // namespace render
