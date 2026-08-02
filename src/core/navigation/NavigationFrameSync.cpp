#include "core/navigation/Navigation.hpp"
#include "core/navigation/NavigationDetailTypes.hpp"

namespace core {

using namespace navigation_detail;

void NavigationSystem::syncFrame(const World& world, const NavigationRuntime& runtime, FrameSceneData& frame) const {
    frame.navigation.clear();
    frame.navigation.polygons.reserve(runtime.asset.polygons.size());
    frame.navigation.links.reserve(runtime.asset.links.size());
    for (const NavPolygon& polygon : runtime.asset.polygons) {
        FrameNavDebugPolygon debugPolygon{};
        debugPolygon.id = polygon.id;
        debugPolygon.elevationY = polygon.elevationY;
        debugPolygon.color = kWalkableOverlayColor;
        debugPolygon.vertices.reserve(polygon.verticesXZ.size());
        for (const glm::vec2& vertex : polygon.verticesXZ) {
            debugPolygon.vertices.push_back(glm::vec3(vertex.x, polygon.elevationY, vertex.y));
        }
        frame.navigation.polygons.push_back(std::move(debugPolygon));
    }
    for (const NavLink& link : runtime.asset.links) {
        frame.navigation.links.push_back(FrameNavDebugLink{
            link.id,
            link.fromPoint,
            link.toPoint,
            link.bidirectional
        });
    }
    for (EntityId entity : world.navAgents.entities()) {
        const NavAgentComponent& agent = world.navAgents.get(entity);
        frame.navigation.path = agent.pathCorners;
        frame.navigation.destination = agent.destination;
        break;
    }
    if (runtime.editor.polygonCaptureActive) {
        frame.navigation.captureElevationY = runtime.editor.polygonCaptureElevation;
        for (const glm::vec2& vertex : runtime.editor.polygonCaptureVertices) {
            frame.navigation.captureVertices.push_back(glm::vec3(vertex.x, runtime.editor.polygonCaptureElevation, vertex.y));
        }
    }
}


}  // namespace core
