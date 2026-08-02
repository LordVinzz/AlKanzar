#include "core/navigation/NavigationDetailClearance.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace core::navigation_detail {

AgentClearanceProfile resolveAgentClearanceProfile(const World& world, EntityId entity, const NavAgentComponent& agent) {
    if (agent.clearanceSource == NavAgentClearanceSource::None) {
        return AgentClearanceProfile{};
    }

    const TransformComponent* transform = world.transforms.tryGet(entity);
    if (transform == nullptr) {
        return AgentClearanceProfile{};
    }

    const SphereColliderComponent* sphere = world.sphereColliders.tryGet(entity);
    const BoxColliderComponent* box = world.boxColliders.tryGet(entity);
    switch (agent.clearanceSource) {
        case NavAgentClearanceSource::SphereCollider:
            if (sphere != nullptr) {
                return AgentClearanceProfile{
                    AgentClearanceShape::Sphere,
                    glm::vec2(sphere->center.x * transform->scale.x, sphere->center.z * transform->scale.z),
                    sphere->radius * planarAbsMax(*transform),
                    glm::vec2(0.0f)
                };
            }
            return AgentClearanceProfile{};
        case NavAgentClearanceSource::BoxCollider:
            if (box != nullptr) {
                return AgentClearanceProfile{
                    AgentClearanceShape::Box,
                    glm::vec2(box->center.x * transform->scale.x, box->center.z * transform->scale.z),
                    0.0f,
                    glm::vec2(
                        std::abs(box->halfExtents.x * transform->scale.x),
                        std::abs(box->halfExtents.z * transform->scale.z)
                    )
                };
            }
            return AgentClearanceProfile{};
        case NavAgentClearanceSource::Auto:
            if (sphere != nullptr) {
                return AgentClearanceProfile{
                    AgentClearanceShape::Sphere,
                    glm::vec2(sphere->center.x * transform->scale.x, sphere->center.z * transform->scale.z),
                    sphere->radius * planarAbsMax(*transform),
                    glm::vec2(0.0f)
                };
            }
            if (box != nullptr) {
                return AgentClearanceProfile{
                    AgentClearanceShape::Box,
                    glm::vec2(box->center.x * transform->scale.x, box->center.z * transform->scale.z),
                    0.0f,
                    glm::vec2(
                        std::abs(box->halfExtents.x * transform->scale.x),
                        std::abs(box->halfExtents.z * transform->scale.z)
                    )
                };
            }
            return AgentClearanceProfile{};
        case NavAgentClearanceSource::None:
        default:
            return AgentClearanceProfile{};
    }
}

glm::vec2 travelDirectionForSegment(const glm::vec3& from, const glm::vec3& to, const glm::vec2& fallback) {
    return normalizeOrFallback(glm::vec2(to.x - from.x, to.z - from.z), fallback);
}

float conservativeClearanceRadius(const AgentClearanceProfile& profile) {
    if (profile.shape == AgentClearanceShape::Sphere) {
        return glm::length(profile.centerXZ) + profile.sphereRadius;
    }
    if (profile.shape == AgentClearanceShape::Box) {
        return glm::length(profile.centerXZ) + glm::length(profile.boxHalfExtentsXZ);
    }
    return 0.0f;
}

AgentClearanceProfile headingIndependentNodeClearance(
    const AgentClearanceProfile& profile
) {
    if (profile.empty()) {
        return {};
    }
    const float minimumExtent =
        profile.shape == AgentClearanceShape::Sphere
        ? profile.sphereRadius
        : std::min(
            profile.boxHalfExtentsXZ.x,
            profile.boxHalfExtentsXZ.y
        );
    const float inscribedRadius = std::max(
        0.0f,
        minimumExtent - glm::length(profile.centerXZ)
    );
    if (inscribedRadius <= kPolygonEpsilon) {
        return {};
    }
    return AgentClearanceProfile{
        AgentClearanceShape::Sphere,
        glm::vec2(0.0f),
        inscribedRadius,
        glm::vec2(0.0f)
    };
}

std::optional<SharedPortalResult> shrinkPortal(
    const glm::vec2& a,
    const glm::vec2& b,
    const AgentClearanceProfile& profile,
    const glm::vec2& travelDirection
) {
    if (profile.empty()) {
        return SharedPortalResult{a, b};
    }

    const glm::vec2 delta = b - a;
    const float length = glm::length(delta);
    if (length <= kPolygonEpsilon) {
        return std::nullopt;
    }
    const glm::vec2 direction = delta / length;
    const glm::vec2 forward = normalizeOrFallback(travelDirection);
    const glm::vec2 right(forward.y, -forward.x);
    const glm::vec2 center =
        rotateLocalXZToPlanar(profile.centerXZ, forward);
    const float centerProjection = glm::dot(direction, center);
    const float symmetricExtent =
        profile.shape == AgentClearanceShape::Sphere
        ? profile.sphereRadius
        : std::abs(glm::dot(direction, right)) *
                profile.boxHalfExtentsXZ.x +
            std::abs(glm::dot(direction, forward)) *
                profile.boxHalfExtentsXZ.y;
    // The collider center may be offset from the agent origin. Its feasible
    // origin interval is therefore asymmetric along the portal.
    const float startInset = std::max(
        0.0f,
        symmetricExtent - centerProjection
    );
    const float endInset = std::max(
        0.0f,
        symmetricExtent + centerProjection
    );
    if (length <= startInset + endInset + kPolygonEpsilon) {
        return std::nullopt;
    }
    return SharedPortalResult{
        a + direction * startInset,
        b - direction * endInset
    };
}

bool portalIsInternalToSharedAuthoredPolygon(
    const NavigationSolveView& runtime,
    std::size_t cellA,
    std::size_t cellB,
    const SharedPortalResult& portal
) {
    if (cellA >= runtime.cellToPolygonIndices.size() || cellB >= runtime.cellToPolygonIndices.size()) {
        return false;
    }
    const glm::vec2 portalMidpoint = (portal.a + portal.b) * 0.5f;
    const std::vector<std::size_t>& polyA = runtime.cellToPolygonIndices[cellA];
    const std::vector<std::size_t>& polyB = runtime.cellToPolygonIndices[cellB];
    for (std::size_t indexA : polyA) {
        for (std::size_t indexB : polyB) {
            if (indexA != indexB ||
                indexA >= runtime.asset.polygons.size()) {
                continue;
            }
            const std::vector<glm::vec2>& authoredVertices =
                runtime.asset.polygons[indexA].verticesXZ;
            if (pointInOrOnPolygonXZ(
                    portalMidpoint,
                    authoredVertices)) {
                return true;
            }
        }
    }
    return false;
}

// Forward declarations for functions defined later in the file
bool pointInsideAuthoredWalkableSurface(const NavigationSolveView& runtime, const glm::vec3& point);
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
    const std::vector<std::size_t>* candidateCells
);
bool sphereSweepInsideWalkableSurface(
    const NavigationSolveView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const AgentClearanceProfile& profile,
    const glm::vec2& preferredTravelDirection,
    const std::vector<std::size_t>* candidateCells
);
template <typename RuntimeView>
bool segmentInsideBakedWalkableSurface(
    const RuntimeView& runtime,
    const glm::vec3& from,
    const glm::vec3& to,
    const std::vector<std::size_t>* candidateCells = nullptr
);
std::optional<std::size_t> findNearestCell(const NavigationSolveView& runtime, const glm::vec3& point);


}  // namespace core::navigation_detail
