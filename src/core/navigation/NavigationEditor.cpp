#include "core/navigation/Navigation.hpp"
#include "core/navigation/NavigationDetailEditor.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"

#include <cmath>
#include <limits>

#include "core/systems/PickingSystem.hpp"

namespace core {

using namespace navigation_detail;

std::vector<EntityId> NavigationSystem::collectRenderableSelectionTargets(const World& world, EntityId selected) const {
    std::vector<EntityId> targets{};
    if (!selected.valid() || !world.isAlive(selected)) {
        return targets;
    }

    if (world.navSources.contains(selected)) {
        targets.push_back(selected);
        return targets;
    }

    const auto visit = [&](const auto& self, EntityId entity) -> void {
        for (EntityId child : world.parents.entities()) {
            const ParentComponent* parent = world.parents.tryGet(child);
            if (parent == nullptr || parent->parent != entity) {
                continue;
            }
            if (world.navSources.contains(child)) {
                targets.push_back(child);
            }
            self(self, child);
        }
    };
    visit(visit, selected);
    return targets;
}

void NavigationSystem::applyTagOverride(World& world, NavigationRuntime& runtime, EntityId entity, NavSourceTag tag) const {
    NavSourceComponent* source = world.navSources.tryGet(entity);
    if (source == nullptr) {
        return;
    }
    source->effectiveTag = tag;
    updateSourceOverride(runtime.asset.sourceTagOverrides, *source, tag);
}

bool NavigationSystem::capturePolygonClick(
    NavigationRuntime& runtime,
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight,
    int mouseX,
    int mouseY
) const {
    const std::optional<ViewportRay> ray = makeViewportRay(camera, viewportWidth, viewportHeight, mouseX, mouseY);
    if (!ray.has_value() || std::abs(ray->direction.y) <= kPlaneEpsilon) {
        return false;
    }

    const float t = (runtime.editor.polygonCaptureElevation - ray->origin.y) / ray->direction.y;
    if (t < 0.0f) {
        return false;
    }
    const glm::vec3 worldPoint = ray->origin + ray->direction * t;
    const glm::vec2 pointXZ(worldPoint.x, worldPoint.z);
    if (!runtime.editor.polygonCaptureVertices.empty() &&
        nearlyEqualVec2(runtime.editor.polygonCaptureVertices.back(), pointXZ, 0.01f)) {
        return false;
    }
    runtime.editor.polygonCaptureVertices.push_back(pointXZ);
    return true;
}

bool NavigationSystem::commitCapturedPolygon(NavigationRuntime& runtime, std::string* error) const {
    if (runtime.editor.polygonCaptureVertices.size() < 3u) {
        if (error) {
            *error = "Need at least three captured vertices.";
        }
        return false;
    }

    runtime.asset.polygons.push_back(NavPolygon{
        nextPolygonId(runtime.asset),
        runtime.editor.polygonCaptureElevation,
        runtime.editor.polygonCaptureVertices
    });
    runtime.editor.polygonCaptureVertices.clear();
    runtime.editor.polygonCaptureActive = false;
    return rebuildRuntime(runtime, error);
}

void NavigationSystem::clearCapturedPolygon(NavigationRuntime& runtime) const {
    runtime.editor.polygonCaptureVertices.clear();
    runtime.editor.polygonCaptureActive = false;
}

bool NavigationSystem::seedPendingLink(NavigationRuntime& runtime, int fromPolygonId, int toPolygonId, std::string* error) const {
    const auto fromIndex = findPolygonIndexById(runtime, fromPolygonId);
    const auto toIndex = findPolygonIndexById(runtime, toPolygonId);
    if (!fromIndex.has_value() || !toIndex.has_value()) {
        if (error) {
            *error = "Select valid polygons before seeding a link.";
        }
        return false;
    }

    const NavPolygon& fromPolygon = runtime.asset.polygons[*fromIndex];
    const NavPolygon& toPolygon = runtime.asset.polygons[*toIndex];
    float bestDistance = std::numeric_limits<float>::max();
    glm::vec2 bestFrom = fromPolygon.verticesXZ.front();
    glm::vec2 bestTo = toPolygon.verticesXZ.front();
    for (const glm::vec2& fromVertex : fromPolygon.verticesXZ) {
        for (const glm::vec2& toVertex : toPolygon.verticesXZ) {
            const float distance = glm::distance(fromVertex, toVertex);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestFrom = fromVertex;
                bestTo = toVertex;
            }
        }
    }

    runtime.editor.pendingLinkFromPolygonId = fromPolygonId;
    runtime.editor.pendingLinkToPolygonId = toPolygonId;
    runtime.editor.pendingLinkFromPoint = glm::vec3(bestFrom.x, fromPolygon.elevationY, bestFrom.y);
    runtime.editor.pendingLinkToPoint = glm::vec3(bestTo.x, toPolygon.elevationY, bestTo.y);
    runtime.editor.pendingLinkBidirectional = true;
    return true;
}

bool NavigationSystem::commitPendingLink(NavigationRuntime& runtime, std::string* error) const {
    if (runtime.editor.pendingLinkFromPolygonId < 0 || runtime.editor.pendingLinkToPolygonId < 0) {
        if (error) {
            *error = "Select source and target polygons first.";
        }
        return false;
    }

    runtime.asset.links.push_back(NavLink{
        nextLinkId(runtime.asset),
        runtime.editor.pendingLinkFromPolygonId,
        runtime.editor.pendingLinkToPolygonId,
        runtime.editor.pendingLinkFromPoint,
        runtime.editor.pendingLinkToPoint,
        runtime.editor.pendingLinkBidirectional
    });
    runtime.editor.pendingLinkFromPolygonId = -1;
    runtime.editor.pendingLinkToPolygonId = -1;
    runtime.editor.pendingLinkFromPoint = glm::vec3(0.0f);
    runtime.editor.pendingLinkToPoint = glm::vec3(0.0f);
    runtime.editor.pendingLinkBidirectional = true;
    return rebuildRuntime(runtime, error);
}


}  // namespace core
