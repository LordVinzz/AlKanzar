#include "PartyOrderSystem.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include <glm/geometric.hpp>

#include "core/ecs/World.hpp"
#include "core/navigation/Navigation.hpp"
#include "core/simulation/CharacterComponents.hpp"
#include "core/systems/TaskScheduler.hpp"

namespace core {
namespace {

constexpr std::array<glm::vec2, kMaximumPartySize> kFormationOffsets{
    glm::vec2(0.0f, 0.0f),
    glm::vec2(-0.6f, -0.9f),
    glm::vec2(0.6f, -0.9f),
    glm::vec2(-1.2f, -1.8f),
    glm::vec2(0.0f, -1.8f),
    glm::vec2(1.2f, -1.8f),
};

glm::vec2 normalizedForward(const glm::vec2& forward) {
    const float length = glm::length(forward);
    return length > 1.0e-5f ? forward / length : glm::vec2(0.0f, 1.0f);
}

bool isEligiblePartyAgent(const World& world, EntityId entity) {
    return world.isAlive(entity) && world.characters.contains(entity) &&
        world.transforms.contains(entity) && world.navAgents.contains(entity) &&
        isActivePlayerPartyMember(
            world.characterControllers.tryGet(entity),
            world.partyMembers.tryGet(entity)
        );
}

}  // namespace

std::vector<glm::vec3> makePartyFormationDestinations(
    const glm::vec3& center,
    const glm::vec2& forward,
    std::size_t memberCount,
    float spacing
) {
    const std::size_t count = std::min(memberCount, kMaximumPartySize);
    std::vector<glm::vec3> destinations{};
    destinations.reserve(count);
    if (count == 0u) {
        return destinations;
    }

    glm::vec2 offsetCenter(0.0f);
    for (std::size_t index = 0u; index < count; ++index) {
        offsetCenter += kFormationOffsets[index];
    }
    offsetCenter /= static_cast<float>(count);

    const glm::vec2 formationForward = normalizedForward(forward);
    const glm::vec2 formationRight(formationForward.y, -formationForward.x);
    const float safeSpacing = std::max(spacing, 0.1f);
    for (std::size_t index = 0u; index < count; ++index) {
        const glm::vec2 local = (kFormationOffsets[index] - offsetCenter) * safeSpacing;
        const glm::vec2 worldOffset =
            formationRight * local.x + formationForward * local.y;
        destinations.emplace_back(
            center.x + worldOffset.x,
            center.y,
            center.z + worldOffset.y
        );
    }
    return destinations;
}

PartyMoveOrderResult PartyOrderSystem::issueMoveOrder(
    World& world,
    const NavigationRuntime& runtime,
    NavigationSystem& navigation,
    TaskScheduler& scheduler,
    const std::vector<EntityId>& selected,
    const glm::vec3& destination
) const {
    std::vector<EntityId> agents{};
    agents.reserve(std::min(selected.size(), kMaximumPartySize));
    glm::vec2 groupCenter(0.0f);
    float spacing = 1.1f;
    for (const EntityId entity : selected) {
        if (agents.size() == kMaximumPartySize) {
            break;
        }
        if (!isEligiblePartyAgent(world, entity) ||
            std::find(agents.begin(), agents.end(), entity) != agents.end()) {
            continue;
        }
        agents.push_back(entity);
        const glm::vec3 position = world.transforms.get(entity).position;
        groupCenter += glm::vec2(position.x, position.z);
        spacing = std::max(
            spacing,
            world.characters.get(entity).groundIndicatorRadius * 2.2f
        );
    }

    PartyMoveOrderResult result{};
    if (agents.empty()) {
        return result;
    }
    groupCenter /= static_cast<float>(agents.size());
    const glm::vec2 forward(
        destination.x - groupCenter.x,
        destination.z - groupCenter.y
    );
    const std::vector<glm::vec3> formation = makePartyFormationDestinations(
        destination,
        forward,
        agents.size(),
        spacing
    );
    result.assignments.reserve(agents.size());
    for (std::size_t index = 0u; index < agents.size(); ++index) {
        PartyMoveAssignment assignment{};
        assignment.entity = agents[index];
        assignment.desiredDestination = formation[index];
        assignment.navigationDestination = navigation.projectAgentDestination(
            world,
            runtime,
            assignment.entity,
            assignment.desiredDestination
        );
        if (assignment.navigationDestination.has_value()) {
            assignment.requestAccepted = navigation.requestAgentDestination(
                world,
                runtime,
                scheduler,
                assignment.entity,
                *assignment.navigationDestination
            );
        }
        if (assignment.requestAccepted) {
            ++result.requestedCount;
        }
        result.assignments.push_back(std::move(assignment));
    }
    return result;
}

}  // namespace core
