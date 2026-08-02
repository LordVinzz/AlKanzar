#include "core/navigation/NavigationDetailBake.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"

#include <algorithm>
#include <unordered_set>

namespace core::navigation_detail {

void appendGraphEdgeIfMissing(
    std::vector<NavGraphEdge>& edges,
    const NavGraphEdge& candidate
) {
    const auto it = std::find_if(edges.begin(), edges.end(), [&](const NavGraphEdge& existing) {
        return existing.targetCellIndex == candidate.targetCellIndex &&
            existing.viaLink == candidate.viaLink &&
            existing.linkId == candidate.linkId &&
            nearlyEqualVec2(existing.portalA, candidate.portalA) &&
            nearlyEqualVec2(existing.portalB, candidate.portalB) &&
            nearlyEqualVec3(existing.linkStartPoint, candidate.linkStartPoint) &&
            nearlyEqualVec3(existing.linkEndPoint, candidate.linkEndPoint);
    });
    if (it == edges.end()) {
        edges.push_back(candidate);
    }
}

void rebuildCellBoundaryVertexCache(NavigationRuntime& runtime) {
    runtime.bakedCellBoundaryVertices.clear();
    runtime.bakedCellBoundaryVertices.resize(runtime.bakedCells.size());
    std::unordered_set<QuantizedLayerPoint, QuantizedLayerPointHash> boundaryPoints{};

    for (std::size_t cellIndex = 0u; cellIndex < runtime.bakedCells.size(); ++cellIndex) {
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        runtime.bakedCellBoundaryVertices[cellIndex].assign(cell.verticesXZ.size(), 0u);
        if (cell.verticesXZ.size() < 3u || cellIndex >= runtime.graph.size()) {
            continue;
        }

        const auto edgeCoveredFromVertex = [&](const glm::vec2& vertex, const glm::vec2& adjacent) {
            const glm::vec2 edgeDelta = adjacent - vertex;
            const float edgeLength = glm::length(edgeDelta);
            if (edgeLength <= kPlaneEpsilon) {
                return false;
            }
            const float probeDistance = std::min(edgeLength * 0.25f, kPolygonEpsilon * 8.0f);
            const glm::vec2 probe = vertex + edgeDelta * (probeDistance / edgeLength);
            for (const NavGraphEdge& graphEdge : runtime.graph[cellIndex]) {
                if (!graphEdge.viaLink && pointOnSegmentXZ(probe, graphEdge.portalA, graphEdge.portalB)) {
                    return true;
                }
            }
            return false;
        };

        for (std::size_t vertexIndex = 0u; vertexIndex < cell.verticesXZ.size(); ++vertexIndex) {
            const glm::vec2& vertex = cell.verticesXZ[vertexIndex];
            const glm::vec2& previous = cell.verticesXZ[
                (vertexIndex + cell.verticesXZ.size() - 1u) % cell.verticesXZ.size()
            ];
            const glm::vec2& next = cell.verticesXZ[(vertexIndex + 1u) % cell.verticesXZ.size()];
            if (!edgeCoveredFromVertex(vertex, previous) || !edgeCoveredFromVertex(vertex, next)) {
                boundaryPoints.insert(quantizeLayerPoint(vertex, cell.elevationY));
            }
        }
    }

    for (std::size_t cellIndex = 0u; cellIndex < runtime.bakedCells.size(); ++cellIndex) {
        const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
        std::vector<std::uint8_t>& flags = runtime.bakedCellBoundaryVertices[cellIndex];
        for (std::size_t vertexIndex = 0u; vertexIndex < cell.verticesXZ.size(); ++vertexIndex) {
            flags[vertexIndex] = boundaryPoints.contains(
                quantizeLayerPoint(cell.verticesXZ[vertexIndex], cell.elevationY)
            ) ? 1u : 0u;
        }
    }
}
void setRuntimeStatus(NavigationRuntime& runtime, std::string message, bool isError) {
    runtime.statusMessage = std::move(message);
    runtime.statusIsError = isError;
}

std::optional<std::size_t> findPolygonIndexById(const NavigationRuntime& runtime, int polygonId) {
    const auto it = runtime.polygonIndexById.find(polygonId);
    if (it == runtime.polygonIndexById.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::size_t> findPolygonIndexById(const NavMeshAsset& asset, int polygonId) {
    for (std::size_t index = 0; index < asset.polygons.size(); ++index) {
        if (asset.polygons[index].id == polygonId) {
            return index;
        }
    }
    return std::nullopt;
}

glm::vec3 cellCenter3(const NavigationRuntime& runtime, std::size_t cellIndex) {
    const NavRuntimeCell& cell = runtime.bakedCells[cellIndex];
    const glm::vec2 center = cellIndex < runtime.bakedCellCenters.size()
        ? runtime.bakedCellCenters[cellIndex]
        : polygonCentroidXZ(cell.verticesXZ);
    return glm::vec3(center.x, cell.elevationY, center.y);
}

}  // namespace core::navigation_detail
