#include "MeshBuffer.hpp"

#include <array>
#include <cstddef>
#include <vector>

#include <glm/ext/vector_uint4.hpp>
#include <spdlog/spdlog.h>

namespace render {

namespace {

struct GpuVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
    glm::vec4 color{1.0f};
    glm::uvec4 joints{0u};
    glm::vec4 weights{0.0f};
};

}  // namespace

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
    vertexBufferBytes_ = 0u;
    indexBufferBytes_ = 0u;
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

    std::vector<GpuVertex> vertices;
    vertices.reserve(uploadMesh.vertexCount());
    for (std::size_t vertexIndex = 0; vertexIndex < uploadMesh.vertexCount(); ++vertexIndex) {
        GpuVertex vertex{};
        vertex.position = uploadMesh.positions[vertexIndex];
        vertex.normal = uploadMesh.normals[vertexIndex];
        vertex.tangent = uploadMesh.tangents[vertexIndex];
        vertex.uv0 = uploadMesh.uvSets[0][vertexIndex];
        vertex.uv1 = uploadMesh.uvSets[1][vertexIndex];
        vertex.color = uploadMesh.colors[vertexIndex];
        vertex.joints = uploadMesh.jointIndices[vertexIndex];
        vertex.weights = uploadMesh.jointWeights[vertexIndex];
        vertices.push_back(vertex);
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    vertexBufferBytes_ = vertices.size() * sizeof(GpuVertex);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertexBufferBytes_), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    indexBufferBytes_ = uploadMesh.indices.size() * sizeof(unsigned int);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(indexBufferBytes_),
        uploadMesh.indices.data(),
        GL_STATIC_DRAW
    );

    constexpr GLsizei stride = static_cast<GLsizei>(sizeof(GpuVertex));
    // Position
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(GpuVertex, position)));
    // Normal
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(GpuVertex, normal)));
    // Tangent
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(GpuVertex, tangent)));
    // UV0
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(GpuVertex, uv0)));
    // UV1
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(GpuVertex, uv1)));
    // Color
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(GpuVertex, color)));
    // JOINTS_0
    glEnableVertexAttribArray(6);
    glVertexAttribIPointer(6, 4, GL_UNSIGNED_INT, stride, reinterpret_cast<void*>(offsetof(GpuVertex, joints)));
    // WEIGHTS_0
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(GpuVertex, weights)));

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
