#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "core/navigation/NavigationDetailTypes.hpp"

namespace core::navigation_detail {

NavigationSolveView makeSolveView(const NavigationRuntime& runtime);
NavigationSolveView makeSolveView(const NavigationSolveSnapshot& snapshot);
render::FrameCounterRecord makeNavigationCounter(const char* name, std::int64_t value);
void applyPathResult(
    NavAgentComponent& agent,
    const glm::vec3& destination,
    std::vector<glm::vec3> corners
);
void snapAgentToResolvedStart(
    World& world,
    EntityId entity,
    TransformComponent& transform,
    const glm::vec3& resolvedStart
);
std::optional<NavigationSystem::PartialPathResult> consumePartialPathResult(
    const std::shared_ptr<NavigationSystem::PendingPathProgress>& progress
);
bool pathSegmentsAreValid(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const std::vector<glm::vec3>& corners,
    const AgentClearanceProfile& profile
);
bool segmentMatchesRuntimeLink(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    float epsilon
);
bool canAdvancePathCorner(
    const NavigationSolveView& runtime,
    const glm::vec3& currentPosition,
    const std::vector<glm::vec3>& corners,
    float arrivalRadius,
    const AgentClearanceProfile& profile
);
std::optional<SolvedPath> solvePathCorners(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const glm::vec3& destination,
    float arrivalRadius,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled = nullptr
);
std::vector<glm::vec3> trimPathCornersFromCurrentPosition(
    const NavigationSolveView& runtime,
    const glm::vec3& currentPosition,
    std::vector<glm::vec3> corners,
    float arrivalRadius,
    const AgentClearanceProfile& profile
);

}  // namespace core::navigation_detail
