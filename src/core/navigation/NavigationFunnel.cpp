#include "core/navigation/NavigationDetailCorridor.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailRuntime.hpp"

#include <optional>
#include <vector>

namespace core::navigation_detail {

struct OrientedFunnelPortal {
    glm::vec2 left{0.0f};
    glm::vec2 right{0.0f};
};

OrientedFunnelPortal orientPortalForCorridor(
    const NavigationSolveView&,
    std::size_t,
    std::size_t,
    const SharedPortalResult& portal
) {
    return OrientedFunnelPortal{portal.a, portal.b};
}

std::vector<glm::vec2> pullFunnelCorners(
    const glm::vec2& start,
    const std::vector<OrientedFunnelPortal>& corridorPortals,
    const glm::vec2& destination
) {
    std::vector<OrientedFunnelPortal> portals{};
    portals.reserve(corridorPortals.size() + 2u);
    portals.push_back(OrientedFunnelPortal{start, start});
    for (const OrientedFunnelPortal& portal : corridorPortals) {
        if (nearlyEqualVec2(portal.left, portal.right)) {
            continue;
        }
        if (!portals.empty() &&
            nearlyEqualVec2(portals.back().left, portal.left) &&
            nearlyEqualVec2(portals.back().right, portal.right)) {
            continue;
        }
        portals.push_back(portal);
    }
    portals.push_back(OrientedFunnelPortal{destination, destination});

    std::vector<glm::vec2> corners{start};
    glm::vec2 apex = start;
    glm::vec2 left = start;
    glm::vec2 right = start;
    std::size_t apexIndex = 0u;
    std::size_t leftIndex = 0u;
    std::size_t rightIndex = 0u;

    for (std::size_t portalIndex = 1u; portalIndex < portals.size(); ++portalIndex) {
        const glm::vec2 candidateLeft = portals[portalIndex].left;
        const glm::vec2 candidateRight = portals[portalIndex].right;

        if (funnelArea2(apex, right, candidateRight) >=
            -funnelAreaTolerance(apex, right, candidateRight)) {
            if (nearlyEqualVec2(apex, right) ||
                funnelArea2(apex, left, candidateRight) <=
                    funnelAreaTolerance(apex, left, candidateRight)) {
                right = candidateRight;
                rightIndex = portalIndex;
            } else {
                if (!nearlyEqualVec2(corners.back(), left)) {
                    corners.push_back(left);
                }
                apex = left;
                apexIndex = leftIndex;
                left = apex;
                right = apex;
                leftIndex = apexIndex;
                rightIndex = apexIndex;
                portalIndex = apexIndex;
                continue;
            }
        }

        if (funnelArea2(apex, left, candidateLeft) <=
            funnelAreaTolerance(apex, left, candidateLeft)) {
            if (nearlyEqualVec2(apex, left) ||
                funnelArea2(apex, right, candidateLeft) >=
                    -funnelAreaTolerance(apex, right, candidateLeft)) {
                left = candidateLeft;
                leftIndex = portalIndex;
            } else {
                if (!nearlyEqualVec2(corners.back(), right)) {
                    corners.push_back(right);
                }
                apex = right;
                apexIndex = rightIndex;
                left = apex;
                right = apex;
                leftIndex = apexIndex;
                rightIndex = apexIndex;
                portalIndex = apexIndex;
            }
        }
    }

    if (!nearlyEqualVec2(corners.back(), destination)) {
        corners.push_back(destination);
    }
    return corners;
}

std::optional<std::vector<glm::vec3>> buildFunnelPath(
    const NavigationSolveView& runtime,
    const ResolvedPathEndpoints& endpoints,
    const std::vector<NavCorridorStep>& corridor
) {
    std::vector<glm::vec3> corners{};
    glm::vec3 segmentStart = endpoints.resolvedStart;
    float segmentElevation = segmentStart.y;
    std::vector<OrientedFunnelPortal> portals{};

    const auto flushFunnel = [&](const glm::vec3& segmentEnd) {
        const std::vector<glm::vec2> pulled = pullFunnelCorners(
            glm::vec2(segmentStart.x, segmentStart.z),
            portals,
            glm::vec2(segmentEnd.x, segmentEnd.z)
        );
        for (std::size_t pointIndex = 1u; pointIndex < pulled.size(); ++pointIndex) {
            const bool isEndpoint = pointIndex + 1u == pulled.size();
            const float elevation = isEndpoint ? segmentEnd.y : segmentElevation;
            appendPathCorner(
                corners,
                glm::vec3(pulled[pointIndex].x, elevation, pulled[pointIndex].y),
                kPolygonEpsilon
            );
        }
    };

    for (const NavCorridorStep& step : corridor) {
        if (step.fromCellIndex >= runtime.graph.size() ||
            step.edgeIndex >= runtime.graph[step.fromCellIndex].size()) {
            return std::nullopt;
        }
        const NavGraphEdge& edge =
            runtime.graph[step.fromCellIndex][step.edgeIndex];
        if (!edge.viaLink) {
            if (!step.safePortal.has_value()) {
                return std::nullopt;
            }
            portals.push_back(orientPortalForCorridor(
                runtime,
                step.fromCellIndex,
                step.toCellIndex,
                *step.safePortal
            ));
            continue;
        }

        flushFunnel(edge.linkStartPoint);
        appendPathCorner(corners, edge.linkEndPoint, kPolygonEpsilon);
        segmentStart = edge.linkEndPoint;
        segmentElevation = edge.linkEndPoint.y;
        portals.clear();
    }

    flushFunnel(endpoints.resolvedDestination);
    return corners;
}


}  // namespace core::navigation_detail
