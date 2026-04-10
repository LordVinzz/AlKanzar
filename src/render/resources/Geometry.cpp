#include "Geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <glm/geometric.hpp>

namespace render {

namespace {

glm::vec3 fallbackTangent(const glm::vec3& normal) {
    const glm::vec3 up = std::abs(normal.y) < 0.999f
        ? glm::vec3(0.0f, 1.0f, 0.0f)
        : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 tangent = glm::cross(up, normal);
    const float lengthSq = glm::dot(tangent, tangent);
    if (lengthSq <= std::numeric_limits<float>::epsilon()) {
        return glm::vec3(1.0f, 0.0f, 0.0f);
    }
    return tangent / std::sqrt(lengthSq);
}

}  // namespace

void ensureMeshAttributeSizes(Mesh& mesh, std::size_t uvSetCount) {
    const std::size_t vertexCount = mesh.positions.size();
    mesh.normals.resize(vertexCount, glm::vec3(0.0f, 1.0f, 0.0f));
    mesh.tangents.resize(vertexCount, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    mesh.colors.resize(vertexCount, glm::vec4(1.0f));
    mesh.jointIndices.resize(vertexCount, glm::uvec4(0u));
    mesh.jointWeights.resize(vertexCount, glm::vec4(0.0f));

    if (mesh.uvSets.size() < uvSetCount) {
        mesh.uvSets.resize(uvSetCount);
    }

    for (auto& uvSet : mesh.uvSets) {
        uvSet.resize(vertexCount, glm::vec2(0.0f));
    }
}

void computeTangents(Mesh& mesh) {
    ensureMeshAttributeSizes(mesh, std::max<std::size_t>(mesh.uvSets.size(), 1));
    if (mesh.positions.empty()) {
        return;
    }

    std::vector<glm::vec3> tangentAccum(mesh.positions.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitangentAccum(mesh.positions.size(), glm::vec3(0.0f));
    const std::vector<glm::vec2>& uv0 = mesh.uvSets[0];

    for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
        const unsigned int i0 = mesh.indices[index + 0];
        const unsigned int i1 = mesh.indices[index + 1];
        const unsigned int i2 = mesh.indices[index + 2];
        if (i0 >= mesh.positions.size() || i1 >= mesh.positions.size() || i2 >= mesh.positions.size()) {
            continue;
        }

        const glm::vec3& p0 = mesh.positions[i0];
        const glm::vec3& p1 = mesh.positions[i1];
        const glm::vec3& p2 = mesh.positions[i2];

        const glm::vec2& w0 = uv0[i0];
        const glm::vec2& w1 = uv0[i1];
        const glm::vec2& w2 = uv0[i2];

        const glm::vec3 edge1 = p1 - p0;
        const glm::vec3 edge2 = p2 - p0;
        const glm::vec2 delta1 = w1 - w0;
        const glm::vec2 delta2 = w2 - w0;

        const float det = (delta1.x * delta2.y) - (delta1.y * delta2.x);
        if (std::abs(det) <= 1.0e-8f) {
            continue;
        }

        const float invDet = 1.0f / det;
        const glm::vec3 tangent = (edge1 * delta2.y - edge2 * delta1.y) * invDet;
        const glm::vec3 bitangent = (edge2 * delta1.x - edge1 * delta2.x) * invDet;

        tangentAccum[i0] += tangent;
        tangentAccum[i1] += tangent;
        tangentAccum[i2] += tangent;
        bitangentAccum[i0] += bitangent;
        bitangentAccum[i1] += bitangent;
        bitangentAccum[i2] += bitangent;
    }

    for (std::size_t vertexIndex = 0; vertexIndex < mesh.positions.size(); ++vertexIndex) {
        glm::vec3 normal = mesh.normals[vertexIndex];
        if (glm::dot(normal, normal) <= 1.0e-8f) {
            normal = glm::vec3(0.0f, 1.0f, 0.0f);
        } else {
            normal = glm::normalize(normal);
        }

        glm::vec3 tangent = tangentAccum[vertexIndex];
        if (glm::dot(tangent, tangent) <= 1.0e-8f) {
            tangent = fallbackTangent(normal);
            mesh.tangents[vertexIndex] = glm::vec4(glm::normalize(tangent), 1.0f);
            continue;
        }

        tangent = glm::normalize(tangent - normal * glm::dot(normal, tangent));
        glm::vec3 bitangent = bitangentAccum[vertexIndex];
        if (glm::dot(bitangent, bitangent) <= 1.0e-8f) {
            bitangent = glm::cross(normal, tangent);
        }

        const float handedness = glm::dot(glm::cross(normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
        mesh.tangents[vertexIndex] = glm::vec4(tangent, handedness);
    }
}

}  // namespace render
