#pragma once

#include <array>

#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "core/ecs/Components.hpp"

namespace core {

struct OrientedBox {
    glm::vec3 center{0.0f};
    std::array<glm::vec3, 3u> axes{{
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
    }};
    glm::vec3 halfExtents{0.5f};
    std::array<glm::vec3, 8u> corners{};
    render::Bounds3 aabb{};
    glm::mat4 modelMatrix{1.0f};
};

glm::mat4 composeTransform(const TransformComponent& transform);
glm::mat3 normalMatrixFromModel(const glm::mat4& model);
render::Bounds3 transformBounds(const render::Bounds3& bounds, const glm::mat4& model);
OrientedBox makeOrientedBox(const TransformComponent& transform, const BoxColliderComponent& collider);
OrientedBox makeOrientedBox(const glm::mat4& modelMatrix, const BoxColliderComponent& collider);

}  // namespace core
