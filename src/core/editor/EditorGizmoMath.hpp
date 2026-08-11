#pragma once

#include <optional>

#include <glm/mat4x4.hpp>

#include "core/ecs/Components.hpp"
#include "core/ecs/Entity.hpp"

namespace core {

class World;

[[nodiscard]] glm::mat4 editorEntityWorldMatrix(const World& world, EntityId entity);
[[nodiscard]] bool editorParentHasNonUniformScale(const glm::mat4& parentWorld);
[[nodiscard]] bool editorLocalTransformFromWorld(
    const glm::mat4& worldMatrix,
    const std::optional<glm::mat4>& parentWorld,
    TransformComponent& outTransform
);

}  // namespace core
