#include "core/navigation/Navigation.hpp"
#include "core/navigation/NavigationDetailBake.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"
#include "core/navigation/NavigationDetailRuntime.hpp"
#include "core/navigation/NavigationDetailTraversal.hpp"
#include "core/navigation/NavigationDetailTypes.hpp"
#include "core/navigation/Polyanya.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>

namespace core {

using namespace navigation_detail;

bool NavigationSystem::rebuildRuntime(NavigationRuntime& runtime, std::string* error) const {
    return rebuildRuntimeInternal(runtime, error, false);
}

bool NavigationSystem::rebuildRuntimeInternal(
    NavigationRuntime& runtime,
    std::string* error,
    bool sourcePolygonsAreDisjoint
) const {
    invalidatePendingPathRequests();
    runtime.solveSnapshot.reset();
    runtime.polyanyaMesh.reset();
    runtime.exactPathfindingWarning.clear();
    runtime.bakedCellsHaveInteriorOverlap = false;
    ++runtime.solveRevision;
    runtime.polygonIndexById.clear();
    runtime.polygonCenters.clear();
    runtime.bakedCells.clear();
    runtime.bakedCellCenters.clear();
    runtime.bakedCellMinXZ.clear();
    runtime.bakedCellMaxXZ.clear();
    runtime.bakedCellBoundaryVertices.clear();
    runtime.polygonToCellIndices.clear();
    runtime.cellToPolygonIndices.clear();
    runtime.graph.clear();

    std::vector<BakeLayerData> bakeLayers{};
    runtime.polygonToCellIndices.resize(runtime.asset.polygons.size());
    for (std::size_t index = 0; index < runtime.asset.polygons.size(); ++index) {
        const NavPolygon& polygon = runtime.asset.polygons[index];
        if (!polygonValid(polygon)) {
            if (error) {
                *error = "Invalid polygon geometry for polygon id " + std::to_string(polygon.id);
            }
            return false;
        }
        runtime.polygonIndexById[polygon.id] = index;
        runtime.polygonCenters.push_back(polygonCentroidXZ(polygon.verticesXZ));

        std::vector<glm::vec2> normalized = normalizePolygonVertices(polygon.verticesXZ);
        if (!polygonIsSimple(normalized)) {
            if (error) {
                *error = "Self-intersecting polygon geometry for polygon id " + std::to_string(polygon.id);
            }
            return false;
        }
        if (!polygonHasArea(normalized)) {
            if (error) {
                *error = "Degenerate polygon geometry for polygon id " + std::to_string(polygon.id);
            }
            return false;
        }
        const std::vector<std::array<glm::vec2, 3u>> triangles = triangulateSimplePolygon(normalized);
        if (triangles.empty()) {
            if (error) {
                *error = "Failed to triangulate polygon id " + std::to_string(polygon.id);
            }
            return false;
        }

        auto layerIt = std::find_if(bakeLayers.begin(), bakeLayers.end(), [&](const BakeLayerData& layer) {
            return std::abs(layer.elevationY - polygon.elevationY) <= kLayerGroupingEpsilon;
        });
        if (layerIt == bakeLayers.end()) {
            bakeLayers.push_back(BakeLayerData{polygon.elevationY});
            layerIt = bakeLayers.end() - 1;
        }

        layerIt->polygons.push_back(AuthoredBakePolygon{index, polygon.elevationY, normalized});
        for (const auto& triangle : triangles) {
            layerIt->triangles.push_back(BakedTriangle{
                index,
                polygon.elevationY,
                triangle
            });
        }
    }

    for (const BakeLayerData& layer : bakeLayers) {
        std::vector<std::vector<std::size_t>> layerCellToPolygonIndices{};
        const bool mayUseDisjointFastPath = sourcePolygonsAreDisjoint ||
            !bakeLayerHasInteriorPolygonOverlap(layer);
        std::vector<NavRuntimeCell> layerCells{};
        if (mayUseDisjointFastPath) {
            layerCells = bakeDisjointLayerRuntimeCells(
                layer,
                layerCellToPolygonIndices
            );
            std::vector<glm::vec2> layerMinXZ{};
            std::vector<glm::vec2> layerMaxXZ{};
            layerMinXZ.reserve(layerCells.size());
            layerMaxXZ.reserve(layerCells.size());
            for (const NavRuntimeCell& cell : layerCells) {
                const auto [minXZ, maxXZ] =
                    polygonBoundsXZ(cell.verticesXZ);
                layerMinXZ.push_back(minXZ);
                layerMaxXZ.push_back(maxXZ);
            }
            // The source hint accelerates the common generated-mesh case, but
            // never bypasses verification. If clipping/quantization produced
            // a real overlap, rebuild this layer as an explicit union.
            if (runtimeCellsHaveInteriorOverlap(
                    layerCells,
                    layerMinXZ,
                    layerMaxXZ)) {
                layerCells = bakeLayerRuntimeCells(
                    layer,
                    layerCellToPolygonIndices
                );
            }
        } else {
            layerCells = bakeLayerRuntimeCells(
                layer,
                layerCellToPolygonIndices
            );
        }
        for (std::size_t localCellIndex = 0; localCellIndex < layerCells.size(); ++localCellIndex) {
            const std::vector<glm::vec2>& cellVertices =
                layerCells[localCellIndex].verticesXZ;
            const std::size_t globalCellIndex = runtime.bakedCells.size();
            const auto [cellMinXZ, cellMaxXZ] = polygonBoundsXZ(layerCells[localCellIndex].verticesXZ);
            runtime.bakedCellCenters.push_back(polygonCentroidXZ(layerCells[localCellIndex].verticesXZ));
            runtime.bakedCellMinXZ.push_back(cellMinXZ);
            runtime.bakedCellMaxXZ.push_back(cellMaxXZ);
            runtime.bakedCells.push_back(std::move(layerCells[localCellIndex]));
            runtime.cellToPolygonIndices.push_back(layerCellToPolygonIndices[localCellIndex]);
            for (std::size_t polygonIndex : runtime.cellToPolygonIndices.back()) {
                if (polygonIndex < runtime.polygonToCellIndices.size()) {
                    runtime.polygonToCellIndices[polygonIndex].push_back(globalCellIndex);
                }
            }
        }
    }

    runtime.bakedCellsHaveInteriorOverlap = runtimeCellsHaveInteriorOverlap(
        runtime.bakedCells,
        runtime.bakedCellMinXZ,
        runtime.bakedCellMaxXZ
    );
    runtime.graph.resize(runtime.bakedCells.size());
    std::vector<std::size_t> graphSweepOrder(runtime.bakedCells.size());
    for (std::size_t cellIndex = 0u;
         cellIndex < graphSweepOrder.size();
         ++cellIndex) {
        graphSweepOrder[cellIndex] = cellIndex;
    }
    std::sort(
        graphSweepOrder.begin(),
        graphSweepOrder.end(),
        [&](std::size_t lhs, std::size_t rhs) {
            if (runtime.bakedCellMinXZ[lhs].x !=
                runtime.bakedCellMinXZ[rhs].x) {
                return runtime.bakedCellMinXZ[lhs].x <
                    runtime.bakedCellMinXZ[rhs].x;
            }
            return lhs < rhs;
        }
    );
    for (std::size_t lhsOrder = 0u;
         lhsOrder < graphSweepOrder.size();
         ++lhsOrder) {
        const std::size_t lhsIndex = graphSweepOrder[lhsOrder];
        const NavRuntimeCell& lhs = runtime.bakedCells[lhsIndex];
        for (std::size_t rhsOrder = lhsOrder + 1u;
             rhsOrder < graphSweepOrder.size();
             ++rhsOrder) {
            const std::size_t rhsIndex = graphSweepOrder[rhsOrder];
            if (runtime.bakedCellMinXZ[rhsIndex].x >
                runtime.bakedCellMaxXZ[lhsIndex].x +
                    kPortalBroadPhaseEpsilon) {
                break;
            }
            const NavRuntimeCell& rhs = runtime.bakedCells[rhsIndex];
            if (std::abs(lhs.elevationY - rhs.elevationY) >
                    kLayerGroupingEpsilon ||
                runtime.bakedCellMinXZ[rhsIndex].y >
                    runtime.bakedCellMaxXZ[lhsIndex].y +
                        kPortalBroadPhaseEpsilon ||
                runtime.bakedCellMaxXZ[rhsIndex].y <
                    runtime.bakedCellMinXZ[lhsIndex].y -
                        kPortalBroadPhaseEpsilon) {
                continue;
            }
            const std::vector<SharedPortalResult> portals =
                sharedBoundaryPortals(lhs, rhs);
            if (portals.empty()) {
                continue;
            }
            for (const SharedPortalResult& portal : portals) {
                runtime.graph[lhsIndex].push_back(NavGraphEdge{
                    rhsIndex,
                    false,
                    -1,
                    portal.b,
                    portal.a,
                    glm::vec3(0.0f),
                    glm::vec3(0.0f)
                });
                runtime.graph[rhsIndex].push_back(NavGraphEdge{
                    lhsIndex,
                    false,
                    -1,
                    portal.a,
                    portal.b,
                    glm::vec3(0.0f),
                    glm::vec3(0.0f)
                });
            }
        }
    }

    rebuildCellBoundaryVertexCache(runtime);

    const NavigationSolveView solveView = makeSolveView(runtime);
    for (const NavLink& link : runtime.asset.links) {
        const auto fromIndex = findPolygonIndexById(solveView, link.fromPolygonId);
        const auto toIndex = findPolygonIndexById(solveView, link.toPolygonId);
        if (!fromIndex.has_value() || !toIndex.has_value()) {
            continue;
        }
        std::vector<std::size_t> fromCells = findLinkEndpointCells(solveView, *fromIndex, link.fromPoint);
        std::vector<std::size_t> toCells = findLinkEndpointCells(solveView, *toIndex, link.toPoint);
        if (fromCells.empty()) {
            if (const auto fallback = findNearestCandidateCell(solveView, runtime.polygonToCellIndices[*fromIndex], link.fromPoint);
                fallback.has_value()) {
                fromCells.push_back(*fallback);
            }
        }
        if (toCells.empty()) {
            if (const auto fallback = findNearestCandidateCell(solveView, runtime.polygonToCellIndices[*toIndex], link.toPoint);
                fallback.has_value()) {
                toCells.push_back(*fallback);
            }
        }
        for (std::size_t fromCellIndex : fromCells) {
            for (std::size_t toCellIndex : toCells) {
                appendGraphEdgeIfMissing(runtime.graph[fromCellIndex], NavGraphEdge{
                    toCellIndex,
                    true,
                    link.id,
                    glm::vec2(0.0f),
                    glm::vec2(0.0f),
                    link.fromPoint,
                    link.toPoint
                });
                if (link.bidirectional) {
                    appendGraphEdgeIfMissing(runtime.graph[toCellIndex], NavGraphEdge{
                        fromCellIndex,
                        true,
                        link.id,
                        glm::vec2(0.0f),
                        glm::vec2(0.0f),
                        link.toPoint,
                        link.fromPoint
                    });
                }
            }
        }
    }
    std::string polyanyaError{};
    if (runtime.bakedCellsHaveInteriorOverlap) {
        polyanyaError =
            "runtime cells still have positive-area interior overlap";
    } else {
        runtime.polyanyaMesh = navigation_detail::buildPolyanyaMesh(
            runtime.bakedCells,
            runtime.graph,
            &polyanyaError
        );
    }
    if (runtime.polyanyaMesh == nullptr) {
        runtime.exactPathfindingWarning =
            "Exact Polyanya pathfinding unavailable: " + polyanyaError +
            ". Using the validated A* + funnel fallback.";
    }
    runtime.solveSnapshot = buildSolveSnapshot(runtime);
    return true;
}

}  // namespace core
