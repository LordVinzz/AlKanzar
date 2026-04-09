#include "TransformSystem.hpp"

#include <algorithm>
#include <vector>

#include "TransformMath.hpp"

namespace core {

namespace {

glm::mat4 resolveWorldMatrix(World& world, EntityId entity, std::vector<bool>& active) {
    world.ensureCacheSize(entity.index + 1u);
    TransformCacheEntry& cache = world.transformCache_[entity.index];
    if (cache.valid) {
        return cache.worldMatrix;
    }

    if (entity.index < active.size() && active[entity.index]) {
        return glm::mat4(1.0f);
    }
    if (entity.index < active.size()) {
        active[entity.index] = true;
    }

    glm::mat4 model(1.0f);
    if (const TransformComponent* transform = world.transforms.tryGet(entity)) {
        model = composeTransform(*transform);
    }

    if (const ParentComponent* parent = world.parents.tryGet(entity);
        parent != nullptr && world.isAlive(parent->parent)) {
        model = resolveWorldMatrix(world, parent->parent, active) * model;
    }

    cache.worldMatrix = model;
    if (const BoundsComponent* bounds = world.bounds.tryGet(entity)) {
        cache.worldBounds = transformBounds(bounds->localBounds, model);
    }
    cache.valid = true;

    if (entity.index < active.size()) {
        active[entity.index] = false;
    }
    return model;
}

}  // namespace

void TransformSystem::update(World& world) const {
    if (!world.transformsDirty()) {
        return;
    }

    world.ensureCacheSize(std::max(
        world.renderables.size(),
        world.transforms.size()
    ));
    std::fill(world.transformCache_.begin(), world.transformCache_.end(), TransformCacheEntry{});
    std::vector<bool> active(world.transformCache_.size(), false);

    for (EntityId entity : world.transforms.entities()) {
        resolveWorldMatrix(world, entity, active);
    }
    for (EntityId entity : world.renderables.entities()) {
        resolveWorldMatrix(world, entity, active);
    }

    world.clearTransformDirty();
}

}  // namespace core
