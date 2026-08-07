#include "core/navigation/Navigation.hpp"

#include <optional>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailRuntime.hpp"

namespace core {

std::optional<glm::vec3> NavigationSystem::projectAgentDestination(
    const World& world,
    const NavigationRuntime& runtime,
    EntityId agentEntity,
    const glm::vec3& destination
) const {
    const NavAgentComponent* agent = world.navAgents.tryGet(agentEntity);
    const TransformComponent* transform = world.transforms.tryGet(agentEntity);
    const std::shared_ptr<const NavigationSolveSnapshot> snapshot =
        runtime.solveSnapshot;
    if (agent == nullptr || transform == nullptr || snapshot == nullptr ||
        snapshot->bakedCells.empty()) {
        return std::nullopt;
    }

    const navigation_detail::NavigationSolveView view =
        navigation_detail::makeSolveView(*snapshot);
    std::optional<glm::vec3> projected =
        navigation_detail::projectEndpointOntoWalkableLayer(view, destination);
    if (!projected.has_value()) {
        const std::optional<std::size_t> nearestCell =
            navigation_detail::findNearestCell(view, destination);
        if (!nearestCell.has_value()) {
            return std::nullopt;
        }
        const NavRuntimeCell& cell = view.bakedCells[*nearestCell];
        const glm::vec2 nearestPoint = navigation_detail::closestPointOnPolygonXZ(
            glm::vec2(destination.x, destination.z),
            cell.verticesXZ
        );
        projected = glm::vec3(nearestPoint.x, cell.elevationY, nearestPoint.y);
    }

    const navigation_detail::AgentClearanceProfile clearance =
        navigation_detail::resolveAgentClearanceProfile(world, agentEntity, *agent);
    if (clearance.empty()) {
        return projected;
    }
    const glm::vec2 travelDirection = navigation_detail::travelDirectionForSegment(
        transform->position,
        *projected
    );
    return navigation_detail::resolvePointWithClearance(
        view,
        *projected,
        clearance,
        travelDirection
    );
}

}  // namespace core
