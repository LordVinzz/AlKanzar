#pragma once

#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>

#include "core/ecs/Components.hpp"

namespace core {

glm::mat4 composeTransform(const TransformComponent& transform);
glm::mat3 normalMatrixFromModel(const glm::mat4& model);
render::Bounds3 transformBounds(const render::Bounds3& bounds, const glm::mat4& model);

}  // namespace core
