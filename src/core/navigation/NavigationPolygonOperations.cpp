#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"

#include <algorithm>
#include <cmath>

namespace core::navigation_detail {

bool blockerOverlapsLayer(const BlockingFootprint& blocker, float elevationY) {
    return elevationY >= blocker.minY - kLayerGroupingEpsilon &&
        elevationY <= blocker.maxY + kLayerGroupingEpsilon;
}

std::optional<BlockingFootprint> makeBlockingFootprint(
    std::vector<glm::vec2> vertices,
    float minY,
    float maxY
) {
    vertices = normalizePolygonVertices(vertices);
    if (!polygonHasArea(vertices)) {
        return std::nullopt;
    }

    BlockingFootprint footprint{};
    footprint.minY = std::min(minY, maxY);
    footprint.maxY = std::max(minY, maxY);
    footprint.verticesXZ = std::move(vertices);
    footprint.minXZ = footprint.verticesXZ.front();
    footprint.maxXZ = footprint.verticesXZ.front();
    for (const glm::vec2& vertex : footprint.verticesXZ) {
        footprint.minXZ = glm::min(footprint.minXZ, vertex);
        footprint.maxXZ = glm::max(footprint.maxXZ, vertex);
    }
    return footprint;
}

bool boundsOverlapXZ(
    const glm::vec2& lhsMin,
    const glm::vec2& lhsMax,
    const glm::vec2& rhsMin,
    const glm::vec2& rhsMax
) {
    return lhsMax.x >= rhsMin.x - kPolygonEpsilon &&
        rhsMax.x >= lhsMin.x - kPolygonEpsilon &&
        lhsMax.y >= rhsMin.y - kPolygonEpsilon &&
        rhsMax.y >= lhsMin.y - kPolygonEpsilon;
}

std::pair<glm::vec2, glm::vec2> polygonBoundsXZ(const std::vector<glm::vec2>& polygon) {
    glm::vec2 minPoint = polygon.front();
    glm::vec2 maxPoint = polygon.front();
    for (const glm::vec2& vertex : polygon) {
        minPoint = glm::min(minPoint, vertex);
        maxPoint = glm::max(maxPoint, vertex);
    }
    return {minPoint, maxPoint};
}

std::vector<std::vector<glm::vec2>> subtractConvexPolygon(
    const std::vector<glm::vec2>& subject,
    const std::vector<glm::vec2>& clipperVertices,
    const glm::vec2& clipperMin,
    const glm::vec2& clipperMax
) {
    if (!polygonHasArea(subject)) {
        return {};
    }
    const auto [subjectMin, subjectMax] = polygonBoundsXZ(subject);
    if (!boundsOverlapXZ(
            subjectMin,
            subjectMax,
            clipperMin,
            clipperMax)) {
        return {subject};
    }

    std::vector<std::vector<glm::vec2>> outsidePieces{};
    std::vector<glm::vec2> intersection = subject;
    // Each outside piece satisfies all prior clipper half-planes, so the
    // pieces are convex and non-overlapping while their union is subject - clipper.
    for (std::size_t edgeIndex = 0; edgeIndex < clipperVertices.size(); ++edgeIndex) {
        const glm::vec2& edgeA = clipperVertices[edgeIndex];
        const glm::vec2& edgeB = clipperVertices[
            (edgeIndex + 1u) % clipperVertices.size()
        ];

        std::vector<glm::vec2> outside =
            clipConvexPolygonAgainstHalfPlane(
                intersection,
                edgeA,
                edgeB,
                false,
                0.0f
            );
        if (polygonHasArea(outside)) {
            outsidePieces.push_back(std::move(outside));
        }
        intersection = clipConvexPolygonAgainstHalfPlane(
            intersection,
            edgeA,
            edgeB,
            true,
            0.0f
        );
        if (!polygonHasArea(intersection)) {
            break;
        }
    }
    return outsidePieces;
}

std::vector<std::vector<glm::vec2>> subtractConvexPolygon(
    const std::vector<glm::vec2>& subject,
    const BlockingFootprint& clipper
) {
    return subtractConvexPolygon(
        subject,
        clipper.verticesXZ,
        clipper.minXZ,
        clipper.maxXZ
    );
}

bool convexPolygonsHaveInteriorOverlap(
    const std::vector<glm::vec2>& lhs,
    const std::vector<glm::vec2>& rhs
) {
    if (!polygonHasArea(lhs) || !polygonHasArea(rhs)) {
        return false;
    }
    const auto removedArea = [](
        const std::vector<glm::vec2>& subject,
        const std::vector<glm::vec2>& clipper
    ) {
        const auto [clipperMin, clipperMax] = polygonBoundsXZ(clipper);
        const double subjectArea = std::abs(
            static_cast<double>(polygonSignedArea(subject))
        );
        double outsideArea = 0.0;
        for (const std::vector<glm::vec2>& piece : subtractConvexPolygon(
                 subject,
                 clipper,
                 clipperMin,
                 clipperMax)) {
            outsideArea += std::abs(
                static_cast<double>(polygonSignedArea(piece))
            );
        }
        return std::max(0.0, subjectArea - outsideArea);
    };
    const auto perimeter = [](const std::vector<glm::vec2>& polygon) {
        double result = 0.0;
        for (std::size_t index = 0u; index < polygon.size(); ++index) {
            result += glm::distance(
                polygon[index],
                polygon[(index + 1u) % polygon.size()]
            );
        }
        return result;
    };
    const double areaTolerance = static_cast<double>(kPolygonEpsilon) *
        std::max({1.0, perimeter(lhs), perimeter(rhs)}) * 2.0;
    return std::max(removedArea(lhs, rhs), removedArea(rhs, lhs)) >
        areaTolerance;
}

bool convexCellSetHasInteriorOverlap(
    const std::vector<NavRuntimeCell>& cells
) {
    for (std::size_t lhs = 0u; lhs < cells.size(); ++lhs) {
        const auto [lhsMin, lhsMax] =
            polygonBoundsXZ(cells[lhs].verticesXZ);
        for (std::size_t rhs = lhs + 1u; rhs < cells.size(); ++rhs) {
            if (std::abs(cells[lhs].elevationY - cells[rhs].elevationY) >
                    kLayerGroupingEpsilon) {
                continue;
            }
            const auto [rhsMin, rhsMax] =
                polygonBoundsXZ(cells[rhs].verticesXZ);
            if (boundsOverlapXZ(lhsMin, lhsMax, rhsMin, rhsMax) &&
                convexPolygonsHaveInteriorOverlap(
                    cells[lhs].verticesXZ,
                    cells[rhs].verticesXZ)) {
                return true;
            }
        }
    }
    return false;
}


}  // namespace core::navigation_detail
