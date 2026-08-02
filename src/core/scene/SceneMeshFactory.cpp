#include "SceneMeshFactory.hpp"

#include <array>

#include <glm/common.hpp>

namespace core {
namespace {

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
    glm::vec4 color{1.0f};
};

void pushVertex(const Vertex& vertex, render::Mesh& mesh) {
    mesh.positions.push_back(vertex.position);
    mesh.normals.push_back(vertex.normal);
    mesh.colors.push_back(vertex.color);
    if (mesh.uvSets.size() < 2) {
        mesh.uvSets.resize(2);
    }
    mesh.uvSets[0].push_back(vertex.uv0);
    mesh.uvSets[1].push_back(vertex.uv1);
}

void addQuad(const std::array<Vertex, 4>& vertices, render::Mesh& mesh) {
    const unsigned int base = static_cast<unsigned int>(mesh.positions.size());
    for (const Vertex& vertex : vertices) {
        pushVertex(vertex, mesh);
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

}  // namespace

render::Mesh SceneMeshFactory::createBox(
    const glm::vec3& minCorner,
    const glm::vec3& maxCorner,
    const glm::vec4& color
) {
    const float minX = minCorner.x;
    const float minY = minCorner.y;
    const float minZ = minCorner.z;
    const float maxX = maxCorner.x;
    const float maxY = maxCorner.y;
    const float maxZ = maxCorner.z;
    render::Mesh mesh{};

    const std::array<std::array<Vertex, 4>, 6> faces{{
        {{{glm::vec3(maxX, minY, minZ), {1, 0, 0}, {0, 0}, {0, 0}, color}, {glm::vec3(maxX, maxY, minZ), {1, 0, 0}, {0, 1}, {0, 0}, color}, {glm::vec3(maxX, maxY, maxZ), {1, 0, 0}, {1, 1}, {0, 0}, color}, {glm::vec3(maxX, minY, maxZ), {1, 0, 0}, {1, 0}, {0, 0}, color}}},
        {{{glm::vec3(minX, minY, maxZ), {-1, 0, 0}, {1, 0}, {0, 0}, color}, {glm::vec3(minX, maxY, maxZ), {-1, 0, 0}, {1, 1}, {0, 0}, color}, {glm::vec3(minX, maxY, minZ), {-1, 0, 0}, {0, 1}, {0, 0}, color}, {glm::vec3(minX, minY, minZ), {-1, 0, 0}, {0, 0}, {0, 0}, color}}},
        {{{glm::vec3(minX, maxY, minZ), {0, 1, 0}, {0, 0}, {0, 0}, color}, {glm::vec3(minX, maxY, maxZ), {0, 1, 0}, {0, 1}, {0, 0}, color}, {glm::vec3(maxX, maxY, maxZ), {0, 1, 0}, {1, 1}, {0, 0}, color}, {glm::vec3(maxX, maxY, minZ), {0, 1, 0}, {1, 0}, {0, 0}, color}}},
        {{{glm::vec3(minX, minY, minZ), {0, -1, 0}, {0, 0}, {0, 0}, color}, {glm::vec3(maxX, minY, minZ), {0, -1, 0}, {1, 0}, {0, 0}, color}, {glm::vec3(maxX, minY, maxZ), {0, -1, 0}, {1, 1}, {0, 0}, color}, {glm::vec3(minX, minY, maxZ), {0, -1, 0}, {0, 1}, {0, 0}, color}}},
        {{{glm::vec3(minX, minY, maxZ), {0, 0, 1}, {0, 0}, {0, 0}, color}, {glm::vec3(maxX, minY, maxZ), {0, 0, 1}, {1, 0}, {0, 0}, color}, {glm::vec3(maxX, maxY, maxZ), {0, 0, 1}, {1, 1}, {0, 0}, color}, {glm::vec3(minX, maxY, maxZ), {0, 0, 1}, {0, 1}, {0, 0}, color}}},
        {{{glm::vec3(minX, minY, minZ), {0, 0, -1}, {1, 0}, {0, 0}, color}, {glm::vec3(minX, maxY, minZ), {0, 0, -1}, {1, 1}, {0, 0}, color}, {glm::vec3(maxX, maxY, minZ), {0, 0, -1}, {0, 1}, {0, 0}, color}, {glm::vec3(maxX, minY, minZ), {0, 0, -1}, {0, 0}, {0, 0}, color}}},
    }};
    for (const auto& face : faces) {
        addQuad(face, mesh);
    }
    return mesh;
}

render::Bounds3 SceneMeshFactory::computeBounds(const render::Mesh& mesh) {
    render::Bounds3 bounds{};
    if (mesh.positions.empty()) {
        return bounds;
    }
    bounds.min = mesh.positions.front();
    bounds.max = bounds.min;
    for (const glm::vec3& position : mesh.positions) {
        bounds.min = glm::min(bounds.min, position);
        bounds.max = glm::max(bounds.max, position);
    }
    return bounds;
}

}  // namespace core
