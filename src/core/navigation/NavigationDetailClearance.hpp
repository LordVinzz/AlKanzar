#pragma once

#include <atomic>
#include <optional>
#include <vector>

#include "core/navigation/NavigationDetailTypes.hpp"

namespace core::navigation_detail {

glm::vec2 travelDirectionForSegment(
    const glm::vec3& from,
    const glm::vec3& to,
    const glm::vec2& fallback = glm::vec2(0.0f, 1.0f)
);
float conservativeClearanceRadius(const AgentClearanceProfile& profile);
AgentClearanceProfile headingIndependentNodeClearance(const AgentClearanceProfile& profile);
std::optional<SharedPortalResult> shrinkPortal(
    const glm::vec2& a,
    const glm::vec2& b,
    const AgentClearanceProfile& profile,
    const glm::vec2& travelDirection
);
bool portalIsInternalToSharedAuthoredPolygon(
    const NavigationSolveView& runtime,
    std::size_t cellA,
    std::size_t cellB,
    const SharedPortalResult& portal
);
glm::vec2 clearanceSampleDirection(
    int sampleIndex,
    int sampleCount = kClearanceSampleDirections
);

bool pointInsideAuthoredWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& point
);
bool segmentInsideAuthoredWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to
);
bool boxSweepInsideWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile,
    const glm::vec2& preferredTravelDirection,
    const std::vector<std::size_t>* candidateCells = nullptr
);
bool sphereSweepInsideWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile,
    const glm::vec2& preferredTravelDirection,
    const std::vector<std::size_t>* candidateCells = nullptr
);
std::optional<std::size_t> findNearestCell(
    const NavigationSolveView& runtime,
    const glm::vec3& point
);
bool pointInsideAuthoredWalkableSurfaceWithClearance(
    const NavigationSolveView& runtime,
    const glm::vec3& point,
    const AgentClearanceProfile& profile,
    const glm::vec2& travelDirection
);
std::optional<glm::vec3> resolvePointWithClearance(
    const NavigationSolveView& runtime,
    const glm::vec3& point,
    const AgentClearanceProfile& profile,
    const glm::vec2& preferredTravelDirection = glm::vec2(0.0f, 1.0f),
    const std::atomic<bool>* cancelled = nullptr
);
std::optional<glm::vec3> projectEndpointOntoWalkableLayer(
    const NavigationSolveView& runtime,
    const glm::vec3& point
);

}  // namespace core::navigation_detail
