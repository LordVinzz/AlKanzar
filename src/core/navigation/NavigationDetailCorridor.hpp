#pragma once

#include <atomic>
#include <optional>
#include <utility>
#include <vector>

#include "core/navigation/NavigationDetailTypes.hpp"

namespace core::navigation_detail {

struct NavCorridorStep {
    std::size_t fromCellIndex{0u};
    std::size_t toCellIndex{0u};
    std::size_t edgeIndex{0u};
    std::optional<SharedPortalResult> safePortal{};
};

struct NavTraversalState {
    std::size_t fromCellIndex{0u};
    std::size_t toCellIndex{0u};
    std::size_t edgeIndex{0u};
    glm::vec3 anchor{0.0f};
    std::optional<SharedPortalResult> safePortal{};
    bool traversable{false};
};

enum class PortalSamplingMode {
    Midpoint,
    Geometry,
};

std::optional<std::vector<NavCorridorStep>> findAStarCorridor(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled,
    PortalSamplingMode samplingMode,
    const std::vector<std::pair<std::size_t, std::size_t>>& blockedTraversals
);
std::optional<std::vector<glm::vec3>> buildFunnelPath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const std::vector<NavCorridorStep>& corridor
);
std::optional<std::vector<glm::vec3>> solveCorridorClearancePath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const std::vector<NavCorridorStep>& corridor,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled,
    bool allowVisibilityFallback,
    bool restrictToCorridor = true,
    VisibilityTraversalCache* sharedVisibilityCache = nullptr
);
std::optional<std::vector<glm::vec3>> solveLinkedCorridorClearancePath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const std::vector<NavCorridorStep>& corridor,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled
);

}  // namespace core::navigation_detail
