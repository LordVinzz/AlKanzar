#include "core/navigation/NavigationDetailAgents.hpp"
#include "core/navigation/NavigationDetailPath.hpp"
#include "core/navigation/NavigationDetailTraversal.hpp"

#include <cmath>

namespace core::navigation_detail {

std::vector<glm::vec3> trimPathCornersFromCurrentPosition(
    const NavigationSolveView& runtime,
    const glm::vec3& currentPosition,
    std::vector<glm::vec3> corners,
    float arrivalRadius,
    const AgentClearanceProfile& profile
) {
    while (!corners.empty() && glm::distance(currentPosition, corners.front()) <= arrivalRadius) {
        corners.erase(corners.begin());
    }
    for (std::size_t candidateIndex = corners.size(); candidateIndex-- > 0u;) {
        if (segmentInsideAuthoredWalkableSurfaceWithClearance(runtime, currentPosition, corners[candidateIndex], profile)) {
            corners.erase(corners.begin(), corners.begin() + static_cast<std::ptrdiff_t>(candidateIndex));
            break;
        }
    }
    return corners;
}
bool shortestYawStep(float currentYaw, float desiredYaw, float maxStep, float& outYaw) {
    float delta = std::fmod(desiredYaw - currentYaw, 360.0f);
    if (delta > 180.0f) {
        delta -= 360.0f;
    } else if (delta < -180.0f) {
        delta += 360.0f;
    }
    if (std::abs(delta) <= maxStep) {
        outYaw = desiredYaw;
        return true;
    }
    outYaw = currentYaw + std::copysign(maxStep, delta);
    return false;
}

}  // namespace core::navigation_detail

