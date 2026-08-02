#pragma once

#include <atomic>
#include <optional>
#include <vector>

#include "core/navigation/NavigationDetailCorridor.hpp"

namespace core::navigation_detail {

struct VisibilityNodeSet {
    std::vector<glm::vec3> nodes{};
    std::vector<glm::vec3> rankedNodes{};
    std::size_t nextRankedNodeIndex{2u};
};

bool visibilityNodeCanParticipate(
    const NavigationSolveView& runtime,
    const AgentClearanceProfile& profile,
    const glm::vec3& point
);
std::optional<VisibilityNodeSet> buildVisibilityNodes(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const std::vector<NavCorridorStep>& corridor,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled,
    bool restrictToCorridor
);
bool appendRankedVisibilityNodes(
    VisibilityNodeSet& nodeSet,
    std::size_t limit,
    const NavigationSolveView& runtime,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled
);

}  // namespace core::navigation_detail
