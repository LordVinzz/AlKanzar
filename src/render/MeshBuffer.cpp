#include "MeshBuffer.hpp"

#include <array>
#include <vector>

#include <spdlog/spdlog.h>

namespace render {

MeshBuffer::~MeshBuffer() {
    destroy();
}

void MeshBuffer::destroy() {
    if (ebo_ != 0) {
        glDeleteBuffers(1, &ebo_);
        ebo_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    indexCount_ = 0;
}

bool MeshBuffer::upload(const Mesh& mesh) {
    destroy();

    if (mesh.empty()) {
        spdlog::error("MeshBuffer: empty vertex or index data");
        return false;
    }

    Mesh uploadMesh = mesh;
    ensureMeshAttributeSizes(uploadMesh, 2);
    computeTangents(uploadMesh);

    std::vector<float> vertices;
    vertices.reserve(uploadMesh.vertexCount() * 18);
    for (std::size_t vertexIndex = 0; vertexIndex < uploadMesh.vertexCount(); ++vertexIndex) {
        const glm::vec3& position = uploadMesh.positions[vertexIndex];
        const glm::vec3& normal = uploadMesh.normals[vertexIndex];
        const glm::vec4& tangent = uploadMesh.tangents[vertexIndex];
        const glm::vec2& uv0 = uploadMesh.uvSets[0][vertexIndex];
        const glm::vec2& uv1 = uploadMesh.uvSets[1][vertexIndex];
        const glm::vec4& color = uploadMesh.colors[vertexIndex];
        vertices.insert(vertices.end(), {
            position.x, position.y, position.z,
            normal.x, normal.y, normal.z,
            tangent.x, tangent.y, tangent.z, tangent.w,
            uv0.x, uv0.y,
            uv1.x, uv1.y,
            color.r, color.g, color.b, color.a
        });
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(uploadMesh.indices.size() * sizeof(unsigned int)),
        uploadMesh.indices.data(),
        GL_STATIC_DRAW
    );

    constexpr GLsizei stride = static_cast<GLsizei>(18 * sizeof(float));
    // Position
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    // Normal
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
    // Tangent
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));
    // UV0
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(10 * sizeof(float)));
    // UV1
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(12 * sizeof(float)));
    // Color
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(14 * sizeof(float)));

    glBindVertexArray(0);

    indexCount_ = static_cast<GLsizei>(uploadMesh.indices.size());
    return true;
}

void MeshBuffer::draw() const {
    if (!valid()) {
        return;
    }
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

void MeshBuffer::drawInstanced(GLsizei instanceCount) const {
    if (!valid() || instanceCount <= 0) {
        return;
    }
    glBindVertexArray(vao_);
    glDrawElementsInstanced(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr, instanceCount);
    glBindVertexArray(0);
}

}  // namespace render
