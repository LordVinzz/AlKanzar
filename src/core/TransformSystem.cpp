#include "TransformSystem.hpp"

#include <algorithm>
#include <vector>

#include "TransformMath.hpp"

namespace core {

namespace {

render::Bounds3 mergeBounds(const render::Bounds3& lhs, const render::Bounds3& rhs) {
    return render::Bounds3{
        glm::min(lhs.min, rhs.min),
        glm::max(lhs.max, rhs.max),
    };
}

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
        cache.hasWorldBounds = true;
    } else {
        cache.hasWorldBounds = false;
    }
    cache.valid = true;

    if (entity.index < active.size()) {
        active[entity.index] = false;
    }
    return model;
}

bool resolveHierarchyBounds(
    World& world,
    EntityId entity,
    const std::vector<std::vector<EntityId>>& childrenByParent,
    std::vector<bool>& active
) {
    if (!entity.valid() || entity.index >= world.transformCache_.size()) {
        return false;
    }

    if (entity.index < active.size() && active[entity.index]) {
        return world.transformCache_[entity.index].hasWorldBounds;
    }
    if (entity.index < active.size()) {
        active[entity.index] = true;
    }

    TransformCacheEntry& cache = world.transformCache_[entity.index];
    bool hasBounds = cache.hasWorldBounds;

    if (entity.index < childrenByParent.size()) {
        for (EntityId child : childrenByParent[entity.index]) {
            if (!world.isAlive(child) || child.index >= world.transformCache_.size()) {
                continue;
            }

            if (!resolveHierarchyBounds(world, child, childrenByParent, active)) {
                continue;
            }

            const TransformCacheEntry& childCache = world.transformCache_[child.index];
            cache.worldBounds = hasBounds ? mergeBounds(cache.worldBounds, childCache.worldBounds) : childCache.worldBounds;
            hasBounds = true;
        }
    }

    cache.hasWorldBounds = hasBounds;
    if (entity.index < active.size()) {
        active[entity.index] = false;
    }
    return hasBounds;
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

    std::vector<std::vector<EntityId>> childrenByParent(world.transformCache_.size());
    for (EntityId child : world.parents.entities()) {
        if (!world.isAlive(child)) {
            continue;
        }
        const ParentComponent& parent = world.parents.get(child);
        if (!parent.parent.valid() || !world.isAlive(parent.parent)) {
            continue;
        }
        world.ensureCacheSize(std::max(child.index, parent.parent.index) + 1u);
        if (childrenByParent.size() <= parent.parent.index) {
            childrenByParent.resize(parent.parent.index + 1u);
        }
        childrenByParent[parent.parent.index].push_back(child);
    }

    std::fill(active.begin(), active.end(), false);
    for (EntityId entity : world.transforms.entities()) {
        resolveHierarchyBounds(world, entity, childrenByParent, active);
    }
    for (EntityId entity : world.renderables.entities()) {
        resolveHierarchyBounds(world, entity, childrenByParent, active);
    }

    world.clearTransformDirty();
}

}  // namespace core
