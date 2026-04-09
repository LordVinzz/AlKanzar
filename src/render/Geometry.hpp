#pragma once

#include <cstddef>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace render {

struct Mesh {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec4> tangents;
    std::vector<std::vector<glm::vec2>> uvSets;
    std::vector<glm::vec4> colors;
    std::vector<unsigned int> indices;

    [[nodiscard]] bool empty() const {
        return positions.empty() || indices.empty();
    }

    [[nodiscard]] std::size_t vertexCount() const {
        return positions.size();
    }
};

void ensureMeshAttributeSizes(Mesh& mesh, std::size_t uvSetCount = 2);
void computeTangents(Mesh& mesh);

}  // namespace render
