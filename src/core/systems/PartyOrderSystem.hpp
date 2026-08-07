#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "core/ecs/Entity.hpp"

namespace core {

class NavigationSystem;
class TaskScheduler;
class World;
struct NavigationRuntime;

struct PartyMoveAssignment {
    EntityId entity{};
    glm::vec3 desiredDestination{0.0f};
    std::optional<glm::vec3> navigationDestination{};
    bool requestAccepted{false};
};

struct PartyMoveOrderResult {
    std::vector<PartyMoveAssignment> assignments{};
    std::size_t requestedCount{0u};
};

[[nodiscard]] std::vector<glm::vec3> makePartyFormationDestinations(
    const glm::vec3& center,
    const glm::vec2& forward,
    std::size_t memberCount,
    float spacing
);

class PartyOrderSystem {
public:
    [[nodiscard]] PartyMoveOrderResult issueMoveOrder(
        World& world,
        const NavigationRuntime& runtime,
        NavigationSystem& navigation,
        TaskScheduler& scheduler,
        const std::vector<EntityId>& selected,
        const glm::vec3& destination
    ) const;
};

}  // namespace core
