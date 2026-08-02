#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"
#include "core/navigation/NavigationDetailSurface.hpp"
#include "core/navigation/NavigationDetailTraversal.hpp"
#include "core/navigation/NavigationDetailRuntime.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace core::navigation_detail {

glm::vec3 cellCenter3(const NavigationSolveView& runtime, std::size_t cellIndex) {
    const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
    const glm::vec2 center = cellIndex < runtime.bakedCellCenters.size()
        ? runtime.bakedCellCenters[cellIndex]
        : polygonCentroidXZ(cell.verticesXZ);
    return glm::vec3(center.x, cell.elevationY, center.y);
}

std::vector<std::size_t> findContainingCells(const NavigationSolveView& runtime, const glm::vec3& point) {
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

std::optional<std::size_t> findNearestCell(const NavigationSolveView& runtime, const glm::vec3& point) {
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

std::optional<std::size_t> findPolygonIndexById(const NavigationSolveView& runtime, int polygonId) {
    const auto it = runtime.polygonIndexById.find(polygonId);
    if (it == runtime.polygonIndexById.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::size_t> findLinkEndpointCells(
    const NavigationSolveView& runtime,
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
    const NavigationSolveView& runtime,
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

bool pointInsideAuthoredWalkableSurface(const NavigationSolveView& runtime, const glm::vec3& point) {
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
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to
) {
    return segmentInsideBakedWalkableSurface(runtime, from, to);
}

bool segmentInsideAuthoredWalkableSurfaceWithClearance(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile
) {
    if (profile.empty()) {
        return segmentInsideAuthoredWalkableSurface(runtime, from, to);
    }
    if (!segmentInsideAuthoredWalkableSurface(runtime, from, to)) {
        return false;
    }
    const glm::vec2 travelDirection = travelDirectionForSegment(from, to);
    if (profile.shape == AgentClearanceShape::Box) {
        return boxSweepInsideWalkableSurface(
            runtime,
            from,
            to,
            profile,
            travelDirection
        );
    }

    if (!visitClearanceBoundaryOffsets(
            profile,
            travelDirection,
            kSegmentClearanceSampleDirections,
            [&](const glm::vec2& planarOffset) {
                const glm::vec3 offset(
                    planarOffset.x,
                    0.0f,
                    planarOffset.y
                );
                return segmentInsideAuthoredWalkableSurface(
                    runtime,
                    from + offset,
                    to + offset
                );
            })) {
        return false;
    }
    return sphereSweepInsideWalkableSurface(
        runtime,
        from,
        to,
        profile,
        travelDirection
    );
}

bool segmentInsideSelectedWalkableCellsWithClearance(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile,
    const std::vector<std::size_t>& candidateCells
) {
    if (!segmentInsideBakedWalkableSurface(
            runtime,
            from,
            to,
            &candidateCells)) {
        return false;
    }
    const glm::vec2 travelDirection = travelDirectionForSegment(from, to);
    if (profile.shape == AgentClearanceShape::Box) {
        return boxSweepInsideWalkableSurface(
            runtime,
            from,
            to,
            profile,
            travelDirection,
            &candidateCells
        );
    }
    if (!visitClearanceBoundaryOffsets(
            profile,
            travelDirection,
            kSegmentClearanceSampleDirections,
            [&](const glm::vec2& planarOffset) {
                const glm::vec3 offset(
                    planarOffset.x,
                    0.0f,
                    planarOffset.y
                );
                return segmentInsideBakedWalkableSurface(
                    runtime,
                    from + offset,
                    to + offset,
                    &candidateCells
                );
            })) {
        return false;
    }
    return sphereSweepInsideWalkableSurface(
        runtime,
        from,
        to,
        profile,
        travelDirection,
        &candidateCells
    );
}

std::vector<glm::vec3> shortcutPathCorners(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const std::vector<glm::vec3>& corners,
    float arrivalRadius,
    const AgentClearanceProfile& profile
) {
    std::vector<glm::vec3> optimized{};
    glm::vec3 anchor = start;
    std::size_t index = 0u;
    while (index < corners.size()) {
        std::size_t bestReach = index;
        for (std::size_t candidate = corners.size(); candidate-- > index;) {
            if (segmentInsideAuthoredWalkableSurfaceWithClearance(
                    runtime, anchor, corners[candidate], profile)) {
                bestReach = candidate;
                break;
            }
        }
        appendPathCorner(optimized, corners[bestReach], arrivalRadius);
        anchor = corners[bestReach];
        index = bestReach + 1u;
    }
    return optimized;
}

// --- Pathfinding pipeline ---

std::optional<ResolvedPathEndpoints> resolvePathEndpoints(
    const NavigationSolveView& runtime,
    const glm::vec3& start,
    const glm::vec3& destination,
    const AgentClearanceProfile& profile,
    const std::atomic<bool>* cancelled
) {
    const auto isCancelled = [&]() {
        return cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed);
    };
    if (isCancelled()) {
        return std::nullopt;
    }
    ResolvedPathEndpoints endpoints{};
    const glm::vec3 projectedStart = projectEndpointOntoWalkableLayer(
        runtime,
        start
    ).value_or(start);
    const glm::vec3 projectedDestination =
        projectEndpointOntoWalkableLayer(runtime, destination)
            .value_or(destination);
    if (!profile.empty()) {
        const glm::vec2 travelDir = travelDirectionForSegment(
            projectedStart,
            projectedDestination,
            glm::vec2(0.0f, 1.0f)
        );
        endpoints.resolvedStart = resolvePointWithClearance(
            runtime,
            projectedStart,
            profile,
            travelDir,
            cancelled
        ).value_or(projectedStart);
        if (isCancelled()) {
            return std::nullopt;
        }

        // Destination: prefer approach-line projection for natural corner clearance.
        // Walk from click along reverse-approach direction until a safe position is found,
        // then binary-search back to find the nearest safe point along that line.
        const glm::vec2 approachDir = -travelDir;
        if (pointInsideAuthoredWalkableSurfaceWithClearance(
                runtime,
                projectedDestination,
                profile,
                approachDir)) {
            endpoints.resolvedDestination = projectedDestination;
        } else {
            const float maxSearch = glm::distance(
                glm::vec2(projectedStart.x, projectedStart.z),
                glm::vec2(
                    projectedDestination.x,
                    projectedDestination.z)
            );
            const float nominalClearance = std::max(
                supportDistance(profile, glm::vec2(1.0f, 0.0f), approachDir),
                supportDistance(profile, glm::vec2(0.0f, 1.0f), approachDir)
            );
            const float step = std::max(nominalClearance * 0.25f, 0.05f);
            glm::vec3 safeFar{};
            bool foundSafe = false;
            for (float t = step; t <= maxSearch; t += step) {
                if (isCancelled()) {
                    return std::nullopt;
                }
                safeFar = glm::vec3(
                    projectedDestination.x + approachDir.x * t,
                    projectedDestination.y,
                    projectedDestination.z + approachDir.y * t
                );
                if (pointInsideAuthoredWalkableSurface(runtime, safeFar) &&
                    pointInsideAuthoredWalkableSurfaceWithClearance(runtime, safeFar, profile, approachDir)) {
                    foundSafe = true;
                    break;
                }
            }
            if (foundSafe) {
                float unsafeT = 0.0f;
                float safeT = 1.0f;
                for (int s = 0; s < kClearanceBinarySearchSteps; ++s) {
                    const float midT = (unsafeT + safeT) * 0.5f;
                    const glm::vec3 mid = projectedDestination +
                        (safeFar - projectedDestination) * midT;
                    if (pointInsideAuthoredWalkableSurface(runtime, mid) &&
                        pointInsideAuthoredWalkableSurfaceWithClearance(runtime, mid, profile, approachDir)) {
                        safeT = midT;
                    } else {
                        unsafeT = midT;
                    }
                }
                endpoints.resolvedDestination = projectedDestination +
                    (safeFar - projectedDestination) * safeT;
            } else {
                endpoints.resolvedDestination = resolvePointWithClearance(
                    runtime,
                    projectedDestination,
                    profile,
                    -travelDir,
                    cancelled
                ).value_or(projectedDestination);
            }
        }
    } else {
        endpoints.resolvedStart = projectedStart;
        endpoints.resolvedDestination = projectedDestination;
    }
    endpoints.rawStartCells = findContainingCells(runtime, start);
    endpoints.rawTargetCells = findContainingCells(runtime, destination);
    if (endpoints.rawStartCells.empty()) {
        endpoints.rawStartCells = findContainingCells(
            runtime,
            projectedStart
        );
    }
    if (endpoints.rawTargetCells.empty()) {
        endpoints.rawTargetCells = findContainingCells(
            runtime,
            projectedDestination
        );
    }
    endpoints.startCells = findContainingCells(runtime, endpoints.resolvedStart);
    endpoints.targetCells = findContainingCells(runtime, endpoints.resolvedDestination);
    if (endpoints.startCells.empty()) {
        endpoints.startCells = endpoints.rawStartCells;
    }
    if (endpoints.targetCells.empty()) {
        endpoints.targetCells = endpoints.rawTargetCells;
    }
    if (endpoints.startCells.empty()) {
        if (const auto nearest = findNearestCell(runtime, endpoints.resolvedStart); nearest.has_value()) {
            endpoints.startCells.push_back(*nearest);
        }
    }
    if (endpoints.targetCells.empty()) {
        if (const auto nearest = findNearestCell(runtime, endpoints.resolvedDestination); nearest.has_value()) {
            endpoints.targetCells.push_back(*nearest);
        }
    }
    if (endpoints.startCells.empty() || endpoints.targetCells.empty()) {
        return std::nullopt;
    }
    return endpoints;
}

bool canSolveDirectPath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const AgentClearanceProfile& profile
) {
    return segmentInsideAuthoredWalkableSurfaceWithClearance(
        runtime,
        endpoints.resolvedStart,
        endpoints.resolvedDestination,
        profile
    );
}


}  // namespace core::navigation_detail
