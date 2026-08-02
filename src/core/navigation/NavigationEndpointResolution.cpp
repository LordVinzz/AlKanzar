#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace core::navigation_detail {

glm::vec2 clearanceSampleDirection(int sampleIndex, int sampleCount) {
    const float angle = kTau * static_cast<float>(sampleIndex) / static_cast<float>(sampleCount);
    return glm::vec2(std::cos(angle), std::sin(angle));
}

bool pointInsideAuthoredWalkableSurfaceWithClearanceSamples(
    const NavigationSolveView& runtime,
    const glm::vec3& point,
    const AgentClearanceProfile& profile,
    const glm::vec2& travelDirection,
    int sampleDirections
) {
    if (!pointInsideAuthoredWalkableSurface(runtime, point)) {
        return false;
    }
    if (profile.empty()) {
        return true;
    }
    if (profile.shape == AgentClearanceShape::Box) {
        return boxSweepInsideWalkableSurface(
            runtime,
            point,
            point,
            profile,
            travelDirection
        );
    }
    if (!visitClearanceBoundaryOffsets(
            profile,
            travelDirection,
            sampleDirections,
            [&](const glm::vec2& offset) {
                const glm::vec3 samplePoint(
                    point.x + offset.x,
                    point.y,
                    point.z + offset.y
                );
                return pointInsideAuthoredWalkableSurface(
                    runtime,
                    samplePoint
                );
            })) {
        return false;
    }
    return sphereSweepInsideWalkableSurface(
        runtime,
        point,
        point,
        profile,
        travelDirection
    );
}

bool pointInsideAuthoredWalkableSurfaceWithClearance(
    const NavigationSolveView& runtime,
    const glm::vec3& point,
    const AgentClearanceProfile& profile,
    const glm::vec2& travelDirection
) {
    return pointInsideAuthoredWalkableSurfaceWithClearanceSamples(
        runtime,
        point,
        profile,
        travelDirection,
        kClearanceSampleDirections
    );
}

std::optional<glm::vec3> resolvePointWithClearance(
    const NavigationSolveView& runtime,
    const glm::vec3& point,
    const AgentClearanceProfile& profile,
    const glm::vec2& preferredTravelDirection,
    const std::atomic<bool>* cancelled
) {
    const auto isCancelled = [&]() {
        return cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed);
    };
    if (isCancelled()) {
        return std::nullopt;
    }
    if (profile.empty()) {
        return pointInsideAuthoredWalkableSurface(runtime, point)
            ? std::optional<glm::vec3>(point)
            : std::optional<glm::vec3>{};
    }
    if (pointInsideAuthoredWalkableSurfaceWithClearance(runtime, point, profile, preferredTravelDirection)) {
        return point;
    }

    glm::vec3 resolved = point;
    if (!pointInsideAuthoredWalkableSurface(runtime, resolved)) {
        const std::optional<std::size_t> nearest = findNearestCell(runtime, point);
        if (!nearest.has_value()) {
            return std::nullopt;
        }
        const NavRuntimeCell& nearestCell = runtime.bakedCells[*nearest];
        const glm::vec2 nearestPoint = closestPointOnPolygonXZ(
            glm::vec2(point.x, point.z),
            nearestCell.verticesXZ
        );
        resolved = glm::vec3(
            nearestPoint.x,
            nearestCell.elevationY,
            nearestPoint.y
        );
    }
    const glm::vec2 inwardDirection = travelDirectionForSegment(point, resolved, preferredTravelDirection);
    if (pointInsideAuthoredWalkableSurfaceWithClearance(runtime, resolved, profile, inwardDirection)) {
        return resolved;
    }

    for (int iteration = 0; iteration < kClearanceProjectionIterations; ++iteration) {
        if (isCancelled()) {
            return std::nullopt;
        }
        glm::vec2 correction(0.0f);
        bool anyOutside = false;
        bool cancelledDuringSamples = false;
        int boundarySampleCount = 0;
        visitClearanceBoundaryOffsets(
            profile,
            inwardDirection,
            kClearanceSampleDirections,
            [&](const glm::vec2& offset) {
            if (isCancelled()) {
                cancelledDuringSamples = true;
                return false;
            }
            ++boundarySampleCount;
            const glm::vec3 samplePoint(
                resolved.x + offset.x,
                resolved.y,
                resolved.z + offset.y
            );
            if (pointInsideAuthoredWalkableSurface(runtime, samplePoint)) {
                return true;
            }

            anyOutside = true;
            const float clearanceDistance = glm::length(offset);
            if (clearanceDistance <= kPlaneEpsilon) {
                return true;
            }
            const glm::vec2 direction = offset / clearanceDistance;
            float insideT = 0.0f;
            float outsideT = 1.0f;
            for (int searchStep = 0; searchStep < kClearanceBinarySearchSteps; ++searchStep) {
                const float midT = (insideT + outsideT) * 0.5f;
                const glm::vec3 midPoint(
                    resolved.x + direction.x * clearanceDistance * midT,
                    resolved.y,
                    resolved.z + direction.y * clearanceDistance * midT
                );
                if (pointInsideAuthoredWalkableSurface(runtime, midPoint)) {
                    insideT = midT;
                } else {
                    outsideT = midT;
                }
            }
            correction -= direction * (clearanceDistance * (1.0f - insideT));
            return true;
        });
        if (cancelledDuringSamples) {
            return std::nullopt;
        }

        if (!anyOutside) {
            return resolved;
        }

        const float correctionLength = glm::length(correction);
        if (correctionLength <= kPolygonEpsilon) {
            break;
        }

        const glm::vec2 delta = correction /
            static_cast<float>(std::max(boundarySampleCount, 1));
        bool advanced = false;
        float stepScale = 1.0f;
        while (stepScale >= 0.125f) {
            const glm::vec3 candidate(
                resolved.x + delta.x * stepScale,
                resolved.y,
                resolved.z + delta.y * stepScale
            );
            if (pointInsideAuthoredWalkableSurface(runtime, candidate)) {
                resolved = candidate;
                advanced = true;
                break;
            }
            stepScale *= 0.5f;
        }
        if (!advanced) {
            break;
        }
        if (pointInsideAuthoredWalkableSurfaceWithClearance(
                runtime,
                resolved,
                profile,
                travelDirectionForSegment(point, resolved, preferredTravelDirection))) {
            return resolved;
        }
    }

    std::optional<glm::vec3> bestCandidate{};
    float bestDistance = std::numeric_limits<float>::max();
    const auto considerCandidate = [&](const glm::vec3& candidate) {
        if (!pointInsideAuthoredWalkableSurfaceWithClearance(
                runtime,
                candidate,
                profile,
                travelDirectionForSegment(point, candidate, preferredTravelDirection))) {
            return;
        }
        const float distance = glm::distance(point, candidate);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestCandidate = candidate;
        }
    };
    const auto considerProjectedCandidate = [&](const glm::vec3& safeTarget) {
        if (!pointInsideAuthoredWalkableSurfaceWithClearance(
                runtime,
                safeTarget,
                profile,
                travelDirectionForSegment(point, safeTarget, preferredTravelDirection))) {
            return;
        }
        float unsafeT = 0.0f;
        float safeT = 1.0f;
        for (int searchStep = 0; searchStep < kClearanceBinarySearchSteps; ++searchStep) {
            const float midT = (unsafeT + safeT) * 0.5f;
            const glm::vec3 midPoint = point + (safeTarget - point) * midT;
            if (pointInsideAuthoredWalkableSurfaceWithClearance(
                    runtime,
                    midPoint,
                    profile,
                    travelDirectionForSegment(point, midPoint, preferredTravelDirection))) {
                safeT = midT;
            } else {
                unsafeT = midT;
            }
        }
        considerCandidate(point + (safeTarget - point) * safeT);
    };

    considerCandidate(resolved);
    considerProjectedCandidate(resolved);

    std::vector<std::pair<float, std::size_t>> nearestCells{};
    nearestCells.reserve(runtime.bakedCells.size());
    const glm::vec2 pointXZ(point.x, point.z);
    for (std::size_t cellIndex = 0u;
         cellIndex < runtime.bakedCells.size();
         ++cellIndex) {
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        const glm::vec2 closest =
            closestPointOnPolygonXZ(pointXZ, cell.verticesXZ);
        const float score =
            glm::distance(pointXZ, closest) +
            std::abs(point.y - cell.elevationY) * 2.0f;
        nearestCells.emplace_back(score, cellIndex);
    }
    const std::size_t candidateCellCount = std::min(
        nearestCells.size(),
        kMaxClearanceProjectionCandidateCells
    );
    std::partial_sort(
        nearestCells.begin(),
        nearestCells.begin() +
            static_cast<std::ptrdiff_t>(candidateCellCount),
        nearestCells.end()
    );
    for (std::size_t candidateIndex = 0u;
         candidateIndex < candidateCellCount;
         ++candidateIndex) {
        if (isCancelled()) {
            return std::nullopt;
        }
        const std::size_t cellIndex = nearestCells[candidateIndex].second;
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        const glm::vec3 cellCenter = cellCenter3(runtime, cellIndex);
        const glm::vec2 closest =
            closestPointOnPolygonXZ(pointXZ, cell.verticesXZ);
        const glm::vec2 centerXZ(cellCenter.x, cellCenter.z);
        const glm::vec2 towardCenter = centerXZ - closest;
        const float distanceToCenter = glm::length(towardCenter);
        if (distanceToCenter > kPlaneEpsilon) {
            const float insetDistance = std::min(
                distanceToCenter,
                std::max(
                    conservativeClearanceRadius(profile) * 1.25f,
                    0.05f
                )
            );
            const glm::vec2 inset =
                closest + towardCenter / distanceToCenter * insetDistance;
            const glm::vec3 insetPoint(
                inset.x,
                cell.elevationY,
                inset.y
            );
            considerCandidate(insetPoint);
            considerProjectedCandidate(insetPoint);
        }
        considerCandidate(cellCenter);
        considerProjectedCandidate(cellCenter);
    }

    const float nominalClearance = std::max(
        supportDistance(profile, glm::vec2(1.0f, 0.0f), preferredTravelDirection),
        supportDistance(profile, glm::vec2(0.0f, 1.0f), preferredTravelDirection)
    );
    float searchLimit = std::max(nominalClearance * 4.0f, 1.0f);
    if (const std::optional<std::size_t> nearest = findNearestCell(runtime, point); nearest.has_value()) {
        const NavRuntimeCell& cell = runtime.bakedCells[*nearest];
        const glm::vec2 closest =
            closestPointOnPolygonXZ(pointXZ, cell.verticesXZ);
        const glm::vec3 closestPoint(
            closest.x,
            cell.elevationY,
            closest.y
        );
        searchLimit = std::max(
            searchLimit,
            glm::distance(point, closestPoint) +
                nominalClearance * 2.0f
        );
    }

    const float radiusStep = std::max(nominalClearance * 0.25f, 0.05f);
    for (float radius = radiusStep; radius <= searchLimit; radius += radiusStep) {
        if (isCancelled()) {
            return std::nullopt;
        }
        for (int sampleIndex = 0; sampleIndex < kClearanceSampleDirections; ++sampleIndex) {
            const glm::vec2 direction = clearanceSampleDirection(sampleIndex);
            considerCandidate(glm::vec3(
                point.x + direction.x * radius,
                resolved.y,
                point.z + direction.y * radius
            ));
        }
    }
    return bestCandidate;
}

std::optional<glm::vec3> projectEndpointOntoWalkableLayer(
    const NavigationSolveView& runtime,
    const glm::vec3& point
) {
    const glm::vec2 pointXZ(point.x, point.z);
    std::optional<glm::vec3> projected{};
    float nearestVerticalDistance = kEndpointProjectionTolerance +
        kPolygonEpsilon;
    for (std::size_t cellIndex = 0u;
         cellIndex < runtime.bakedCells.size();
         ++cellIndex) {
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        const float verticalDistance =
            std::abs(point.y - cell.elevationY);
        if (verticalDistance > nearestVerticalDistance) {
            continue;
        }
        if (cellIndex < runtime.bakedCellMinXZ.size() &&
            cellIndex < runtime.bakedCellMaxXZ.size() &&
            (pointXZ.x < runtime.bakedCellMinXZ[cellIndex].x -
                    kPolygonEpsilon ||
             pointXZ.x > runtime.bakedCellMaxXZ[cellIndex].x +
                    kPolygonEpsilon ||
             pointXZ.y < runtime.bakedCellMinXZ[cellIndex].y -
                    kPolygonEpsilon ||
             pointXZ.y > runtime.bakedCellMaxXZ[cellIndex].y +
                    kPolygonEpsilon)) {
            continue;
        }
        if (!pointInOrOnPolygonXZ(pointXZ, cell.verticesXZ)) {
            continue;
        }
        nearestVerticalDistance = verticalDistance;
        projected = glm::vec3(point.x, cell.elevationY, point.z);
    }
    return projected;
}


}  // namespace core::navigation_detail
