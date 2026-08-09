#include "core/navigation/NavigationDetailAgents.hpp"
#include "core/navigation/NavigationDetailPath.hpp"
#include "core/navigation/NavigationDetailTraversal.hpp"

#include <algorithm>
#include <cmath>

namespace core::navigation_detail {

bool canAdvancePathCorner(
    const NavigationSolveView& runtime,
    const glm::vec3& currentPosition,
    const std::vector<glm::vec3>& corners,
    float arrivalRadius,
    const AgentClearanceProfile& profile
) {
    if (corners.empty() ||
        glm::distance(currentPosition, corners.front()) > arrivalRadius) {
        return false;
    }
    if (corners.size() == 1u) {
        return true;
    }

    const glm::vec3& nextCorner = corners[1u];
    return segmentInsideAuthoredWalkableSurfaceWithClearance(
        runtime,
        currentPosition,
        nextCorner,
        profile
    ) ||
        segmentMatchesRuntimeLink(
            runtime,
            currentPosition,
            nextCorner,
            std::max(arrivalRadius, kPolygonEpsilon * 8.0f)
        );
}

std::vector<glm::vec3> trimPathCornersFromCurrentPosition(
    const NavigationSolveView& runtime,
    const glm::vec3& currentPosition,
    std::vector<glm::vec3> corners,
    float arrivalRadius,
    const AgentClearanceProfile& profile
) {
    while (canAdvancePathCorner(
            runtime,
            currentPosition,
            corners,
            arrivalRadius,
            profile)) {
        corners.erase(corners.begin());
    }
    for (std::size_t candidateIndex = corners.size(); candidateIndex-- > 0u;) {
        if (segmentInsideAuthoredWalkableSurfaceWithClearance(
                runtime,
                currentPosition,
                corners[candidateIndex],
                profile)) {
            corners.erase(
                corners.begin(),
                corners.begin() +
                    static_cast<std::ptrdiff_t>(candidateIndex)
            );
            break;
        }
    }
    return corners;
}

float interpolateYawShortestPath(
    float currentYaw,
    float desiredYaw,
    float maxStep,
    float interpolationAlpha
) {
    float delta = std::fmod(desiredYaw - currentYaw, 360.0f);
    if (delta > 180.0f) {
        delta -= 360.0f;
    } else if (delta < -180.0f) {
        delta += 360.0f;
    }
    if (std::abs(delta) <= kPlaneEpsilon) {
        return currentYaw;
    }
    const float step = std::min(
        std::abs(delta) * std::clamp(interpolationAlpha, 0.0f, 1.0f),
        std::max(maxStep, 0.0f)
    );
    return currentYaw + std::copysign(step, delta);
}

}  // namespace core::navigation_detail
