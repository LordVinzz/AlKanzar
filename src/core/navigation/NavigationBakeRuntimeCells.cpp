#include "core/navigation/NavigationDetailBake.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"
#include "core/navigation/NavigationDetailSurface.hpp"

#include <algorithm>
#include <cmath>

namespace core::navigation_detail {

std::vector<NavRuntimeCell> bakeLayerRuntimeCells(
    const BakeLayerData& layer,
    std::vector<std::vector<std::size_t>>& outCellToPolygonIndices
) {
    // Build the geometric union incrementally.  The previous implementation
    // extended every triangle edge into an infinite split line and applied all
    // of those lines to every triangle.  Tiny overlaps introduced by asset
    // serialization therefore turned a few dozen authored polygons into
    // thousands of runtime cells.
    std::vector<NavRuntimeCell> cells{};
    for (const BakedTriangle& triangle : layer.triangles) {
        appendConvexPolygonToUnion(
            cells,
            {
                triangle.verticesXZ[0],
                triangle.verticesXZ[1],
                triangle.verticesXZ[2],
            },
            layer.elevationY
        );
    }
    mergeAdjacentConvexCells(cells);

    // Normalize once after the first greedy pass. Imported/generated input can
    // already contain overlapping convex pieces; the guarded merge above can
    // only preserve disjointness when its input is a partition.
    std::vector<NavRuntimeCell> disjointCells{};
    disjointCells.reserve(cells.size());
    for (const NavRuntimeCell& cell : cells) {
        appendConvexPolygonToUnion(
            disjointCells,
            cell.verticesXZ,
            layer.elevationY
        );
    }
    mergeAdjacentConvexCells(disjointCells);
    cells = std::move(disjointCells);
    if (convexCellSetHasInteriorOverlap(cells)) {
        std::vector<NavRuntimeCell> finalDisjointCells{};
        finalDisjointCells.reserve(cells.size());
        for (const NavRuntimeCell& cell : cells) {
            appendConvexPolygonToUnion(
                finalDisjointCells,
                cell.verticesXZ,
                layer.elevationY
            );
        }
        cells = std::move(finalDisjointCells);
    }

    // Membership is metadata used by authored links and by clearance handling.
    // A runtime cell can cross an authored-polygon boundary after union/merge,
    // so determine membership from positive-area triangle intersections rather
    // than from the centroid alone.
    outCellToPolygonIndices.clear();
    outCellToPolygonIndices.resize(cells.size());
    for (std::size_t cellIndex = 0u; cellIndex < cells.size(); ++cellIndex) {
        std::vector<std::size_t>& memberships = outCellToPolygonIndices[cellIndex];
        for (const BakedTriangle& triangle : layer.triangles) {
            const std::vector<glm::vec2> triangleVertices{
                triangle.verticesXZ[0],
                triangle.verticesXZ[1],
                triangle.verticesXZ[2],
            };
            if (!polygonHasArea(clipConvexPolygons(
                    cells[cellIndex].verticesXZ,
                    triangleVertices))) {
                continue;
            }
            if (std::find(
                    memberships.begin(),
                    memberships.end(),
                    triangle.authoredPolygonIndex) == memberships.end()) {
                memberships.push_back(triangle.authoredPolygonIndex);
            }
        }
        std::sort(memberships.begin(), memberships.end());
    }
    return cells;
}

std::vector<NavRuntimeCell> bakeDisjointLayerRuntimeCells(
    const BakeLayerData& layer,
    std::vector<std::vector<std::size_t>>& outCellToPolygonIndices
) {
    std::vector<NavRuntimeCell> cells{};
    for (const AuthoredBakePolygon& polygon : layer.polygons) {
        std::vector<NavRuntimeCell> polygonCells{};
        for (const BakedTriangle& triangle : layer.triangles) {
            if (triangle.authoredPolygonIndex != polygon.assetIndex) {
                continue;
            }
            polygonCells.push_back(NavRuntimeCell{
                layer.elevationY,
                {
                    triangle.verticesXZ[0],
                    triangle.verticesXZ[1],
                    triangle.verticesXZ[2],
                }
            });
        }
        mergeAdjacentConvexCells(polygonCells);
        for (NavRuntimeCell& cell : polygonCells) {
            cells.push_back(std::move(cell));
            outCellToPolygonIndices.push_back({polygon.assetIndex});
        }
    }
    return cells;
}

bool bakeLayerHasInteriorPolygonOverlap(const BakeLayerData& layer) {
    struct TriangleGeometry {
        std::size_t authoredPolygonIndex{0u};
        std::vector<glm::vec2> verticesXZ{};
        glm::vec2 minXZ{0.0f};
        glm::vec2 maxXZ{0.0f};
    };

    std::vector<TriangleGeometry> triangles{};
    triangles.reserve(layer.triangles.size());
    for (const BakedTriangle& triangle : layer.triangles) {
        std::vector<glm::vec2> vertices{
            triangle.verticesXZ[0],
            triangle.verticesXZ[1],
            triangle.verticesXZ[2],
        };
        const auto [minXZ, maxXZ] = polygonBoundsXZ(vertices);
        triangles.push_back(TriangleGeometry{
            triangle.authoredPolygonIndex,
            std::move(vertices),
            minXZ,
            maxXZ
        });
    }

    std::vector<std::size_t> sweepOrder(triangles.size());
    for (std::size_t index = 0u; index < sweepOrder.size(); ++index) {
        sweepOrder[index] = index;
    }
    std::sort(sweepOrder.begin(), sweepOrder.end(), [&](std::size_t lhs, std::size_t rhs) {
        if (triangles[lhs].minXZ.x != triangles[rhs].minXZ.x) {
            return triangles[lhs].minXZ.x < triangles[rhs].minXZ.x;
        }
        return lhs < rhs;
    });

    for (std::size_t orderIndex = 0u; orderIndex < sweepOrder.size(); ++orderIndex) {
        const TriangleGeometry& lhs = triangles[sweepOrder[orderIndex]];
        for (std::size_t candidateOrder = orderIndex + 1u; candidateOrder < sweepOrder.size(); ++candidateOrder) {
            const TriangleGeometry& rhs = triangles[sweepOrder[candidateOrder]];
            if (rhs.minXZ.x > lhs.maxXZ.x + kPolygonEpsilon) {
                break;
            }
            if (lhs.authoredPolygonIndex == rhs.authoredPolygonIndex ||
                !boundsOverlapXZ(lhs.minXZ, lhs.maxXZ, rhs.minXZ, rhs.maxXZ)) {
                continue;
            }
            if (convexPolygonsHaveInteriorOverlap(
                    lhs.verticesXZ,
                    rhs.verticesXZ)) {
                return true;
            }
        }
    }
    return false;
}

bool runtimeCellsHaveInteriorOverlap(
    const std::vector<NavRuntimeCell>& cells,
    const std::vector<glm::vec2>& cellMinXZ,
    const std::vector<glm::vec2>& cellMaxXZ
) {
    if (cellMinXZ.size() != cells.size() || cellMaxXZ.size() != cells.size()) {
        return true;
    }
    std::vector<std::size_t> sweepOrder(cells.size());
    for (std::size_t index = 0u; index < sweepOrder.size(); ++index) {
        sweepOrder[index] = index;
    }
    std::sort(
        sweepOrder.begin(),
        sweepOrder.end(),
        [&](std::size_t lhs, std::size_t rhs) {
            return cellMinXZ[lhs].x < cellMinXZ[rhs].x;
        }
    );
    for (std::size_t orderIndex = 0u;
         orderIndex < sweepOrder.size();
         ++orderIndex) {
        const std::size_t lhs = sweepOrder[orderIndex];
        for (std::size_t candidateOrder = orderIndex + 1u;
             candidateOrder < sweepOrder.size();
             ++candidateOrder) {
            const std::size_t rhs = sweepOrder[candidateOrder];
            if (cellMinXZ[rhs].x > cellMaxXZ[lhs].x + kPolygonEpsilon) {
                break;
            }
            if (std::abs(cells[lhs].elevationY - cells[rhs].elevationY) >
                    kLayerGroupingEpsilon ||
                !boundsOverlapXZ(
                    cellMinXZ[lhs],
                    cellMaxXZ[lhs],
                    cellMinXZ[rhs],
                    cellMaxXZ[rhs])) {
                continue;
            }
            if (convexPolygonsHaveInteriorOverlap(
                    cells[lhs].verticesXZ,
                    cells[rhs].verticesXZ)) {
                return true;
            }
        }
    }
    return false;
}


}  // namespace core::navigation_detail
