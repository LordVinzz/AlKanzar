#include "EditorGizmoMath.hpp"

#include <algorithm>
#include <unordered_set>

#include <glm/glm.hpp>

#include "core/ecs/World.hpp"
#include "core/transform/TransformMath.hpp"

namespace core {
namespace {

glm::mat4 resolveWorldMatrix(
    const World& world,
    EntityId entity,
    std::unordered_set<EntityId>& active
) {
    if (!entity.valid() || !world.isAlive(entity) || !active.insert(entity).second) {
        return glm::mat4(1.0f);
    }
    glm::mat4 matrix(1.0f);
    if (const TransformComponent* transform = world.transforms.tryGet(entity)) {
        matrix = composeTransform(*transform);
    }
    if (const ParentComponent* parent = world.parents.tryGet(entity);
        parent != nullptr && world.isAlive(parent->parent)) {
        matrix = resolveWorldMatrix(world, parent->parent, active) * matrix;
    }
    active.erase(entity);
    return matrix;
}

}  // namespace

glm::mat4 editorEntityWorldMatrix(const World& world, EntityId entity) {
    std::unordered_set<EntityId> active{};
    return resolveWorldMatrix(world, entity, active);
}

bool editorParentHasNonUniformScale(const glm::mat4& parentWorld) {
    const glm::vec3 scale{
        glm::length(glm::vec3(parentWorld[0])),
        glm::length(glm::vec3(parentWorld[1])),
        glm::length(glm::vec3(parentWorld[2]))
    };
    const float maximum = std::max(scale.x, std::max(scale.y, scale.z));
    const float minimum = std::min(scale.x, std::min(scale.y, scale.z));
    return maximum - minimum > std::max(1.0e-4f, maximum * 1.0e-4f);
}

bool editorLocalTransformFromWorld(
    const glm::mat4& worldMatrix,
    const std::optional<glm::mat4>& parentWorld,
    TransformComponent& outTransform
) {
    const glm::mat4 local = parentWorld.has_value()
        ? glm::inverse(*parentWorld) * worldMatrix
        : worldMatrix;
    return decomposeTransform(local, outTransform);
}

}  // namespace core
