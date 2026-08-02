#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailCorridor.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPath.hpp"
#include "core/navigation/NavigationDetailRuntime.hpp"
#include "core/navigation/NavigationDetailTraversal.hpp"

#include <atomic>
#include <optional>

namespace core::navigation_detail {

bool pathSegmentsAreValid(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const std::vector<glm::vec3>& corners,
    const AgentClearanceProfile& profile
) {
    glm::vec3 previous = start;
    for (const glm::vec3& corner : corners) {
        if (!segmentInsideAuthoredWalkableSurfaceWithClearance(runtime, previous, corner, profile) &&
            !segmentMatchesRuntimeLink(runtime, previous, corner, kPolygonEpsilon * 8.0f)) {
            return false;
        }
        previous = corner;
    }
    return true;
}

std::optional<std::vector<glm::vec3>> solveLinkedCorridorClearancePath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const std::vector<NavCorridorStep>& corridor,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled
) {
    const auto isCancelled = [&]() {
        return cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed);
    };
    if (profile.empty() || isCancelled()) {
        return std::nullopt;
    }

    std::vector<glm::vec3> result{};
    std::vector<NavCorridorStep> planarCorridor{};
    ResolvedPathEndpoints planarEndpoints{};
    planarEndpoints.resolvedStart = endpoints.resolvedStart;
    planarEndpoints.startCells = endpoints.startCells;

    const auto flushPlanar = [&](
        const glm::vec3& segmentDestination,
        const std::vector<std::size_t>& targetCells
    ) -> bool {
        if (isCancelled()) {
            return false;
        }
        planarEndpoints.resolvedDestination = segmentDestination;
        planarEndpoints.targetCells = targetCells;

        std::optional<std::vector<glm::vec3>> segmentCorners =
            buildFunnelPath(runtime, planarEndpoints, planarCorridor);
        if (!segmentCorners.has_value() ||
            !pathSegmentsAreValid(
                runtime,
                planarEndpoints.resolvedStart,
                *segmentCorners,
                profile)) {
            segmentCorners = solveCorridorClearancePath(
                runtime,
                planarEndpoints,
                planarCorridor,
                profile,
                cancelled,
                true
            );
        }
        if (!segmentCorners.has_value() ||
            !pathSegmentsAreValid(
                runtime,
                planarEndpoints.resolvedStart,
                *segmentCorners,
                profile)) {
            return false;
        }
        for (const glm::vec3& corner : *segmentCorners) {
            appendPathCorner(result, corner, kPolygonEpsilon);
        }
        planarCorridor.clear();
        planarEndpoints.resolvedStart = segmentDestination;
        planarEndpoints.startCells = targetCells;
        return true;
    };

    for (const NavCorridorStep& step : corridor) {
        if (isCancelled() || step.fromCellIndex >= runtime.graph.size() ||
            step.edgeIndex >= runtime.graph[step.fromCellIndex].size()) {
            return std::nullopt;
        }
        const NavGraphEdge& edge =
            runtime.graph[step.fromCellIndex][step.edgeIndex];
        if (!edge.viaLink) {
            planarCorridor.push_back(step);
            continue;
        }

        const std::vector<std::size_t> linkStartCells{step.fromCellIndex};
        if (!flushPlanar(edge.linkStartPoint, linkStartCells)) {
            return std::nullopt;
        }
        appendPathCorner(result, edge.linkEndPoint, kPolygonEpsilon);
        planarEndpoints.resolvedStart = edge.linkEndPoint;
        planarEndpoints.startCells = {step.toCellIndex};
    }

    if (!flushPlanar(
            endpoints.resolvedDestination,
            endpoints.targetCells)) {
        return std::nullopt;
    }
    if (!pathSegmentsAreValid(
            runtime,
            endpoints.resolvedStart,
            result,
            profile)) {
        return std::nullopt;
    }
    return result;
}

}  // namespace core::navigation_detail
