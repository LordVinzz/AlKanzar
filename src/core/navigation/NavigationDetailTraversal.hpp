#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"

namespace core::navigation_detail {

bool pointInsideAuthoredWalkableSurface(
    const NavigationRuntime& runtime,
    const glm::vec3& point
);
bool pointInsideAuthoredWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& point
);
std::vector<std::size_t> findContainingCells(
    const NavigationRuntime& runtime,
    const glm::vec3& point
);
std::optional<std::size_t> findNearestCell(
    const NavigationRuntime& runtime,
    const glm::vec3& point
);
std::vector<std::size_t> findLinkEndpointCells(
    const NavigationRuntime& runtime,
    std::size_t authoredPolygonIndex,
    const glm::vec3& point
);
std::optional<std::size_t> findNearestCandidateCell(
    const NavigationRuntime& runtime,
    const std::vector<std::size_t>& candidates,
    const glm::vec3& point
);
bool segmentInsideAuthoredWalkableSurface(
    const NavigationRuntime& runtime,
    const glm::vec3& from,
    const glm::vec3& to
);

glm::vec3 cellCenter3(const NavigationSolveView& runtime, std::size_t cellIndex);
std::vector<std::size_t> findContainingCells(
    const NavigationSolveView& runtime,
    const glm::vec3& point
);
std::optional<std::size_t> findNearestCell(
    const NavigationSolveView& runtime,
    const glm::vec3& point
);
std::optional<std::size_t> findPolygonIndexById(
    const NavigationSolveView& runtime,
    int polygonId
);
std::vector<std::size_t> findLinkEndpointCells(
    const NavigationSolveView& runtime,
    std::size_t authoredPolygonIndex,
    const glm::vec3& point
);
std::optional<std::size_t> findNearestCandidateCell(
    const NavigationSolveView& runtime,
    const std::vector<std::size_t>& candidates,
    const glm::vec3& point
);
bool segmentInsideAuthoredWalkableSurfaceWithClearance(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile
);
std::optional<ResolvedPathEndpoints> resolvePathEndpoints(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const glm::vec3& destination,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled = nullptr
);
bool canSolveDirectPath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const AgentClearanceProfile& profile
);

template <typename RuntimeView>
bool segmentInsideBakedWalkableSurface(
    const RuntimeView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const std::vector<std::size_t>* candidateCells = nullptr
) {
    if (std::abs(from.y - to.y) > kLayerGroupingEpsilon) {
        return false;
    }

    const glm::vec2 segmentStart(from.x, from.z);
    const glm::vec2 segmentEnd(to.x, to.z);
    const glm::vec2 segmentDelta = segmentEnd - segmentStart;
    if (glm::dot(segmentDelta, segmentDelta) <= kPlaneEpsilon * kPlaneEpsilon) {
        if (candidateCells != nullptr) {
            for (std::size_t cellIndex : *candidateCells) {
                if (cellIndex >= runtime.bakedCells.size()) {
                    continue;
                }
                const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
                if (std::abs(from.y - cell.elevationY) <= kLayerGroupingEpsilon &&
                    pointInOrOnPolygonXZ(segmentStart, cell.verticesXZ)) {
                    return true;
                }
            }
            return false;
        }
        return pointInsideAuthoredWalkableSurface(runtime, from);
    }

    const glm::vec2 segmentMin = glm::min(segmentStart, segmentEnd);
    const glm::vec2 segmentMax = glm::max(segmentStart, segmentEnd);
    thread_local std::vector<std::pair<float, float>> coveredIntervals{};
    coveredIntervals.clear();
    const std::size_t candidateCount =
        candidateCells != nullptr ? candidateCells->size() : runtime.bakedCells.size();
    if (coveredIntervals.capacity() < candidateCount) {
        coveredIntervals.reserve(candidateCount);
    }
    const auto collectCoveredInterval = [&](std::size_t cellIndex) {
        if (cellIndex >= runtime.bakedCells.size()) {
            return;
        }
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        if (std::abs(from.y - cell.elevationY) > kLayerGroupingEpsilon) {
            return;
        }
        if (cellIndex < runtime.bakedCellMinXZ.size() &&
            cellIndex < runtime.bakedCellMaxXZ.size() &&
            !boundsOverlapXZ(
                segmentMin,
                segmentMax,
                runtime.bakedCellMinXZ[cellIndex],
                runtime.bakedCellMaxXZ[cellIndex])) {
            return;
        }

        const float orientation = polygonSignedArea(cell.verticesXZ) >= 0.0f ? 1.0f : -1.0f;
        float entryT = 0.0f;
        float exitT = 1.0f;
        bool intersects = true;
        for (std::size_t edgeIndex = 0u; edgeIndex < cell.verticesXZ.size(); ++edgeIndex) {
            const glm::vec2& edgeStart = cell.verticesXZ[edgeIndex];
            const glm::vec2& edgeEnd = cell.verticesXZ[(edgeIndex + 1u) % cell.verticesXZ.size()];
            const glm::vec2 edge = edgeEnd - edgeStart;
            const float startSide = orientation * cross2(edge, segmentStart - edgeStart);
            const float sideDelta = orientation * cross2(edge, segmentDelta);
            if (std::abs(sideDelta) <= kPlaneEpsilon) {
                if (startSide < -kPolygonEpsilon) {
                    intersects = false;
                    break;
                }
                continue;
            }

            const float boundaryT = (-kPolygonEpsilon - startSide) / sideDelta;
            if (sideDelta > 0.0f) {
                entryT = std::max(entryT, boundaryT);
            } else {
                exitT = std::min(exitT, boundaryT);
            }
            if (entryT > exitT + kPolygonEpsilon) {
                intersects = false;
                break;
            }
        }
        if (intersects && exitT >= -kPolygonEpsilon && entryT <= 1.0f + kPolygonEpsilon) {
            coveredIntervals.emplace_back(
                std::clamp(entryT, 0.0f, 1.0f),
                std::clamp(exitT, 0.0f, 1.0f)
            );
        }
    };
    if (candidateCells != nullptr) {
        for (std::size_t cellIndex : *candidateCells) {
            collectCoveredInterval(cellIndex);
        }
    } else {
        for (std::size_t cellIndex = 0u; cellIndex < runtime.bakedCells.size(); ++cellIndex) {
            collectCoveredInterval(cellIndex);
        }
    }

    std::sort(coveredIntervals.begin(), coveredIntervals.end());
    float coveredUntil = 0.0f;
    for (const auto& [entryT, exitT] : coveredIntervals) {
        if (exitT < coveredUntil - kPolygonEpsilon) {
            continue;
        }
        if (entryT > coveredUntil + kPolygonEpsilon) {
            return false;
        }
        coveredUntil = std::max(coveredUntil, exitT);
        if (coveredUntil >= 1.0f - kPolygonEpsilon) {
            return true;
        }
    }
    return false;
}

}  // namespace core::navigation_detail
