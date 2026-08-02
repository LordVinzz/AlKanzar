#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"
#include "core/navigation/NavigationDetailSurface.hpp"
#include "core/navigation/NavigationDetailTraversal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace core::navigation_detail {

std::vector<std::size_t> findContainingCells(const NavigationRuntime& runtime, const glm::vec3& point) {
    std::vector<std::size_t> containing{};
    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t index = 0; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        if (std::abs(point.y - cell.elevationY) > kLayerGroupingEpsilon) {
            continue;
        }
        if (index < runtime.bakedCellMinXZ.size() &&
            index < runtime.bakedCellMaxXZ.size() &&
            (pointXZ.x < runtime.bakedCellMinXZ[index].x - kPolygonEpsilon ||
             pointXZ.x > runtime.bakedCellMaxXZ[index].x + kPolygonEpsilon ||
             pointXZ.y < runtime.bakedCellMinXZ[index].y - kPolygonEpsilon ||
             pointXZ.y > runtime.bakedCellMaxXZ[index].y + kPolygonEpsilon)) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            containing.push_back(index);
        }
    }
    return containing;
}

bool containsCellIndex(const std::vector<std::size_t>& indices, std::size_t cellIndex) {
    return std::find(indices.begin(), indices.end(), cellIndex) != indices.end();
}

std::optional<std::size_t> findNearestCell(const NavigationRuntime& runtime, const glm::vec3& point) {
    std::optional<std::size_t> bestIndex{};
    float bestDistance = std::numeric_limits<float>::max();
    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t index = 0; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        const glm::vec2 closest =
            closestPointOnPolygonXZ(pointXZ, cell.verticesXZ);
        const float planarDistance = glm::distance(closest, pointXZ);
        const float verticalDistance = std::abs(point.y - cell.elevationY);
        const float score = planarDistance + verticalDistance * 2.0f;
        if (score < bestDistance) {
            bestDistance = score;
            bestIndex = index;
        }
    }
    return bestIndex;
}

std::vector<std::size_t> findLinkEndpointCells(
    const NavigationRuntime& runtime,
    std::size_t authoredPolygonIndex,
    const glm::vec3& point
) {
    std::vector<std::size_t> containing{};
    if (authoredPolygonIndex >= runtime.polygonToCellIndices.size()) {
        return containing;
    }

    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t cellIndex : runtime.polygonToCellIndices[authoredPolygonIndex]) {
        if (cellIndex >= runtime.bakedCells.size()) {
            continue;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        if (std::abs(point.y - cell.elevationY) > kLayerGroupingEpsilon) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            containing.push_back(cellIndex);
        }
    }
    return containing;
}

std::optional<std::size_t> findNearestCandidateCell(
    const NavigationRuntime& runtime,
    const std::vector<std::size_t>& candidates,
    const glm::vec3& point
) {
    std::optional<std::size_t> bestIndex{};
    float bestScore = std::numeric_limits<float>::max();
    for (std::size_t cellIndex : candidates) {
        if (cellIndex >= runtime.bakedCells.size()) {
            continue;
        }
        const glm::vec2 center = cellIndex < runtime.bakedCellCenters.size()
            ? runtime.bakedCellCenters[cellIndex]
            : polygonCentroidXZ(runtime.bakedCells[cellIndex].verticesXZ);
        const float planarDistance = glm::distance(center, glm::vec2(point.x, point.z));
        const float verticalDistance = std::abs(point.y - runtime.bakedCells[cellIndex].elevationY);
        const float score = planarDistance + verticalDistance * 2.0f;
        if (score < bestScore) {
            bestScore = score;
            bestIndex = cellIndex;
        }
    }
    return bestIndex;
}

bool pointInsideAuthoredWalkableSurface(const NavigationRuntime& runtime, const glm::vec3& point) {
    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t index = 0u; index < runtime.bakedCells.size(); ++index) {
        const NavRuntimeCell& cell = runtime.bakedCells[index];
        if (std::abs(point.y - cell.elevationY) > kLayerGroupingEpsilon) {
            continue;
        }
        if (index < runtime.bakedCellMinXZ.size() && index < runtime.bakedCellMaxXZ.size() &&
            (pointXZ.x < runtime.bakedCellMinXZ[index].x - kPolygonEpsilon ||
             pointXZ.x > runtime.bakedCellMaxXZ[index].x + kPolygonEpsilon ||
             pointXZ.y < runtime.bakedCellMinXZ[index].y - kPolygonEpsilon ||
             pointXZ.y > runtime.bakedCellMaxXZ[index].y + kPolygonEpsilon)) {
            continue;
        }
        if (pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            return true;
        }
    }
    return false;
}


bool segmentInsideAuthoredWalkableSurface(
    const NavigationRuntime& runtime,
    const glm::vec3& from,
    const glm::vec3& to
) {
    return segmentInsideBakedWalkableSurface(runtime, from, to);
}

// --- NavigationSolveView overloads for pathfinding pipeline ---


}  // namespace core::navigation_detail
