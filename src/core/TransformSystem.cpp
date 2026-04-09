#include "TransformSystem.hpp"

#include <algorithm>
#include <cstdint>
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

std::size_t taskGrain(std::size_t count, std::size_t workerCount) {
    const std::size_t lanes = std::max<std::size_t>(1u, workerCount + 1u) * 2u;
    return std::max<std::size_t>(32u, (count + lanes - 1u) / lanes);
}

void ensureWorkingSize(World& world, std::vector<std::uint8_t>& flags, std::size_t size) {
    world.ensureCacheSize(size);
    if (flags.size() < size) {
        flags.resize(size, 0u);
    }
}

void markRelevantEntity(
    World& world,
    EntityId entity,
    std::vector<std::uint8_t>& relevantMask,
    std::vector<EntityId>& relevantEntities
) {
    if (!entity.valid() || !world.isAlive(entity)) {
        return;
    }

    ensureWorkingSize(world, relevantMask, entity.index + 1u);
    if (relevantMask[entity.index] != 0u) {
        return;
    }

    relevantMask[entity.index] = 1u;
    relevantEntities.push_back(entity);
}

void collectRelevantChain(
    World& world,
    EntityId start,
    std::vector<std::uint8_t>& relevantMask,
    std::vector<EntityId>& relevantEntities
) {
    EntityId current = start;
    while (current.valid() && world.isAlive(current)) {
        markRelevantEntity(world, current, relevantMask, relevantEntities);

        const ParentComponent* parent = world.parents.tryGet(current);
        if (parent == nullptr || !parent->parent.valid() || !world.isAlive(parent->parent)) {
            break;
        }

        ensureWorkingSize(world, relevantMask, parent->parent.index + 1u);
        if (relevantMask[parent->parent.index] != 0u) {
            break;
        }

        current = parent->parent;
    }
}

void processRootSubtree(
    World& world,
    EntityId entity,
    const std::vector<std::vector<EntityId>>& childrenByParent,
    const glm::mat4& parentMatrix,
    std::vector<std::uint8_t>& processed
) {
    TransformCacheEntry& cache = world.transformCache_[entity.index];
    glm::mat4 worldMatrix = parentMatrix;
    if (const TransformComponent* transform = world.transforms.tryGet(entity)) {
        worldMatrix = parentMatrix * composeTransform(*transform);
    }

    cache.worldMatrix = worldMatrix;
    cache.valid = true;
    if (const BoundsComponent* bounds = world.bounds.tryGet(entity)) {
        cache.worldBounds = transformBounds(bounds->localBounds, worldMatrix);
        cache.hasWorldBounds = true;
    } else {
        cache.hasWorldBounds = false;
    }

    if (entity.index < childrenByParent.size()) {
        for (EntityId child : childrenByParent[entity.index]) {
            processRootSubtree(world, child, childrenByParent, worldMatrix, processed);
        }
    }

    bool hasBounds = cache.hasWorldBounds;
    if (entity.index < childrenByParent.size()) {
        for (EntityId child : childrenByParent[entity.index]) {
            const TransformCacheEntry& childCache = world.transformCache_[child.index];
            if (!childCache.hasWorldBounds) {
                continue;
            }
            cache.worldBounds = hasBounds ? mergeBounds(cache.worldBounds, childCache.worldBounds) : childCache.worldBounds;
            hasBounds = true;
        }
    }

    cache.hasWorldBounds = hasBounds;
    processed[entity.index] = 1u;
}

glm::mat4 resolveWorldMatrix(World& world, EntityId entity, std::vector<std::uint8_t>& active) {
    world.ensureCacheSize(entity.index + 1u);
    TransformCacheEntry& cache = world.transformCache_[entity.index];
    if (cache.valid) {
        return cache.worldMatrix;
    }

    if (entity.index < active.size() && active[entity.index] != 0u) {
        return glm::mat4(1.0f);
    }
    if (entity.index < active.size()) {
        active[entity.index] = 1u;
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
        active[entity.index] = 0u;
    }
    return model;
}

bool resolveHierarchyBounds(
    World& world,
    EntityId entity,
    const std::vector<std::vector<EntityId>>& childrenByParent,
    std::vector<std::uint8_t>& active
) {
    if (!entity.valid() || entity.index >= world.transformCache_.size()) {
        return false;
    }

    if (entity.index < active.size() && active[entity.index] != 0u) {
        return world.transformCache_[entity.index].hasWorldBounds;
    }
    if (entity.index < active.size()) {
        active[entity.index] = 1u;
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
        active[entity.index] = 0u;
    }
    return hasBounds;
}

}  // namespace

void TransformSystem::update(World& world, TaskScheduler& scheduler, bool useParallel) const {
    if (!world.transformsDirty()) {
        return;
    }

    std::vector<std::uint8_t> relevantMask{};
    std::vector<EntityId> relevantEntities{};
    relevantEntities.reserve(
        world.transforms.size() +
        world.renderables.size() +
        world.bounds.size() +
        world.parents.size()
    );

    for (EntityId entity : world.transforms.entities()) {
        collectRelevantChain(world, entity, relevantMask, relevantEntities);
    }
    for (EntityId entity : world.renderables.entities()) {
        collectRelevantChain(world, entity, relevantMask, relevantEntities);
    }
    for (EntityId entity : world.bounds.entities()) {
        collectRelevantChain(world, entity, relevantMask, relevantEntities);
    }
    for (EntityId entity : world.parents.entities()) {
        collectRelevantChain(world, entity, relevantMask, relevantEntities);
    }

    if (world.transformCache_.size() < relevantMask.size()) {
        world.ensureCacheSize(relevantMask.size());
    }
    std::fill(world.transformCache_.begin(), world.transformCache_.end(), TransformCacheEntry{});

    std::vector<std::vector<EntityId>> childrenByParent(world.transformCache_.size());
    std::vector<EntityId> roots{};
    roots.reserve(relevantEntities.size());
    for (EntityId child : relevantEntities) {
        const ParentComponent* parent = world.parents.tryGet(child);
        if (parent == nullptr || !parent->parent.valid() || !world.isAlive(parent->parent)) {
            roots.push_back(child);
            continue;
        }

        ensureWorkingSize(world, relevantMask, parent->parent.index + 1u);
        if (relevantMask[parent->parent.index] == 0u) {
            roots.push_back(child);
            continue;
        }

        childrenByParent[parent->parent.index].push_back(child);
    }

    if (!useParallel) {
        std::vector<std::uint8_t> active(world.transformCache_.size(), 0u);
        for (EntityId entity : relevantEntities) {
            resolveWorldMatrix(world, entity, active);
        }
        std::fill(active.begin(), active.end(), 0u);
        for (EntityId entity : relevantEntities) {
            resolveHierarchyBounds(world, entity, childrenByParent, active);
        }

        world.clearTransformDirty();
        return;
    }

    std::vector<std::uint8_t> processed(world.transformCache_.size(), 0u);
    TaskGroup group;
    scheduler.parallelFor(
        group,
        roots.size(),
        taskGrain(roots.size(), scheduler.workerCount()),
        "Transform Root",
        [&](std::size_t begin, std::size_t end) {
            for (std::size_t index = begin; index < end; ++index) {
                processRootSubtree(world, roots[index], childrenByParent, glm::mat4(1.0f), processed);
            }
        }
    );
    scheduler.wait(group);

    std::vector<std::uint8_t> active(world.transformCache_.size(), 0u);
    for (EntityId entity : relevantEntities) {
        if (entity.index < processed.size() && processed[entity.index] == 0u) {
            resolveWorldMatrix(world, entity, active);
        }
    }
    std::fill(active.begin(), active.end(), 0u);
    for (EntityId entity : relevantEntities) {
        if (entity.index < processed.size() && processed[entity.index] == 0u) {
            resolveHierarchyBounds(world, entity, childrenByParent, active);
        }
    }

    world.clearTransformDirty();
}

}  // namespace core
