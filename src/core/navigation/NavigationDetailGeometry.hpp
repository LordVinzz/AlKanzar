#pragma once

#include <array>
#include <cmath>
#include <string>

#include "core/navigation/NavigationDetailTypes.hpp"

namespace core::navigation_detail {

float cross2(const glm::vec2& lhs, const glm::vec2& rhs);
float triArea2(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c);
double preciseTriArea2(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c);
double funnelArea2(const glm::vec2& apex, const glm::vec2& side, const glm::vec2& candidate);
double funnelAreaTolerance(const glm::vec2& apex, const glm::vec2& side, const glm::vec2& candidate);

bool nearlyEqual(float lhs, float rhs, float epsilon = kPolygonEpsilon);
bool nearlyEqualVec2(const glm::vec2& lhs, const glm::vec2& rhs, float epsilon = kPolygonEpsilon);
bool nearlyEqualVec3(const glm::vec3& lhs, const glm::vec3& rhs, float epsilon = kPolygonEpsilon);
QuantizedVec2 quantizeVec2(const glm::vec2& value);
QuantizedLayerPoint quantizeLayerPoint(const glm::vec2& point, float elevation);
ExactLayerPoint exactLayerPoint(const glm::vec3& point);
std::string lowercaseCopy(std::string value);
std::string entityName(const World& world, EntityId entity);
float planarAbsMax(const TransformComponent& transform);
glm::vec2 normalizeOrFallback(
    const glm::vec2& vector,
    const glm::vec2& fallback = glm::vec2(0.0f, 1.0f)
);
glm::vec2 rotateLocalXZToPlanar(const glm::vec2& local, const glm::vec2& forward);
glm::vec2 clearanceOrientationForward(
    const AgentClearanceProfile& profile,
    const glm::vec2& requestedForward
);
float supportDistance(
    const AgentClearanceProfile& profile,
    const glm::vec2& sampleDirection,
    const glm::vec2& travelDirection
);
glm::vec3 cellCenter3(const NavigationSolveView& runtime, std::size_t cellIndex);
std::vector<glm::vec2> clipConvexPolygonAgainstHalfPlane(
    const std::vector<glm::vec2>& polygon,
    const glm::vec2& lineA,
    const glm::vec2& lineB,
    bool keepLeft,
    float tolerance = kPolygonEpsilon
);
std::string canonicalPolygonKey(const std::vector<glm::vec2>& vertices);
std::vector<glm::vec2> buildConvexHull(std::vector<glm::vec2> points);
void mergeAdjacentConvexCells(std::vector<NavRuntimeCell>& cells);
std::vector<SharedPortalResult> sharedBoundaryPortals(
    const NavRuntimeCell& lhs,
    const NavRuntimeCell& rhs
);
glm::vec2 closestPointOnPolygonXZ(
    const glm::vec2& point,
    const std::vector<glm::vec2>& polygon
);
bool pointInOrOnPolygonXZ(
    const glm::vec2& point,
    const std::vector<glm::vec2>& polygon
);
AgentClearanceProfile resolveAgentClearanceProfile(
    const World& world,
    EntityId entity,
    const NavAgentComponent& agent
);

template <typename Visitor>
bool visitClearanceBoundaryOffsets(
    const AgentClearanceProfile& profile,
    const glm::vec2& travelDirection,
    int sphereSampleDirections,
    Visitor&& visitor
) {
    if (profile.empty()) {
        return true;
    }

    const glm::vec2 forward = clearanceOrientationForward(
        profile,
        travelDirection
    );
    const glm::vec2 right(forward.y, -forward.x);
    const glm::vec2 center = rotateLocalXZToPlanar(
        profile.centerXZ,
        forward
    );
    if (profile.shape == AgentClearanceShape::Sphere) {
        for (int sampleIndex = 0; sampleIndex < sphereSampleDirections; ++sampleIndex) {
            const float angle = kTau * static_cast<float>(sampleIndex) /
                static_cast<float>(sphereSampleDirections);
            const glm::vec2 offset = center +
                glm::vec2(std::cos(angle), std::sin(angle)) * profile.sphereRadius;
            if (!visitor(offset)) {
                return false;
            }
        }
        return true;
    }

    const glm::vec2 lateral = right * profile.boxHalfExtentsXZ.x;
    const glm::vec2 longitudinal = forward * profile.boxHalfExtentsXZ.y;
    const std::array<glm::vec2, 4u> offsets{
        center + lateral + longitudinal,
        center + lateral - longitudinal,
        center - lateral + longitudinal,
        center - lateral - longitudinal,
    };
    for (const glm::vec2& offset : offsets) {
        if (!visitor(offset)) {
            return false;
        }
    }
    return true;
}

}  // namespace core::navigation_detail
