#include "core/navigation/Navigation.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"
#include "core/navigation/NavigationDetailTypes.hpp"

#include <cmath>
#include <optional>

#include "core/systems/PickingSystem.hpp"

namespace core {

using namespace navigation_detail;

std::optional<NavHitResult> NavigationSystem::hitTest(
    const NavigationRuntime& runtime,
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight,
    int mouseX,
    int mouseY
) const {
    const std::optional<ViewportRay> ray = makeViewportRay(camera, viewportWidth, viewportHeight, mouseX, mouseY);
    if (!ray.has_value()) {
        return std::nullopt;
    }

    std::optional<NavHitResult> bestHit{};
    for (const NavPolygon& polygon : runtime.asset.polygons) {
        if (std::abs(ray->direction.y) <= kPlaneEpsilon) {
            continue;
        }
        const float t = (polygon.elevationY - ray->origin.y) / ray->direction.y;
        if (t < 0.0f) {
            continue;
        }
        const glm::vec3 point = ray->origin + ray->direction * t;
        if (!pointInPolygonXZ(glm::vec2(point.x, point.z), polygon.verticesXZ)) {
            continue;
        }
        if (!bestHit.has_value() || t < bestHit->distance) {
            bestHit = NavHitResult{polygon.id, point, t};
        }
    }
    return bestHit;
}

}  // namespace core

