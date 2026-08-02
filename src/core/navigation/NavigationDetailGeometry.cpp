#include "core/navigation/NavigationDetailGeometry.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>

#include <glm/geometric.hpp>

namespace core::navigation_detail {

float cross2(const glm::vec2& lhs, const glm::vec2& rhs) {
    return lhs.x * rhs.y - lhs.y * rhs.x;
}

float triArea2(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    return cross2(b - a, c - a);
}

double preciseTriArea2(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c) {
    const double abX = static_cast<double>(b.x) - a.x;
    const double abY = static_cast<double>(b.y) - a.y;
    const double acX = static_cast<double>(c.x) - a.x;
    const double acY = static_cast<double>(c.y) - a.y;
    return abX * acY - abY * acX;
}

double funnelArea2(const glm::vec2& apex, const glm::vec2& side, const glm::vec2& candidate) {
    const double sideX = static_cast<double>(side.x) - apex.x;
    const double sideY = static_cast<double>(side.y) - apex.y;
    const double candidateX = static_cast<double>(candidate.x) - apex.x;
    const double candidateY = static_cast<double>(candidate.y) - apex.y;
    return sideX * candidateY - sideY * candidateX;
}

double funnelAreaTolerance(const glm::vec2& apex, const glm::vec2& side, const glm::vec2& candidate) {
    const double sideLength = glm::distance(glm::dvec2(apex), glm::dvec2(side));
    const double candidateLength = glm::distance(glm::dvec2(apex), glm::dvec2(candidate));
    return 1.0e-10 * std::max(1.0, sideLength * candidateLength);
}

bool nearlyEqual(float lhs, float rhs, float epsilon) {
    return std::abs(lhs - rhs) <= epsilon;
}

bool nearlyEqualVec2(const glm::vec2& lhs, const glm::vec2& rhs, float epsilon) {
    return glm::length(lhs - rhs) <= epsilon;
}

bool nearlyEqualVec3(const glm::vec3& lhs, const glm::vec3& rhs, float epsilon) {
    return glm::length(lhs - rhs) <= epsilon;
}

QuantizedVec2 quantizeVec2(const glm::vec2& value) {
    return {
        static_cast<long long>(std::llround(static_cast<double>(value.x) * 10000.0)),
        static_cast<long long>(std::llround(static_cast<double>(value.y) * 10000.0))
    };
}

QuantizedLayerPoint quantizeLayerPoint(const glm::vec2& point, float elevation) {
    return {quantizeVec2(point), static_cast<long long>(std::llround(static_cast<double>(elevation) * 10000.0))};
}

ExactLayerPoint exactLayerPoint(const glm::vec3& point) {
    return {
        std::bit_cast<std::uint32_t>(point.x),
        std::bit_cast<std::uint32_t>(point.y),
        std::bit_cast<std::uint32_t>(point.z),
    };
}

std::string lowercaseCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string entityName(const World& world, EntityId entity) {
    if (const NameComponent* name = world.names.tryGet(entity)) {
        return name->value;
    }
    return "Entity " + std::to_string(entity.index);
}

float planarAbsMax(const TransformComponent& transform) {
    return std::max(std::abs(transform.scale.x), std::abs(transform.scale.z));
}

glm::vec2 normalizeOrFallback(const glm::vec2& vector, const glm::vec2& fallback) {
    const float length = glm::length(vector);
    return length > kPlaneEpsilon ? vector / length : fallback;
}

glm::vec2 rotateLocalXZToPlanar(const glm::vec2& local, const glm::vec2& forward) {
    const glm::vec2 safeForward = normalizeOrFallback(forward);
    const glm::vec2 right(safeForward.y, -safeForward.x);
    return right * local.x + safeForward * local.y;
}

float supportDistance(
    const AgentClearanceProfile& profile,
    const glm::vec2& sampleDirection,
    const glm::vec2& travelDirection
) {
    if (profile.empty()) {
        return 0.0f;
    }

    const glm::vec2 direction = normalizeOrFallback(sampleDirection);
    if (profile.shape == AgentClearanceShape::Sphere) {
        const glm::vec2 center = rotateLocalXZToPlanar(profile.centerXZ, travelDirection);
        return std::abs(glm::dot(direction, center)) + profile.sphereRadius;
    }

    const glm::vec2 forward = normalizeOrFallback(travelDirection);
    const glm::vec2 right(forward.y, -forward.x);
    const glm::vec2 center = rotateLocalXZToPlanar(profile.centerXZ, travelDirection);
    return std::abs(glm::dot(direction, center)) +
        std::abs(glm::dot(direction, right)) * profile.boxHalfExtentsXZ.x +
        std::abs(glm::dot(direction, forward)) * profile.boxHalfExtentsXZ.y;
}

}  // namespace core::navigation_detail
