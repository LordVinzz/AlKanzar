#pragma once

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "core/navigation/NavigationDetailTypes.hpp"

namespace core::navigation_detail {

bool pointInPolygonXZ(const glm::vec2& point, const std::vector<glm::vec2>& polygon);
bool pointOnSegmentXZ(const glm::vec2& point, const glm::vec2& a, const glm::vec2& b);
bool pointInOrOnPolygonXZ(const glm::vec2& point, const std::vector<glm::vec2>& polygon);
glm::vec2 closestPointOnSegmentXZ(
    const glm::vec2& point,
    const glm::vec2& segmentStart,
    const glm::vec2& segmentEnd
);
glm::vec2 closestPointOnPolygonXZ(const glm::vec2& point, const std::vector<glm::vec2>& polygon);
bool pointInTriangleXZ(const glm::vec2& point, const WalkableTriangle& tri);
float polygonSignedArea(const std::vector<glm::vec2>& vertices);
bool polygonHasArea(const std::vector<glm::vec2>& vertices);
std::vector<glm::vec2> normalizePolygonVertices(const std::vector<glm::vec2>& vertices);
glm::vec2 polygonCentroidXZ(const std::vector<glm::vec2>& vertices);
bool polygonValid(const NavPolygon& polygon);
bool polygonIsSimple(const std::vector<glm::vec2>& vertices);
std::vector<std::array<glm::vec2, 3u>> triangulateSimplePolygon(
    const std::vector<glm::vec2>& polygon
);

bool blockerOverlapsLayer(const BlockingFootprint& blocker, float elevationY);
std::optional<BlockingFootprint> makeBlockingFootprint(
    std::vector<glm::vec2> vertices,
    float minY,
    float maxY
);
bool boundsOverlapXZ(
    const glm::vec2& lhsMin,
    const glm::vec2& lhsMax,
    const glm::vec2& rhsMin,
    const glm::vec2& rhsMax
);
std::pair<glm::vec2, glm::vec2> polygonBoundsXZ(const std::vector<glm::vec2>& polygon);
std::vector<std::vector<glm::vec2>> subtractConvexPolygon(
    const std::vector<glm::vec2>& subject,
    const std::vector<glm::vec2>& clipperVertices,
    const glm::vec2& clipperMin,
    const glm::vec2& clipperMax
);
std::vector<std::vector<glm::vec2>> subtractConvexPolygon(
    const std::vector<glm::vec2>& subject,
    const BlockingFootprint& clipper
);
bool convexPolygonsHaveInteriorOverlap(
    const std::vector<glm::vec2>& lhs,
    const std::vector<glm::vec2>& rhs
);
bool convexCellSetHasInteriorOverlap(const std::vector<NavRuntimeCell>& cells);

}  // namespace core::navigation_detail
