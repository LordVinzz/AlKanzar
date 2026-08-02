#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailTraversal.hpp"
#include "core/navigation/NavigationDetailVisibility.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace core::navigation_detail {
namespace {

bool isVisibilityBuildCancelled(const std::atomic<bool>* cancelled) {
    return cancelled != nullptr && cancelled->load(std::memory_order_relaxed);
}

std::vector<std::uint8_t> collectVisibilityCells(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const std::vector<NavCorridorStep>& corridor,
    bool restrictToCorridor
) {
    std::vector<std::uint8_t> cells(runtime.bakedCells.size(), 0u);
    for (std::size_t cell : endpoints.startCells) {
        if (cell < cells.size()) {
            cells[cell] = 1u;
        }
    }
    for (std::size_t cell : endpoints.targetCells) {
        if (cell < cells.size()) {
            cells[cell] = 1u;
        }
    }
    for (const NavCorridorStep& step : corridor) {
        if (step.fromCellIndex < cells.size()) {
            cells[step.fromCellIndex] = 1u;
        }
        if (step.toCellIndex < cells.size()) {
            cells[step.toCellIndex] = 1u;
        }
    }
    if (restrictToCorridor) {
        return cells;
    }

    std::fill(cells.begin(), cells.end(), 0u);
    std::vector<std::size_t> pending{};
    for (std::size_t cell : endpoints.startCells) {
        if (cell < cells.size() && cells[cell] == 0u) {
            cells[cell] = 1u;
            pending.push_back(cell);
        }
    }
    while (!pending.empty()) {
        const std::size_t cell = pending.back();
        pending.pop_back();
        for (const NavGraphEdge& edge : runtime.graph[cell]) {
            if (!edge.viaLink && edge.targetCellIndex < cells.size() &&
                cells[edge.targetCellIndex] == 0u) {
                cells[edge.targetCellIndex] = 1u;
                pending.push_back(edge.targetCellIndex);
            }
        }
    }
    return cells;
}

}  // namespace

bool visibilityNodeCanParticipate(
    const NavigationSolveView& runtime,
    const AgentClearanceProfile& profile,
    const glm::vec3& point
) {
    const AgentClearanceProfile nodeClearance = headingIndependentNodeClearance(profile);
    if (nodeClearance.empty()) {
        return true;
    }
    return visitClearanceBoundaryOffsets(
        nodeClearance,
        glm::vec2(0.0f, 1.0f),
        kSegmentClearanceSampleDirections,
        [&](const glm::vec2& offset) {
            return segmentInsideBakedWalkableSurface(
                runtime,
                glm::vec3(point.x + offset.x, point.y, point.z + offset.y),
                glm::vec3(point.x + offset.x, point.y, point.z + offset.y)
            );
        }
    );
}

std::optional<VisibilityNodeSet> buildVisibilityNodes(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const std::vector<NavCorridorStep>& corridor,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled,
    bool restrictToCorridor
) {
    if (isVisibilityBuildCancelled(cancelled)) {
        return std::nullopt;
    }

    VisibilityNodeSet result{};
    result.nodes = {endpoints.resolvedStart, endpoints.resolvedDestination};
    std::unordered_set<QuantizedLayerPoint, QuantizedLayerPointHash> emitted{
        quantizeLayerPoint(glm::vec2(endpoints.resolvedStart.x, endpoints.resolvedStart.z), endpoints.resolvedStart.y),
        quantizeLayerPoint(glm::vec2(endpoints.resolvedDestination.x, endpoints.resolvedDestination.z), endpoints.resolvedDestination.y),
    };
    const auto addNode = [&](const glm::vec3& point) {
        if (isVisibilityBuildCancelled(cancelled) ||
            std::abs(point.y - endpoints.resolvedStart.y) > kLayerGroupingEpsilon ||
            !segmentInsideBakedWalkableSurface(runtime, point, point)) {
            return;
        }
        if (emitted.insert(quantizeLayerPoint(glm::vec2(point.x, point.z), point.y)).second) {
            result.nodes.push_back(point);
        }
    };

    for (const NavCorridorStep& step : corridor) {
        if (isVisibilityBuildCancelled(cancelled) || !step.safePortal.has_value()) {
            continue;
        }
        const float elevation = step.fromCellIndex < runtime.bakedCells.size()
            ? runtime.bakedCells[step.fromCellIndex].elevationY
            : endpoints.resolvedStart.y;
        const glm::vec2 midpoint = (step.safePortal->a + step.safePortal->b) * 0.5f;
        addNode(glm::vec3(midpoint.x, elevation, midpoint.y));
        addNode(glm::vec3(step.safePortal->a.x, elevation, step.safePortal->a.y));
        addNode(glm::vec3(step.safePortal->b.x, elevation, step.safePortal->b.y));
    }

    const std::vector<std::uint8_t> cells = collectVisibilityCells(
        runtime, endpoints, corridor, restrictToCorridor
    );
    const float clearanceRadius = conservativeClearanceRadius(profile) * 1.05f;
    constexpr int kCornerCandidateDirections = 8;
    for (std::size_t cellIndex = 0u; cellIndex < runtime.bakedCells.size(); ++cellIndex) {
        if (isVisibilityBuildCancelled(cancelled)) {
            return std::nullopt;
        }
        if (cells[cellIndex] == 0u) {
            continue;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        for (std::size_t vertexIndex = 0u; vertexIndex < cell.verticesXZ.size(); ++vertexIndex) {
            if (cellIndex < runtime.bakedCellBoundaryVertices.size() &&
                vertexIndex < runtime.bakedCellBoundaryVertices[cellIndex].size() &&
                runtime.bakedCellBoundaryVertices[cellIndex][vertexIndex] == 0u) {
                continue;
            }
            const glm::vec2 vertex = cell.verticesXZ[vertexIndex];
            for (int directionIndex = 0; directionIndex < kCornerCandidateDirections; ++directionIndex) {
                const glm::vec2 direction = clearanceSampleDirection(
                    directionIndex, kCornerCandidateDirections
                );
                addNode(glm::vec3(
                    vertex.x + direction.x * clearanceRadius,
                    cell.elevationY,
                    vertex.y + direction.y * clearanceRadius
                ));
            }
        }
    }
    if (isVisibilityBuildCancelled(cancelled)) {
        return std::nullopt;
    }

    constexpr std::size_t kMaximumRankedNodes = 512u;
    constexpr std::size_t kInitialLocalVisibilityNodes = 64u;
    if (result.nodes.size() > 2u) {
        std::stable_sort(result.nodes.begin() + 2, result.nodes.end(), [&](const glm::vec3& lhs, const glm::vec3& rhs) {
            const float lhsScore = glm::distance(endpoints.resolvedStart, lhs) +
                glm::distance(lhs, endpoints.resolvedDestination);
            const float rhsScore = glm::distance(endpoints.resolvedStart, rhs) +
                glm::distance(rhs, endpoints.resolvedDestination);
            return lhsScore < rhsScore;
        });
        if (result.nodes.size() > kMaximumRankedNodes) {
            result.nodes.resize(kMaximumRankedNodes);
        }
    }
    result.rankedNodes = std::move(result.nodes);
    result.nodes = {result.rankedNodes[0], result.rankedNodes[1]};
    if (!appendRankedVisibilityNodes(
            result, kInitialLocalVisibilityNodes, runtime, profile, cancelled)) {
        return std::nullopt;
    }
    return result;
}

bool appendRankedVisibilityNodes(
    VisibilityNodeSet& nodeSet,
    std::size_t limit,
    const NavigationSolveView& runtime,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled
) {
    while (nodeSet.nextRankedNodeIndex < nodeSet.rankedNodes.size() &&
           nodeSet.nodes.size() < limit) {
        if (isVisibilityBuildCancelled(cancelled)) {
            return false;
        }
        const glm::vec3& candidate = nodeSet.rankedNodes[nodeSet.nextRankedNodeIndex++];
        if (visibilityNodeCanParticipate(runtime, profile, candidate)) {
            nodeSet.nodes.push_back(candidate);
        }
    }
    return true;
}

}  // namespace core::navigation_detail
