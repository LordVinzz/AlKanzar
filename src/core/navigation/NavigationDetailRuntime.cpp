#include "core/navigation/NavigationDetailRuntime.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <unordered_set>

#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPath.hpp"

namespace core::navigation_detail {

NavigationSolveView makeSolveView(const NavigationRuntime& runtime) {
    return NavigationSolveView{
        runtime.asset,
        runtime.polygonIndexById,
        runtime.polygonCenters,
        runtime.bakedCells,
        runtime.bakedCellCenters,
        runtime.bakedCellMinXZ,
        runtime.bakedCellMaxXZ,
        runtime.bakedCellBoundaryVertices,
        runtime.polygonToCellIndices,
        runtime.cellToPolygonIndices,
        runtime.graph,
        runtime.polyanyaMesh.get(),
        runtime.bakedCellsHaveInteriorOverlap
    };
}

NavigationSolveView makeSolveView(const NavigationSolveSnapshot& snapshot) {
    return NavigationSolveView{
        snapshot.asset,
        snapshot.polygonIndexById,
        snapshot.polygonCenters,
        snapshot.bakedCells,
        snapshot.bakedCellCenters,
        snapshot.bakedCellMinXZ,
        snapshot.bakedCellMaxXZ,
        snapshot.bakedCellBoundaryVertices,
        snapshot.polygonToCellIndices,
        snapshot.cellToPolygonIndices,
        snapshot.graph,
        snapshot.polyanyaMesh.get(),
        snapshot.bakedCellsHaveInteriorOverlap
    };
}

std::shared_ptr<const NavigationSolveSnapshot> buildSolveSnapshot(const NavigationRuntime& runtime) {
    return std::make_shared<const NavigationSolveSnapshot>(NavigationSolveSnapshot{
        runtime.asset,
        runtime.polygonIndexById,
        runtime.polygonCenters,
        runtime.bakedCells,
        runtime.bakedCellCenters,
        runtime.bakedCellMinXZ,
        runtime.bakedCellMaxXZ,
        runtime.bakedCellBoundaryVertices,
        runtime.polygonToCellIndices,
        runtime.cellToPolygonIndices,
        runtime.graph,
        runtime.polyanyaMesh,
        runtime.bakedCellsHaveInteriorOverlap
    });
}

void appendPathCorner(std::vector<glm::vec3>& corners, const glm::vec3& point, float arrivalRadius) {
    if (!corners.empty() && nearlyEqualVec3(corners.back(), point, arrivalRadius)) {
        return;
    }
    corners.push_back(point);
}

float pathLength(
    const glm::vec3& start,
    const std::vector<glm::vec3>& corners
) {
    float length = 0.0f;
    glm::vec3 previous = start;
    for (const glm::vec3& corner : corners) {
        length += glm::distance(previous, corner);
        previous = corner;
    }
    return length;
}

render::FrameCounterRecord makeNavigationCounter(const char* name, std::int64_t value) {
    return render::FrameCounterRecord{name, value, "Navigation"};
}

void applyPathResult(NavAgentComponent& agent, const glm::vec3& destination, std::vector<glm::vec3> corners) {
    agent.pathCorners = std::move(corners);
    agent.destination = destination;
    agent.moving = !agent.pathCorners.empty();
    agent.desiredVelocity = glm::vec3(0.0f);
    agent.physicsStepStart.reset();
    agent.traversingLink = false;
}

void snapAgentToResolvedStart(
    World& world,
    EntityId entity,
    TransformComponent& transform,
    const glm::vec3& resolvedStart
) {
    if (nearlyEqualVec3(
            transform.position,
            resolvedStart,
            kPolygonEpsilon)) {
        return;
    }
    transform.position = resolvedStart;
    if (RigidbodyComponent* rigidbody = world.rigidbodies.tryGet(entity)) {
        rigidbody->velocity = glm::vec3(0.0f);
    }
    world.markTransformsDirty(entity);
}

std::optional<NavigationSystem::PartialPathResult> consumePartialPathResult(
    const std::shared_ptr<NavigationSystem::PendingPathProgress>& progress
) {
    if (!progress) {
        return std::nullopt;
    }
    std::lock_guard lock(progress->mutex);
    if (!progress->partialPath.has_value()) {
        return std::nullopt;
    }
    std::optional<NavigationSystem::PartialPathResult> result = std::move(progress->partialPath);
    progress->partialPath.reset();
    return result;
}

ParentPathData buildStableIdPaths(const World& world) {
    ParentPathData data{};
    std::unordered_set<EntityId> entities{};
    for (EntityId entity : world.names.entities()) {
        entities.insert(entity);
    }
    for (EntityId entity : world.parents.entities()) {
        entities.insert(entity);
        const ParentComponent& parent = world.parents.get(entity);
        if (parent.parent.valid() && world.isAlive(parent.parent)) {
            entities.insert(parent.parent);
            data.childrenByParent[parent.parent].push_back(entity);
        }
    }
    for (EntityId entity : world.renderables.entities()) {
        entities.insert(entity);
    }
    for (EntityId entity : world.transforms.entities()) {
        entities.insert(entity);
    }

    std::vector<EntityId> roots{};
    roots.reserve(entities.size());
    for (EntityId entity : entities) {
        const ParentComponent* parent = world.parents.tryGet(entity);
        if (parent == nullptr || !parent->parent.valid() || !world.isAlive(parent->parent)) {
            roots.push_back(entity);
        }
    }

    const auto compareByLabel = [&world](EntityId lhs, EntityId rhs) {
        const std::string lhsLabel = entityName(world, lhs);
        const std::string rhsLabel = entityName(world, rhs);
        if (lhsLabel != rhsLabel) {
            return lhsLabel < rhsLabel;
        }
        return lhs.index < rhs.index;
    };

    std::sort(roots.begin(), roots.end(), compareByLabel);
    for (auto& [parent, children] : data.childrenByParent) {
        std::sort(children.begin(), children.end(), compareByLabel);
    }

    const auto visit = [&](const auto& self, EntityId entity, const std::string& parentPath) -> void {
        std::vector<EntityId> siblings{};
        const ParentComponent* parent = world.parents.tryGet(entity);
        if (parent != nullptr && parent->parent.valid() && world.isAlive(parent->parent)) {
            auto it = data.childrenByParent.find(parent->parent);
            if (it != data.childrenByParent.end()) {
                siblings = it->second;
            }
        } else {
            siblings = roots;
        }

        const std::string label = entityName(world, entity);
        int ordinal = 0;
        int matchingCount = 0;
        for (EntityId sibling : siblings) {
            if (entityName(world, sibling) != label) {
                continue;
            }
            if (sibling == entity) {
                ordinal = matchingCount;
            }
            ++matchingCount;
        }

        std::string segment = label;
        if (matchingCount > 1) {
            segment += "#" + std::to_string(ordinal);
        }
        const std::string path = parentPath.empty() ? segment : parentPath + "/" + segment;
        data.pathByEntity[entity] = path;

        auto it = data.childrenByParent.find(entity);
        if (it == data.childrenByParent.end()) {
            return;
        }
        for (EntityId child : it->second) {
            self(self, child, path);
        }
    };

    for (EntityId root : roots) {
        visit(visit, root, std::string{});
    }

    return data;
}

NavSourceTag defaultTagForLayer(render::RenderLayer layer) {
    switch (layer) {
        case render::RenderLayer::Ground:
            return NavSourceTag::Walkable;
        case render::RenderLayer::Actors:
            return NavSourceTag::Ignored;
        case render::RenderLayer::Geometry:
        default:
            return NavSourceTag::Blocking;
    }
}

}  // namespace core::navigation_detail
