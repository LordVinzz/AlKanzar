#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"
#include "core/navigation/NavigationDetailSurface.hpp"
#include "core/navigation/NavigationDetailTraversal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace core::navigation_detail {

bool boxSweepInsideWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile,
    const glm::vec2& preferredTravelDirection,
    const std::vector<std::size_t>* candidateCells
) {
    if (profile.shape != AgentClearanceShape::Box ||
        std::abs(from.y - to.y) > kLayerGroupingEpsilon) {
        return false;
    }

    const glm::vec2 forward = travelDirectionForSegment(
        from,
        to,
        preferredTravelDirection
    );
    const glm::vec2 right(forward.y, -forward.x);
    const glm::vec2 centerOffset =
        rotateLocalXZToPlanar(profile.centerXZ, forward);
    const glm::vec2 startCenter(from.x, from.z);
    const glm::vec2 endCenter(to.x, to.z);
    const glm::vec2 lateral = right * profile.boxHalfExtentsXZ.x;
    const glm::vec2 longitudinal = forward * profile.boxHalfExtentsXZ.y;
    const glm::vec2 back = startCenter + centerOffset - longitudinal;
    const glm::vec2 front = endCenter + centerOffset + longitudinal;
    const std::vector<glm::vec2> swept{
        back + lateral,
        front + lateral,
        front - lateral,
        back - lateral,
    };
    for (std::size_t edgeIndex = 0u;
         edgeIndex < swept.size();
         ++edgeIndex) {
        const glm::vec2& edgeStart = swept[edgeIndex];
        const glm::vec2& edgeEnd =
            swept[(edgeIndex + 1u) % swept.size()];
        if (!segmentInsideBakedWalkableSurface(
                runtime,
                glm::vec3(edgeStart.x, from.y, edgeStart.y),
                glm::vec3(edgeEnd.x, from.y, edgeEnd.y),
                candidateCells)) {
            return false;
        }
    }
    return convexFootprintInsideWalkableSurface(
        runtime,
        swept,
        from.y,
        candidateCells
    );
}

bool sphereSweepInsideWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile,
    const glm::vec2& preferredTravelDirection,
    const std::vector<std::size_t>* candidateCells
) {
    if (profile.shape != AgentClearanceShape::Sphere ||
        std::abs(from.y - to.y) > kLayerGroupingEpsilon) {
        return false;
    }

    constexpr int kCapsuleSides = 32;
    const glm::vec2 forward = travelDirectionForSegment(
        from,
        to,
        preferredTravelDirection
    );
    const glm::vec2 centerOffset =
        rotateLocalXZToPlanar(profile.centerXZ, forward);
    const glm::vec2 startCenter(from.x, from.z);
    const glm::vec2 endCenter(to.x, to.z);
    const float radius = std::max(0.0f, profile.sphereRadius);
    if (radius <= kPolygonEpsilon) {
        return true;
    }

    // A regular polygon whose inradius is the collider radius contains the
    // exact disk. Its Minkowski sum with the travelled segment is therefore a
    // conservative capsule: area coverage cannot miss a hole between samples.
    const float circumradius = radius / std::cos(
        3.14159265358979323846f / static_cast<float>(kCapsuleSides)
    );
    std::vector<glm::vec2> capsulePoints{};
    capsulePoints.reserve(static_cast<std::size_t>(kCapsuleSides) * 2u);
    for (int index = 0; index < kCapsuleSides; ++index) {
        // Half-step rotation makes the polygon sides (rather than protruding
        // vertices) tangent on the world axes. The polygon still contains the
        // exact disk, while an exact circle tangent to an axis-aligned navmesh
        // boundary remains a valid placement.
        const float angle = kTau *
            (static_cast<float>(index) + 0.5f) /
            static_cast<float>(kCapsuleSides);
        const glm::vec2 radial(
            std::cos(angle) * circumradius,
            std::sin(angle) * circumradius
        );
        capsulePoints.push_back(startCenter + centerOffset + radial);
        capsulePoints.push_back(endCenter + centerOffset + radial);
    }
    const std::vector<glm::vec2> capsule = buildConvexHull(
        std::move(capsulePoints)
    );
    return convexFootprintInsideWalkableSurface(
        runtime,
        capsule,
        from.y,
        candidateCells
    );
}


}  // namespace core::navigation_detail
