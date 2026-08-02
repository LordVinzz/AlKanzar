#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "render/resources/Geometry.hpp"
#include "render/engine/RenderTypes.hpp"

namespace core {

// Factory for the procedural meshes used by scene blueprints.
class SceneMeshFactory final {
public:
    [[nodiscard]] static render::Mesh createBox(
        const glm::vec3& minCorner,
        const glm::vec3& maxCorner,
        const glm::vec4& color
    );

    [[nodiscard]] static render::Bounds3 computeBounds(const render::Mesh& mesh);
};

}  // namespace core
